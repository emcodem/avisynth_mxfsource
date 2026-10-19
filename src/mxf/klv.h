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

// Index of the UL's Version Number byte (ST 336 byte 8). It records which
// registry revision a label was published in, not what the label means, so
// it is never part of a UL's identity and both helpers below skip it --
// real files carry e.g. PCM/DNxHR coding ULs and KLV Fill keys at versions
// other than our tables' (observed: 0x0a PCM/DNxHR, 0x01 KLV Fill).
inline constexpr size_t kUlVersionByteIndex = 7;

// Compares the first prefixLen bytes only -- several UL families (see
// ul_tables.h) are only specified/matched up to a variable-length prefix.
// Ignores the Version Number byte.
bool UlPrefixEquals(const Ul& key, const uint8_t* prefix, size_t prefixLen);
// Ignores the Version Number byte.
bool UlEquals(const Ul& a, const Ul& b);

// True if `path` has an http:// or https:// scheme prefix (case-insensitive).
// Shared by MxfReader's own FileByteSource/HttpByteSource dispatch and
// mxf/index_transport.h's identical local-path-vs-URL dispatch for
// publishing/fetching a shared essence index.
bool IsHttpUrl(const std::string& path);

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

  // True if arbitrary byte ranges of this source can be fetched
  // independently of one another (e.g. concurrent ranged GETs against the
  // same URL) -- local files and a range-capable HTTP server are both true;
  // an HTTP server that ignores Range requests (HttpByteSource falls back to
  // discard/restart-based emulated seeking in that case) is false. Used by
  // index_table.h's parallel HTTP index-extraction path to decide whether
  // fanning out concurrent partition fetches is even possible before trying.
  virtual bool SupportsRandomAccess() const { return true; }

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
  // Wraps an already-constructed byte source directly, bypassing the
  // path/URL dispatch above -- used internally by index_table.cpp's
  // parallel HTTP index-extraction path to run the normal KLV-parsing
  // functions (ReadKlvHeader, ReadPartitionPackValue, ScanPartitionForIndexSegments,
  // ...) unmodified against an in-memory buffer (MemoryRangeByteSource)
  // instead of a live file/HTTP stream.
  explicit MxfReader(std::unique_ptr<IByteSource> source);
  ~MxfReader();
  MxfReader(const MxfReader&) = delete;
  MxfReader& operator=(const MxfReader&) = delete;

  bool IsOpen() const { return source_ && source_->IsOpen(); }
  bool SupportsRandomAccess() const { return source_ && source_->SupportsRandomAccess(); }
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

// Every legitimate KLV key begins with the SMPTE UL registry designator
// (06.0E.2B.34, ST 298). Some real-world recorders (observed: Atomos Shogun
// XAVC-Intra/XAVC-L recordings) pad a partition out to a KAG boundary using
// raw zero bytes instead of a spec-compliant KLV Fill Item -- such a run of
// zeros parses as a spurious zero-length "KLV" with an all-zero key, and
// naively trusting its (bogus) length field walks forward one KLV-header's
// worth of bytes at a time, which is generally not a multiple of the
// actual pad length. That desyncs a naive walk so it lands misaligned
// inside real KLV data afterward and fails outright instead of just
// skipping the padding. Shared by EssenceWalker and the Index Table
// Segment partition scanner (mxf/index_table.h), both of which walk
// forward through a byte range that can contain this kind of padding.
bool LooksLikeKlvKey(const Ul& key);

// Scans forward byte-by-byte from the reader's current position for the
// next occurrence of the UL designator, leaving the reader positioned
// exactly at its first byte so the caller can retry ReadKlvHeader there.
// Returns false if the designator never occurs before EOF.
bool ResyncToUlDesignator(MxfReader& reader);

}  // namespace mxfsrc
