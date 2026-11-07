#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Opens the plain byte source for `path`: HttpByteSource for an http(s)://
// URL, FileByteSource otherwise (local and UNC paths).
std::unique_ptr<IByteSource> OpenByteSource(const std::string& path, const std::string& httpHeaders);

// True for a path whose reads go over the network: http(s)://, a UNC path
// (\\server\share, //server/share) or, on Windows, a mapped network drive.
bool IsNetworkSource(const std::string& path);

// Read cache for one source file, shared by every reader SourceEngine opens
// on it: the background decoder's, each restart's, and the one-off readers
// for separately decoded frames and audio blocks. Holds the file in 64 KiB
// pages, least recently used dropped first, up to a byte budget.
//
// Without it, every jump read the same bytes again from the source: a
// decoder restart re-reads the GOP up to its target, each separately decoded
// frame re-reads its whole GOP over a new connection. Over HTTP that cost
// several times the file's own bitrate during backward stepping (measured:
// ~2.3 MB transferred per frame of a ~0.3 MB/frame XDCAM HD file).
//
// Thread-safe. Pages are immutable once inserted; readers keep a shared_ptr
// to the page they are reading, so eviction never pulls data out from under
// them.
class ByteCache {
 public:
  static constexpr uint64_t kPageSize = 64 * 1024;
  using Page = std::shared_ptr<const std::vector<uint8_t>>;

  ByteCache(std::string path, std::string httpHeaders, uint64_t size, bool randomAccess, uint64_t capacityBytes);

  const std::string& Path() const { return path_; }
  uint64_t Size() const { return size_; }
  bool RandomAccess() const { return randomAccess_; }
  uint64_t CapacityBytes() const { return capacity_; }

  // The page, marked most recently used; nullptr if not cached.
  Page Find(uint64_t page);
  bool Contains(uint64_t page);
  void Insert(uint64_t page, Page data);
  // A page-sized buffer for the next fill: an evicted page nobody holds any
  // more if there is one (saves allocating and zeroing 64 KiB per page while
  // streaming through the file), a new one otherwise.
  std::shared_ptr<std::vector<uint8_t>> AllocatePage();

  // A further plain source for another reader of the same file. For HTTP it
  // reuses the size and range support found at open (no probe request).
  std::unique_ptr<IByteSource> OpenSource() const;

  // Bytes read from the source into the cache, and bytes readers got from
  // it (both counted by CachingByteSource).
  void CountFetched(uint64_t n) { fetchedBytes_ += n; }
  void CountServed(uint64_t n) { servedBytes_ += n; }
  uint64_t FetchedBytes() const { return fetchedBytes_; }
  uint64_t ServedBytes() const { return servedBytes_; }

 private:
  using Lru = std::list<std::pair<uint64_t, Page>>;

  const std::string path_;
  const std::string httpHeaders_;
  const uint64_t size_;
  const bool randomAccess_;
  const uint64_t capacity_;

  std::mutex mutex_;
  Lru lru_;  // front = most recently used
  std::unordered_map<uint64_t, Lru::iterator> pages_;
  uint64_t bytes_ = 0;
  std::vector<std::shared_ptr<std::vector<uint8_t>>> spare_;

  std::atomic<uint64_t> fetchedBytes_{0};
  std::atomic<uint64_t> servedBytes_{0};
};

// IByteSource that reads through a ByteCache. One per reader, used by one
// thread at a time like any other byte source. The plain source underneath
// is opened on the first cache miss (`source` may be null), so a reader
// whose reads all hit the cache never opens a connection.
//
// On a miss it reads the missing pages from the source: just the pages the
// read needs after a jump, 1 MiB ahead while reading on sequentially, never
// past the next page already cached. After a jump it tells the source where
// the next cached data starts (SeekForRange), so an HTTP source requests
// only the gap instead of the rest of the file.
//
// The partial last page of the file is never cached: it is read straight
// from the source, so a file still being written can grow past it.
class CachingByteSource : public IByteSource {
 public:
  CachingByteSource(std::shared_ptr<ByteCache> cache, std::unique_ptr<IByteSource> source);

  bool IsOpen() const override { return !source_ || source_->IsOpen(); }
  uint64_t Tell() const override { return pos_; }
  bool Seek(uint64_t pos) override {
    pos_ = pos;
    return true;
  }
  uint64_t Size() const override { return cache_->Size(); }
  bool ReadBytes(void* dst, size_t n) override;
  bool Skip(uint64_t n) override {
    pos_ += n;
    return true;
  }
  bool SupportsRandomAccess() const override { return cache_->RandomAccess(); }
  std::string LastError() const override { return source_ ? source_->LastError() : lastError_; }

 private:
  bool EnsureSource();
  // Reads the missing pages starting at `page` (the read needs `bytesFromPageStart`
  // bytes from that page's start) into the cache and makes `page` current.
  bool Fill(uint64_t page, uint64_t bytesFromPageStart);
  // Reads straight from the source, bypassing the cache (the file's tail).
  bool ReadDirect(uint8_t* out, size_t n);

  std::shared_ptr<ByteCache> cache_;
  std::unique_ptr<IByteSource> source_;
  std::string lastError_;
  uint64_t pos_ = 0;
  uint64_t fullPages_;  // pages entirely within the file size known at open
  // Where this reader's own source is positioned after its last fill; a
  // miss exactly there continues a sequential read.
  uint64_t sourceEnd_ = UINT64_MAX;
  uint64_t currentIndex_ = UINT64_MAX;
  ByteCache::Page current_;
};

}  // namespace mxfsrc
