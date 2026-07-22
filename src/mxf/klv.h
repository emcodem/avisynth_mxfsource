#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace mxfsrc {

// A 16-byte SMPTE Universal Label (ST 298), used both as a KLV Key and as
// the value of many Header Metadata properties (e.g. PictureEssenceCoding).
using Ul = std::array<uint8_t, 16>;

// Compares the first prefixLen bytes only -- several UL families (see
// ul_tables.h) are only specified/matched up to a variable-length prefix.
bool UlPrefixEquals(const Ul& key, const uint8_t* prefix, size_t prefixLen);
bool UlEquals(const Ul& a, const Ul& b);

// Underlying byte-access backend for MxfReader -- one implementation reads a
// local file (FileByteSource, src/mxf/file_byte_source.h), another streams
// over HTTP/HTTPS (HttpByteSource, src/net/http_byte_source.h) so the same
// MxfReader API works whether the "path" is a filesystem path or a URL (e.g.
// an S3 presigned URL). Every method mirrors MxfReader's own contract.
class IByteSource {
 public:
  virtual ~IByteSource() = default;

  virtual bool IsOpen() const = 0;
  virtual uint64_t Tell() const = 0;
  virtual bool Seek(uint64_t pos) = 0;
  virtual uint64_t Size() const = 0;
  virtual bool ReadBytes(void* dst, size_t n) = 0;
  virtual bool Skip(uint64_t n) = 0;

  // Human-readable detail for the most recent failure (e.g. "HTTP 403
  // Forbidden (if this is an S3 presigned URL, it may have expired)").
  // Empty if the source doesn't have anything more specific to say than the
  // bool return already conveyed (this is always true for local files).
  virtual std::string LastError() const = 0;
};

// Random-access byte reader over an MXF source. Header Metadata parsing
// needs random access (jump to the footer partition, read the RIP at EOF);
// the essence walker (Phase 2) only ever calls ReadBytes/Skip moving forward
// from wherever header parsing left off.
//
// Dispatches to a FileByteSource or HttpByteSource under the hood based on
// whether `path` has an http:// / https:// scheme prefix; callers never see
// the difference. `httpHeaders` is a raw "Name: value\r\n..." block applied
// only when the path resolves to an HTTP(S) source (ignored for local
// files) -- see OpenOptions::http_headers.
class MxfReader {
 public:
  explicit MxfReader(const std::string& path, const std::string& httpHeaders = {});
  ~MxfReader();
  MxfReader(const MxfReader&) = delete;
  MxfReader& operator=(const MxfReader&) = delete;

  bool IsOpen() const { return source_ && source_->IsOpen(); }
  uint64_t Tell() const;
  bool Seek(uint64_t pos);
  uint64_t Size() const;
  std::string LastError() const;

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
  std::unique_ptr<IByteSource> source_;
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
