#pragma once

#include <cstring>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// An IByteSource backed by an in-memory buffer covering
// [baseOffset, baseOffset + data.size()) of a real, larger file whose true
// total size is `realFileSize`. Lets the existing sequential-reader-oriented
// parsing functions (ReadKlvHeader, ReadPartitionPackValue,
// ReadIndexTableSegmentValue, ScanPartitionForIndexSegments, ...) run
// completely unmodified against a partially-downloaded chunk, by reporting
// Size() as the *real* file size (exactly like HttpByteSource does
// regardless of how much is actually buffered) while distinguishing two very
// different kinds of "can't do that" for the caller:
//  - a position/range genuinely at or past the real file's end (a normal,
//    final EOF the existing parsers already handle correctly), vs.
//  - a position/range that's legitimately still inside the real file but
//    hasn't been downloaded into `data` yet.
// Only the second case sets HitBufferLimit() -- the signal index_table.cpp's
// parallel HTTP index-extraction path uses to know "grow the buffer and
// re-run the same parse from scratch" rather than "this is a genuine parse
// failure/EOF, accept the result as final".
class MemoryRangeByteSource : public IByteSource {
 public:
  MemoryRangeByteSource(uint64_t baseOffset, std::vector<uint8_t> data, uint64_t realFileSize)
      : baseOffset_(baseOffset), data_(std::move(data)), realFileSize_(realFileSize), pos_(baseOffset) {}

  bool IsOpen() const override { return true; }
  uint64_t Tell() const override { return pos_; }
  uint64_t Size() const override { return realFileSize_; }
  std::string LastError() const override { return {}; }
  // Never used for the memory-buffer path: EnumeratePartitionsForIndex-style
  // callers only ever get here already knowing random access is possible
  // (that's what made building this buffer via ranged fetches viable in the
  // first place), and nothing reads this flag off a MemoryRangeByteSource.
  bool SupportsRandomAccess() const override { return true; }

  bool Seek(uint64_t pos) override {
    if (pos < baseOffset_) return false;
    if (pos > baseOffset_ + data_.size()) {
      if (pos <= realFileSize_) hitLimit_ = true;
      return false;
    }
    pos_ = pos;
    return true;
  }

  bool ReadBytes(void* dst, size_t n) override {
    if (pos_ < baseOffset_) return false;
    const uint64_t rel = pos_ - baseOffset_;
    if (rel + n > data_.size()) {
      if (pos_ + n <= realFileSize_) hitLimit_ = true;
      return false;
    }
    std::memcpy(dst, data_.data() + rel, n);
    pos_ += n;
    return true;
  }

  bool Skip(uint64_t n) override { return Seek(pos_ + n); }

  // True if any Seek/ReadBytes call this instance ever served was asked for
  // a position/range that's legitimately within the real file but beyond
  // what's currently buffered. Sticky for this instance's lifetime (never
  // cleared) -- callers construct a fresh instance per parse attempt.
  bool HitBufferLimit() const { return hitLimit_; }

 private:
  uint64_t baseOffset_;
  std::vector<uint8_t> data_;
  uint64_t realFileSize_;
  uint64_t pos_;
  bool hitLimit_ = false;
};

}  // namespace mxfsrc
