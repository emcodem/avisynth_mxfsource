#include "mxf/range_byte_source.h"

#include <algorithm>
#include <cstring>

#include "mxf/file_byte_source.h"
#include "net/http_byte_source.h"

namespace mxfsrc {

RangeByteSource::RangeByteSource(std::unique_ptr<IByteSource> source) : source_(std::move(source)) {}

bool RangeByteSource::Load(uint64_t start, uint64_t end) {
  end = std::min(end, Size());
  if (start >= end) return false;
  const size_t len = static_cast<size_t>(end - start);
  buffer_.resize(len);
  ++requests_;
  if (!source_->SeekForRange(start, end) || !source_->ReadBytes(buffer_.data(), len)) {
    buffer_.clear();
    return false;
  }
  bufferStart_ = start;
  return true;
}

void RangeByteSource::Prefetch(uint64_t start, uint64_t end) {
  end = std::min(end, Size());
  if (start >= end) return;
  if (start >= bufferStart_ && end <= bufferStart_ + buffer_.size()) return;
  Load(start, end);  // a failure shows up on the read that needs the bytes
}

bool RangeByteSource::ReadBytes(void* dst, size_t n) {
  if (!IsOpen()) return n == 0;
  auto* out = static_cast<uint8_t*>(dst);
  if (pos_ + n > Size()) {
    // Past the size known at open (a growing file): straight from the source.
    ++requests_;
    if (!source_->Seek(pos_) || !source_->ReadBytes(out, n)) return false;
    pos_ += n;
    return true;
  }
  while (n > 0) {
    if (pos_ < bufferStart_ || pos_ >= bufferStart_ + buffer_.size()) {
      if (!Load(pos_, pos_ + std::max<uint64_t>(kMissBytes, n))) return false;
    }
    const size_t offset = static_cast<size_t>(pos_ - bufferStart_);
    const size_t chunk = std::min(n, buffer_.size() - offset);
    std::memcpy(out, buffer_.data() + offset, chunk);
    out += chunk;
    n -= chunk;
    pos_ += chunk;
  }
  return true;
}

std::unique_ptr<IByteSource> OpenRangeByteSource(const std::string& path, const std::string& httpHeaders) {
  if (IsHttpUrl(path)) return std::make_unique<HttpByteSource>(path, httpHeaders);
  return std::make_unique<FileByteSource>(path, /*unbuffered=*/true);
}

}  // namespace mxfsrc
