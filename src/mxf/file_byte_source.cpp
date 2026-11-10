#include "mxf/file_byte_source.h"

#include <algorithm>
#include <cstring>

#include "mxf/file_open.h"

#if defined(_WIN32)
#define MXFSRC_FSEEK _fseeki64
#define MXFSRC_FTELL _ftelli64
using mxfsrc_off_t = int64_t;
#else
#define MXFSRC_FSEEK fseeko
#define MXFSRC_FTELL ftello
using mxfsrc_off_t = off_t;
#endif

namespace mxfsrc {

FileByteSource::FileByteSource(const std::string& path, bool unbuffered) {
  file_ = OpenFile(path, "rb");
  if (file_) {
    if (unbuffered) setvbuf(file_, nullptr, _IONBF, 0);
    MXFSRC_FSEEK(file_, 0, SEEK_END);
    mxfsrc_off_t end = MXFSRC_FTELL(file_);
    size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
    MXFSRC_FSEEK(file_, 0, SEEK_SET);
  }
}

FileByteSource::~FileByteSource() {
  if (file_) fclose(file_);
}

uint64_t FileByteSource::Tell() const {
  if (!file_) return 0;
  mxfsrc_off_t pos = MXFSRC_FTELL(file_);
  return pos > 0 ? static_cast<uint64_t>(pos) : 0;
}

bool FileByteSource::Seek(uint64_t pos) {
  if (!file_) return false;
  return MXFSRC_FSEEK(file_, static_cast<mxfsrc_off_t>(pos), SEEK_SET) == 0;
}

bool FileByteSource::ReadBytes(void* dst, size_t n) {
  if (!file_ || n == 0) return n == 0;
  return fread(dst, 1, n, file_) == n;
}

bool FileByteSource::Skip(uint64_t n) {
  if (!file_) return false;
  // Small forward skips are read-and-discarded rather than fseek'd past --
  // unlike glibc (which can satisfy an in-buffer seek with no syscall at
  // all), the Windows CRT's fseek unconditionally discards its stdio buffer
  // and forces a fresh underlying read on the very next fread, regardless of
  // how close the target is. EssenceWalker calls this for every KLV it isn't
  // interested in (System Items, audio elements, Index Table Segments, KLV
  // Fill) -- one or more per essence element on a typical frame-wrapped
  // OP1a file -- so each one was forcing an extra real read (an extra
  // network round trip, for a file opened over an SMB share) that plain
  // sequential fread'ing through the same bytes wouldn't. Large skips still
  // use a real seek: reading megabytes just to throw them away would cost
  // far more (real bytes actually transferred over the wire) than the round
  // trip it avoids.
  constexpr uint64_t kMaxDiscardRead = 64 * 1024;
  if (n <= kMaxDiscardRead) {
    uint8_t discard[4096];
    uint64_t remaining = n;
    while (remaining > 0) {
      const size_t chunk = remaining < sizeof(discard) ? static_cast<size_t>(remaining) : sizeof(discard);
      if (fread(discard, 1, chunk, file_) != chunk) return false;
      remaining -= chunk;
    }
    return true;
  }
  return MXFSRC_FSEEK(file_, static_cast<mxfsrc_off_t>(n), SEEK_CUR) == 0;
}

StreamingFileByteSource::StreamingFileByteSource(const std::string& path, size_t blockBytes)
    : file_(path, /*unbuffered=*/true), block_(blockBytes > 0 ? blockBytes : 1) {}

bool StreamingFileByteSource::Load(uint64_t pos) {
  // Just past the current block and within one more block: read on from the
  // block's end, so the file is read contiguously without a seek.
  const uint64_t blockEnd = blockStart_ + blockLen_;
  const uint64_t start = (blockLen_ > 0 && pos >= blockEnd && pos - blockEnd < block_.size()) ? blockEnd : pos;
  const uint64_t size = file_.Size();
  if (start >= size) return false;
  const size_t len = static_cast<size_t>(std::min<uint64_t>(block_.size(), size - start));
  if (filePos_ != start && !file_.Seek(start)) {
    filePos_ = UINT64_MAX;
    return false;
  }
  if (!file_.ReadBytes(block_.data(), len)) {
    filePos_ = UINT64_MAX;
    blockLen_ = 0;
    return false;
  }
  filePos_ = start + len;
  blockStart_ = start;
  blockLen_ = len;
  ++blocksRead_;
  return true;
}

bool StreamingFileByteSource::ReadBytes(void* dst, size_t n) {
  if (!file_.IsOpen()) return n == 0;
  auto* out = static_cast<uint8_t*>(dst);
  if (pos_ + n > file_.Size()) {
    // Past the size known at open (a growing file): straight from the file.
    if (!file_.Seek(pos_) || !file_.ReadBytes(out, n)) {
      filePos_ = UINT64_MAX;
      return false;
    }
    pos_ += n;
    filePos_ = pos_;
    return true;
  }
  while (n > 0) {
    if (pos_ < blockStart_ || pos_ >= blockStart_ + blockLen_) {
      if (!Load(pos_)) return false;
    }
    const size_t offset = static_cast<size_t>(pos_ - blockStart_);
    const size_t chunk = std::min(n, blockLen_ - offset);
    std::memcpy(out, block_.data() + offset, chunk);
    out += chunk;
    n -= chunk;
    pos_ += chunk;
  }
  return true;
}

}  // namespace mxfsrc
