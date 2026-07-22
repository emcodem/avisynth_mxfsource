#include "mxf/file_byte_source.h"

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

FileByteSource::FileByteSource(const std::string& path) {
  file_ = fopen(path.c_str(), "rb");
  if (file_) {
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
  return MXFSRC_FSEEK(file_, static_cast<mxfsrc_off_t>(n), SEEK_CUR) == 0;
}

}  // namespace mxfsrc
