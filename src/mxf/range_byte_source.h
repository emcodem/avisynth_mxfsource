#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// IByteSource for a reader that jumps through a file and reads a few small
// pieces at each place (verify=true). The caller announces each range it is
// about to read (Prefetch); the range is fetched with one request into a
// buffer and every read inside it is served from there. A read outside the
// buffer fetches kMissBytes from there, also with one request. Seek and Skip
// only move the position.
//
// Over a network every request costs a round trip, however small, so this
// turns "a few reads per place" into one. The source underneath should not
// buffer itself (FileByteSource with `unbuffered`): a buffered FILE splits
// one read into two requests when its length isn't a multiple of the stdio
// buffer.
class RangeByteSource : public IByteSource {
 public:
  static constexpr size_t kMissBytes = 64 * 1024;

  explicit RangeByteSource(std::unique_ptr<IByteSource> source);

  bool IsOpen() const override { return source_ && source_->IsOpen(); }
  uint64_t Tell() const override { return pos_; }
  bool Seek(uint64_t pos) override {
    pos_ = pos;
    return true;
  }
  uint64_t Size() const override { return source_ ? source_->Size() : 0; }
  bool ReadBytes(void* dst, size_t n) override;
  bool Skip(uint64_t n) override {
    pos_ += n;
    return true;
  }
  bool SupportsRandomAccess() const override { return source_ && source_->SupportsRandomAccess(); }
  std::string LastError() const override { return source_ ? source_->LastError() : std::string(); }
  // Fetches [start, end) (clipped to the file) unless the buffer holds it.
  void Prefetch(uint64_t start, uint64_t end) override;

  // Requests made to the source underneath (for tests and benchmarks).
  uint64_t Requests() const { return requests_; }

 private:
  bool Load(uint64_t start, uint64_t end);

  std::unique_ptr<IByteSource> source_;
  std::vector<uint8_t> buffer_;
  uint64_t bufferStart_ = 0;
  uint64_t pos_ = 0;
  uint64_t requests_ = 0;
};

// The source verify reads `path` through with RangeByteSource: an unbuffered
// FileByteSource for a local or UNC path, an HttpByteSource for a URL.
std::unique_ptr<IByteSource> OpenRangeByteSource(const std::string& path, const std::string& httpHeaders);

}  // namespace mxfsrc
