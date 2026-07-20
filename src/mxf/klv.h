#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace mxfsrc {

// A 16-byte SMPTE Universal Label (ST 298), used both as a KLV Key and as
// the value of many Header Metadata properties (e.g. PictureEssenceCoding).
using Ul = std::array<uint8_t, 16>;

// Compares the first prefixLen bytes only -- several UL families (see
// ul_tables.h) are only specified/matched up to a variable-length prefix.
bool UlPrefixEquals(const Ul& key, const uint8_t* prefix, size_t prefixLen);
bool UlEquals(const Ul& a, const Ul& b);

// Random-access byte reader over an MXF file. Header Metadata parsing needs
// random access (jump to the footer partition, read the RIP at EOF); the
// essence walker (Phase 2) only ever calls ReadBytes/Skip moving forward
// from wherever header parsing left off.
class MxfReader {
 public:
  explicit MxfReader(const std::string& path);
  ~MxfReader();
  MxfReader(const MxfReader&) = delete;
  MxfReader& operator=(const MxfReader&) = delete;

  bool IsOpen() const { return file_ != nullptr; }
  uint64_t Tell() const;
  bool Seek(uint64_t pos);
  uint64_t Size() const { return size_; }

  bool ReadBytes(void* dst, size_t n);
  bool ReadU8(uint8_t& v);
  bool ReadU16BE(uint16_t& v);
  bool ReadU32BE(uint32_t& v);
  bool ReadU64BE(uint64_t& v);
  bool ReadUl(Ul& v);
  bool Skip(uint64_t n);

  // BER length per SMPTE 336: short form is a single byte < 0x80 holding the
  // length directly; long form is 0x80|numLenBytes followed by that many
  // big-endian bytes.
  bool ReadBerLength(uint64_t& length);

 private:
  FILE* file_ = nullptr;
  uint64_t size_ = 0;
};

struct KlvHeader {
  Ul key{};
  uint64_t length = 0;
  uint64_t valueOffset = 0;  // file offset of the first Value byte
};

// Reads a 16-byte Key + BER Length at the reader's current position. Leaves
// the reader positioned at the start of the Value; the caller decides
// whether to read or skip it (via Seek(valueOffset + length)).
bool ReadKlvHeader(MxfReader& reader, KlvHeader& out);

}  // namespace mxfsrc
