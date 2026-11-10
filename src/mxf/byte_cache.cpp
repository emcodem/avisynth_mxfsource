#include "mxf/byte_cache.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "mxf/file_byte_source.h"
#include "net/http_byte_source.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <cstdio>
#endif

namespace mxfsrc {

std::unique_ptr<IByteSource> OpenByteSource(const std::string& path, const std::string& httpHeaders) {
  if (IsHttpUrl(path)) return std::make_unique<HttpByteSource>(path, httpHeaders);
  return std::make_unique<FileByteSource>(path);
}

bool IsNetworkSource(const std::string& path) {
  if (IsHttpUrl(path)) return true;
  auto isSep = [](char c) { return c == '\\' || c == '/'; };
  if (path.size() >= 2 && isSep(path[0]) && isSep(path[1])) {
    // \\?\C:\... and \\.\ are local; \\?\UNC\server\share is not.
    if (path.size() >= 4 && (path[2] == '?' || path[2] == '.') && isSep(path[3])) {
      return path.size() >= 7 && std::toupper(static_cast<unsigned char>(path[4])) == 'U' &&
             std::toupper(static_cast<unsigned char>(path[5])) == 'N' &&
             std::toupper(static_cast<unsigned char>(path[6])) == 'C';
    }
    return true;
  }
#if defined(_WIN32)
  if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
    const char root[] = {path[0], ':', '\\', '\0'};
    return GetDriveTypeA(root) == DRIVE_REMOTE;
  }
#endif
  return false;
}

bool IsLocalSolidState(const std::string& path) {
  if (path.empty() || IsNetworkSource(path)) return false;
#if defined(_WIN32)
  std::wstring wide;
  for (const UINT codePage : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
    const DWORD flags = codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    const int len = MultiByteToWideChar(codePage, flags, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) continue;
    wide.assign(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(codePage, flags, path.data(), static_cast<int>(path.size()), wide.data(), len);
    break;
  }
  if (wide.empty()) return false;
  wchar_t mountPoint[MAX_PATH + 1] = {};
  if (!GetVolumePathNameW(wide.c_str(), mountPoint, MAX_PATH)) return false;
  wchar_t volume[MAX_PATH + 1] = {};
  if (!GetVolumeNameForVolumeMountPointW(mountPoint, volume, MAX_PATH)) return false;
  std::wstring device = volume;  // \\?\Volume{guid}\ -- the volume itself without the backslash
  if (!device.empty() && device.back() == L'\\') device.pop_back();
  const HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                               nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR penalty{};
  DWORD bytes = 0;
  const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &penalty, sizeof(penalty),
                                  &bytes, nullptr);
  CloseHandle(h);
  return ok && bytes >= sizeof(penalty) && !penalty.IncursSeekPenalty;
#elif defined(__linux__)
  // The block device's queue/rotational (a partition has it on its
  // parent disk).
  struct stat st {};
  if (stat(path.c_str(), &st) != 0) return false;
  const std::string dev = "/sys/dev/block/" + std::to_string(major(st.st_dev)) + ":" + std::to_string(minor(st.st_dev));
  for (const char* rel : {"/queue/rotational", "/../queue/rotational"}) {
    if (FILE* f = std::fopen((dev + rel).c_str(), "r")) {
      const int c = std::fgetc(f);
      std::fclose(f);
      return c == '0';
    }
  }
  return false;
#else
  return false;
#endif
}

ByteCache::ByteCache(std::string path, std::string httpHeaders, uint64_t size, bool randomAccess,
                     uint64_t capacityBytes)
    : path_(std::move(path)),
      httpHeaders_(std::move(httpHeaders)),
      size_(size),
      randomAccess_(randomAccess),
      capacity_(capacityBytes) {}

ByteCache::Page ByteCache::Find(uint64_t page) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pages_.find(page);
  if (it == pages_.end()) return nullptr;
  lru_.splice(lru_.begin(), lru_, it->second);
  return it->second->second;
}

bool ByteCache::Contains(uint64_t page) {
  std::lock_guard<std::mutex> lock(mutex_);
  return pages_.count(page) != 0;
}

void ByteCache::Insert(uint64_t page, Page data) {
  const uint64_t added = data->size();
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = pages_.find(page);
  if (it != pages_.end()) {  // another reader fetched it meanwhile; same bytes
    lru_.splice(lru_.begin(), lru_, it->second);
    return;
  }
  lru_.emplace_front(page, std::move(data));
  pages_[page] = lru_.begin();
  bytes_ += added;
  constexpr size_t kMaxSpare = 32;
  while (bytes_ > capacity_ && !lru_.empty()) {
    Page& evicted = lru_.back().second;
    bytes_ -= evicted->size();
    // Created non-const by AllocatePage; only the cache's own reference left.
    if (evicted.use_count() == 1 && spare_.size() < kMaxSpare && evicted->size() == kPageSize) {
      spare_.push_back(std::const_pointer_cast<std::vector<uint8_t>>(std::move(evicted)));
    }
    pages_.erase(lru_.back().first);
    lru_.pop_back();
  }
}

std::shared_ptr<std::vector<uint8_t>> ByteCache::AllocatePage() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!spare_.empty()) {
      auto page = std::move(spare_.back());
      spare_.pop_back();
      return page;
    }
  }
  return std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(kPageSize));
}

std::unique_ptr<IByteSource> ByteCache::OpenSource() const {
  if (IsHttpUrl(path_)) return std::make_unique<HttpByteSource>(path_, httpHeaders_, size_, randomAccess_);
  return std::make_unique<FileByteSource>(path_);
}

CachingByteSource::CachingByteSource(std::shared_ptr<ByteCache> cache, std::unique_ptr<IByteSource> source)
    : cache_(std::move(cache)), source_(std::move(source)), fullPages_(cache_->Size() / ByteCache::kPageSize) {}

bool CachingByteSource::EnsureSource() {
  if (!source_) {
    source_ = cache_->OpenSource();
    sourceEnd_ = UINT64_MAX;
  }
  if (!source_ || !source_->IsOpen()) {
    if (!source_) lastError_ = "failed to open " + cache_->Path();
    return false;
  }
  return true;
}

bool CachingByteSource::ReadBytes(void* dst, size_t n) {
  constexpr uint64_t kPage = ByteCache::kPageSize;
  auto* out = static_cast<uint8_t*>(dst);
  cache_->CountServed(n);
  while (n > 0) {
    const uint64_t page = pos_ / kPage;
    if (page >= fullPages_) return ReadDirect(out, n);
    const uint64_t offset = pos_ % kPage;
    if (currentIndex_ != page) {
      current_ = cache_->Find(page);
      currentIndex_ = current_ ? page : UINT64_MAX;
      if (!current_ && !Fill(page, offset + n)) return false;
    }
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(kPage - offset, n));
    std::memcpy(out, current_->data() + offset, chunk);
    out += chunk;
    n -= chunk;
    pos_ += chunk;
  }
  return true;
}

bool CachingByteSource::Fill(uint64_t page, uint64_t bytesFromPageStart) {
  constexpr uint64_t kPage = ByteCache::kPageSize;
  // Sequential reading (the decoder playing on) fetches 1 MiB at a time; the
  // source underneath streams anyway, this only sets how often we ask it.
  constexpr uint64_t kSequentialPages = 16;
  // How far ahead to look for cached data that bounds a jump's request.
  constexpr uint64_t kBoundScanPages = 1024;  // 64 MiB

  if (!EnsureSource()) return false;
  const uint64_t start = page * kPage;
  uint64_t want = std::max<uint64_t>(1, (bytesFromPageStart + kPage - 1) / kPage);
  if (sourceEnd_ == start) want = std::max(want, kSequentialPages);
  want = std::min(want, fullPages_ - page);

  uint64_t count = 1;
  while (count < want && !cache_->Contains(page + count)) ++count;

  if (sourceEnd_ != start) {
    uint64_t endHint = 0;
    if (count < want) {
      endHint = (page + count) * kPage;  // stopped at a cached page
    } else {
      const uint64_t scanEnd = std::min(fullPages_, page + kBoundScanPages);
      for (uint64_t p = page + count; p < scanEnd; ++p) {
        if (cache_->Contains(p)) {
          endHint = p * kPage;
          break;
        }
      }
    }
    const bool positioned = endHint != 0 ? source_->SeekForRange(start, endHint) : source_->Seek(start);
    if (!positioned) {
      sourceEnd_ = UINT64_MAX;
      return false;
    }
  }

  ByteCache::Page first;
  for (uint64_t i = 0; i < count; ++i) {
    auto data = cache_->AllocatePage();
    if (!source_->ReadBytes(data->data(), data->size())) {
      sourceEnd_ = UINT64_MAX;
      return false;
    }
    if (i == 0) first = data;
    cache_->Insert(page + i, std::move(data));
  }
  cache_->CountFetched(count * kPage);
  sourceEnd_ = (page + count) * kPage;
  current_ = std::move(first);
  currentIndex_ = page;
  return true;
}

bool CachingByteSource::ReadDirect(uint8_t* out, size_t n) {
  if (!EnsureSource()) return false;
  if (source_->Tell() != pos_ && !source_->Seek(pos_)) return false;
  sourceEnd_ = UINT64_MAX;
  while (n > 0) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(n, ByteCache::kPageSize));
    if (!source_->ReadBytes(out, chunk)) return false;
    cache_->CountFetched(chunk);
    out += chunk;
    n -= chunk;
    pos_ += chunk;
  }
  return true;
}

}  // namespace mxfsrc
