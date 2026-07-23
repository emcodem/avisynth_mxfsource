#pragma once

// Shared byte-builder helpers for hand-crafting minimal, spec-shaped OP1a
// MXF headers in tests, since no real sample .mxf files are available in
// this environment (see plan Phase 8 for real-file testing). Used by both
// header_parser_test.cpp and essence_walker_test.cpp.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "mxf/essence_walker.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"
#include "mxf/partition.h"

namespace mxfsrc_test {

using mxfsrc::Ul;

inline void AppendU16BE(std::vector<uint8_t>& buf, uint16_t v) {
  buf.push_back(static_cast<uint8_t>(v >> 8));
  buf.push_back(static_cast<uint8_t>(v));
}

inline void AppendU32BE(std::vector<uint8_t>& buf, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) buf.push_back(static_cast<uint8_t>(v >> shift));
}

inline void AppendU64BE(std::vector<uint8_t>& buf, uint64_t v) {
  for (int shift = 56; shift >= 0; shift -= 8) buf.push_back(static_cast<uint8_t>(v >> shift));
}

inline void AppendI64BE(std::vector<uint8_t>& buf, int64_t v) { AppendU64BE(buf, static_cast<uint64_t>(v)); }

inline void AppendUl(std::vector<uint8_t>& buf, const Ul& ul) { buf.insert(buf.end(), ul.begin(), ul.end()); }

// Full BER length (SMPTE 336 short/long form) -- essence payloads routinely
// exceed the 0x80 short-form limit (e.g. a single UYVY frame), so this must
// switch to long form rather than silently truncating a uint8_t length.
inline void AppendBerLength(std::vector<uint8_t>& buf, uint64_t length) {
  if (length < 0x80) {
    buf.push_back(static_cast<uint8_t>(length));
    return;
  }
  uint8_t bytes[8];
  int n = 0;
  uint64_t v = length;
  do {
    bytes[n++] = static_cast<uint8_t>(v & 0xFF);
    v >>= 8;
  } while (v != 0);
  buf.push_back(static_cast<uint8_t>(0x80 | n));
  for (int i = n - 1; i >= 0; --i) buf.push_back(bytes[i]);
}

inline void AppendKlvHeader(std::vector<uint8_t>& buf, const Ul& key, uint64_t length) {
  AppendUl(buf, key);
  AppendBerLength(buf, length);
}

inline Ul MakeUid(uint8_t marker) {
  Ul ul{};
  ul[0] = 0x42;  // arbitrary, just needs to be distinct per marker
  ul[15] = marker;
  return ul;
}

inline void AppendLocalSet(std::vector<uint8_t>& buf, uint8_t typeHighByte, uint8_t typeLowByte,
                            const std::vector<uint8_t>& entries) {
  Ul key = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53, 0x01, 0x01,
            0x0d, 0x01, 0x01, 0x01, 0x01, 0x01, typeHighByte, typeLowByte};
  AppendKlvHeader(buf, key, entries.size());
  buf.insert(buf.end(), entries.begin(), entries.end());
}

inline void AppendTagUl(std::vector<uint8_t>& entries, uint16_t tag, const Ul& value) {
  AppendU16BE(entries, tag);
  AppendU16BE(entries, 16);
  AppendUl(entries, value);
}

inline void AppendTagU32(std::vector<uint8_t>& entries, uint16_t tag, uint32_t value) {
  AppendU16BE(entries, tag);
  AppendU16BE(entries, 4);
  AppendU32BE(entries, value);
}

inline void AppendTagI64(std::vector<uint8_t>& entries, uint16_t tag, int64_t value) {
  AppendU16BE(entries, tag);
  AppendU16BE(entries, 8);
  AppendI64BE(entries, value);
}

inline void AppendTagRational(std::vector<uint8_t>& entries, uint16_t tag, int32_t num, int32_t den) {
  AppendU16BE(entries, tag);
  AppendU16BE(entries, 8);
  AppendU32BE(entries, static_cast<uint32_t>(num));
  AppendU32BE(entries, static_cast<uint32_t>(den));
}

inline void AppendTagUlArray(std::vector<uint8_t>& entries, uint16_t tag, const std::vector<Ul>& uls) {
  AppendU16BE(entries, tag);
  AppendU16BE(entries, static_cast<uint16_t>(8 + uls.size() * 16));
  AppendU32BE(entries, static_cast<uint32_t>(uls.size()));
  AppendU32BE(entries, 16);
  for (const Ul& ul : uls) AppendUl(entries, ul);
}

// Data Definition (Picture) UL from ul_tables.h's kDataDefinitionUls[0], full 16 bytes.
inline const Ul kPictureDataDefinition = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01,
                                           0x01, 0x03, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00};
// Uncompressed 4:2:2 8-bit PictureEssenceCoding UL from ul_tables.h.
inline const Ul kRawVideoCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0A,
                                    0x04, 0x01, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00};

struct SyntheticFile {
  int64_t sequenceDuration = 10;
  bool includeFooterPartitionOffset = true;
  uint32_t videoTrackNumber = 0x01010100;  // arbitrary but must be nonzero/distinct for essence key matching
  uint32_t width = 320;
  uint32_t height = 240;
  Ul pictureEssenceCoding = kRawVideoCoding;

  // Adds a second Package -- a SourcePackage -- alongside the MaterialPackage
  // built unconditionally below, so tests can exercise Origin handling (see
  // header_parser.h's MxfStreamInfo::origin/sourcePackageDuration): only a
  // SourcePackage's Track.Origin (tag 0x4B02) is meaningful/read by the
  // parser. Reuses the same CDCI Descriptor as the MaterialPackage rather
  // than building a second one -- irrelevant to what Origin handling tests.
  bool includeSourcePackage = false;
  int64_t sourcePackageDuration = 10;  // SP's raw/untrimmed Sequence.Duration
  int64_t sourcePackageOrigin = 0;     // SP's Track.Origin (0x4B02)
};

// Builds just the Header Metadata local-set KLVs (Primer Pack ->
// ContentStorage -> MaterialPackage -> Track(TrackNumber) ->
// Sequence(Picture) -> CDCI Descriptor, plus an optional SourcePackage --
// see SyntheticFile::includeSourcePackage), with no enclosing Partition
// Pack. Factored out of BuildSyntheticHeader so the same metadata-building
// logic can be reused for a Footer Partition's redundant copy (see
// BuildFooterPrecedenceTestFile) with a different SyntheticFile config.
inline std::vector<uint8_t> BuildHeaderMetadataBlock(const SyntheticFile& cfg) {
  const Ul uidContentStorage = MakeUid(1);
  const Ul uidPackage = MakeUid(2);
  const Ul uidTrack = MakeUid(3);
  const Ul uidSequence = MakeUid(4);
  const Ul uidDescriptor = MakeUid(5);
  const Ul uidSourcePackage = MakeUid(6);
  const Ul uidSourceTrack = MakeUid(7);
  const Ul uidSourceSequence = MakeUid(8);

  std::vector<uint8_t> metadata;  // everything from right after the Partition Pack KLV onward

  // Primer Pack: zero entries (baseline tags read directly by number, see metadata_sets.h).
  AppendKlvHeader(metadata, mxfsrc::kPrimerPackKey, 8);
  AppendU32BE(metadata, 0);   // count
  AppendU32BE(metadata, 18);  // item size (tag+UL)

  // ContentStorage
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidContentStorage);
    std::vector<Ul> packageUids = {uidPackage};
    if (cfg.includeSourcePackage) packageUids.push_back(uidSourcePackage);
    AppendTagUlArray(entries, 0x1901, packageUids);
    AppendLocalSet(metadata, 0x18, 0x00, entries);
  }
  // MaterialPackage
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidPackage);
    AppendTagUlArray(entries, 0x4403, {uidTrack});
    AppendTagUl(entries, 0x4701, uidDescriptor);
    AppendLocalSet(metadata, 0x36, 0x00, entries);
  }
  // Track (Generic Track)
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidTrack);
    AppendTagRational(entries, 0x4B01, 25, 1);
    AppendTagUl(entries, 0x4803, uidSequence);
    AppendTagU32(entries, 0x4804, cfg.videoTrackNumber);
    AppendLocalSet(metadata, 0x3B, 0x00, entries);
  }
  // Sequence
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidSequence);
    AppendTagUl(entries, 0x0201, kPictureDataDefinition);
    AppendTagI64(entries, 0x0202, cfg.sequenceDuration);
    AppendLocalSet(metadata, 0x0F, 0x00, entries);
  }
  // CDCI Descriptor
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidDescriptor);
    AppendTagU32(entries, 0x3203, cfg.width);
    AppendTagU32(entries, 0x3202, cfg.height);
    AppendTagUl(entries, 0x3201, cfg.pictureEssenceCoding);
    AppendLocalSet(metadata, 0x28, 0x00, entries);
  }

  if (cfg.includeSourcePackage) {
    // SourcePackage -- shares the MaterialPackage's Descriptor (see
    // SyntheticFile::includeSourcePackage). Only its Track.Origin
    // (0x4B02) and Sequence.Duration (0x0202) are what Origin-handling
    // tests actually care about.
    {
      std::vector<uint8_t> entries;
      AppendTagUl(entries, 0x3C0A, uidSourcePackage);
      AppendTagUlArray(entries, 0x4403, {uidSourceTrack});
      AppendTagUl(entries, 0x4701, uidDescriptor);
      AppendLocalSet(metadata, 0x37, 0x00, entries);
    }
    // Source Track (Generic Track)
    {
      std::vector<uint8_t> entries;
      AppendTagUl(entries, 0x3C0A, uidSourceTrack);
      AppendTagRational(entries, 0x4B01, 25, 1);
      AppendTagI64(entries, 0x4B02, cfg.sourcePackageOrigin);
      AppendTagUl(entries, 0x4803, uidSourceSequence);
      AppendTagU32(entries, 0x4804, cfg.videoTrackNumber);
      AppendLocalSet(metadata, 0x3B, 0x00, entries);
    }
    // Source Sequence
    {
      std::vector<uint8_t> entries;
      AppendTagUl(entries, 0x3C0A, uidSourceSequence);
      AppendTagUl(entries, 0x0201, kPictureDataDefinition);
      AppendTagI64(entries, 0x0202, cfg.sourcePackageDuration);
      AppendLocalSet(metadata, 0x0F, 0x00, entries);
    }
  }

  return metadata;
}

// Builds a minimal OP1a-shaped Header Partition + Header Metadata (see
// BuildHeaderMetadataBlock). Returns just the Header Partition bytes;
// essence_walker_test.cpp appends a Body Partition after this to test the
// forward essence walk.
inline std::vector<uint8_t> BuildSyntheticHeader(const SyntheticFile& cfg) {
  std::vector<uint8_t> metadata = BuildHeaderMetadataBlock(cfg);

  // Header Partition Pack (fixed 88-byte value; see header_parser.cpp for field order).
  std::vector<uint8_t> file;
  Ul headerKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                   0x0d, 0x01, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00};
  AppendKlvHeader(file, headerKey, 88);
  AppendU16BE(file, 1);                                              // MajorVersion
  AppendU16BE(file, 2);                                              // MinorVersion
  AppendU32BE(file, 1);                                              // KAGSize
  AppendU64BE(file, 0);                                              // ThisPartition
  AppendU64BE(file, 0);                                              // PreviousPartition
  AppendU64BE(file, cfg.includeFooterPartitionOffset ? 100000 : 0);  // FooterPartition
  AppendU64BE(file, metadata.size());                                // HeaderByteCount
  AppendU64BE(file, 0);                                              // IndexByteCount
  AppendU32BE(file, 0);                                              // IndexSID
  AppendU64BE(file, 0);                                              // BodyOffset
  AppendU32BE(file, 1);                                              // BodySID
  Ul zeroUl{};
  AppendUl(file, zeroUl);  // OperationalPattern (unused by parser)
  AppendU32BE(file, 0);    // EssenceContainers count
  AppendU32BE(file, 16);   // EssenceContainers item size

  file.insert(file.end(), metadata.begin(), metadata.end());
  return file;
}

// Body/Footer Partition Pack, fixed 88-byte value layout (field order per
// header_parser.cpp's ReadPartitionPackValue). Field contents beyond the
// discriminator bytes in `key` don't matter for EssenceWalker, which skips
// every Partition Pack unconditionally.
inline void AppendPartitionPack(std::vector<uint8_t>& buf, mxfsrc::PartitionKind kind, uint64_t indexByteCount = 0,
                                  uint32_t indexSid = 0) {
  uint8_t discriminator = kind == mxfsrc::PartitionKind::kBody ? 0x03 : 0x04;
  Ul key = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
            0x0d, 0x01, 0x02, 0x01, 0x01, discriminator, 0x01, 0x00};
  AppendKlvHeader(buf, key, 88);
  AppendU16BE(buf, 1);
  AppendU16BE(buf, 2);
  AppendU32BE(buf, 1);
  AppendU64BE(buf, 0);  // ThisPartition
  AppendU64BE(buf, 0);  // PreviousPartition
  AppendU64BE(buf, 0);  // FooterPartition
  AppendU64BE(buf, 0);  // HeaderByteCount
  AppendU64BE(buf, indexByteCount);
  AppendU32BE(buf, indexSid);
  AppendU64BE(buf, 0);  // BodyOffset
  AppendU32BE(buf, 1);  // BodySID
  Ul zeroUl{};
  AppendUl(buf, zeroUl);
  AppendU32BE(buf, 0);
  AppendU32BE(buf, 16);
}

// Partition Pack status byte (key[14]) values, per SMPTE 377-1 -- see
// mxf/partition.h's PartitionStatus this must match.
enum class TestPartitionStatus : uint8_t {
  kOpenIncomplete = 0x01,
  kOpenComplete = 0x02,
  kClosedIncomplete = 0x03,
  kClosedComplete = 0x04,
};

// Generalizes AppendPartitionPack with an explicit status byte, FooterPartition
// offset, and HeaderByteCount -- needed to synthesize the open-header/
// closed-footer scenario real "growing file" recorders (e.g. Omneon)
// produce: a Header Partition marked Open & Incomplete (Duration/Origin
// genuinely unresolved) alongside a Footer Partition marked Closed &
// Complete, carrying its own full redundant Header Metadata copy with the
// final values. See BuildFooterPrecedenceTestFile.
inline void AppendPartitionPackWithFields(std::vector<uint8_t>& buf, mxfsrc::PartitionKind kind,
                                            TestPartitionStatus status, uint64_t thisPartition,
                                            uint64_t footerPartitionOffset, uint64_t headerByteCount,
                                            uint64_t indexByteCount = 0, uint32_t indexSid = 0) {
  uint8_t kindByte = 0x02;
  if (kind == mxfsrc::PartitionKind::kBody) kindByte = 0x03;
  if (kind == mxfsrc::PartitionKind::kFooter) kindByte = 0x04;
  Ul key = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
            0x0d, 0x01, 0x02, 0x01, 0x01, kindByte, static_cast<uint8_t>(status), 0x00};
  AppendKlvHeader(buf, key, 88);
  AppendU16BE(buf, 1);
  AppendU16BE(buf, 2);
  AppendU32BE(buf, 1);
  AppendU64BE(buf, thisPartition);
  AppendU64BE(buf, 0);  // PreviousPartition
  AppendU64BE(buf, footerPartitionOffset);
  AppendU64BE(buf, headerByteCount);
  AppendU64BE(buf, indexByteCount);
  AppendU32BE(buf, indexSid);
  AppendU64BE(buf, 0);  // BodyOffset
  AppendU32BE(buf, 1);  // BodySID
  Ul zeroUl{};
  AppendUl(buf, zeroUl);
  AppendU32BE(buf, 0);
  AppendU32BE(buf, 16);
}

// Builds the exact real-world scenario that motivated MXF footer-precedence
// handling (see header_parser.cpp): a Header Partition marked Open &
// Incomplete whose Header Metadata copy has an unresolvable Duration
// (`headerCfg.sequenceDuration = -1`), and a Footer Partition marked Closed
// & Complete carrying its own full, correct Header Metadata copy
// (`footerCfg`). No Body Partition/essence in between -- irrelevant to
// header_parser_test.cpp, which only exercises metadata resolution.
inline std::vector<uint8_t> BuildFooterPrecedenceTestFile(const SyntheticFile& headerCfg,
                                                             const SyntheticFile& footerCfg) {
  std::vector<uint8_t> headerMetadata = BuildHeaderMetadataBlock(headerCfg);
  std::vector<uint8_t> footerMetadata = BuildHeaderMetadataBlock(footerCfg);

  std::vector<uint8_t> file;
  // Header Partition Pack: ThisPartition=0, FooterPartition filled in below
  // once we know the footer's actual byte offset.
  const size_t headerPackPos = file.size();
  AppendPartitionPackWithFields(file, mxfsrc::PartitionKind::kHeader, TestPartitionStatus::kOpenIncomplete, 0, 0,
                                  headerMetadata.size());
  file.insert(file.end(), headerMetadata.begin(), headerMetadata.end());

  const uint64_t footerOffset = file.size();
  AppendPartitionPackWithFields(file, mxfsrc::PartitionKind::kFooter, TestPartitionStatus::kClosedComplete,
                                  footerOffset, footerOffset, footerMetadata.size());
  file.insert(file.end(), footerMetadata.begin(), footerMetadata.end());

  // Patch the Header Partition Pack's FooterPartition field (byte offset
  // 32 into its 88-byte Value, right after the 17-byte KLV header: 2+2+4+8+8=24,
  // so FooterPartition starts at KLV-header(17) + 24 = 41) now that it's known.
  const size_t footerFieldOffset = headerPackPos + 17 + 24;
  for (int i = 0; i < 8; ++i) {
    file[footerFieldOffset + static_cast<size_t>(i)] = static_cast<uint8_t>(footerOffset >> (56 - i * 8));
  }

  return file;
}

// Builds a Random Index Pack from (BodySID, byteOffset) pairs, computing
// its own trailing "total pack length" field dynamically so it stays
// correct regardless of the BER length form AppendKlvHeader picks -- see
// mxf/partition.h's ReadRandomIndexPack, which this exercises.
inline void AppendRandomIndexPack(std::vector<uint8_t>& buf,
                                    const std::vector<std::pair<uint32_t, uint64_t>>& entries) {
  const size_t packStart = buf.size();
  std::vector<uint8_t> value;
  for (const auto& entry : entries) {
    AppendU32BE(value, entry.first);
    AppendU64BE(value, entry.second);
  }
  AppendKlvHeader(buf, mxfsrc::kRandomIndexPackKey, value.size() + 4);
  buf.insert(buf.end(), value.begin(), value.end());
  const uint32_t totalPackLength = static_cast<uint32_t>(buf.size() - packStart + 4);
  AppendU32BE(buf, totalPackLength);
}

// Reproduces the real-world Omneon bug this exact scenario was found on:
// the Header Partition Pack's own FooterPartition field is left 0/unknown
// even in an otherwise-complete file, so the Footer can only be located via
// the Random Index Pack at the end of the file (see
// mxf/partition.h::ReadRandomIndexPack, header_parser.cpp's RIP fallback).
inline std::vector<uint8_t> BuildRipFooterFallbackTestFile(const SyntheticFile& headerCfg,
                                                              const SyntheticFile& footerCfg) {
  std::vector<uint8_t> headerMetadata = BuildHeaderMetadataBlock(headerCfg);
  std::vector<uint8_t> footerMetadata = BuildHeaderMetadataBlock(footerCfg);

  std::vector<uint8_t> file;
  AppendPartitionPackWithFields(file, mxfsrc::PartitionKind::kHeader, TestPartitionStatus::kOpenIncomplete, 0, 0,
                                  headerMetadata.size());
  file.insert(file.end(), headerMetadata.begin(), headerMetadata.end());

  const uint64_t footerOffset = file.size();
  AppendPartitionPackWithFields(file, mxfsrc::PartitionKind::kFooter, TestPartitionStatus::kClosedComplete,
                                  footerOffset, 0, footerMetadata.size());
  file.insert(file.end(), footerMetadata.begin(), footerMetadata.end());

  // Matches the real file's observed RIP shape: one entry per partition,
  // BodySID=0 for Header/Footer (no essence of their own), last entry is
  // always the Footer's offset.
  AppendRandomIndexPack(file, {{0, 0}, {0, footerOffset}});
  return file;
}

// One IndexEntryArray row spec, mirroring mxf/index_table.h's
// IndexTableEntry -- TemporalOffset/KeyFrameOffset are always written as 0
// (unused by the parser, see index_table.h's comment).
struct IndexTableEntrySpec {
  uint8_t flags = 0;
  uint64_t streamOffset = 0;
};

// Builds an IndexEntryArray tag's raw Value bytes: {4-byte count}{4-byte
// itemLength} header, then `entries.size()` rows of `itemLength` bytes each
// (itemLength must be >= 11; any bytes beyond the 11-byte fixed prefix are
// zero-padded, simulating unused trailing SliceOffset[]/PosTable[] data --
// used to test that GetIndexEntryArray's stride skips them correctly).
inline std::vector<uint8_t> BuildIndexEntryArrayBytes(const std::vector<IndexTableEntrySpec>& entries,
                                                        uint32_t itemLength = 11) {
  std::vector<uint8_t> value;
  AppendU32BE(value, static_cast<uint32_t>(entries.size()));
  AppendU32BE(value, itemLength);
  for (const auto& e : entries) {
    value.push_back(0);  // TemporalOffset -- unused
    value.push_back(0);  // KeyFrameOffset -- unused
    value.push_back(e.flags);
    AppendU64BE(value, e.streamOffset);
    for (uint32_t pad = 11; pad < itemLength; ++pad) value.push_back(0);
  }
  return value;
}

// Index Table Segment spec for AppendIndexTableSegment -- mirrors
// mxf/index_table.h's IndexTableSegment. `entries` is only written when
// editUnitByteCount == 0 (VBR); a CBR segment (editUnitByteCount != 0)
// omits the IndexEntryArray tag entirely, matching real files.
struct IndexTableSegmentSpec {
  uint32_t indexEditRateNum = 25;
  uint32_t indexEditRateDen = 1;
  int64_t indexStartPosition = 0;
  int64_t indexDuration = 0;
  uint32_t editUnitByteCount = 0;
  uint32_t indexSid = 1;
  uint32_t bodySid = 1;
  std::vector<IndexTableEntrySpec> entries;
  uint32_t indexEntryItemLength = 11;
};

// Appends one Index Table Segment KLV using the literal
// mxfsrc::kIndexTableSegmentKey -- NOT AppendLocalSet, which hardcodes the
// Header-Metadata set-type key prefix (a different key family).
inline void AppendIndexTableSegment(std::vector<uint8_t>& buf, const IndexTableSegmentSpec& spec) {
  std::vector<uint8_t> entries;
  AppendTagRational(entries, 0x3F0B, static_cast<int32_t>(spec.indexEditRateNum),
                      static_cast<int32_t>(spec.indexEditRateDen));
  AppendTagI64(entries, 0x3F0C, spec.indexStartPosition);
  AppendTagI64(entries, 0x3F0D, spec.indexDuration);
  AppendTagU32(entries, 0x3F05, spec.editUnitByteCount);
  AppendTagU32(entries, 0x3F06, spec.indexSid);
  AppendTagU32(entries, 0x3F07, spec.bodySid);
  if (spec.editUnitByteCount == 0 && !spec.entries.empty()) {
    std::vector<uint8_t> arrayBytes = BuildIndexEntryArrayBytes(spec.entries, spec.indexEntryItemLength);
    AppendU16BE(entries, 0x3F0A);
    AppendU16BE(entries, static_cast<uint16_t>(arrayBytes.size()));
    entries.insert(entries.end(), arrayBytes.begin(), arrayBytes.end());
  }
  AppendKlvHeader(buf, mxfsrc::kIndexTableSegmentKey, entries.size());
  buf.insert(buf.end(), entries.begin(), entries.end());
}

inline void AppendKlvFill(std::vector<uint8_t>& buf, uint8_t padLen) {
  AppendKlvHeader(buf, mxfsrc::kKlvFillKey, padLen);
  for (uint8_t i = 0; i < padLen; ++i) buf.push_back(0);
}

inline Ul MakeEssenceElementKey(uint32_t trackNumber) {
  Ul key{};
  std::memcpy(key.data(), mxfsrc::kEssenceElementKeyPrefix, 12);
  key[12] = static_cast<uint8_t>(trackNumber >> 24);
  key[13] = static_cast<uint8_t>(trackNumber >> 16);
  key[14] = static_cast<uint8_t>(trackNumber >> 8);
  key[15] = static_cast<uint8_t>(trackNumber);
  return key;
}

inline void AppendEssenceElement(std::vector<uint8_t>& buf, uint32_t trackNumber,
                                  const std::vector<uint8_t>& payload) {
  AppendKlvHeader(buf, MakeEssenceElementKey(trackNumber), payload.size());
  buf.insert(buf.end(), payload.begin(), payload.end());
}

inline std::filesystem::path WriteTempFile(const std::vector<uint8_t>& bytes, const char* name) {
  std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();
  return path;
}

}  // namespace mxfsrc_test
