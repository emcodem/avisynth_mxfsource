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

// Short-form BER length only (every length used by these synthetic files is < 0x80).
inline void AppendBerLenShort(std::vector<uint8_t>& buf, uint8_t len) { buf.push_back(len); }

inline void AppendKlvHeader(std::vector<uint8_t>& buf, const Ul& key, uint8_t shortLen) {
  AppendUl(buf, key);
  AppendBerLenShort(buf, shortLen);
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
  AppendKlvHeader(buf, key, static_cast<uint8_t>(entries.size()));
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
};

// Builds a minimal OP1a-shaped Header Partition + Header Metadata:
// ContentStorage -> MaterialPackage -> Track(TrackNumber) -> Sequence(Picture)
// -> CDCI Descriptor (320x240, uncompressed 4:2:2, 25fps). Returns just the
// Header Partition bytes; essence_walker_test.cpp appends a Body Partition
// after this to test the forward essence walk.
inline std::vector<uint8_t> BuildSyntheticHeader(const SyntheticFile& cfg) {
  const Ul uidContentStorage = MakeUid(1);
  const Ul uidPackage = MakeUid(2);
  const Ul uidTrack = MakeUid(3);
  const Ul uidSequence = MakeUid(4);
  const Ul uidDescriptor = MakeUid(5);

  std::vector<uint8_t> metadata;  // everything from right after the Partition Pack KLV onward

  // Primer Pack: zero entries (baseline tags read directly by number, see metadata_sets.h).
  AppendKlvHeader(metadata, mxfsrc::kPrimerPackKey, 8);
  AppendU32BE(metadata, 0);   // count
  AppendU32BE(metadata, 18);  // item size (tag+UL)

  // ContentStorage
  {
    std::vector<uint8_t> entries;
    AppendTagUl(entries, 0x3C0A, uidContentStorage);
    AppendTagUlArray(entries, 0x1901, {uidPackage});
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
    AppendTagU32(entries, 0x3203, 320);
    AppendTagU32(entries, 0x3202, 240);
    AppendTagUl(entries, 0x3201, kRawVideoCoding);
    AppendLocalSet(metadata, 0x28, 0x00, entries);
  }

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
inline void AppendPartitionPack(std::vector<uint8_t>& buf, mxfsrc::PartitionKind kind) {
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
  AppendU64BE(buf, 0);  // IndexByteCount
  AppendU32BE(buf, 0);  // IndexSID
  AppendU64BE(buf, 0);  // BodyOffset
  AppendU32BE(buf, 1);  // BodySID
  Ul zeroUl{};
  AppendUl(buf, zeroUl);
  AppendU32BE(buf, 0);
  AppendU32BE(buf, 16);
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
  AppendKlvHeader(buf, MakeEssenceElementKey(trackNumber), static_cast<uint8_t>(payload.size()));
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
