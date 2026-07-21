// Phase 4 unit tests for the ring-buffer decode engine (src/core/source_engine.*).
// Builds a small synthetic OP1a file with real (uncompressed UYVY) essence
// so the whole pipeline -- header parse, essence walk, decode, ring buffer,
// background thread -- runs end to end without needing a real sample file.
#include <filesystem>
#include <iostream>

#include "core/source_engine.h"
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
      NormalizedFrame frame;
      std::string err;
      FetchStatus st = engine.GetVideoFrame(i, frame, &err);
      CHECK(st == FetchStatus::kOk);
      CHECK(frame.format == PixelFormat::kUyvy422);
      CHECK(frame.width == width && frame.height == height);
      CHECK(!frame.planeData[0].empty());
      if (!frame.planeData[0].empty()) {
        CHECK(frame.planeData[0][0] == static_cast<uint8_t>(0x10 + i));  // forward file order preserved
      }
    }

    // Re-requesting an already-produced frame should hit the buffer, not re-decode.
    NormalizedFrame again;
    CHECK(engine.GetVideoFrame(2, again, nullptr) == FetchStatus::kOk);
    CHECK(!again.planeData[0].empty() && again.planeData[0][0] == 0x12);

    // Requesting past the declared frame count reports end-of-stream, not a hang/crash.
    NormalizedFrame beyond;
    CHECK(engine.GetVideoFrame(numFrames, beyond, nullptr) == FetchStatus::kEndOfStream);
  }

  std::filesystem::remove(path);
}

void TestEvictionWhenBufferTooSmall() {
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
    // requested -- exactly the scenario the plan calls out as a hard error
    // rather than a silent reseek.
    NormalizedFrame frame;
    CHECK(engine.GetVideoFrame(numFrames - 1, frame, nullptr) == FetchStatus::kOk);

    std::string err;
    FetchStatus st = engine.GetVideoFrame(0, frame, &err);
    CHECK(st == FetchStatus::kEvicted);
    CHECK(!err.empty());
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

    NormalizedFrame frame;
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

    NormalizedFrame frame;
    std::string err;
    CHECK(engine.GetVideoFrame(0, frame, &err) == FetchStatus::kOk);
    FetchStatus second = engine.GetVideoFrame(0, frame, &err);
    CHECK(second == FetchStatus::kOk);
    if (second != FetchStatus::kOk) std::cerr << "second request for frame 0: " << err << std::endl;
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

}  // namespace

int main() {
  TestSequentialPlayback();
  TestEvictionWhenBufferTooSmall();
  TestOpenPeekDoesNotEvictFrameZero();
  TestRepeatedRequestForSameFrameSucceeds();
  TestUnsupportedCodecFailsOpenCleanly();

  if (g_failures == 0) {
    std::cout << "all source_engine tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
