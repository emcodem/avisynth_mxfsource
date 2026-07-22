// Tests for HttpByteSource (src/net/http_byte_source.h) against a real
// loopback HTTP server (mini_http_server.h) -- no live network access
// required. Covers the behaviors called out in the HTTP/HTTPS + S3
// presigned URL support plan: Size() via ranged GET (both a range-capable
// and a range-ignoring server), sequential reads, small forward seeks
// (discard-based), large forward/backward seeks (new-range-request-based),
// a simulated 403 (the S3-presigned-URL-expired case), and a dropped
// connection recovered via automatic retry.
#include <iostream>
#include <vector>

#include "mini_http_server.h"
#include "net/http_byte_source.h"

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

std::vector<uint8_t> MakePattern(size_t n) {
  std::vector<uint8_t> data(n);
  for (size_t i = 0; i < n; ++i) data[i] = static_cast<uint8_t>(i % 256);
  return data;
}

bool MatchesPattern(const uint8_t* buf, size_t n, size_t startOffset) {
  for (size_t i = 0; i < n; ++i) {
    if (buf[i] != static_cast<uint8_t>((startOffset + i) % 256)) return false;
  }
  return true;
}

void TestSizeAndSequentialRead() {
  auto pattern = MakePattern(1'000'000);
  MiniHttpServer server(pattern);

  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());
  CHECK(src.Size() == pattern.size());

  std::vector<uint8_t> buf(1000);
  CHECK(src.ReadBytes(buf.data(), buf.size()));
  CHECK(MatchesPattern(buf.data(), buf.size(), 0));
  CHECK(src.Tell() == 1000);
}

void TestSmallForwardSeekDiscard() {
  auto pattern = MakePattern(1'000'000);
  MiniHttpServer server(pattern);
  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());

  CHECK(src.Seek(5000));  // well under the small-seek threshold
  std::vector<uint8_t> buf(256);
  CHECK(src.ReadBytes(buf.data(), buf.size()));
  CHECK(MatchesPattern(buf.data(), buf.size(), 5000));
}

void TestLargeForwardSeekRestartsTransfer() {
  auto pattern = MakePattern(1'000'000);
  MiniHttpServer server(pattern);
  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());

  CHECK(src.Seek(500'000));  // exceeds the small-seek discard threshold
  std::vector<uint8_t> buf(256);
  CHECK(src.ReadBytes(buf.data(), buf.size()));
  CHECK(MatchesPattern(buf.data(), buf.size(), 500'000));
  CHECK(src.Tell() == 500'000 + buf.size());
}

void TestBackwardSeek() {
  auto pattern = MakePattern(1'000'000);
  MiniHttpServer server(pattern);
  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());

  CHECK(src.Seek(800'000));
  std::vector<uint8_t> buf(64);
  CHECK(src.ReadBytes(buf.data(), buf.size()));

  CHECK(src.Seek(10));  // backward
  CHECK(src.ReadBytes(buf.data(), buf.size()));
  CHECK(MatchesPattern(buf.data(), buf.size(), 10));
}

void TestNonRangeCapableServerFallback() {
  auto pattern = MakePattern(1'000'000);
  MiniHttpServer server(pattern);
  server.SetRangeSupport(false);

  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());
  CHECK(src.Size() == pattern.size());  // determined via Content-Length fallback

  CHECK(src.Seek(400'000));  // no range support -> forced discard, regardless of distance
  std::vector<uint8_t> buf(64);
  CHECK(src.ReadBytes(buf.data(), buf.size()));
  CHECK(MatchesPattern(buf.data(), buf.size(), 400'000));
}

void TestForced403SurfacesExpiredHint() {
  auto pattern = MakePattern(1000);
  MiniHttpServer server(pattern);
  server.SetForceStatus(403);

  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(!src.IsOpen());
  CHECK(src.LastError().find("expired") != std::string::npos);
}

void TestDroppedConnectionRecoversViaRetry() {
  auto pattern = MakePattern(1000);
  MiniHttpServer server(pattern);
  server.SetDropFirstNConnections(1);

  HttpByteSource src(server.BaseUrl() + "/test.mxf", "");
  CHECK(src.IsOpen());  // first connection dropped, retry succeeds
  CHECK(src.Size() == pattern.size());
  CHECK(server.RequestCount() >= 1);
}

}  // namespace

int main() {
  TestSizeAndSequentialRead();
  TestSmallForwardSeekDiscard();
  TestLargeForwardSeekRestartsTransfer();
  TestBackwardSeek();
  TestNonRangeCapableServerFallback();
  TestForced403SurfacesExpiredHint();
  TestDroppedConnectionRecoversViaRetry();

  if (g_failures == 0) {
    std::cout << "all http_byte_source tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
