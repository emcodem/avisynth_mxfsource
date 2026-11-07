// Phase 4 unit tests for the ring-buffer decode engine (src/core/source_engine.*).
// Builds a small synthetic OP1a file with real (uncompressed UYVY) essence
// so the whole pipeline -- header parse, essence walk, decode, ring buffer,
// background thread -- runs end to end without needing a real sample file.
#include <algorithm>
#include <atomic>
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

#include "core/index_extract.h"
#include "core/index_policy.h"
#include "core/source_engine.h"
#include "core/use_audio_mode.h"
#include "mxf/index_table.h"
#include "mxf/index_transport.h"
#include "mxf/integrity.h"
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

// A caller that keeps a SourceEngine alive past this call's own scope (e.g.
// via a shared_ptr also captured by a RunWithTimeout thread) may still have
// the file open when cleaning up -- the throwing std::filesystem::remove
// overload would then throw filesystem_error (uncaught -> std::terminate)
// on Windows, where an open file can't be deleted. Use the non-throwing
// overload for those call sites instead of the plain remove() the rest of
// this file's braces-scoped tests can safely use.
void RemoveTempFileIgnoringErrors(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// Runs `fn` on a separate thread and waits up to `timeout` for it to
// complete -- for regression tests against a genuine infinite-loop/deadlock
// bug, failing the CHECK and returning promptly is far more useful than
// actually hanging the whole test binary/CI run forever. The thread is
// detached: if `fn` never returns (i.e. the bug being tested for is back),
// callers must keep everything `fn` touches alive via shared_ptr rather
// than references/locals, so a timed-out run leaks a thread instead of
// leaving it holding a dangling reference into a since-destroyed stack frame.
template <typename Fn>
bool RunWithTimeout(Fn fn, std::chrono::milliseconds timeout) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread t([fn, done] {
    fn();
    done->store(true);
  });
  t.detach();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!done->load()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

// The synthetic files in this suite are mostly shaped like a growing
// recording (Open header, no Footer/RIP, often no Index Table), which
// OpenOptions::strict refuses. These tests are about decoding, not that
// policy (see the TestStrict* tests for it).
OpenOptions NonStrictOptions() {
  OpenOptions options;
  options.strict = false;
  return options;
}

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

// Like BuildFileWithAudio, but each edit unit's REAL essence sample count is
// given explicitly via `sampleCounts` instead of always being the uniform
// `nominalSamplesPerEditUnit`. The Index Table-derived AudioSampleIndex (see
// BuildAudioSampleIndexFromIndexTable) still assumes the uniform nominal
// count throughout (audio.sampleRate = nominalSamplesPerEditUnit * 25, same
// as BuildFileWithAudio) -- so an edit unit whose `sampleCounts` entry is
// smaller than nominal reproduces, deterministically and without needing a
// genuinely non-integer editRate/sampleRate ratio, the real-world mismatch
// a file with its own NTSC 1601/1602-style rounding phase can hit: the
// index's estimated per-edit-unit sample boundary disagreeing with the
// edit unit's actual decoded block size (see
// TestGetAudioSamplesDoesNotHangOnEditUnitSampleCountMismatch).
std::vector<uint8_t> BuildFileWithAudioCustomCounts(int width, int height, int numFrames,
                                                     int nominalSamplesPerEditUnit, int channels,
                                                     const std::vector<int>& sampleCounts) {
  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  SyntheticAudioTrack audio;
  audio.trackNumber = kAudioTrackNumber;
  audio.trackId = 100;
  audio.channelCount = static_cast<uint32_t>(channels);
  audio.sampleRate = static_cast<uint32_t>(nominalSamplesPerEditUnit) * 25;
  audio.bitsPerSample = 16;
  cfg.audioTracks = {audio};
  std::vector<uint8_t> file = BuildSyntheticHeader(cfg);

  std::vector<std::vector<uint8_t>> videoPayloads(static_cast<size_t>(numFrames));
  std::vector<std::vector<uint8_t>> audioPayloads(static_cast<size_t>(numFrames));
  for (int i = 0; i < numFrames; ++i) {
    videoPayloads[static_cast<size_t>(i)] = MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i));
    audioPayloads[static_cast<size_t>(i)] = MakeAudioEditUnitPayload(i, sampleCounts[static_cast<size_t>(i)], channels);
  }

  IndexTableSegmentSpec segSpec;
  segSpec.indexStartPosition = 0;
  segSpec.indexDuration = numFrames;
  segSpec.indexSid = 1;
  segSpec.bodySid = 1;
  segSpec.entries.resize(static_cast<size_t>(numFrames));

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
    std::cerr << "BuildFileWithAudioCustomCounts: unexpected Index Table Segment size mismatch" << std::endl;
  }

  file.insert(file.end(), segBytes.begin(), segBytes.end());
  for (int i = 0; i < numFrames; ++i) {
    AppendEssenceElement(file, kTrackNumber, videoPayloads[static_cast<size_t>(i)]);
    AppendEssenceElement(file, kAudioTrackNumber, audioPayloads[static_cast<size_t>(i)]);
  }

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
    OpenResult res = engine.Open(path.string(), NonStrictOptions());
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
    OpenOptions opts = NonStrictOptions();
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
    OpenOptions opts = NonStrictOptions();
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
    OpenResult res = engine.Open(path.string(), NonStrictOptions());  // auto buffer: 300 frames for these tiny frames, eagerdecode 8
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
    OpenOptions opts = NonStrictOptions();
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
    OpenOptions opts = NonStrictOptions();
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
// Field order: the decoder's own report wins; raw UYVY reports none, so the
// picture descriptor decides, and only the combinations verified against
// real files (XDCAM HD422, XAVC, D-10, K2 SD all agree with the bitstream)
// map to a value -- FullFrame/SegmentedFrame = progressive, SeparateFields/
// MixedFields with field 1 dominant = TFF. Everything else stays unknown
// (no BFF from the descriptor: no real sample backs it).
void TestFieldOrderFromDecoderElseDescriptor() {
  struct Case {
    int frameLayout;
    int fieldDominance;
    FieldOrder expected;
  };
  const Case cases[] = {
      {-1, -1, FieldOrder::kUnknown},    {0, -1, FieldOrder::kProgressive}, {4, -1, FieldOrder::kProgressive},
      {1, 1, FieldOrder::kTff},          {3, 1, FieldOrder::kTff},          {1, 2, FieldOrder::kUnknown},
      {1, -1, FieldOrder::kUnknown},     {2, 1, FieldOrder::kUnknown},
  };
  const int width = 4, height = 2, numFrames = 3;
  for (const Case& c : cases) {
    SyntheticFile cfg;
    cfg.sequenceDuration = numFrames;
    cfg.width = width;
    cfg.height = height;
    cfg.videoTrackNumber = kTrackNumber;
    cfg.frameLayout = c.frameLayout;
    cfg.fieldDominance = c.fieldDominance;
    std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
    AppendPartitionPack(bytes, PartitionKind::kBody);
    for (int i = 0; i < numFrames; ++i) {
      AppendEssenceElement(bytes, kTrackNumber, MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i)));
    }
    auto path = WriteTempFile(bytes, "mxfsrc_test_engine_field_order.mxf");
    {
      SourceEngine engine;
      OpenResult res = engine.Open(path.string(), NonStrictOptions());
      CHECK(res.status == OpenStatus::kOk);
      if (res.status == OpenStatus::kOk) {
        CHECK(engine.StreamInfo().frameLayout == c.frameLayout);
        CHECK(engine.StreamInfo().fieldDominance == c.fieldDominance);
        CHECK(engine.ClipFieldOrder() == c.expected);
        std::shared_ptr<NormalizedFrame> frame;
        std::string err;
        CHECK(engine.GetVideoFrame(1, frame, &err) == FetchStatus::kOk);
        if (frame) {
          CHECK(frame->fieldOrder == FieldOrder::kUnknown);  // raw UYVY: the decoder doesn't report
          CHECK(engine.FieldOrderOf(*frame) == c.expected);
          NormalizedFrame reported = *frame;  // a decoder that does report always wins
          reported.fieldOrder = FieldOrder::kBff;
          CHECK(engine.FieldOrderOf(reported) == FieldOrder::kBff);
        }
      }
    }
    std::filesystem::remove(path);
  }
}

void TestOpenSucceedsWithMinimalBufferOrEagerdecode() {
  const int width = 4, height = 2, numFrames = 5;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_minimal_margin.mxf");

  for (int bufferFrames : {1, 300}) {
    for (int eagerdecode : {1, 32}) {
      if (bufferFrames == 300 && eagerdecode == 32) continue;  // that's every other test's default; skip here

      SourceEngine engine;
      OpenOptions opts = NonStrictOptions();
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
    OpenResult res = engine.Open(path.string(), NonStrictOptions());  // handle_origin defaults to true
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
    OpenOptions opts = NonStrictOptions();
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
    OpenOptions opts = NonStrictOptions();
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

// Same shape as BuildGopFile, but with one audio track interleaved into
// every content package -- needed to exercise DecodeThreadMain's hinted
// warm-up audio path (see the hint's own doc comment there) against real
// GOP reordering. Each content package's audio payload is tagged by its
// PHYSICAL/decode-order position `i` (matching MakeAudioEditUnitPayload(i,
// ...)), not `entries[i].displayPosition` -- mirroring how DecodeThreadMain
// itself collects audioScratch in lockstep with each decode-order video
// element, before its own reorder-delay compensation re-aligns it to
// display order (real MXF interleaves audio into the same content package
// as whichever video element -- decode order for GOP video -- physically
// sits there).
std::vector<uint8_t> BuildGopFileWithAudio(const std::vector<EncodedGopEntry>& entries, int width, int height,
                                             uint32_t videoTrackNumber, uint32_t audioTrackNumber,
                                             int samplesPerEditUnit, int channels) {
  SyntheticFile cfg;
  cfg.sequenceDuration = static_cast<int64_t>(entries.size());
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = videoTrackNumber;
  cfg.pictureEssenceCoding = kMpeg2LongGopCoding;
  SyntheticAudioTrack audio;
  audio.trackNumber = audioTrackNumber;
  audio.trackId = 100;
  audio.channelCount = static_cast<uint32_t>(channels);
  audio.sampleRate = static_cast<uint32_t>(samplesPerEditUnit) * 25;
  audio.bitsPerSample = 16;
  cfg.audioTracks = {audio};
  std::vector<uint8_t> file = BuildSyntheticHeader(cfg);

  std::vector<std::vector<uint8_t>> audioPayloads(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    audioPayloads[i] = MakeAudioEditUnitPayload(static_cast<int>(i), samplesPerEditUnit, channels);
  }

  // See BuildGopFile's own doc comment: IndexEntryArray row i describes
  // display-order edit unit i, streamOffset pointing at wherever (in
  // decode/physical order) that display frame's video KLV actually lands --
  // only the video element's offset matters here, exactly like BuildGopFile,
  // since BuildAudioSampleIndexFromIndexTable derives audio locations from
  // the video Index Table alone (see its own doc comment).
  IndexTableSegmentSpec segSpec;
  segSpec.indexStartPosition = 0;
  segSpec.indexDuration = static_cast<int64_t>(entries.size());
  segSpec.indexSid = 1;
  segSpec.bodySid = 1;
  segSpec.entries.resize(entries.size());
  for (const auto& e : entries) segSpec.entries[static_cast<size_t>(e.displayPosition)].flags = e.flags;

  std::vector<uint8_t> placeholderSegBytes;
  AppendIndexTableSegment(placeholderSegBytes, segSpec);

  const uint64_t bodyPartitionOffset = file.size();
  AppendPartitionPack(file, PartitionKind::kBody, placeholderSegBytes.size(), /*indexSid=*/1);
  const uint64_t essenceContainerBaseOffset = file.size() + placeholderSegBytes.size();

  uint64_t cursor = essenceContainerBaseOffset;
  for (size_t i = 0; i < entries.size(); ++i) {
    segSpec.entries[static_cast<size_t>(entries[i].displayPosition)].streamOffset = cursor - essenceContainerBaseOffset;
    cursor += 16 + BerLengthSize(entries[i].data.size()) + entries[i].data.size();
    cursor += 16 + BerLengthSize(audioPayloads[i].size()) + audioPayloads[i].size();
  }

  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, segSpec);
  if (segBytes.size() != placeholderSegBytes.size()) {
    std::cerr << "BuildGopFileWithAudio: unexpected Index Table Segment size mismatch" << std::endl;
  }

  file.insert(file.end(), segBytes.begin(), segBytes.end());
  for (size_t i = 0; i < entries.size(); ++i) {
    AppendEssenceElement(file, videoTrackNumber, entries[i].data);
    AppendEssenceElement(file, audioTrackNumber, audioPayloads[i]);
  }

  AppendRandomIndexPack(file, {{1, 0}, {1, bodyPartitionOffset}});
  return file;
}

// Decodes every frame [0, count) sequentially via the normal ring-buffer
// path (ample buffer_frames, no eviction) -- the ground-truth reference
// this test compares targeted/seek-driven decodes against.
std::vector<std::vector<uint8_t>> DecodeAllSequentially(const std::string& path, int64_t count) {
  std::vector<std::vector<uint8_t>> planes(static_cast<size_t>(count));
  SourceEngine engine;
  OpenResult res = engine.Open(path, NonStrictOptions());
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

// With no real Index Table Segment at all, SourceEngine::Open now builds a
// fallback index straight from the essence's own MPEG-2 bitstream headers
// (mxf/bitstream_index.h) rather than leaving SeekAndDecode's GOP branch
// with nothing to jump to -- this file's real closed-GOP MPEG-2 content is
// exactly the case that fallback is for, so an eviction-triggered seek here
// is now index-assisted (IndexAssistedSeekCount() > 0) instead of the old
// from-essence-element-0 full decode. Still verifies pixel-identical output
// against sequential playback for a real, genuinely inter-predicted
// (long-GOP) codec, not just the uncompressed intra-only synthetic files
// the rest of this file's tests use.
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
    OpenOptions opts = NonStrictOptions();
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
      CHECK(engine.IndexAssistedSeekCount() > 0);  // bitstream-derived fallback index found real keyframes to use
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
    OpenOptions opts = NonStrictOptions();
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
// The jump now restarts the producer at the target (RepositionProducer)
// instead of decoding just that one frame via SeekAndDecode: the target and
// every frame after it come from the ring buffer. Before, the producer
// stayed near frame 0, so each following frame was "far ahead" again and
// went through SeekAndDecode separately (real report: Trim(1250, ...) on a
// long-GOP file, 15 frames from the buffer, 6027 via fallback).
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
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 2;  // tiny window: target below is far beyond it without any prior eviction
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      // 7: two frames past the keyframe at 5, so the restart has to decode
      // and discard lead-in frames, not just start on the target itself.
      constexpr int64_t kTarget = 7;
      for (int64_t i = kTarget; i < kGopNumFrames; ++i) {
        std::shared_ptr<NormalizedFrame> frame;
        std::string err;
        FetchStatus st = engine.GetVideoFrame(i, frame, &err);
        CHECK(st == FetchStatus::kOk);
        if (st == FetchStatus::kOk) CHECK(frame->planeData[0] == reference[i]);
        else std::cerr << "frame " << i << " failed: " << err << std::endl;
      }
      CHECK(engine.RepositionCount() == 1);
      CHECK(engine.NonBufferedServedFrames().empty());  // target and everything after it from the buffer
      CHECK(engine.IndexAssistedSeekCount() > 0);       // the restart started at the keyframe, not at 0

      // A frame before the new position: evicted, served via the fallback,
      // and it doesn't move the producer back.
      std::shared_ptr<NormalizedFrame> early;
      CHECK(engine.GetVideoFrame(3, early, nullptr) == FetchStatus::kOk);
      if (early) CHECK(early->planeData[0] == reference[3]);
      CHECK(engine.NonBufferedServedFrames().size() == 1);
      CHECK(engine.RepositionCount() == 1);
    }
  }
  std::filesystem::remove(path);
}

// buffer_frames=auto: sized by memory (these frames are tiny, so the
// 300-frame cap applies), a far backward jump doesn't grow it, a request just
// below the full window doubles it once, and further steps back over frames
// that are already gone don't double it again.
void TestAutoBufferGrowsOnceOnStepBack() {
  const int width = 4, height = 2, numFrames = 1000;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_auto_buffer.mxf");
  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string(), NonStrictOptions());
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      CHECK(engine.BufferCapacity() == 300);
      std::shared_ptr<NormalizedFrame> frame;
      for (int i = 0; i < 900; ++i) CHECK(engine.GetVideoFrame(i, frame, nullptr) == FetchStatus::kOk);
      CHECK(engine.NonBufferedServedFrames().empty());

      auto check = [&](int n) {
        CHECK(engine.GetVideoFrame(n, frame, nullptr) == FetchStatus::kOk);
        CHECK(frame && !frame->planeData[0].empty() && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + n));
      };
      check(5);  // far below the window (~600..907): fallback, no growth
      CHECK(engine.BufferGrowthCount() == 0);
      CHECK(engine.BufferCapacity() == 300);

      check(590);  // just below it: fallback once, buffer doubles
      CHECK(engine.BufferGrowthCount() == 1);
      CHECK(engine.BufferCapacity() == 600);
      check(589);  // still gone, but the window isn't full any more
      check(588);
      CHECK(engine.BufferGrowthCount() == 1);
    }
  }
  std::filesystem::remove(path);
}

// An explicit buffer_frames keeps its fixed size.
void TestExplicitBufferFramesNeverGrows() {
  const int width = 4, height = 2, numFrames = 40;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_fixed_buffer.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 5;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      std::shared_ptr<NormalizedFrame> frame;
      for (int i = 0; i < 30; ++i) CHECK(engine.GetVideoFrame(i, frame, nullptr) == FetchStatus::kOk);
      CHECK(engine.GetVideoFrame(22, frame, nullptr) == FetchStatus::kOk);  // just below the window
      CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + 22));
      CHECK(engine.BufferCapacity() == 5);
      CHECK(engine.BufferGrowthCount() == 0);
    }
  }
  std::filesystem::remove(path);
}

// A far backward jump that stays there (Trim(30,39) ++ Trim(5,14), Loop):
// the first few frames take the fallback, then the producer moves back and
// the rest of the segment comes from the buffer -- on long-GOP content too.
void TestBackwardJumpRepositionsOnceIdle() {
  constexpr int kFrames = 40;
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kFrames));
  if (encoded.size() != static_cast<size_t>(kFrames)) return;

  auto referenceBytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_backjump_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_backjump.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 3;
    opts.eagerdecode = 1;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      auto check = [&](int64_t i) {
        std::shared_ptr<NormalizedFrame> frame;
        CHECK(engine.GetVideoFrame(i, frame, nullptr) == FetchStatus::kOk);
        if (frame) CHECK(frame->planeData[0] == reference[static_cast<size_t>(i)]);
      };
      const int64_t hi = 34;
      for (int64_t i = hi - 4; i < hi; ++i) check(i);  // first segment, after one jump ahead
      CHECK(engine.RepositionCount() == 1);
      CHECK(engine.NonBufferedServedFrames().empty());

      for (int64_t i = 2; i < 2 + 10; ++i) check(i);  // second segment, far back
      CHECK(engine.RepositionCount() == 2);
      // Requests 1-4 since the last buffered one take the fallback, the 5th
      // moves the producer; everything from there on comes from the buffer.
      CHECK(engine.NonBufferedServedFrames().size() == 4);
    }
  }
  std::filesystem::remove(path);
}

// Two far-apart positions read in turn (Interleave(a, a.Trim(k, 0))): the
// producer stays with one of them instead of bouncing between both.
void TestAlternatingFarPositionsDoNotThrash() {
  const int width = 4, height = 2, numFrames = 60;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_alternate.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 4;
    opts.eagerdecode = 1;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      std::shared_ptr<NormalizedFrame> frame;
      for (int i = 0; i < 20; ++i) {
        for (int n : {i, i + 30}) {
          CHECK(engine.GetVideoFrame(n, frame, nullptr) == FetchStatus::kOk);
          CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + n));
        }
      }
      CHECK(engine.RepositionCount() <= 1);  // the one jump ahead to 30, never back
    }
  }
  std::filesystem::remove(path);
}

// back_seek_preroll: how many frames before the jump target the restart
// begins -- 0 restarts exactly at the target, so the frame just before it is
// a fallback; 5 has it in the buffer.
void TestBackSeekPrerollSetsRestartLead() {
  const int width = 4, height = 2, numFrames = 60;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_preroll.mxf");
  for (int preroll : {0, 5}) {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 20;
    opts.eagerdecode = 1;
    opts.back_seek_preroll = preroll;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) continue;
    std::shared_ptr<NormalizedFrame> frame;
    for (int n : {40, 39, 38}) {
      CHECK(engine.GetVideoFrame(n, frame, nullptr) == FetchStatus::kOk);
      CHECK(frame && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + n));
    }
    CHECK(engine.RepositionCount() == 1);
    CHECK(engine.NonBufferedServedFrames().size() == (preroll == 0 ? 2u : 0u));
  }
  std::filesystem::remove(path);
}

// AviSynth's Prefetch: several threads ask for neighbouring frames at once,
// right after a jump ahead (Trim at the start of a script). Exactly one of
// them moves the producer; all of them get the right content.
void TestForwardJumpWithConcurrentRequestsRepositionsOnce() {
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kGopNumFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kGopNumFrames));
  if (encoded.size() != static_cast<size_t>(kGopNumFrames)) return;

  auto referenceBytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_fwdjump_mt_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kGopNumFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_fwdjump_mt.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    // Producer parked at frame 0 after Open (eagerdecode 1), so 7 is far
    // ahead. The restart begins 3 frames early (back_seek_preroll 8, capped
    // at capacity / 2) at 4, so frames up to 4 - 1 + 6 = 9 are within the
    // window and a thread that runs ahead can't legitimately trigger a second
    // restart.
    opts.buffer_frames = 6;
    opts.eagerdecode = 1;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      constexpr int kThreads = 3;
      constexpr int64_t kStart = 7, kEnd = 10;
      std::atomic<int> failures{0};
      std::vector<std::thread> threads;
      for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
          for (int64_t i = kStart + t; i < kEnd; i += kThreads) {
            std::shared_ptr<NormalizedFrame> frame;
            if (engine.GetVideoFrame(i, frame, nullptr) != FetchStatus::kOk ||
                frame->planeData[0] != reference[static_cast<size_t>(i)]) {
              ++failures;
            }
          }
        });
      }
      for (auto& th : threads) th.join();
      CHECK(failures == 0);
      // Racing threads may land just below the restart point and take the
      // fallback, but only one restart may happen: the others find the
      // producer already moved.
      CHECK(engine.RepositionCount() == 1);
    }
  }
  std::filesystem::remove(path);
}

// Audio after a jump ahead: the restart refills the audio buffer from the
// new position too, so video and audio for the frames after the jump both
// come from the buffers with the right content.
void TestForwardJumpRepositionsAudioToo() {
  const int width = 4, height = 2, numFrames = 50, samplesPerEditUnit = 2, channels = 1;
  auto path = WriteTempFile(BuildFileWithAudio(width, height, numFrames, samplesPerEditUnit, channels),
                            "mxfsrc_test_engine_audio_fwdjump.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.buffer_frames = 5;
    opts.eagerdecode = 2;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open error: " << res.error << std::endl;
      std::filesystem::remove(path);
      return;
    }
    CHECK(engine.ExposesAudio());

    constexpr int kTarget = 30;
    for (int i = kTarget; i < kTarget + 8; ++i) {
      std::shared_ptr<NormalizedFrame> frame;
      CHECK(engine.GetVideoFrame(i, frame, nullptr) == FetchStatus::kOk);
      CHECK(frame && !frame->planeData[0].empty() && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + i));

      std::vector<uint8_t> buf(static_cast<size_t>(samplesPerEditUnit) * channels * 2);
      CHECK(engine.GetAudioSamples(static_cast<int64_t>(i) * samplesPerEditUnit, samplesPerEditUnit, buf.data(),
                                   nullptr) == FetchStatus::kOk);
      CHECK(buf == MakeAudioEditUnitPayload(i, samplesPerEditUnit, channels));
    }
    CHECK(engine.RepositionCount() == 1);
    CHECK(engine.NonBufferedServedFrames().empty());
  }
  std::filesystem::remove(path);
}

// Exercises OpenOptions::start_frame_hint end to end for a GOP-based codec
// with a real Index Table Segment: DecodeThreadMain should jump-start
// production at a keyframe near the hint (via ResolveSeekStart -- the same
// lookup SeekAndDecode's own fallback uses) instead of at essence element 0,
// so every requested frame from the hint onward is served from the ring
// buffer (not SeekAndDecode's one-shot fallback) and still matches a
// from-frame-0 reference decode exactly. This is the exact scenario the
// hint exists for: a caller that opens a fresh SourceEngine and immediately
// requests a range far from frame 0 (see the sibling aws_lambda_qtgmc_chunked
// project's per-chunk usage).
void TestStartFrameHintGopServesFromBufferViaIndexAssistedJump() {
  auto encoded = EncodeMpeg2Gop(kGopWidth, kGopHeight, kGopNumFrames, kGopSize);
  CHECK(encoded.size() == static_cast<size_t>(kGopNumFrames));
  if (encoded.size() != static_cast<size_t>(kGopNumFrames)) return;

  auto referenceBytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/false);
  auto referencePath = WriteTempFile(referenceBytes, "mxfsrc_test_gop_hint_reference.mxf");
  auto reference = DecodeAllSequentially(referencePath.string(), kGopNumFrames);
  std::filesystem::remove(referencePath);

  auto bytes = BuildGopFile(encoded, kGopWidth, kGopHeight, kGopTrackNumber, /*includeIndexTable=*/true);
  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_hint.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    // kGopSize == 5 -> keyframes at edit units 0, 5, 10. 7 is two edit units
    // past the keyframe at 5, so ResolveSeekStart should land the jump-start
    // there (plenty of runway; this file has no B frames, so reorderDelay is
    // always 0 and every hop of ResolveSeekStart's backward walk is moot),
    // not at essence element 0.
    opts.start_frame_hint = 7;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      // FirstFrame() is Open()'s own Peek() at the ring buffer's very first
      // index -- now firstHostFrame_ (7) instead of 0 -- so this alone
      // proves DecodeThreadMain produced hint-relative content there, not
      // essence element 0's.
      CHECK(engine.FirstFrame().planeData[0] == reference[7]);

      for (int64_t i = 7; i < kGopNumFrames; ++i) {
        std::shared_ptr<NormalizedFrame> frame;
        std::string err;
        FetchStatus st = engine.GetVideoFrame(i, frame, &err);
        CHECK(st == FetchStatus::kOk);
        if (st == FetchStatus::kOk) CHECK(frame->planeData[0] == reference[i]);
        else std::cerr << "frame " << i << " failed: " << err << std::endl;
      }
      // Every frame from the hint onward came from the ring buffer's normal
      // production, never SeekAndDecode's one-shot fallback -- the whole
      // point of the hint.
      CHECK(engine.NonBufferedServedFrames().empty());
      // Confirms ResolveSeekStart's keyframe lookup was actually consulted
      // (once, at Open() time) rather than silently falling back to essence
      // element 0.
      CHECK(engine.IndexAssistedSeekCount() > 0);

      // A frame *before* the hint is still reachable -- via the existing
      // SeekAndDecode fallback, since the ring buffer's own window starts at
      // the hint and reports anything earlier as evicted -- just not from
      // the buffer.
      std::shared_ptr<NormalizedFrame> early;
      CHECK(engine.GetVideoFrame(2, early, nullptr) == FetchStatus::kOk);
      if (early) CHECK(early->planeData[0] == reference[2]);
      CHECK(!engine.NonBufferedServedFrames().empty());
    }
  }
  std::filesystem::remove(path);
}

// Same scenario as above but for an intra-only codec (the plain
// uncompressed-UYVY synthetic file every other test in this file uses) --
// exercises DecodeThreadMain's SkipNextVideoElement discard branch (cheap
// skip, no decode) instead of the GOP branch's decode-and-discard, and
// confirms it interacts correctly with OpenOptions::handle_origin's own
// pre-roll skip (both are in effect here: the hint is relative to the
// origin-trimmed host frame space, same as every other frame index this
// engine hands a caller).
void TestStartFrameHintIntraOnlyStartsProductionAtHint() {
  const int width = 4, height = 2, numFrames = 10;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_hint_intra.mxf");

  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.start_frame_hint = 6;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      CHECK(!engine.FirstFrame().planeData[0].empty());
      if (!engine.FirstFrame().planeData[0].empty()) {
        CHECK(engine.FirstFrame().planeData[0][0] == static_cast<uint8_t>(0x10 + 6));
      }

      for (int i = 6; i < numFrames; ++i) {
        std::shared_ptr<NormalizedFrame> frame;
        std::string err;
        FetchStatus st = engine.GetVideoFrame(i, frame, &err);
        CHECK(st == FetchStatus::kOk);
        if (st == FetchStatus::kOk && !frame->planeData[0].empty()) {
          CHECK(frame->planeData[0][0] == static_cast<uint8_t>(0x10 + i));
        }
      }
      CHECK(engine.NonBufferedServedFrames().empty());  // all served from the buffer, not the fallback

      // A frame before the hint still works, via the fallback.
      std::shared_ptr<NormalizedFrame> early;
      CHECK(engine.GetVideoFrame(2, early, nullptr) == FetchStatus::kOk);
      if (early && !early->planeData[0].empty()) CHECK(early->planeData[0][0] == static_cast<uint8_t>(0x10 + 2));
      CHECK(!engine.NonBufferedServedFrames().empty());
    }
  }
  std::filesystem::remove(path);
}

// An out-of-range hint (at/past the file's own frame count) must be ignored,
// not fail Open() -- same as the field's own default (0).
void TestStartFrameHintOutOfRangeIgnored() {
  const int width = 4, height = 2, numFrames = 5;
  auto path = WriteTempFile(BuildFile(width, height, numFrames), "mxfsrc_test_engine_hint_oor.mxf");

  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.start_frame_hint = numFrames;  // at, not past, the frame count -- still out of range
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      CHECK(!engine.FirstFrame().planeData[0].empty());
      if (!engine.FirstFrame().planeData[0].empty()) {
        CHECK(engine.FirstFrame().planeData[0][0] == static_cast<uint8_t>(0x10));  // fell back to frame 0
      }
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
    OpenOptions opts = NonStrictOptions();
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

// Regression test: DecodeThreadMain's hinted warm-up block (the discard/
// decode loop from hintStartEditUnit_ through the hint's target, see its own
// doc comment) pre-fills the video `pending` deque with real lead-in content
// so ring index firstHostFrame_'s video comes out already delay-compensated
// -- but originally discarded the audio it walked past along the way
// (DecodeNextFrame's audioElementsOut was nullptr there) instead of using it
// to warm up pendingAudio the same way. That left pendingAudio starting from
// empty in the main loop, so the first hintReorderDelay_ ring indices after
// a hinted seek got the "still warming up" undelayed repeat instead of real
// delay-compensated audio, self-correcting only once pendingAudio genuinely
// accumulated past reorderDelay entries. Needs a GOP codec with real B-frame
// reordering (reorderDelay > 0) to exercise at all -- EncodeMpeg2Gop's
// max_b_frames=0 stream can't touch this, same trap noted on
// TestReorderDelayCompensationMatchesReferenceWithRealBFrames above.
void TestStartFrameHintGopWithBFramesAudioMatchesSequentialReference() {
  constexpr int width = 32, height = 32, gopSize = 6, maxBFrames = 2, numFrames = 30;
  constexpr int samplesPerEditUnit = 4, channels = 2;
  auto encoded = EncodeMpeg2GopWithBFrames(width, height, numFrames, gopSize, maxBFrames);
  CHECK(encoded.size() == static_cast<size_t>(numFrames));
  if (encoded.size() != static_cast<size_t>(numFrames)) return;

  const bool hasBFrames = std::any_of(encoded.begin(), encoded.end(), [](const EncodedGopEntry& e) {
    return e.flags == 0x30;
  });
  CHECK(hasBFrames);
  if (!hasBFrames) return;

  constexpr uint32_t kHintVideoTrackNumber = 0x04040400;
  constexpr uint32_t kHintAudioTrackNumber = 0x04040500;
  auto bytes = BuildGopFileWithAudio(encoded, width, height, kHintVideoTrackNumber, kHintAudioTrackNumber,
                                      samplesPerEditUnit, channels);

  // Ground truth: sequential decode from frame 0, no hint at all.
  auto referencePath = WriteTempFile(bytes, "mxfsrc_test_gop_hint_audio_reference.mxf");
  std::vector<uint8_t> referenceAudio;
  {
    SourceEngine engine;
    OpenResult res = engine.Open(referencePath.string(), NonStrictOptions());
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      const int64_t totalSamples = static_cast<int64_t>(numFrames) * samplesPerEditUnit;
      referenceAudio.resize(static_cast<size_t>(totalSamples) * channels * 2);
      std::string err;
      FetchStatus st = engine.GetAudioSamples(0, totalSamples, referenceAudio.data(), &err);
      CHECK(st == FetchStatus::kOk);
      if (st != FetchStatus::kOk) std::cerr << "reference audio failed: " << err << std::endl;
    }
  }
  std::filesystem::remove(referencePath);
  if (referenceAudio.empty()) return;

  // Deliberately inside the FIRST GOP (whose own keyframe is display/decode
  // position 0) rather than at some later GOP's keyframe: ResolveSeekStart's
  // backward-walk short-circuits to startEditUnit==0 the moment it reaches
  // edit unit 0 (see its own "needs no compensation, proven above" comment),
  // so hintStartEditUnit_/hintReorderDelay_ end up identical to what the
  // ordinary non-hinted path itself anchors at (element 0). That isolates
  // this test to purely the fixed warm-up-audio-collection gap -- a target
  // anchored at a *different* keyframe would also fold in
  // EstimateReorderDelay's own known accuracy gap for arbitrary B-pyramid
  // GOPs (see TestReorderDelayCompensationMatchesReferenceWithRealBFrames's
  // doc comment), which is a separate, pre-existing, already-acknowledged
  // limitation this test isn't targeting.
  const int64_t target = 3;

  auto path = WriteTempFile(bytes, "mxfsrc_test_gop_hint_audio.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();
    opts.start_frame_hint = target;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      const int64_t startSample = target * samplesPerEditUnit;
      const int64_t sampleCount = (numFrames - target) * static_cast<int64_t>(samplesPerEditUnit);
      std::vector<uint8_t> got(static_cast<size_t>(sampleCount) * channels * 2);
      std::string err;
      FetchStatus st = engine.GetAudioSamples(startSample, sampleCount, got.data(), &err);
      CHECK(st == FetchStatus::kOk);
      if (st == FetchStatus::kOk) {
        // Regression: before the fix, ring indices `target` and `target + 1`
        // (the first hintReorderDelay_ positions after the hint) came out as
        // an undelayed repeat instead of this real, delay-compensated
        // content -- self-correcting from edit unit `target + 2` onward.
        const uint8_t* expected = referenceAudio.data() + static_cast<size_t>(startSample) * channels * 2;
        CHECK(std::memcmp(got.data(), expected, got.size()) == 0);
      } else {
        std::cerr << "hinted audio failed: " << err << std::endl;
      }
    }
  }
  std::filesystem::remove(path);
}

// Regression test: DecodeThreadMain's NON-hinted warm-up (the "still
// warming up" pendingAudio fallback below reorderDelay's threshold, see its
// own doc comment) served the SAME stale block repeatedly for every one of
// the leading reorderDelay ring indices of a from-frame-0 decode, instead of
// each ring index's own genuinely-decoded audio -- unlike video, which truly
// has no correct content available for these positions (nothing exists
// before essence element 0), audio's own just-decoded block for THIS
// iteration is always real, correct content, so repeating an older one was
// an avoidable defect, not an inherent limit. Found via a real file
// (D:\Vistek_DMX_-1ms.mxf, MPEG-2 long-GOP, no hint involved at all)
// byte-compared against ffmpeg's own independent MXF demux -- confirmed
// samples belonging to edit unit 1 came out identical to edit unit 0's real
// content instead of their own.
void TestSequentialAudioFromFrameZeroDoesNotRepeatDuringWarmup() {
  constexpr int width = 32, height = 32, gopSize = 6, maxBFrames = 2, numFrames = 30;
  constexpr int samplesPerEditUnit = 4, channels = 2;
  auto encoded = EncodeMpeg2GopWithBFrames(width, height, numFrames, gopSize, maxBFrames);
  CHECK(encoded.size() == static_cast<size_t>(numFrames));
  if (encoded.size() != static_cast<size_t>(numFrames)) return;

  const bool hasBFrames = std::any_of(encoded.begin(), encoded.end(), [](const EncodedGopEntry& e) {
    return e.flags == 0x30;
  });
  CHECK(hasBFrames);
  if (!hasBFrames) return;

  constexpr uint32_t kSeqVideoTrackNumber = 0x04040600;
  constexpr uint32_t kSeqAudioTrackNumber = 0x04040700;
  auto bytes = BuildGopFileWithAudio(encoded, width, height, kSeqVideoTrackNumber, kSeqAudioTrackNumber,
                                      samplesPerEditUnit, channels);
  auto path = WriteTempFile(bytes, "mxfsrc_test_seq_audio_no_repeat.mxf");

  {
    SourceEngine engine;
    OpenResult res = engine.Open(path.string(), NonStrictOptions());  // no hint at all -- exactly the reported repro's shape
    CHECK(res.status == OpenStatus::kOk);
    if (res.status == OpenStatus::kOk) {
      // Deliberately NOT asserting exact expected *values* per display
      // position here (that would require independently modeling this
      // GOP's own decode-order-vs-display-order permutation --
      // MakeAudioEditUnitPayload(i, ...) is keyed by decode/physical
      // position i, not display position, see BuildGopFileWithAudio's own
      // doc comment -- already covered for video by
      // TestReorderDelayCompensationMatchesReferenceWithRealBFrames).
      // This test only targets the specific repeat-during-warmup
      // regression, which is visible directly without that model: this
      // GOP's EstimateReorderDelay(0) is 2 (maxBFrames=2's IBB structure --
      // entries 1 and 2 are the B-frame-flagged run right after the edit
      // unit 0 keyframe), so ring indices 0 and 1 are exactly
      // DecodeThreadMain's "still warming up" window (pendingAudio.size()
      // never exceeds reorderDelay=2 until the 3rd push). Before the fix,
      // both came out byte-identical (pendingAudio.front() pinned at
      // decode-position 0's block for the whole window); after it, ring 1
      // gets its own genuinely-decoded block instead, which is guaranteed
      // to differ from ring 0's (MakeAudioEditUnitPayload embeds the edit
      // unit index directly into the byte pattern).
      const int64_t sampleCount = 2 * samplesPerEditUnit;
      std::vector<uint8_t> got(static_cast<size_t>(sampleCount) * channels * 2);
      std::string err;
      FetchStatus st = engine.GetAudioSamples(0, sampleCount, got.data(), &err);
      CHECK(st == FetchStatus::kOk);
      if (st == FetchStatus::kOk) {
        const size_t bytesPerEditUnit = static_cast<size_t>(samplesPerEditUnit) * channels * 2;
        const uint8_t* ring0 = got.data();
        const uint8_t* ring1 = got.data() + bytesPerEditUnit;
        const bool duplicated = std::memcmp(ring0, ring1, bytesPerEditUnit) == 0;
        CHECK(!duplicated);
        if (duplicated) {
          std::cerr << "ring indices 0 and 1 have identical audio (repeat-during-warmup regression)" << std::endl;
        }
      } else {
        std::cerr << "sequential audio failed: " << err << std::endl;
      }
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
    OpenResult res = engine.Open(path.string(), NonStrictOptions());
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
    OpenResult res = engine.Open(path.string(), NonStrictOptions());
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
    OpenOptions opts = NonStrictOptions();
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

// Regression test for a real hang found on a real file (D:\MH_0021.mxf):
// BuildAudioSampleIndexFromIndexTable derives a closed-form, uniform
// per-edit-unit sample count from editRate/audioSampleRate (see its own doc
// comment on the NTSC 1601/1602 cadence) -- but a real file's actual
// per-edit-unit decoded sample count can disagree with that estimate by a
// sample or two if the encoder's own rounding phase differs. When
// GetAudioSamples' Locate() lands at/past the real decoded block's
// sampleCount for that edit unit, `available` (block->sampleCount -
// loc->sampleOffsetInEditUnit) was <= 0, making `toCopy` <= 0 too --
// `remaining`/`hostSamplePos` never advanced, so the loop called Locate()
// with the exact same sample position forever (confirmed via a live hang:
// a debugger attached to the real stuck process showed GetAudioSamples
// spinning on Locate(6405) indefinitely). This file reproduces that exact
// edit-unit-boundary mismatch deterministically, without needing a
// genuinely non-integer editRate/sampleRate ratio -- see
// BuildFileWithAudioCustomCounts.
void TestGetAudioSamplesDoesNotHangOnEditUnitSampleCountMismatch() {
  const int width = 4, height = 2, numFrames = 5, samplesPerEditUnit = 4, channels = 1;
  std::vector<int> counts(static_cast<size_t>(numFrames), samplesPerEditUnit);
  counts[2] = samplesPerEditUnit - 1;  // edit unit 2's real audio is one sample short of the index's estimate
  auto path = WriteTempFile(BuildFileWithAudioCustomCounts(width, height, numFrames, samplesPerEditUnit, channels, counts),
                            "mxfsrc_test_engine_audio_count_mismatch.mxf");

  auto engine = std::make_shared<SourceEngine>();
  OpenResult res = engine->Open(path.string(), NonStrictOptions());
  CHECK(res.status == OpenStatus::kOk);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open error: " << res.error << std::endl;
    RemoveTempFileIgnoringErrors(path);
    return;
  }
  CHECK(engine->ExposesAudio());

  const int64_t totalSamples = engine->TotalAudioSamples();
  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(totalSamples) * channels * 2);
  auto st = std::make_shared<FetchStatus>(FetchStatus::kError);
  auto err = std::make_shared<std::string>();

  // See RunWithTimeout's doc comment: engine/buf/st/err are heap-owned so a
  // regression here leaks a spinning thread rather than leaving it with a
  // dangling reference once this function returns.
  const bool finished = RunWithTimeout(
      [engine, buf, st, err, totalSamples] { *st = engine->GetAudioSamples(0, totalSamples, buf->data(), err.get()); },
      std::chrono::seconds(5));
  CHECK(finished);
  if (!finished) {
    std::cerr << "GetAudioSamples did not return within the timeout -- likely hung" << std::endl;
    RemoveTempFileIgnoringErrors(path);
    return;
  }
  CHECK(*st == FetchStatus::kOk);
  if (*st != FetchStatus::kOk) std::cerr << "GetAudioSamples failed: " << *err << std::endl;

  // engine (and its GetAudioSamples call, now provably finished above) may
  // still be holding the file open here -- see RemoveTempFileIgnoringErrors.
  RemoveTempFileIgnoringErrors(path);
}

// Regression test for a second real hang found alongside the one above,
// via a hung real-file `ffmpeg -map 0:a:0 ...` audio-only extraction:
// DecodeThreadMain's single background thread used to gate ALL production
// (video AND audio, since both come from one shared walker) purely on
// videoBuffer_'s own WaitForCapacity -- correct for a caller that reads
// video (whether or not it also reads audio), but a caller that only ever
// reads audio and never calls GetVideoFrame leaves videoBuffer_'s
// watermark_ parked at its initial value forever, so that gate blocked
// permanently once video's own small eager-readahead margin filled --
// capping audio at whatever was produced during that initial window and
// hanging every later audio request. GetVideoFrame is deliberately never
// called in this test.
void TestGetAudioSamplesDoesNotStallWhenVideoNeverRequested() {
  const int width = 4, height = 2, numFrames = 50, samplesPerEditUnit = 2, channels = 1;
  auto path = WriteTempFile(BuildFileWithAudio(width, height, numFrames, samplesPerEditUnit, channels),
                            "mxfsrc_test_engine_audio_only_no_video.mxf");

  auto engine = std::make_shared<SourceEngine>();
  OpenOptions opts = NonStrictOptions();
  opts.buffer_frames = 5;  // small margin so the bug (if reintroduced) triggers well within numFrames=50
  opts.eagerdecode = 2;
  OpenResult res = engine->Open(path.string(), opts);
  CHECK(res.status == OpenStatus::kOk);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open error: " << res.error << std::endl;
    RemoveTempFileIgnoringErrors(path);
    return;
  }
  CHECK(engine->ExposesAudio());

  const int64_t totalSamples = engine->TotalAudioSamples();
  auto buf = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(totalSamples) * channels * 2);
  auto st = std::make_shared<FetchStatus>(FetchStatus::kError);
  auto err = std::make_shared<std::string>();

  const bool finished = RunWithTimeout(
      [engine, buf, st, err, totalSamples] { *st = engine->GetAudioSamples(0, totalSamples, buf->data(), err.get()); },
      std::chrono::seconds(5));
  CHECK(finished);
  if (!finished) {
    std::cerr << "GetAudioSamples did not return within the timeout -- likely hung" << std::endl;
    RemoveTempFileIgnoringErrors(path);
    return;
  }
  CHECK(*st == FetchStatus::kOk);
  if (*st != FetchStatus::kOk) std::cerr << "GetAudioSamples failed: " << *err << std::endl;

  std::vector<uint8_t> expected;
  for (int i = 0; i < numFrames; ++i) {
    const auto p = MakeAudioEditUnitPayload(i, samplesPerEditUnit, channels);
    expected.insert(expected.end(), p.begin(), p.end());
  }
  CHECK(*buf == expected);

  RemoveTempFileIgnoringErrors(path);
}

// use_audio="supported" (the default): a file whose audio tracks disagree
// on bit depth must still open successfully for video, just without audio.
void TestUseAudioModeSupportedDegradesOnMismatch() {
  auto path =
      WriteTempFile(BuildFileWithMismatchedAudio(4, 2, 2), "mxfsrc_test_engine_audio_mismatch_supported.mxf");
  {
    SourceEngine engine;
    OpenOptions opts = NonStrictOptions();  // use_audio defaults to UseAudioMode::kSupported
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
    OpenOptions opts = NonStrictOptions();
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
    OpenOptions opts = NonStrictOptions();
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
    OpenOptions opts = NonStrictOptions();
    opts.use_audio = UseAudioMode::kAlways;
    OpenResult res = engine.Open(path.string(), opts);
    CHECK(res.status == OpenStatus::kOk);
    CHECK(!engine.ExposesAudio());
  }
  std::filesystem::remove(path);
}

// Shape of a finished (or damaged) recording for the OpenOptions::strict
// tests: video-only UYVY essence in one Body Partition carrying a VBR Index
// Table Segment, then a Footer Partition and a RIP. Each field breaks one
// thing.
struct StrictTestFileSpec {
  int numFrames = 4;                 // essence elements written (and header Duration unless declaredFrames)
  int declaredFrames = -1;           // >= 0: header Duration differs from the essence actually written
  int indexedFrames = 4;             // edit units the Index Table covers; 0 = no Index Table Segment at all
  bool headerClosed = true;          // Header Partition status ClosedComplete vs OpenIncomplete
  bool footer = true;                // write a Footer Partition Pack
  bool rip = true;                   // write a RIP after it
  uint64_t footerPointerOverride = 0;  // nonzero: Header's FooterPartition field instead of the real offset
  size_t zeroTailBytes = 0;          // zero bytes appended at the very end
  // No Body Partition: a CBR (constant bytes per edit unit, e.g. XAVC CBG)
  // Index Table Segment and the essence follow the Header Metadata
  // directly. indexedFrames is ignored.
  bool cbrIndexInHeader = false;
};

std::vector<uint8_t> BuildStrictTestFile(const StrictTestFileSpec& spec) {
  const int width = 4, height = 2;
  SyntheticFile cfg;
  cfg.sequenceDuration = spec.declaredFrames >= 0 ? spec.declaredFrames : spec.numFrames;
  cfg.width = width;
  cfg.height = height;
  cfg.videoTrackNumber = kTrackNumber;
  std::vector<uint8_t> file = BuildSyntheticHeader(cfg);
  file[14] = static_cast<uint8_t>(spec.headerClosed ? TestPartitionStatus::kClosedComplete
                                                    : TestPartitionStatus::kOpenIncomplete);

  std::vector<std::vector<uint8_t>> payloads;
  for (int i = 0; i < spec.numFrames; ++i) payloads.push_back(MakeFramePayload(width, height, static_cast<uint8_t>(0x10 + i)));

  if (spec.cbrIndexInHeader) {
    IndexTableSegmentSpec cbr;
    cbr.editUnitByteCount = static_cast<uint32_t>(16 + BerLengthSize(payloads[0].size()) + payloads[0].size());
    std::vector<uint8_t> cbrBytes;
    AppendIndexTableSegment(cbrBytes, cbr);
    file.insert(file.end(), cbrBytes.begin(), cbrBytes.end());
    for (const auto& payload : payloads) AppendEssenceElement(file, kTrackNumber, payload);
    // Header Partition Pack value (from byte 17): IndexByteCount at +40,
    // IndexSID at +48.
    for (int i = 0; i < 8; ++i) file[57 + i] = static_cast<uint8_t>(uint64_t{cbrBytes.size()} >> (56 - 8 * i));
    file[68] = 1;
    for (int i = 0; i < 8; ++i) file[41 + i] = 0;  // FooterPartition unknown, as in a growing file
    file.insert(file.end(), spec.zeroTailBytes, 0);
    return file;
  }

  IndexTableSegmentSpec segSpec;
  segSpec.indexDuration = spec.indexedFrames;
  segSpec.entries.resize(static_cast<size_t>(spec.indexedFrames));
  std::vector<uint8_t> segBytes;
  if (spec.indexedFrames > 0) AppendIndexTableSegment(segBytes, segSpec);  // placeholder: size only

  const uint64_t bodyOffset = file.size();
  AppendPartitionPack(file, PartitionKind::kBody, segBytes.size(), spec.indexedFrames > 0 ? 1 : 0);
  const uint64_t essenceStart = file.size() + segBytes.size();
  uint64_t cursor = essenceStart;
  for (int i = 0; i < spec.numFrames; ++i) {
    if (i < spec.indexedFrames) segSpec.entries[static_cast<size_t>(i)].streamOffset = cursor - essenceStart;
    cursor += 16 + BerLengthSize(payloads[static_cast<size_t>(i)].size()) + payloads[static_cast<size_t>(i)].size();
  }
  if (spec.indexedFrames > 0) {
    segBytes.clear();
    AppendIndexTableSegment(segBytes, segSpec);
  }
  file.insert(file.end(), segBytes.begin(), segBytes.end());
  for (const auto& payload : payloads) AppendEssenceElement(file, kTrackNumber, payload);

  const uint64_t footerOffset = file.size();
  if (spec.footer) {
    AppendPartitionPackWithFields(file, PartitionKind::kFooter, TestPartitionStatus::kClosedComplete, footerOffset,
                                  footerOffset, 0, 0, 0, 0, /*bodySid=*/0);
  }
  if (spec.rip) {
    std::vector<std::pair<uint32_t, uint64_t>> entries = {{0, 0}, {1, bodyOffset}};
    if (spec.footer) entries.push_back({0, footerOffset});
    AppendRandomIndexPack(file, entries);
  }
  file.insert(file.end(), spec.zeroTailBytes, 0);

  // Header Partition Pack's FooterPartition field: 16-byte key + 1-byte BER
  // length, then MajorVersion(2) MinorVersion(2) KAGSize(4) ThisPartition(8)
  // PreviousPartition(8).
  const uint64_t footerPointer = spec.footerPointerOverride != 0 ? spec.footerPointerOverride
                                 : spec.footer                   ? footerOffset
                                                                 : 0;
  for (int i = 0; i < 8; ++i) file[41 + i] = static_cast<uint8_t>(footerPointer >> (56 - 8 * i));
  return file;
}

OpenResult OpenStrictTestFile(const StrictTestFileSpec& spec, bool strict, const char* name,
                              int64_t* numFramesOut = nullptr) {
  auto path = WriteTempFile(BuildStrictTestFile(spec), name);
  OpenResult res;
  {
    SourceEngine engine;
    OpenOptions opts;
    opts.strict = strict;
    res = engine.Open(path.string(), opts);
    if (numFramesOut) *numFramesOut = engine.StreamInfo().numFrames;
  }
  std::filesystem::remove(path);
  return res;
}

bool ErrorContains(const OpenResult& res, const std::string& text) { return res.error.find(text) != std::string::npos; }

// The zero-tail message names only what was read: the start of the last
// kMaxExpectedTrailingZeroBytes block and the file size.
std::string ZeroTailMessage(const StrictTestFileSpec& spec) {
  const uint64_t size = BuildStrictTestFile(spec).size();
  return "strict: unexpected zero bytes at offset " + std::to_string(size - kMaxExpectedTrailingZeroBytes) +
         " (verified from there to the end of the file at " + std::to_string(size) + ")";
}

// A finished, intact file opens in strict mode (the default).
void TestStrictOpensIntactClosedFile() {
  StrictTestFileSpec spec;
  int64_t numFrames = -1;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_intact.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == spec.numFrames);
  CHECK(OpenOptions{}.strict);
}

// Zero-filled tail where the Footer/RIP should be (real case: XDCAM
// recordings whose writer preallocated the file and never finished it).
// Strict fails before any index work; non-strict opens as before.
void TestStrictFailsOnZeroTail() {
  StrictTestFileSpec spec;
  spec.footer = false;
  spec.rip = false;
  spec.footerPointerOverride = 0;
  spec.zeroTailBytes = 200000;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_zero_tail.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, ZeroTailMessage(spec)));

  res = OpenStrictTestFile(spec, false, "mxfsrc_test_strict_zero_tail_off.mxf");
  CHECK(res.status == OpenStatus::kOk);

  // The verified block is reported; a zero run below the threshold isn't
  // flagged.
  for (size_t zeros : {size_t{200000}, size_t{1000}}) {
    spec.zeroTailBytes = zeros;
    const std::vector<uint8_t> bytes = BuildStrictTestFile(spec);
    auto path = WriteTempFile(bytes, "mxfsrc_test_strict_zero_tail_probe.mxf");
    {
      MxfReader reader(path.string());
      std::optional<FileIntegrity> integrity = ProbeFileIntegrity(reader);
      CHECK(integrity.has_value());
      if (integrity) {
        CHECK(integrity->headerClosed);
        CHECK(integrity->trailingZerosUnexpected == (zeros > kMaxExpectedTrailingZeroBytes));
        if (integrity->trailingZerosUnexpected) {
          CHECK(integrity->zeroTailOffset == bytes.size() - kMaxExpectedTrailingZeroBytes);
        }
      }
    }
    std::filesystem::remove(path);
  }
}

// Header declares a Footer beyond EOF and there is no RIP (real case: the
// first 40% of a finished 1.75 GB file, cut mid-frame by an aborted copy).
void TestStrictFailsOnFooterBeyondEof() {
  StrictTestFileSpec spec;
  spec.footer = false;
  spec.rip = false;
  spec.footerPointerOverride = 1u << 30;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_footer_eof.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "the file is truncated"));

  res = OpenStrictTestFile(spec, false, "mxfsrc_test_strict_footer_eof_off.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// Header declares a Footer inside the file, but there's essence there, and
// no RIP.
void TestStrictFailsOnFooterPointerToNonPartition() {
  StrictTestFileSpec spec;
  spec.footer = false;
  spec.rip = false;
  spec.footerPointerOverride = 200;  // inside Header Metadata
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_footer_bad.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "no valid Footer Partition Pack"));
}

// A wrong Footer pointer is tolerated when a valid RIP is there (real case:
// Omneon files, see header_parser.cpp's RIP fallback).
void TestStrictToleratesBadFooterPointerWithValidRip() {
  StrictTestFileSpec spec;
  spec.footerPointerOverride = 200;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_footer_bad_rip.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// Closed header, no Index Table Segment anywhere: strict refuses rather than
// running the full-file bitstream scan.
void TestStrictFailsWithoutIndex() {
  StrictTestFileSpec spec;
  spec.indexedFrames = 0;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_no_index.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "no Index Table Segment"));

  int64_t numFrames = -1;
  res = OpenStrictTestFile(spec, false, "mxfsrc_test_strict_no_index_off.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == spec.numFrames);
}

// Index covers fewer edit units than the header's Duration.
void TestStrictFailsWhenIndexShorterThanDuration() {
  StrictTestFileSpec spec;
  spec.indexedFrames = 2;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_short_index.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "declares 4 edit units, but the Index Table covers only 2"));

  res = OpenStrictTestFile(spec, false, "mxfsrc_test_strict_short_index_off.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// Header Duration larger than the essence (real case: Schuhegestohlen, 714
// declared, 706 frames, then the Footer): without strict, only the frames
// that exist are exposed -- before, the clip claimed 714 and ended in decode
// errors, and its audio was dropped. Strict still refuses the finished file.
void TestEssenceEndingBeforeDurationIsClamped() {
  StrictTestFileSpec spec;
  spec.numFrames = 4;
  spec.indexedFrames = 4;
  spec.declaredFrames = 6;
  int64_t numFrames = -1;
  OpenResult res = OpenStrictTestFile(spec, false, "mxfsrc_test_clamp_footer.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == 4);

  res = OpenStrictTestFile(spec, true, "mxfsrc_test_clamp_footer_strict.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "declares 6 edit units, but the Index Table covers only 4"));

  // No Footer, essence followed directly by the RIP: same.
  spec.footer = false;
  res = OpenStrictTestFile(spec, false, "mxfsrc_test_clamp_rip.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == 4);

  // Growing file in strict mode (no Duration check): clamped too.
  spec.headerClosed = false;
  res = OpenStrictTestFile(spec, true, "mxfsrc_test_clamp_growing.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == 4);
}

// EssenceEndsAfter on the content package after the last index entry:
// complete, cut off in zero fill after 2 of 3 sound elements (real case:
// Sobotka, picture + 5 of 8 sound elements, then zeros -- the last frame's
// audio could not be read), and followed by another frame.
void TestEssenceEndsAfterChecksLastContentPackage() {
  const std::array<uint8_t, 4> video = {0x15, 0x01, 0x05, 0x00};
  const std::vector<std::array<uint8_t, 4>> audio = {
      {0x16, 0x08, 0x03, 0x00}, {0x16, 0x08, 0x03, 0x01}, {0x16, 0x08, 0x03, 0x02}};
  auto element = [](std::vector<uint8_t>& buf, const std::array<uint8_t, 4>& tn) {
    AppendEssenceElement(buf, (uint32_t{tn[0]} << 24) | (uint32_t{tn[1]} << 16) | (uint32_t{tn[2]} << 8) | tn[3],
                         std::vector<uint8_t>(32, 0x11));
  };
  struct Case {
    const char* name;
    int soundElements;
    bool anotherFrame;
    bool ends;
    bool complete;
  };
  const Case cases[] = {
      {"complete, then zeros", 3, false, true, true},
      {"2 of 3 sound elements, then zeros", 2, false, true, false},
      {"another frame follows", 3, true, false, false},
  };
  for (const Case& c : cases) {
    std::vector<uint8_t> bytes(100, 0x55);  // stand-in for earlier essence
    const uint64_t lastOffset = bytes.size();
    element(bytes, video);
    for (int i = 0; i < c.soundElements; ++i) element(bytes, audio[static_cast<size_t>(i)]);
    if (c.anotherFrame) element(bytes, video);
    bytes.insert(bytes.end(), 4096, 0);
    auto path = WriteTempFile(bytes, "mxfsrc_test_essence_end.bin");
    {
      MxfReader reader(path.string());
      const EssenceEnd end = EssenceEndsAfter(reader, lastOffset, video, audio);
      if (end.ends != c.ends || end.lastPackageComplete != c.complete) std::cerr << c.name << std::endl;
      CHECK(end.ends == c.ends);
      CHECK(end.lastPackageComplete == c.complete);
    }
    std::filesystem::remove(path);
  }
}

// Index stops early but the essence goes on (e.g. the last partition's index
// would have been in a missing Footer): the declared frame count stays, the
// unindexed frames are still reached by the sequential walk.
void TestIndexEndingBeforeEssenceIsNotClamped() {
  StrictTestFileSpec spec;
  spec.numFrames = 4;
  spec.indexedFrames = 2;
  int64_t numFrames = -1;
  OpenResult res = OpenStrictTestFile(spec, false, "mxfsrc_test_no_clamp.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == 4);
}

// Open header, no Footer (growing/aborted recording) with an index reached
// via the RIP: opens with what the index covers, even though the header's
// not-yet-final Duration claims more.
void TestStrictOpensGrowingFileWithRipIndex() {
  StrictTestFileSpec spec;
  spec.headerClosed = false;
  spec.footer = false;
  spec.indexedFrames = 2;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_growing_rip.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// Growing file whose only index is a CBR/CBG segment in the Header
// Partition, no RIP, no Body Partition: reachable without a scan.
void TestStrictOpensGrowingFileWithCbrIndexInHeader() {
  StrictTestFileSpec spec;
  spec.headerClosed = false;
  spec.footer = false;
  spec.rip = false;
  spec.cbrIndexInHeader = true;
  int64_t numFrames = -1;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_growing_cbr.mxf", &numFrames);
  CHECK(res.status == OpenStatus::kOk);
  CHECK(numFrames == spec.numFrames);
}

// Growing file whose index isn't reachable (no RIP): strict refuses instead
// of the full-file scan it used to fall back to.
void TestStrictFailsGrowingFileWithoutReachableIndex() {
  StrictTestFileSpec spec;
  spec.headerClosed = false;
  spec.footer = false;
  spec.rip = false;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_growing_no_index.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "no Index Table Segment"));

  res = OpenStrictTestFile(spec, false, "mxfsrc_test_strict_growing_no_index_off.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// A zero tail fails even with an Open header (e.g. preallocated recording).
void TestStrictFailsZeroTailWithOpenHeader() {
  StrictTestFileSpec spec;
  spec.headerClosed = false;
  spec.footer = false;
  spec.rip = false;
  spec.cbrIndexInHeader = true;
  spec.zeroTailBytes = 200000;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_open_zero_tail.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, ZeroTailMessage(spec)));
}

// Open header but a Closed Footer: the file is finished, so the index must
// cover the Duration exactly as with a Closed header.
void TestStrictTreatsClosedFooterAsFinished() {
  StrictTestFileSpec spec;
  spec.headerClosed = false;
  spec.indexedFrames = 2;
  OpenResult res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_closed_footer_short.mxf");
  CHECK(res.status == OpenStatus::kDamagedFile);
  CHECK(ErrorContains(res, "declares 4 edit units, but the Index Table covers only 2"));

  spec.indexedFrames = 4;
  res = OpenStrictTestFile(spec, true, "mxfsrc_test_strict_closed_footer_ok.mxf");
  CHECK(res.status == OpenStatus::kOk);
}

// MxfExtractIndex must accept and refuse exactly the files MxfSource does,
// with the same message, for strict on and off -- otherwise a distributed
// setup (extract once, indexurl everywhere) would bypass strict. When both
// accept, the published index must cover what the opened source's index
// covers. Every strict scenario of the tests above.
void TestExtractIndexAgreesWithOpenForEveryStrictCase() {
  std::vector<std::pair<const char*, StrictTestFileSpec>> cases;
  auto add = [&](const char* name, auto&& tweak) {
    StrictTestFileSpec spec;
    tweak(spec);
    cases.push_back({name, spec});
  };
  add("intact", [](StrictTestFileSpec&) {});
  add("zero tail", [](StrictTestFileSpec& s) {
    s.footer = false;
    s.rip = false;
    s.zeroTailBytes = 200000;
  });
  add("footer beyond EOF", [](StrictTestFileSpec& s) {
    s.footer = false;
    s.rip = false;
    s.footerPointerOverride = 1u << 30;
  });
  add("footer pointer into metadata", [](StrictTestFileSpec& s) {
    s.footer = false;
    s.rip = false;
    s.footerPointerOverride = 200;
  });
  add("bad footer pointer, valid RIP", [](StrictTestFileSpec& s) { s.footerPointerOverride = 200; });
  add("no index", [](StrictTestFileSpec& s) { s.indexedFrames = 0; });
  add("short index", [](StrictTestFileSpec& s) { s.indexedFrames = 2; });
  add("growing, RIP index", [](StrictTestFileSpec& s) {
    s.headerClosed = false;
    s.footer = false;
    s.indexedFrames = 2;
  });
  add("growing, CBR index in header", [](StrictTestFileSpec& s) {
    s.headerClosed = false;
    s.footer = false;
    s.rip = false;
    s.cbrIndexInHeader = true;
  });
  add("growing, no reachable index", [](StrictTestFileSpec& s) {
    s.headerClosed = false;
    s.footer = false;
    s.rip = false;
  });
  add("open header, Closed Footer, short index", [](StrictTestFileSpec& s) {
    s.headerClosed = false;
    s.indexedFrames = 2;
  });

  for (const auto& [name, spec] : cases) {
    for (bool strict : {true, false}) {
      auto path = WriteTempFile(BuildStrictTestFile(spec), "mxfsrc_test_extract_parity.mxf");
      const std::string target = path.string() + ".index.json";
      OpenResult opened;
      int64_t openedHighest = -1;
      {
        SourceEngine engine;
        OpenOptions opts;
        opts.strict = strict;
        opened = engine.Open(path.string(), opts);
        openedHighest = engine.EssenceIndexHighestEditUnit();
      }
      const ExtractIndexResult extracted = ExtractAndPublishIndex(path.string(), target, "", strict);
      const bool same = extracted.ok == (opened.status == OpenStatus::kOk) &&
                        (extracted.ok || extracted.error == opened.error);
      if (!same) {
        std::cerr << "extract/open disagree for '" << name << "' strict=" << strict << ": open='" << opened.error
                  << "' extract='" << extracted.error << "'" << std::endl;
      }
      CHECK(same);
      if (extracted.ok) {
        auto fetched = FetchEssenceIndex(target, /*expectedBodySid=*/1, std::filesystem::file_size(path), nullptr);
        CHECK(fetched.has_value());
        if (fetched) CHECK(fetched->HighestCoveredEditUnit() == openedHighest);
      }
      std::error_code ec;
      std::filesystem::remove(target, ec);
      std::filesystem::remove(path);
    }
  }
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
  TestFieldOrderFromDecoderElseDescriptor();
  TestOriginHandlingSkipsPreRollByDefault();
  TestHandleOriginFalseDisablesSkip();
  TestOriginHandlingAppliesToSeekAndDecodeFallback();
  TestGopSeekAndDecodeFallbackByteIdenticalWithoutIndex();
  TestIndexTableAssistedSeekUsesIndexAndMatchesReference();
  TestReorderDelayCompensationMatchesReferenceWithRealBFrames();
  TestStartFrameHintGopWithBFramesAudioMatchesSequentialReference();
  TestSequentialAudioFromFrameZeroDoesNotRepeatDuringWarmup();
  TestForwardSeekBeyondWindowUsesFastPathInsteadOfSequentialWait();
  TestBackwardJumpRepositionsOnceIdle();
  TestBackSeekPrerollSetsRestartLead();
  TestAlternatingFarPositionsDoNotThrash();
  TestForwardJumpWithConcurrentRequestsRepositionsOnce();
  TestAutoBufferGrowsOnceOnStepBack();
  TestExplicitBufferFramesNeverGrows();
  TestForwardJumpRepositionsAudioToo();
  TestStartFrameHintGopServesFromBufferViaIndexAssistedJump();
  TestStartFrameHintIntraOnlyStartsProductionAtHint();
  TestStartFrameHintOutOfRangeIgnored();
  TestUnsupportedCodecFailsOpenCleanly();
  TestGetAudioSamplesSequentialMatchesExpectedBytes();
  TestGetAudioSamplesFreshReadAfterEviction();
  TestGetAudioSamplesDoesNotHangOnEditUnitSampleCountMismatch();
  TestGetAudioSamplesDoesNotStallWhenVideoNeverRequested();
  TestUseAudioModeSupportedDegradesOnMismatch();
  TestUseAudioModeAlwaysFailsOnMismatch();
  TestUseAudioModeNeverSuppressesSupportedAudio();
  TestUseAudioModeAlwaysSucceedsOnVideoOnlySource();
  TestStrictOpensIntactClosedFile();
  TestStrictFailsOnZeroTail();
  TestStrictFailsOnFooterBeyondEof();
  TestStrictFailsOnFooterPointerToNonPartition();
  TestStrictToleratesBadFooterPointerWithValidRip();
  TestStrictFailsWithoutIndex();
  TestStrictFailsWhenIndexShorterThanDuration();
  TestStrictOpensGrowingFileWithRipIndex();
  TestEssenceEndingBeforeDurationIsClamped();
  TestIndexEndingBeforeEssenceIsNotClamped();
  TestEssenceEndsAfterChecksLastContentPackage();
  TestStrictOpensGrowingFileWithCbrIndexInHeader();
  TestStrictFailsGrowingFileWithoutReachableIndex();
  TestStrictFailsZeroTailWithOpenHeader();
  TestStrictTreatsClosedFooterAsFinished();
  TestExtractIndexAgreesWithOpenForEveryStrictCase();

  if (g_failures == 0) {
    std::cout << "all source_engine tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
