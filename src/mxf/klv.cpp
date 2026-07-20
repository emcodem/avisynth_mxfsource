#include "mxf/klv.h"

#include <cstring>

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

bool UlPrefixEquals(const Ul& key, const uint8_t* prefix, size_t prefixLen) {
  if (prefixLen > key.size()) return false;
  return std::memcmp(key.data(), prefix, prefixLen) == 0;
}

bool UlEquals(const Ul& a, const Ul& b) { return a == b; }

MxfReader::MxfReader(const std::string& path) {
#if defined(_WIN32)
  file_ = fopen(path.c_str(), "rb");
#else
  file_ = fopen(path.c_str(), "rb");
#endif
  if (file_) {
    MXFSRC_FSEEK(file_, 0, SEEK_END);
    mxfsrc_off_t end = MXFSRC_FTELL(file_);
    size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
    MXFSRC_FSEEK(file_, 0, SEEK_SET);
  }
}

MxfReader::~MxfReader() {
  if (file_) fclose(file_);
}

uint64_t MxfReader::Tell() const {
  if (!file_) return 0;
  mxfsrc_off_t pos = MXFSRC_FTELL(file_);
  return pos > 0 ? static_cast<uint64_t>(pos) : 0;
}

bool MxfReader::Seek(uint64_t pos) {
  if (!file_) return false;
  return MXFSRC_FSEEK(file_, static_cast<mxfsrc_off_t>(pos), SEEK_SET) == 0;
}

bool MxfReader::ReadBytes(void* dst, size_t n) {
  if (!file_ || n == 0) return n == 0;
  return fread(dst, 1, n, file_) == n;
}

bool MxfReader::ReadU8(uint8_t& v) { return ReadBytes(&v, 1); }

bool MxfReader::ReadU16BE(uint16_t& v) {
  uint8_t b[2];
  if (!ReadBytes(b, 2)) return false;
  v = static_cast<uint16_t>((b[0] << 8) | b[1]);
  return true;
}

bool MxfReader::ReadU32BE(uint32_t& v) {
  uint8_t b[4];
  if (!ReadBytes(b, 4)) return false;
  v = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
      (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  return true;
}

bool MxfReader::ReadU64BE(uint64_t& v) {
  uint8_t b[8];
  if (!ReadBytes(b, 8)) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
  return true;
}

bool MxfReader::ReadUl(Ul& v) { return ReadBytes(v.data(), v.size()); }

bool MxfReader::Skip(uint64_t n) {
  if (!file_) return false;
  return MXFSRC_FSEEK(file_, static_cast<mxfsrc_off_t>(n), SEEK_CUR) == 0;
}

bool MxfReader::ReadBerLength(uint64_t& length) {
  uint8_t first = 0;
  if (!ReadU8(first)) return false;
  if ((first & 0x80) == 0) {
    length = first;
    return true;
  }
  const int numLenBytes = first & 0x7F;
  if (numLenBytes == 0 || numLenBytes > 8) return false;
  uint64_t len = 0;
  for (int i = 0; i < numLenBytes; ++i) {
    uint8_t b = 0;
    if (!ReadU8(b)) return false;
    len = (len << 8) | b;
  }
  length = len;
  return true;
}

bool ReadKlvHeader(MxfReader& reader, KlvHeader& out) {
  if (!reader.ReadUl(out.key)) return false;
  if (!reader.ReadBerLength(out.length)) return false;
  out.valueOffset = reader.Tell();
  return true;
}

}  // namespace mxfsrc
