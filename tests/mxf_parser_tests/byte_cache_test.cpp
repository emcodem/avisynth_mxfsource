// Tests for ByteCache / CachingByteSource (src/mxf/byte_cache.h): every read
// returns the source's bytes whatever the access pattern and cache size,
// readers share what any of them read, and a reader that only hits the cache
// never opens the source.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

#include "mini_http_server.h"
#include "mxf/byte_cache.h"
#include "mxf/file_byte_source.h"
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

constexpr uint64_t kPage = ByteCache::kPageSize;

std::vector<uint8_t> MakeData(size_t n) {
  std::vector<uint8_t> data(n);
  uint32_t x = 12345;
  for (auto& b : data) {
    x = x * 1103515245u + 12345u;
    b = static_cast<uint8_t>(x >> 16);
  }
  return data;
}

std::filesystem::path WriteTemp(const std::vector<uint8_t>& data, const char* name) {
  auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                              static_cast<std::streamsize>(data.size()));
  return path;
}

bool ReadMatches(IByteSource& src, const std::vector<uint8_t>& data, uint64_t pos, size_t n) {
  std::vector<uint8_t> buf(n);
  if (!src.Seek(pos) || !src.ReadBytes(buf.data(), n)) return false;
  return std::equal(buf.begin(), buf.end(), data.begin() + static_cast<std::ptrdiff_t>(pos)) &&
         src.Tell() == pos + n;
}

// Random reads of random sizes, a cache from a few pages to the whole file:
// always the file's bytes, also in the partial last page, and a read past
// the end fails.
void TestRandomReadsMatchFile() {
  const auto data = MakeData(static_cast<size_t>(20 * kPage + 12345));  // partial last page
  const auto path = WriteTemp(data, "mxfsrc_test_byte_cache.bin");
  std::mt19937 rng(7);
  for (uint64_t capacityPages : {1u, 3u, 8u, 64u}) {
    auto cache = std::make_shared<ByteCache>(path.string(), "", data.size(), true, capacityPages * kPage);
    CachingByteSource src(cache, std::make_unique<FileByteSource>(path.string()));
    CHECK(src.IsOpen());
    CHECK(src.Size() == data.size());
    for (int i = 0; i < 2000; ++i) {
      const uint64_t pos = rng() % data.size();
      const size_t maxLen = static_cast<size_t>(data.size() - pos);
      const size_t len = std::min<size_t>(maxLen, 1 + rng() % (i % 10 == 0 ? 3 * kPage : 300));
      if (!ReadMatches(src, data, pos, len)) {
        CHECK(false);
        break;
      }
    }
    // Sequential reads in KLV-sized pieces, the decoder's pattern.
    CHECK(src.Seek(0));
    std::vector<uint8_t> all(data.size());
    size_t at = 0;
    while (at < all.size()) {
      const size_t len = std::min<size_t>(all.size() - at, 1 + (at * 7) % 5000);
      CHECK(src.ReadBytes(all.data() + at, len));
      at += len;
    }
    CHECK(all == data);
    uint8_t byte = 0;
    CHECK(!src.ReadBytes(&byte, 1));  // past the end
    CHECK(src.Seek(data.size() - 10));
    std::vector<uint8_t> tail(11);
    CHECK(!src.ReadBytes(tail.data(), tail.size()));
  }
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// A second reader (no source of its own) reads what the first already read
// from the cache: nothing more is fetched.
void TestReadersShareTheCache() {
  const auto data = MakeData(static_cast<size_t>(10 * kPage));
  const auto path = WriteTemp(data, "mxfsrc_test_byte_cache_share.bin");
  auto cache = std::make_shared<ByteCache>(path.string(), "", data.size(), true, 64 * kPage);
  CachingByteSource first(cache, std::make_unique<FileByteSource>(path.string()));
  CHECK(ReadMatches(first, data, 2 * kPage + 100, static_cast<size_t>(4 * kPage)));
  const uint64_t fetched = cache->FetchedBytes();
  CHECK(fetched >= 4 * kPage && fetched <= 6 * kPage);

  CachingByteSource second(cache, nullptr);
  CHECK(ReadMatches(second, data, 3 * kPage, static_cast<size_t>(2 * kPage)));
  CHECK(cache->FetchedBytes() == fetched);
  CHECK(ReadMatches(second, data, 8 * kPage, 100));  // a miss opens its own source
  CHECK(cache->FetchedBytes() > fetched);
  std::error_code ec;  // readers still hold the file open on Windows
  std::filesystem::remove(path, ec);
}

// Over HTTP: a reader whose reads all hit the cache sends no request, and
// stepping backwards through the file (Reverse) fetches each byte once, with
// each jump's request bounded by the data cached above it.
void TestHttpBackwardStepsFetchEachByteOnce() {
  const auto data = MakeData(static_cast<size_t>(40 * kPage));
  MiniHttpServer server(data);
  const std::string url = server.BaseUrl() + "/test.mxf";
  auto main = std::make_unique<HttpByteSource>(url, "");
  CHECK(main->IsOpen());
  auto cache = std::make_shared<ByteCache>(url, "", main->Size(), main->SupportsRandomAccess(), 64 * kPage);
  CachingByteSource src(cache, std::move(main));

  const uint64_t block = 3 * kPage;
  for (uint64_t end = data.size(); end > 0;) {
    const uint64_t start = end > block ? end - block : 0;
    // A restart's walk: forward from `start` through the block and on into
    // what the previous step read.
    const uint64_t readEnd = std::min<uint64_t>(data.size(), end + kPage / 2);
    if (!ReadMatches(src, data, start, static_cast<size_t>(readEnd - start))) {
      CHECK(false);
      break;
    }
    end = start;
  }
  CHECK(cache->FetchedBytes() == data.size());

  const int requests = server.RequestCount();
  CachingByteSource other(cache, nullptr);
  CHECK(ReadMatches(other, data, 5 * kPage + 7, static_cast<size_t>(10 * kPage)));
  CHECK(server.RequestCount() == requests);
}

void TestIsNetworkSource() {
  CHECK(IsNetworkSource("http://host/a.mxf"));
  CHECK(IsNetworkSource("HTTPS://host/a.mxf"));
  CHECK(IsNetworkSource("\\\\server\\share\\a.mxf"));
  CHECK(IsNetworkSource("//server/share/a.mxf"));
  CHECK(IsNetworkSource("\\\\?\\UNC\\server\\share\\a.mxf"));
  CHECK(!IsNetworkSource("\\\\?\\C:\\a.mxf"));
  CHECK(!IsNetworkSource("a.mxf"));
  CHECK(!IsNetworkSource("/tmp/a.mxf"));
}

}  // namespace

int main() {
  TestRandomReadsMatchFile();
  TestReadersShareTheCache();
  TestHttpBackwardStepsFetchEachByteOnce();
  TestIsNetworkSource();

  if (g_failures == 0) {
    std::cout << "all byte_cache tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
