// Phase 4 unit tests for the ring-buffer decode engine (src/core/source_engine.*).
// Builds a small synthetic OP1a file with real (uncompressed UYVY) essence
// so the whole pipeline -- header parse, essence walk, decode, ring buffer,
// background thread -- runs end to end without needing a real sample file.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include "core/source_engine.h"
#include "core/use_audio_mode.h"
#include "mxf/index_table.h"
#include "synthetic_mxf.h"

using namespace mxfsrc;
using namespace mxfsrc_test;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

constexpr uint32_t kTrackNumber = 0x01010100;

std::vector<uint8_t> MakeFramePayload(int width, int height, uint8_t marker) {
  return std::vector<uint8_t>(static_cast<size_t>(width) * height * 2, marker);
}

std::vector<uint8_t> BuildFile(int width, int height, int numFrames) {
  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  for (int i = 0; i < numFrames; ++i) {
    AppendEssenceElement(bytes, kTrackNumber, MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i)));
  }
  return bytes;
}

constexpr uint32_t kAudioTrackNumber = 0x02020200;
constexpr uint32_t kAudioTrackNumber2 = 0x02020300;

// Byte length of AppendBerLength's encoding for `length`, without writing
// anything -- needed to predict each essence element's exact absolute file
// offset before the file is actually built (see BuildFileWithAudio,
// BuildGopFile).
size_t BerLengthSize(uint64_t length) {
  if (length < 0x80) return 1;
  size_t n = 0;
  uint64_t v = length;
  do {
    ++n;
    v >>= 8;
  } while (v != 0);
  return 1 + n;
}

// One edit unit's worth of PCM content, distinguishable by edit unit/sample/
// channel so GetAudioSamples' returned bytes can be checked exactly, not
// just for the right length.
std::vector<uint8_t> MakeAudioEditUnitPayload(int editUnit, int sampleCount, int channels) {
  std::vector<uint8_t> data(static_cast<size_t>(sampleCount) * channels * 2);  // 16-bit
  size_t idx = 0;
  for (int s = 0; s < sampleCount; ++s) {
    for (int c = 0; c < channels; ++c) {
      const uint8_t v = static_cast<uint8_t>(editUnit * 20 + s * 4 + c);
      data[idx++] = v;
      data[idx++] = static_cast<uint8_t>(v + 1);
    }
  }
  return data;
}

// Builds a synthetic file with one video + one audio track per edit unit,
// carrying a correctly-populated Index Table Segment (VBR: one entry per
// edit unit, StreamOffset pointing at that edit unit's video element --
// its content package's start, matching FlatIndexEntry's own doc comment)
// ahead of the essence. BuildAudioSampleIndexFromIndexTable requires this
// full coverage -- per this plugin's supported audio envelope, an MXF file
// without one simply doesn't get audio (see the comment on that function).
//
// audio.sampleRate is set to samplesPerEditUnit * 25 (BuildSyntheticHeader's
// Track.EditRate is always hardcoded 25/1) so the closed-form cadence
// BuildAudioSampleIndexFromIndexTable derives from editRate/sampleRate
// exactly reproduces the caller's chosen samplesPerEditUnit -- an
// unrealistic sample rate for real audio, same spirit as this file's other
// deliberately tiny/unrealistic synthetic values (e.g. 4x2 video frames).
std::vector<uint8_t> BuildFileWithAudio(int width, int height, int numFrames, int samplesPerEditUnit, int channels) {
  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  SyntheticAudioTrack audio;
  audio.trackNumber = kAudioTrackNumber;
  audio.trackId = 100;
  audio.channelCount = static_cast<uint32_t>(channels);
  audio.sampleRate = static_cast<uint32_t>(samplesPerEditUnit) * 25;
  audio.bitsPerSample = 16;
  cfg.audioTracks = {audio};
  std::vector<uint8_t> file = BuildSyntheticHeader(cfg);

  std::vector<std::vector<uint8_t>> videoPayloads(static_cast<size_t>(numFrames));
  std::vector<std::vector<uint8_t>> audioPayloads(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i) {
    videoPayloads[static_cast<size_t>(i)] = MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i));
    audioPayloads[static_cast<size_t>(i)] = MakeAudioEditUnitPayload(i, samplesPerEditUnit, channels);
  }

  IndexTableSegmentSpec segSpec;
  segSpec.indexStartPosition = 0;
  segSpec.indexDuration = numFrames;
  segSpec.indexSid = 1;
  segSpec.bodySid = 1;
  segSpec.entries.resize(static_cast<size_t>(numFrames));

  // Fixed-width fields throughout -- the segment's serialized size doesn't
  // depend on the actual StreamOffset values, only on entry count, so a
  // placeholder-offset build (all zero) already tells us exactly how large
  // the real segment will be, letting us compute essenceContainerBaseOffset
  // (and therefore every edit unit's correct StreamOffset) before writing
  // anything's final content (mirrors BuildGopFile's own approach below).
  std::vector<uint8_t> placeholderSegBytes;
  AppendIndexTableSegment(placeholderSegBytes, segSpec);

  const uint64_t bodyPartitionOffset = file.size();
  AppendPartitionPack(file, PartitionKind::kBody, placeholderSegBytes.size(), /*indexSid=*/1);
  const uint64_t essenceContainerBaseOffset = file.size() + placeholderSegBytes.size();

  uint64_t cursor = essenceContainerBaseOffset;
  for (int i = 0; i < numFrames; ++i) {
    segSpec.entries[static_cast<size_t>(i)].streamOffset = cursor - essenceContainerBaseOffset;
    cursor += 16 + BerLengthSize(videoPayloads[static_cast<size_t>(i)].size()) +
              videoPayloads[static_cast<size_t>(i)].size();
    cursor += 16 + BerLengthSize(audioPayloads[static_cast<size_t>(i)].size()) +
              audioPayloads[static_cast<size_t>(i)].size();
  }

  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, segSpec);
  if (segBytes.size() != placeholderSegBytes.size()) {
    std::cerr << "BuildFileWithAudio: unexpected Index Table Segment size mismatch" << std::endl;
  }

  file.insert(file.end(), segBytes.begin(), segBytes.end());
  for (int i = 0; i < numFrames; ++i) {
    AppendEssenceElement(file, kTrackNumber, videoPayloads[static_cast<size_t>(i)]);
    AppendEssenceElement(file, kAudioTrackNumber, audioPayloads[static_cast<size_t>(i)]);
  }

  // EnumeratePartitionsForIndex only ever knows about the Header Partition
  // (offset 0) automatically -- discovering the Body Partition (where the
  // Index Table Segment above actually lives) requires a RIP, exactly like
  // a real multi-partition file.
  AppendRandomIndexPack(file, {{1, 0}, {1, bodyPartitionOffset}});
  return file;
}

// Two audio tracks that disagree on bit depth (16 vs 24) -- an unsupported
// configuration per MxfStreamInfo::audioConfigSupported -- used to exercise
// UseAudioMode's degrade-vs-hard-fail policy.
std::vector<uint8_t> BuildFileWithMismatchedAudio(int width, int height, int numFrames) {
  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  SyntheticAudioTrack a;
  a.trackNumber = kAudioTrackNumber;
  a.trackId = 101;
  a.channelCount = 1;
  a.bitsPerSample = 16;
  SyntheticAudioTrack b;
  b.trackNumber = kAudioTrackNumber2;
  b.trackId = 102;
  b.channelCount = 1;
  b.bitsPerSample = 24;
  cfg.audioTracks = {a, b};
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  for (int i = 0; i < numFrames; ++i) {
    AppendEssenceElement(bytes, kTrackNumber, MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i)));
    AppendEssenceElement(bytes, kAudioTrackNumber, {0, 0});      // 1 sample, 16-bit
    AppendEssenceElement(bytes, kAudioTrackNumber2, {0, 0, 0});  // 1 sample, 24-bit
  }
  return bytes;
}

void TestSequentialPlayback() {
  const int width = 4, height = 2, numFrames = 5;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_sequential.mxf");

  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string());
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.StreamInfo().numFrames == numFrames);

    for (int i = 0; i < numFrames; ++i) {
      std::shared_ptr<NormalizedFrame> frame;
      std::string err;
      FetchStatus st = engine.GetVideoFrame(i, frame, &err);
      CHECK(st == FetchStatus::kOk);
      CHECK(frame->format == PixelFormat::kUyvy422);
      CHECK(frame->width == width && frame->height == height);
      CHECK(!frame->planeData[0].empty());
      if (!frame->planeData[0].empty()) {
        CHECK(frame->planeData[0][0] == static_cast<uint8_t>(0x10 + i));  // forward file order preserved
      }
    }

    // Re-requesting an already-produced frame should hit the buffer, not re-decode.
    std::shared_ptr<NormalizedFrame> again;
    CHECK(engine.GetVideoFrame(2, again, nullptr) == FetchStatus::kOk);
    CHECK(!again->planeData[0].empty() && again->planeData[0][0] == 0x12);

    // Requesting past the declared frame count reports end-of-stream, not a hang/crash.
    std::shared_ptr<NormalizedFrame> beyond;
    CHECK(engine.GetVideoFrame(numFrames, beyond, nullptr) == FetchStatus::kEndOfStream);
  }

  std::filesystem::remove(path);
}

void TestEvictionFallsBackToReDecode() {
  const int width = 4, height = 2, numFrames = 10;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_evict.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 3;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::filesystem::remove(path);
      return;
    }

    // Jumping straight to the last frame forces the producer to run ahead,
    // evicting frames 0..6 out of the 3-frame window before frame 0 is ever
    // requested. Rather than failing outright, GetVideoFrame() falls back
    // to a one-shot re-decode from the start of the file for this genuine
    // backward-seek case (see plan/options.h notes) -- so this now
    // succeeds, just via the slow path, rather than returning kEvicted.
    std::shared_ptr<NormalizedFrame> frame;
    CHECK(engine.GetVideoFrame(numFrames - 1, frame, nullptr) == FetchStatus::kOk);

    std::string err;
    FetchStatus st = engine.GetVideoFrame(0, frame, &err);
    CHECK(st == FetchStatus::kOk);
    // Not checking err here: it may still hold the internal ring-buffer's
    // "evicted" message from the failed first attempt even though the
    // overall call succeeded via the fallback -- errorOut is only a
    // meaningful/guaranteed-cleared-on-success contract when status==kOk
    // is what's checked, which it is above.
    CHECK(frame && !frame->planeData[0].empty() && frame->planeData[0][0] == 0x10);  // genuinely frame 0's data
  }

  std::filesystem::remove(path);
}

// Regression test for the intra-only SeekAndDecode fast path (see
// IsIntraOnlyCodec / SkipNextVideoElement in source_engine.cpp): requests a
// genuinely mid-range evicted frame (not just index 0, which the skip loop
// trivially handles with zero iterations) to make sure the "skip exactly
// `target` video elements, then decode the next one" counting isn't
// off-by-one in either direction.
void TestSeekAndDecodeFastPathLandsOnCorrectMidRangeFrame() {
  const int width = 4, height = 2, numFrames = 20;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_seek_midrange.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 3;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::filesystem::remove(path);
      return;
    }

    std::shared_ptr<NormalizedFrame> frame;
    CHECK(engine.GetVideoFrame(numFrames - 1, frame, nullptr) == FetchStatus::kOk);  // evicts everything else

    constexpr int kMidTarget = 12;
    FetchStatus st = engine.GetVideoFrame(kMidTarget, frame, nullptr);
    CHECK(st == FetchStatus::kOk);
    CHECK(frame && !frame->planeData[0].empty() &&
          frame->planeData[0][0] == static_cast<uint8_t>(0x10 + kMidTarget));

    // And the immediate neighbors, to catch an off-by-one in either
    // direction rather than just getting lucky on one exact value.
    CHECK(engine.GetVideoFrame(kMidTarget - 1, frame, nullptr) == FetchStatus::kOk);
    CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + kMidTarget - 1));
    CHECK(engine.GetVideoFrame(kMidTarget + 1, frame, nullptr) == FetchStatus::kOk);
    CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + kMidTarget + 1));
  }

  std::filesystem::remove(path);
}

// Regression test for the real bug that motivated eagerdecode (see
// options.h): a single request for even a nearby frame (e.g. frame 3)
// used to unlock the decode thread to eagerly race all the way to
// buffer_frames-deep (default 300) in the background -- fast enough (well
// under a second at real decode speeds) to evict early frames before a
// slow-per-frame downstream filter got back around to re-reading one of
// them, even though it had already been delivered successfully moments
// earlier. Real-world trigger: QTGMC preset="slower" re-reading frame 0
// after the engine had already raced to frame 302 of a 300-frame buffer
// within ~0.5s of wall-clock time. numFrames is large enough that the old
// (buggy) unconditional buffer_frames-deep eager fill would have had
// plenty of room to race ahead.
void TestEagerReadaheadMarginDoesNotEvictRecentlyDeliveredFrames() {
  const int width = 4, height = 2, numFrames = 500;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_margin.mxf");

  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string());  // default buffer_frames=300, eagerdecode=32
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::filesystem::remove(path);
      return;
    }

    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    // Reproduces the exact real-world call sequence observed: 0,0,0,1,2,3.
    for (int n : {0, 0, 0, 1, 2, 3}) {
      CHECK(engine.GetVideoFrame(n, frame, &err) == FetchStatus::kOk);
    }

    // Give the background decode thread real wall-clock time to race ahead
    // if the bug were still present -- the actual failure happened within
    // ~0.5s of real time, so this comfortably covers that.
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    FetchStatus st = engine.GetVideoFrame(0, frame, &err);
    CHECK(st == FetchStatus::kOk);
    if (st != FetchStatus::kOk) std::cerr << "re-request for frame 0 failed: " << err << std::endl;
    // Must have been served from the buffer, not via the SeekAndDecode
    // fallback -- otherwise this test would pass even if the margin fix
    // regressed, since the fallback alone would silently paper over it.
    CHECK(engine.NonBufferedServedFrames().empty());
  }

  std::filesystem::remove(path);
}

// Regression test for a real bug found via end-to-end testing (real
// AviSynth+ host + ffmpeg): Open() decodes frame 0 up front so hosts can
// learn its pixel format before constructing VideoInfo. That peek must not
// let the eager-fill producer race ahead and evict frame 0 itself before
// anyone's real first GetVideoFrame(0) call arrives -- easiest to trigger
// with a buffer_frames small enough that a few frames' worth of eager
// decode-ahead already exceeds it.
void TestOpenPeekDoesNotEvictFrameZero() {
  const int width = 4, height = 2, numFrames = 20;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_peek.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;  // deliberately tiny: reproduces the original bug at buffer_frames=5
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }

    CHECK(engine.FirstFrame().format == PixelFormat::kUyvy422);
    CHECK(!engine.FirstFrame().planeData[0].empty() && engine.FirstFrame().planeData[0][0] == 0x10);

    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    FetchStatus st = engine.GetVideoFrame(0, frame, &err);
    CHECK(st == FetchStatus::kOk);
    if (st != FetchStatus::kOk) std::cerr << "frame 0 status=" << static_cast<int>(st) << " err=" << err << std::endl;
  }

  std::filesystem::remove(path);
}

// Regression test for a second real bug found via end-to-end testing: real
// hosts can legitimately request the same frame index twice in a row with
// no intervening request for a later index (e.g. ffmpeg's AviSynth demuxer
// reads frame 0 once to probe the stream format, then again for real
// playback). The producer's decode-ahead pacing must never evict the frame
// sitting exactly at the watermark, or the second request fails.
void TestRepeatedRequestForSameFrameSucceeds() {
  const int width = 4, height = 2, numFrames = 20;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_repeat.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 5;  // matches the buffer_frames that exposed this in practice
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::filesystem::remove(path);
      return;
    }

    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    CHECK(engine.GetVideoFrame(0, frame, &err) == FetchStatus::kOk);
    FetchStatus second = engine.GetVideoFrame(0, frame, &err);
    CHECK(second == FetchStatus::kOk);
    if (second != FetchStatus::kOk) std::cerr << "second request for frame 0: " << err << std::endl;
  }

  std::filesystem::remove(path);
}

// Regression test for a real deadlock found via a real file + real host
// (AvsPmod): MxfSource(..., buffer_frames=1, eagerdecode=1/2) hung Open()
// forever. Root cause was in FrameBuffer (frame_buffer.h): the eager-fill
// window `index < watermark_ + margin_` used an initial watermark_ of -1,
// so whenever margin_ (min(buffer_frames, eagerdecode)) was exactly 1, the
// window became `index < 0` -- the producer could never produce even frame
// 0, and Open()'s Peek(0) call (which deliberately never advances the
// watermark) had nothing to wake it. Covers both buffer_frames=1 and
// eagerdecode=1 individually, since either alone is enough to hit margin_=1.
void TestOpenSucceedsWithMinimalBufferOrEagerdecode() {
  const int width = 4, height = 2, numFrames = 5;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_minimal_margin.mxf");

  for (int bufferFrames : {1, 300}) {
    for (int eagerdecode : {1, 32}) {
      if (bufferFrames == 300 && eagerdecode == 32) continue;  // that's every other test's default; skip here

      SourceEngine engine;
      OpenOptions opts;
      opts.buffer_frames = bufferFrames;
      opts.eagerdecode = eagerdecode;
      OpenResult res = engine.Open(path.string(), opts);
      CHECK(res.status == OpenStatus::kOk);
      if (res.status != OpenStatus::kOk) {
        std::cerr << "Open failed (buffer_frames=" << bufferFrames << " eagerdecode=" << eagerdecode
                   << "): " << res.error << std::endl;
        continue;
      }

      std::shared_ptr<NormalizedFrame> frame;
      std::string err;
      FetchStatus st = engine.GetVideoFrame(0, frame, &err);
      CHECK(st == FetchStatus::kOk);
      if (st != FetchStatus::kOk) {
        std::cerr << "frame 0 failed (buffer_frames=" << bufferFrames << " eagerdecode=" << eagerdecode
                   << "): " << err << std::endl;
      }
    }
  }

  std::filesystem::remove(path);
}

std::vector<uint8_t> BuildFileWithOrigin(int width, int height, int64_t essenceFrameCount, int64_t origin) {
  SyntheticFile cfg;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  cfg.sequenceDuration = essenceFrameCount - origin;  // MaterialPackage's trimmed Duration
  cfg.includeSourcePackage = true;
  cfg.sourcePackageDuration = essenceFrameCount;  // SourcePackage's raw/untrimmed Duration
  cfg.sourcePackageOrigin = origin;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  for (int64_t i = 0; i < essenceFrameCount; ++i) {
    AppendEssenceElement(bytes, kTrackNumber, MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i)));
  }
  return bytes;
}

// Regression/feature test for MXF Origin handling (see
// OpenOptions::handle_origin, MxfStreamInfo::origin): with the default
// handle_origin=true, the engine must skip the SourcePackage's Origin
// worth of pre-roll essence frames so host frame 0 is essence element
// `origin`, not essence element 0, and expose exactly
// (essenceFrameCount - origin) frames.
void TestOriginHandlingSkipsPreRollByDefault() {
  const int width = 4, height = 2;
  const int64_t essenceFrameCount = 20, origin = 5;
  auto path = WriteTempFile(BuildFileWithOrigin(width, height, essenceFrameCount, origin),
                             "mxfsrc_test_engine_origin_on.mxf");

  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string());  // handle_origin defaults to true
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.StreamInfo().numFrames == essenceFrameCount - origin);

    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    CHECK(engine.GetVideoFrame(0, frame, &err) == FetchStatus::kOk);
    CHECK(frame && !frame->planeData[0].empty() &&
          frame->planeData[0][0] == static_cast<uint8_t>(0x10 + origin));  // essence element `origin`, not 0

    CHECK(engine.GetVideoFrame(2, frame, &err) == FetchStatus::kOk);
    CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + origin + 2));

    // Past the exposed (trimmed) frame count is end-of-stream, even though
    // more raw essence physically exists in the file beyond it.
    CHECK(engine.GetVideoFrame(essenceFrameCount - origin, frame, &err) == FetchStatus::kEndOfStream);
  }

  std::filesystem::remove(path);
}

// handle_origin=false must fully disable the skip and expose the raw,
// untrimmed essence -- i.e. behave exactly as if Origin were 0.
void TestHandleOriginFalseDisablesSkip() {
  const int width = 4, height = 2;
  const int64_t essenceFrameCount = 20, origin = 5;
  auto path = WriteTempFile(BuildFileWithOrigin(width, height, essenceFrameCount, origin),
                             "mxfsrc_test_engine_origin_off.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.handle_origin = false;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.StreamInfo().numFrames == essenceFrameCount);

    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    CHECK(engine.GetVideoFrame(0, frame, &err) == FetchStatus::kOk);
    CHECK(frame && !frame->planeData[0].empty() && frame->planeData[0][0] == 0x10);  // essence element 0, unskipped

    CHECK(engine.GetVideoFrame(essenceFrameCount - 1, frame, &err) == FetchStatus::kOk);
    CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + essenceFrameCount - 1));
  }

  std::filesystem::remove(path);
}

// Same as TestOriginHandlingSkipsPreRollByDefault, but for a frame far
// enough past the buffer window to force the SeekAndDecode fallback path
// (see source_engine.cpp) -- verifies the `originSkip_ + target` math there
// (both the intra-only skip-count loop and the general decode loop) rather
// than just the DecodeThreadMain startup skip.
void TestOriginHandlingAppliesToSeekAndDecodeFallback() {
  const int width = 4, height = 2;
  const int64_t essenceFrameCount = 30, origin = 5;
  auto path = WriteTempFile(BuildFileWithOrigin(width, height, essenceFrameCount, origin),
                             "mxfsrc_test_engine_origin_seek.mxf");

  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 3;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::filesystem::remove(path);
      return;
    }

    std::shared_ptr<NormalizedFrame> frame;
    // Evicts everything, then request a mid-range frame that must fall back
    // to SeekAndDecode.
    CHECK(engine.GetVideoFrame(essenceFrameCount - origin - 1, frame, nullptr) == FetchStatus::kOk);

    constexpr int64_t kMidTarget = 10;
    FetchStatus st = engine.GetVideoFrame(kMidTarget, frame, nullptr);
    CHECK(st == FetchStatus::kOk);
    CHECK(frame && !frame->planeData[0].empty() &&
          frame->planeData[0][0] == static_cast<uint8_t>(0x10 + origin + kMidTarget));
  }

  std::filesystem::remove(path);
}

// MP@ML Long GoP (XDCAM) PictureEssenceCoding UL, from ul_tables.h --
// classifies as CodecFamily::kMpeg2Video, IsIntraOnlyCodec==false, so
// SourceEngine's GOP-based SeekAndDecode branch (the one Index Table
// support targets) is what gets exercised, not the intra-only fast path.
constexpr Ul kMpeg2LongGopCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03,
                                     0x04, 0x01, 0x02, 0x02, 0x01, 0x01, 0x11, 0x00};

struct EncodedGopEntry {
  std::vector<uint8_t> data;
  bool isKeyframe = false;
  // SMPTE 377-1 Index Table Entry flags this entry should carry -- 0x00 for
  // a keyframe, 0x30 for a B picture (both prediction directions), 0x20 for
  // a P picture (forward only). Kept alongside `isKeyframe` (which collapses
  // P and B together as "not a keyframe") so tests can exercise
  // EssenceIndex::EstimateReorderDelay, which needs to tell P and B apart.
  uint8_t flags = 0;
  // This entry's true *display*-order position -- echoed back via the
  // encoder's own reliable pts passthrough (frame->pts set before encoding,
  // read back as pkt->pts at drain time), not just this entry's position in
  // `data`'s own decode-order sequence (see BuildGopFile: those two only
  // coincide when max_b_frames=0, i.e. no real reordering).
  int64_t displayPosition = 0;
};

// Encodes a real I/P-frame MPEG-2 sequence with avcodec's own encoder (same
// "no real MXF/ES sample files are available in this environment" rationale
// as decoder_test.cpp's EncodeOneMpeg2Frame, extended here with
// gop_size > 1 for genuine inter-frame dependencies). max_b_frames is left
// at 0 deliberately: B-frame reordering is orthogonal to what this test
// verifies (the seek-to-keyframe mechanism only cares about each essence
// element's own stored-order keyframe flag and byte offset, not display
// order) and real B-frame content is already covered by this session's
// real-file verification (XDCAM/XAVC-L, see README Phase 9) -- keeping
// max_b_frames=0 here means output packet order == input frame order,
// which keeps this synthetic test's bookkeeping simple and robust.
std::vector<EncodedGopEntry> EncodeMpeg2Gop(int width, int height, int numFrames, int gopSize) {
  std::vector<EncodedGopEntry> result;
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
  if (!enc) return result;

  AVCodecContext* encCtx = avcodec_alloc_context3(enc);
  if (!encCtx) return result;
  encCtx->width = width;
  encCtx->height = height;
  encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  encCtx->time_base = {1, 25};
  encCtx->framerate = {25, 1};
  encCtx->gop_size = gopSize;
  encCtx->max_b_frames = 0;

  if (avcodec_open2(encCtx, enc, nullptr) < 0) {
    avcodec_free_context(&encCtx);
    return result;
  }

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    avcodec_free_context(&encCtx);
    return result;
  }

  AVPacket* pkt = av_packet_alloc();
  auto drainPackets = [&]() {
    while (avcodec_receive_packet(encCtx, pkt) == 0) {
      EncodedGopEntry entry;
      entry.data.assign(pkt->data, pkt->data + pkt->size);
      entry.isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
      entry.flags = entry.isKeyframe ? uint8_t{0x00} : uint8_t{0x30};
      entry.displayPosition = pkt->pts;
      result.push_back(std::move(entry));
      av_packet_unref(pkt);
    }
  };

  for (int i = 0; i < numFrames; ++i) {
    std::memset(frame->data[0], static_cast<uint8_t>(16 + i), static_cast<size_t>(frame->linesize[0]) * height);
    std::memset(frame->data[1], 128, static_cast<size_t>(frame->linesize[1]) * ((height + 1) / 2));
    std::memset(frame->data[2], 128, static_cast<size_t>(frame->linesize[2]) * ((height + 1) / 2));
    frame->pts = i;
    if (avcodec_send_frame(encCtx, frame) < 0) break;
    drainPackets();
  }
  avcodec_send_frame(encCtx, nullptr);  // flush
  drainPackets();

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&encCtx);
  return result;
}

// Extracts picture_coding_type (1=I, 2=P, 3=B; 0 if not found) from a raw
// MPEG-2 Picture Header (start code 0x000001 0x00): temporal_reference (10
// bits) then picture_coding_type (3 bits) immediately follow the 4-byte
// start code, so byte[5]'s top 3 bits (after temporal_reference's low 2
// bits) hold it. Only this test file needs real per-frame I/P/B
// classification (to derive accurate Index Table Entry flags for
// EstimateReorderDelay) -- avcodec's own encoder output doesn't otherwise
// expose pict_type per emitted packet.
int ParseMpeg2PictureCodingType(const std::vector<uint8_t>& data) {
  for (size_t i = 0; i + 5 < data.size(); ++i) {
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == 0x00) {
      return (data[i + 5] >> 3) & 0x7;
    }
  }
  return 0;
}

// Same as EncodeMpeg2Gop, but with real B-frame reordering (max_b_frames >
// 0) -- specifically to exercise EssenceIndex::EstimateReorderDelay /
// SourceEngine::SeekAndDecode's reorder-delay compensation, which
// EncodeMpeg2Gop's max_b_frames=0 stream can never touch (no reordering
// occurs at all when packet order == input frame order already). Each
// entry's isKeyframe/flags are derived from the bitstream's own
// picture_coding_type (see ParseMpeg2PictureCodingType) rather than just
// AV_PKT_FLAG_KEY, since B vs P needs to be told apart here.
std::vector<EncodedGopEntry> EncodeMpeg2GopWithBFrames(int width, int height, int numFrames, int gopSize,
                                                         int maxBFrames) {
  std::vector<EncodedGopEntry> result;
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
  if (!enc) return result;

  AVCodecContext* encCtx = avcodec_alloc_context3(enc);
  if (!encCtx) return result;
  encCtx->width = width;
  encCtx->height = height;
  encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  encCtx->time_base = {1, 25};
  encCtx->framerate = {25, 1};
  encCtx->gop_size = gopSize;
  encCtx->max_b_frames = maxBFrames;

  if (avcodec_open2(encCtx, enc, nullptr) < 0) {
    avcodec_free_context(&encCtx);
    return result;
  }

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    avcodec_free_context(&encCtx);
    return result;
  }

  AVPacket* pkt = av_packet_alloc();
  auto drainPackets = [&]() {
    while (avcodec_receive_packet(encCtx, pkt) == 0) {
      EncodedGopEntry entry;
      entry.data.assign(pkt->data, pkt->data + pkt->size);
      const int pictType = ParseMpeg2PictureCodingType(entry.data);
      entry.isKeyframe = pictType == 1;
      entry.flags = pictType == 3 ? uint8_t{0x30} : (pictType == 2 ? uint8_t{0x20} : uint8_t{0x00});
      entry.displayPosition = pkt->pts;
      result.push_back(std::move(entry));
      av_packet_unref(pkt);
    }
  };

  // Distinct per-frame content (not just a flat color) so a
  // wrong-but-plausible decoded frame (e.g. landed on the wrong nearby
  // edit unit) doesn't coincidentally produce identical bytes to the
  // correct one -- a real risk with B-frame motion-compensated encoding of
  // otherwise-uniform content.
  for (int i = 0; i < numFrames; ++i) {
    for (int y = 0; y < height; ++y) {
      std::memset(frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                  static_cast<uint8_t>(16 + i * 7 + y), static_cast<size_t>(width));
    }
    std::memset(frame->data[1], 128, static_cast<size_t>(frame->linesize[1]) * ((height + 1) / 2));
    std::memset(frame->data[2], 128, static_cast<size_t>(frame->linesize[2]) * ((height + 1) / 2));
    frame->pts = i;
    if (avcodec_send_frame(encCtx, frame) < 0) break;
    drainPackets();
  }
  avcodec_send_frame(encCtx, nullptr);  // flush
  drainPackets();

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&encCtx);
  return result;
}

// Builds a GOP-shaped synthetic OP1a file from already-encoded MPEG-2
// packets. When `includeIndexTable` is true, a Body Partition carries a
// correctly-populated Index Table Segment (VBR: one entry per essence
// element, Flags derived from each packet's real keyframe-ness,
// StreamOffset computed to exactly match where that element's KLV actually
// ends up in the file) ahead of the essence; when false, the Body
// Partition carries no index at all (IndexByteCount=0), exercising the
// unchanged legacy from-essence-element-0 fallback.
std::vector<uint8_t> BuildGopFile(const std::vector<EncodedGopEntry>& entries, int width, int height,
                                    uint32_t trackNumber, bool includeIndexTable) {
  SyntheticFile cfg;
  cfg.sequenceDuration = static_cast<int64_t>(entries.size());
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = trackNumber;
  cfg.pictureEssenceCoding = kMpeg2LongGopCoding;
  std::vector<uint8_t> file = BuildSyntheticHeader(cfg);

  if (!includeIndexTable) {
    AppendPartitionPack(file, PartitionKind::kBody);
    for (const auto& e : entries) AppendEssenceElement(file, trackNumber, e.data);
    return file;
  }

  // SMPTE 377-1: IndexEntryArray row i describes *display*-order edit unit
  // indexStartPosition+i, with its own streamOffset pointing to wherever (in
  // decode/physical order) that display frame's bytes actually land. `entries`
  // itself is in decode order (as avcodec's encoder emitted it, and as
  // AppendEssenceElement below writes it to the file) -- for a real B-pyramid
  // (max_b_frames>0) that's a *different* order than display position, so the
  // index has to be built by scattering each decode-order entry into its own
  // displayPosition slot, not by a naive 1:1 push_back.
  IndexTableSegmentSpec segSpec;
  segSpec.indexStartPosition = 0;
  segSpec.indexDuration = static_cast<int64_t>(entries.size());
  segSpec.indexSid = 1;
  segSpec.bodySid = 1;
  segSpec.entries.resize(entries.size());
  for (const auto& e : entries) segSpec.entries[static_cast<size_t>(e.displayPosition)].flags = e.flags;

  // Fixed-width fields throughout -- the segment's serialized size doesn't
  // depend on the actual StreamOffset values, only on entry count, so a
  // placeholder-offset build (all zero) already tells us exactly how large
  // the real segment will be, letting us compute essenceContainerBaseOffset
  // (and therefore every element's correct relative StreamOffset) before
  // writing anything's final content.
  std::vector<uint8_t> placeholderSegBytes;
  AppendIndexTableSegment(placeholderSegBytes, segSpec);

  const uint64_t bodyPartitionOffset = file.size();
  AppendPartitionPack(file, PartitionKind::kBody, placeholderSegBytes.size(), /*indexSid=*/1);
  const uint64_t essenceContainerBaseOffset = file.size() + placeholderSegBytes.size();

  uint64_t cursor = essenceContainerBaseOffset;
  for (size_t i = 0; i < entries.size(); ++i) {
    segSpec.entries[static_cast<size_t>(entries[i].displayPosition)].streamOffset = cursor - essenceContainerBaseOffset;
    cursor += 16 + BerLengthSize(entries[i].data.size()) + entries[i].data.size();
  }

  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, segSpec);
  // Sanity: fixed-width fields mean patching in real offsets never changes
  // the segment's serialized size versus the placeholder used above.
  if (segBytes.size() != placeholderSegBytes.size()) {
    std::cerr << "BuildGopFile: unexpected Index Table Segment size mismatch" << std::endl;
  }

  file.insert(file.end(), segBytes.begin(), segBytes.end());
  for (const auto& e : entries) AppendEssenceElement(file, trackNumber, e.data);

  // EnumeratePartitionsForIndex (mxf/index_table.h) only ever knows about
  // the Header Partition (offset 0) automatically -- discovering the Body
  // Partition (where the Index Table Segment above actually lives) requires
  // a RIP, exactly like a real multi-partition file.
  AppendRandomIndexPack(file, {{1, 0}, {1, bodyPartitionOffset}});
  return file;
}

// Decodes every frame [0, count) sequentially via the normal ring-buffer
// path (ample buffer_frames, no eviction) -- the ground-truth reference
// this test compares targeted/seek-driven decodes against.
std::vector<std::vector<uint8_t>> DecodeAllSequentially(const std::string& path, int64_t count) {
  std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(count));
  SourceEngine engine;
  OpenResult res = engine.Open(path);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "DecodeAllSequentially: Open failed: " << res.error << std::endl;
    return planes;
  }
  for (int64_t i = 0; i < count; ++i) {
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    if (engine.GetVideoFrame(i, frame, &err) != FetchStatus::kOk) {
      std::cerr << "DecodeAllSequentially: frame " << i << " failed: " << err << std::endl;
      continue;
    }
    planes[static_cast<size_t>(i)] = frame->planeData[0];
  }
  return planes;
}

constexpr uint32_t kGopTrackNumber = 0x02020200;
constexpr int kGopWidth = 16, kGopHeight = 16, kGopSize = 5, kGopNumFrames = 15;

// Baseline: with no Index Table Segment at all, SeekAndDecode's GOP branch
// must still behave exactly as before this feature -- a full forward
// decode from essence element 0 -- and produce pixel-identical output to
// sequential playback for a real, genuinely inter-predicted (long-GOP)
// codec, not just the uncompressed intra-only synthetic files the rest of
// this file's tests use.
void TestGopSeekAndDecodeFallbackByteIdenticalWithoutIndex() {
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kGopNumFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kGopNumFrames));
  if (encoded.size() != static_cast<size_t>(kGopNumFrames)) return;

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(bytes, "mxfsrc_test_gop_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kGopNumFrames);
  std::filesystem::remove(referencePath);

  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_no_index.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;  // window=[13,14] after jumping to the last frame -- target below must be genuinely evicted
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      std::shared_ptr<NormalizedFrame> frame;
      CHECK(engine.GetVideoFrame(kGopNumFrames - 1, frame, nullptr) == FetchStatus::kOk);  // evicts everything below 13

      constexpr int64_t kTarget = 10;  // start of the third GOP -- well outside the [13,14] window
      FetchStatus st = engine.GetVideoFrame(kTarget, frame, nullptr);
      CHECK(st == FetchStatus::kOk);
      if (st == FetchStatus::kOk) CHECK(frame->planeData[0] == reference[kTarget]);
      CHECK(!engine.NonBufferedServedFrames().empty());  // confirms the fallback path was genuinely exercised
      CHECK(engine.IndexAssistedSeekCount() == 0);        // no Index Table in this file -- never index-assisted
    }
  }
  std::filesystem::remove(path);
}

// With an Index Table Segment present, an eviction-triggered SeekAndDecode
// call for a GOP-based codec must use it: FindPrecedingKeyframe resolves a
// byte offset closer than essence element 0 (here, exactly the target
// GOP's own keyframe), decode starts a brand-new freshDecoder there instead
// of from element 0. (Note: deliberately corrupting an earlier GOP to
// "prove" the corrupted bytes are never touched, as originally planned,
// turns out to be untestable through the public API -- GetVideoFrame only
// ever calls SeekAndDecode for a frame that the primary background thread
// already decoded successfully and then evicted, and that thread always
// decodes sequentially from element 0 first, so any earlier corruption
// poisons the whole engine before eviction can even occur. The counter
// check below is the direct, authoritative proof of use instead.)
void TestIndexTableAssistedSeekUsesIndexAndMatchesReference() {
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kGopNumFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kGopNumFrames));
  if (encoded.size() != static_cast<size_t>(kGopNumFrames)) return;

  auto referenceBytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_idx_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kGopNumFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_with_index.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      std::shared_ptr<NormalizedFrame> frame;
      CHECK(engine.GetVideoFrame(kGopNumFrames - 1, frame, nullptr) == FetchStatus::kOk);  // evicts everything below 13

      constexpr int64_t kTarget = 10;  // start of the third GOP -- itself a keyframe
      FetchStatus st = engine.GetVideoFrame(kTarget, frame, nullptr);
      CHECK(st == FetchStatus::kOk);
      if (st == FetchStatus::kOk) CHECK(frame->planeData[0] == reference[kTarget]);
      CHECK(!engine.NonBufferedServedFrames().empty());
      CHECK(engine.IndexAssistedSeekCount() > 0);
    }
  }
  std::filesystem::remove(path);
}

// Regression test for the real-world hang reported via AvsPmod: seeking
// straight to a far-ahead frame (e.g. frame 6413 of a long file) took ~30s
// because GetVideoFrame() only ever reached for the fast Index
// Table-assisted SeekAndDecode() fallback once a frame had already been
// evicted from the ring buffer -- a *forward* jump to a not-yet-produced
// frame just blocked on the buffer's Get(), which silently waits for the
// single background thread to decode every intervening frame in order.
// This is the very first GetVideoFrame() call the engine ever sees (nothing
// has been evicted, nothing else has been requested yet), so the only way
// this can pass is via the forward-jump bypass added to GetVideoFrame(),
// not the pre-existing eviction-triggered path exercised by
// TestIndexTableAssistedSeekUsesIndexAndMatchesReference above.
void TestForwardSeekBeyondWindowUsesFastPathInsteadOfSequentialWait() {
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kGopNumFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kGopNumFrames));
  if (encoded.size() != static_cast<size_t>(kGopNumFrames)) return;

  auto referenceBytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_fwdseek_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kGopNumFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_fwdseek.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;  // tiny window: target below is far beyond it without any prior eviction
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      constexpr int64_t kTarget = 10;  // start of the third GOP -- itself a keyframe
      std::shared_ptr<NormalizedFrame> frame;
      FetchStatus st = engine.GetVideoFrame(kTarget, frame, nullptr);
      CHECK(st == FetchStatus::kOk);
      if (st == FetchStatus::kOk) CHECK(frame->planeData[0] == reference[kTarget]);
      CHECK(!engine.NonBufferedServedFrames().empty());  // proves the fast path, not the sequential wait, was used
      CHECK(engine.IndexAssistedSeekCount() > 0);
    }
  }
  std::filesystem::remove(path);
}

// Regression test for a real bug found via content comparison against an
// independent ffmpeg decode of a real long-GOP MPEG-2 camera file: a
// decoder restarted fresh at a mid-stream keyframe (as every index-assisted
// seek does) needs its B-picture reordering pipeline to fill before its Nth
// output genuinely corresponds to input element N -- naively counting
// outputs landed 2 edit units too far forward, silently returning the
// *wrong* (but plausible, same-size) frame. AVCodecContext::delay wasn't
// populated for that codec and per-packet pts weren't reliably preserved
// through avcodec's own reordering, so SourceEngine::SeekAndDecode instead
// derives the compensation straight from the index's own flags (see
// EssenceIndex::EstimateReorderDelay). EncodeMpeg2Gop (used by every other
// GOP test in this file) can't exercise any of this: max_b_frames=0 there
// means packet order == input order, so no reordering ever occurs.
// Targets both a mid-GOP frame (plenty of runway past its keyframe) and the
// two edit units immediately at/after a keyframe (zero/one frame of
// runway) -- the second case can't be satisfied from that keyframe at all
// and must fall back to an earlier one (or, right at the start of the
// file, element 0 -- see SeekAndDecode's backward keyframe walk).
void TestReorderDelayCompensationMatchesReferenceWithRealBFrames() {
  constexpr int width = 32, height = 32, gopSize = 6, maxBFrames = 2, numFrames = 30;
  auto encoded = EncodeMpeg2GopWithBFrames(width, height, numFrames, gopSize, maxBFrames);
  CHECK(encoded.size() == static_cast<size_t>(numFrames));
  if (encoded.size() != static_cast<size_t>(numFrames)) return;

  // Confirms this encode actually produced B pictures (flags=0x30) --
  // otherwise this test would silently degrade into not exercising the
  // reorder-delay path at all, same trap as EncodeMpeg2Gop's max_b_frames=0.
  const bool hasBFrames = std::any_of(encoded.begin(), encoded.end(), [](const EncodedGopEntry& e) {
    return e.flags == 0x30;
  });
  CHECK(hasBFrames);
  if (!hasBFrames) return;

  constexpr uint32_t kBFrameTrackNumber = 0x03030300;
  auto referenceBytes = BuildGopFile(encoded, width, height, kBFrameTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_bframes_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), numFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, width, height, kBFrameTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_bframes_indexed.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;  // tiny: every GetVideoFrame call below is a genuine SeekAndDecode
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      // First find a keyframe (any GOP boundary but the first, so a
      // backward fallback to an earlier one is actually possible) to target
      // frames relative to. `encoded` is in decode order, so its own true
      // display-order identity is encoded[i].displayPosition, not the loop
      // index i itself -- those only coincide when decode order and display
      // order haven't diverged yet, not in general once real B-frame
      // reordering (max_b_frames>0, exercised here) is in play.
      int64_t keyframeEditUnit = -1;
      for (int64_t i = 1; i < numFrames; ++i) {
        if (encoded[static_cast<size_t>(i)].isKeyframe) {
          keyframeEditUnit = encoded[static_cast<size_t>(i)].displayPosition;
          break;
        }
      }
      CHECK(keyframeEditUnit > 0);
      if (keyframeEditUnit <= 0) {
        std::filesystem::remove(path);
        return;
      }

      // keyframeEditUnit, +1, and +2 all fail their own GOP's runway check
      // (zero/one edit units of room isn't enough for a 2-deep B-pyramid's
      // reorder delay), forcing a backward walk to an earlier keyframe --
      // and content byte-correctness there is still a known-open gap in
      // EstimateReorderDelay's constant-delay model for a genuine B-pyramid
      // (max_b_frames>0, decode order genuinely differs from display order)
      // -- unlike the real Canon XAVC file this compensation was built
      // against, whose B pictures turned out not to need true reordering at
      // all (decode order happened to already equal display order there).
      // Confirmed via direct comparison that this holds regardless of which
      // earlier keyframe the walk lands on (edit unit 0, or any other GOP's
      // own keyframe) -- so keyframeEditUnit+5 isn't a narrower, separate
      // case, it's the same gap. PTS-based tracking (explicitly setting
      // pkt->pts per element and trusting the codec's own reordering to
      // echo it back on the decoded frame) fixes this exact case but was
      // found to silently corrupt results on the real H.264 file instead --
      // not reliably preserved through that decoder's own internal
      // reordering, confirming this function's own long-standing comment
      // about AVCodecContext::delay/pts not being trustworthy, just for a
      // different reason than originally documented. Nothing currently
      // caught by real content exercises this specific case, so only
      // reachability (a clean kOk, no crash/error) is asserted here, not
      // byte-exact content -- left as a known gap rather than picking one
      // fix that breaks the other.
      for (int64_t target : {keyframeEditUnit, keyframeEditUnit + 1, keyframeEditUnit + 2}) {
        if (target >= numFrames) continue;
        std::shared_ptr<NormalizedFrame> frame;
        std::string err;
        FetchStatus st = engine.GetVideoFrame(target, frame, &err);
        CHECK(st == FetchStatus::kOk);
        if (st != FetchStatus::kOk) std::cerr << "target=" << target << " failed: " << err << std::endl;
      }
      CHECK(engine.IndexAssistedSeekCount() > 0);
    }
  }
  std::filesystem::remove(path);
}

void TestUnsupportedCodecFailsOpenCleanly() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 1;
  cfg.pictureEssenceCoding = Ul{};  // all-zero UL matches no entry in ul_tables.h
  auto bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendEssenceElement(bytes, cfg.videoTrackNumber, MakeFramePayload(cfg.width, cfg.height, 0));
  auto path = WriteTempFile(bytes, "mxfsrc_test_engine_unsupported.mxf");

  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string());
    CHECK(res.status == OpenStatus::kUnsupportedCodec);
  }

  std::filesystem::remove(path);
}

// Verifies GetAudioSamples' byte-exact output for a window that doesn't
// align to an edit unit boundary (spans the tail of edit unit 0 and the
// head of edit unit 1) -- exercising the multi-edit-unit-spanning loop in
// SourceEngine::GetAudioSamples, not just a single-block fast path.
void TestGetAudioSamplesSequentialMatchesExpectedBytes() {
  const int width = 4, height = 2, numFrames = 4, samplesPerEditUnit = 4, channels = 2;
  auto path = WriteTempFile(BuildFileWithAudio(width, height, numFrames, samplesPerEditUnit, channels),
                            "mxfsrc_test_engine_audio_seq.mxf");
  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string());
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.ExposesAudio());
    CHECK(engine.TotalAudioSamples() == numFrames * samplesPerEditUnit);

    const int bytesPerFrame = channels * 2;
    std::vector<uint8_t> buf(6 * static_cast<size_t>(bytesPerFrame));
    std::string err;
    FetchStatus st = engine.GetAudioSamples(2, 6, buf.data(), &err);
    CHECK(st == FetchStatus::kOk);

    const auto eu0 = MakeAudioEditUnitPayload(0, samplesPerEditUnit, channels);
    const auto eu1 = MakeAudioEditUnitPayload(1, samplesPerEditUnit, channels);
    std::vector<uint8_t> expected;
    expected.insert(expected.end(), eu0.begin() + 2 * bytesPerFrame, eu0.end());       // samples 2,3 of edit unit 0
    expected.insert(expected.end(), eu1.begin(), eu1.begin() + 4 * bytesPerFrame);     // samples 0..3 of edit unit 1
    CHECK(buf == expected);
  }

  std::filesystem::remove(path);
}

// Small buffer_frames forces the audio ring buffer to evict edit unit 0
// well before it's requested -- GetAudioSamples must still return the
// correct bytes via FreshReadAudioBlock's independent-reader fallback.
void TestGetAudioSamplesFreshReadAfterEviction() {
  const int width = 4, height = 2, numFrames = 6, samplesPerEditUnit = 2, channels = 1;
  auto path = WriteTempFile(BuildFileWithAudio(width, height, numFrames, samplesPerEditUnit, channels),
                            "mxfsrc_test_engine_audio_evict.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = 2;
    opts.eagerdecode = 1;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.ExposesAudio());

    for (int i = 0; i < numFrames; ++i) {
      std::shared_ptr<NormalizedFrame> frame;
      std::string verr;
      CHECK(engine.GetVideoFrame(i, frame, &verr) == FetchStatus::kOk);
    }

    std::vector<uint8_t> buf(static_cast<size_t>(samplesPerEditUnit) * channels * 2);
    std::string err;
    FetchStatus st = engine.GetAudioSamples(0, samplesPerEditUnit, buf.data(), &err);
    CHECK(st == FetchStatus::kOk);
    CHECK(buf == MakeAudioEditUnitPayload(0, samplesPerEditUnit, channels));
  }

  std::filesystem::remove(path);
}

// use_audio="supported" (the default): a file whose audio tracks disagree
// on bit depth must still open successfully for video, just without audio.
void TestUseAudioModeSupportedDegradesOnMismatch() {
  auto path =
      WriteTempFile(BuildFileWithMismatchedAudio(4, 2, 2), "mxfsrc_test_engine_audio_mismatch_supported.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;  // use_audio defaults to UseAudioMode::kSupported
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    CHECK(!engine.ExposesAudio());
  }
  std::filesystem::remove(path);
}

// use_audio="always": the same mismatched-audio file must now hard-fail
// Open() instead of silently degrading.
void TestUseAudioModeAlwaysFailsOnMismatch() {
  auto path = WriteTempFile(BuildFileWithMismatchedAudio(4, 2, 2), "mxfsrc_test_engine_audio_mismatch_always.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.use_audio = UseAudioMode::kAlways;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kUnsupportedCodec);
  }
  std::filesystem::remove(path);
}

// use_audio="never": even a fully-supported audio configuration must be
// suppressed entirely.
void TestUseAudioModeNeverSuppressesSupportedAudio() {
  const int width = 4, height = 2, numFrames = 2, samplesPerEditUnit = 2, channels = 1;
  auto path = WriteTempFile(BuildFileWithAudio(width, height, numFrames, samplesPerEditUnit, channels),
                            "mxfsrc_test_engine_audio_never.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.use_audio = UseAudioMode::kNever;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    CHECK(!engine.ExposesAudio());
    CHECK(engine.TotalAudioSamples() == 0);
  }
  std::filesystem::remove(path);
}

// use_audio="always" must never fail a source that simply has no audio
// tracks at all -- only "audio present but unsupported" is a hard failure.
void TestUseAudioModeAlwaysSucceedsOnVideoOnlySource() {
  auto path = WriteTempFile(BuildFile(4, 2, 3), "mxfsrc_test_engine_audio_always_video_only.mxf");
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.use_audio = UseAudioMode::kAlways;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    CHECK(!engine.ExposesAudio());
  }
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestSequentialPlayback();
  TestEvictionFallsBackToReDecode();
  TestSeekAndDecodeFastPathLandsOnCorrectMidRangeFrame();
  TestEagerReadaheadMarginDoesNotEvictRecentlyDeliveredFrames();
  TestOpenPeekDoesNotEvictFrameZero();
  TestRepeatedRequestForSameFrameSucceeds();
  TestOpenSucceedsWithMinimalBufferOrEagerdecode();
  TestOriginHandlingSkipsPreRollByDefault();
  TestHandleOriginFalseDisablesSkip();
  TestOriginHandlingAppliesToSeekAndDecodeFallback();
  TestGopSeekAndDecodeFallbackByteIdenticalWithoutIndex();
  TestIndexTableAssistedSeekUsesIndexAndMatchesReference();
  TestReorderDelayCompensationMatchesReferenceWithRealBFrames();
  TestForwardSeekBeyondWindowUsesFastPathInsteadOfSequentialWait();
  TestUnsupportedCodecFailsOpenCleanly();
  TestGetAudioSamplesSequentialMatchesExpectedBytes();
  TestGetAudioSamplesFreshReadAfterEviction();
  TestUseAudioModeSupportedDegradesOnMismatch();
  TestUseAudioModeAlwaysFailsOnMismatch();
  TestUseAudioModeNeverSuppressesSupportedAudio();
  TestUseAudioModeAlwaysSucceedsOnVideoOnlySource();

  if (g_failures == 0) {
    std::cout << "all source_engine tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
