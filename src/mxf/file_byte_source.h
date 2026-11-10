#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Local-disk IByteSource: today's plain fopen/fseek/fread implementation,
// unchanged in behavior from the original MxfReader.
class FileByteSource : public IByteSource {
 public:
  // `unbuffered`: no stdio buffer, each read is one read from the file. For
  // a caller that reads in large pieces itself (RangeByteSource,
  // StreamingFileByteSource).
  explicit FileByteSource(const std::string& path, bool unbuffered = false);
  ~FileByteSource() override;
  FileByteSource(const FileByteSource&) = delete;
  FileByteSource& operator=(const FileByteSource&) = delete;

  bool IsOpen() const override { return file_ != nullptr; }
  uint64_t Tell() const override;
  bool Seek(uint64_t pos) override;
  uint64_t Size() const override { return size_; }
  bool ReadBytes(void* dst, size_t n) override;
  bool Skip(uint64_t n) override;
  std::string LastError() const override { return {}; }

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

// Local-file IByteSource for one reader that moves forward through the file
// with small reads and gaps between them (verify=true checks a few bytes per
// edit unit). Reads the file in large contiguous blocks and reads on through
// a gap instead of seeking over it, so a hard disk streams instead of
// seeking for every few bytes. A read before the current block, or more than
// one block beyond it, starts a new block there. Reads past the size known
// at open go straight to the file.
class StreamingFileByteSource : public IByteSource {
 public:
  static constexpr size_t kDefaultBlockBytes = 8 * 1024 * 1024;

  explicit StreamingFileByteSource(const std::string& path, size_t blockBytes = kDefaultBlockBytes);

  bool IsOpen() const override { return file_.IsOpen(); }
  uint64_t Tell() const override { return pos_; }
  bool Seek(uint64_t pos) override {
    pos_ = pos;
    return true;
  }
  uint64_t Size() const override { return file_.Size(); }
  bool ReadBytes(void* dst, size_t n) override;
  bool Skip(uint64_t n) override {
    pos_ += n;
    return true;
  }
  std::string LastError() const override { return {}; }

  // Blocks read from the file so far (for tests and benchmarks).
  uint64_t BlocksRead() const { return blocksRead_; }

 private:
  bool Load(uint64_t pos);

  FileByteSource file_;
  std::vector<uint8_t> block_;
  uint64_t blockStart_ = 0;
  size_t blockLen_ = 0;
  uint64_t filePos_ = 0;  // where file_ is positioned
  uint64_t pos_ = 0;
  uint64_t blocksRead_ = 0;
};

}  // namespace mxfsrc
