#include "mxf/klv.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "mxf/file_byte_source.h"
#include "net/http_byte_source.h"

namespace mxfsrc {

namespace {

bool HasSchemePrefix(const std::string& path, const char* scheme) {
  const size_t len = std::strlen(scheme);
  if (path.size() < len) return false;
  return std::equal(path.begin(), path.begin() + static_cast<std::ptrdiff_t>(len), scheme,
                     [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

bool IsHttpUrl(const std::string& path) { return HasSchemePrefix(path, "http://") || HasSchemePrefix(path, "https://"); }

}  // namespace

bool UlPrefixEquals(const Ul& key, const uint8_t* prefix, size_t prefixLen) {
  if (prefixLen > key.size()) return false;
  return std::memcmp(key.data(), prefix, prefixLen) == 0;
}

bool UlEquals(const Ul& a, const Ul& b) { return a == b; }

MxfReader::MxfReader(const std::string& path, const std::string& httpHeaders) {
  if (IsHttpUrl(path)) {
    source_ = std::make_unique<HttpByteSource>(path, httpHeaders);
  } else {
    source_ = std::make_unique<FileByteSource>(path);
  }
}

MxfReader::~MxfReader() = default;

uint64_t MxfReader::Tell() const { return source_ ? source_->Tell() : 0; }

bool MxfReader::Seek(uint64_t pos) { return source_ && source_->Seek(pos); }

uint64_t MxfReader::Size() const { return source_ ? source_->Size() : 0; }

std::string MxfReader::LastError() const { return source_ ? source_->LastError() : std::string(); }

bool MxfReader::ReadBytes(void* dst, size_t n) { return source_ && source_->ReadBytes(dst, n); }

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

bool MxfReader::Skip(uint64_t n) { return source_ && source_->Skip(n); }

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

namespace {
constexpr uint8_t kUlDesignator[4] = {0x06, 0x0e, 0x2b, 0x34};
}

bool LooksLikeKlvKey(const Ul& key) { return std::memcmp(key.data(), kUlDesignator, sizeof(kUlDesignator)) == 0; }

bool ResyncToUlDesignator(MxfReader& reader) {
  uint8_t window[4] = {};
  size_t have = 0;
  while (reader.Tell() < reader.Size()) {
    uint8_t b = 0;
    if (!reader.ReadU8(b)) return false;
    if (have < 4) {
      window[have++] = b;
    } else {
      window[0] = window[1];
      window[1] = window[2];
      window[2] = window[3];
      window[3] = b;
    }
    if (have == 4 && std::memcmp(window, kUlDesignator, 4) == 0) return reader.Seek(reader.Tell() - 4);
  }
  return false;
}

}  // namespace mxfsrc
