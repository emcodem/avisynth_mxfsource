#pragma once

#include <cstdio>
#include <string>

#include "mxf/klv.h"

namespace mxfsrc {

// Local-disk IByteSource: today's plain fopen/fseek/fread implementation,
// unchanged in behavior from the original MxfReader.
class FileByteSource : public IByteSource {
 public:
  explicit FileByteSource(const std::string& path);
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

}  // namespace mxfsrc
