// End-to-end tests proving the whole SourceEngine::Open()/GetVideoFrame()
// path works when the "file" is actually an http:// URL served by the local
// MiniHttpServer -- not just HttpByteSource in isolation (see
// http_byte_source_test.cpp). Confirms the header-parse-time seeks and the
// essence-walk-time sequential reads both work correctly through the new
// backend, and that SourceEngine::SeekAndDecode's fresh-fallback-reader path
// carries custom HTTP headers (OpenOptions::http_headers) just like the
// primary reader does.
#include <iostream>
#include <memory>

#include "core/source_engine.h"
#include "mini_http_server.h"
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

void TestSequentialPlaybackOverHttp() {
  const int width = 4, height = 2, numFrames = 5;
  MiniHttpServer server(BuildFile(width, height, numFrames));

  SourceEngine engine;
  OpenResult res = engine.Open(server.BaseUrl() + "/test.mxf");
  CHECK(res.status == OpenStatus::kOk);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open error: " << res.error << std::endl;
    return;
  }
  CHECK(engine.StreamInfo().numFrames == numFrames);

  for (int i = 0; i < numFrames; ++i) {
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    FetchStatus st = engine.GetVideoFrame(i, frame, &err);
    CHECK(st == FetchStatus::kOk);
    if (st == FetchStatus::kOk) {
      CHECK(!frame->planeData[0].empty() && frame->planeData[0][0] == static_cast<uint8_t>(0x10 + i));
    }
  }
}

void TestSeekAndDecodeFallbackCarriesCustomHeaders() {
  const int width = 4, height = 2, numFrames = 20;
  MiniHttpServer server(BuildFile(width, height, numFrames));
  server.SetRequiredHeader("X-Test-Auth", "secret123");

  SourceEngine engine;
  OpenOptions opts;
  opts.buffer_frames = 3;  // small window: forces the eviction/fallback path below
  opts.http_headers = "X-Test-Auth: secret123\r\n";
  OpenResult res = engine.Open(server.BaseUrl() + "/test.mxf", opts);
  CHECK(res.status == OpenStatus::kOk);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open error: " << res.error << std::endl;
    return;
  }

  // Jump to the last frame (evicts the small 3-frame window), then request
  // frame 0 -- this can only succeed if SeekAndDecode's fresh MxfReader also
  // sent the required header; without header propagation, the server would
  // 403 that fresh reader's requests and this would fail.
  std::shared_ptr<NormalizedFrame> frame;
  CHECK(engine.GetVideoFrame(numFrames - 1, frame, nullptr) == FetchStatus::kOk);

  std::string err;
  FetchStatus st = engine.GetVideoFrame(0, frame, &err);
  CHECK(st == FetchStatus::kOk);
  if (st != FetchStatus::kOk) {
    std::cerr << "fallback re-decode over HTTP failed: " << err << std::endl;
  } else {
    CHECK(frame && !frame->planeData[0].empty() && frame->planeData[0][0] == 0x10);
  }
}

void TestMissingHeaderIsRejected() {
  const int width = 4, height = 2, numFrames = 3;
  MiniHttpServer server(BuildFile(width, height, numFrames));
  server.SetRequiredHeader("X-Test-Auth", "secret123");

  SourceEngine engine;
  OpenResult res = engine.Open(server.BaseUrl() + "/test.mxf");  // no http_headers set
  CHECK(res.status != OpenStatus::kOk);
}

}  // namespace

int main() {
  TestSequentialPlaybackOverHttp();
  TestSeekAndDecodeFallbackCarriesCustomHeaders();
  TestMissingHeaderIsRejected();

  if (g_failures == 0) {
    std::cout << "all http_source_engine tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
