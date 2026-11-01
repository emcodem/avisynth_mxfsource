#include "mxf/partition.h"

#include <algorithm>

#include "mxf/index_table.h"

namespace mxfsrc {

bool IsPartitionPackKey(const Ul& key, PartitionKind* kindOut) {
  if (!UlPrefixEquals(key, kPartitionPackKeyPrefix, sizeof(kPartitionPackKeyPrefix))) return false;
  const uint8_t discriminator = key[13];
  if (discriminator < 2 || discriminator > 4) return false;
  if (kindOut) {
    switch (discriminator) {
      case 2: *kindOut = PartitionKind::kHeader; break;
      case 3: *kindOut = PartitionKind::kBody; break;
      case 4: *kindOut = PartitionKind::kFooter; break;
    }
  }
  return true;
}

PartitionStatus ClassifyPartitionStatus(const Ul& key) {
  switch (key[14]) {
    case 0x01: return PartitionStatus::kOpenIncomplete;
    case 0x02: return PartitionStatus::kOpenComplete;
    case 0x03: return PartitionStatus::kClosedIncomplete;
    case 0x04: return PartitionStatus::kClosedComplete;
    default: return PartitionStatus::kUnknown;
  }
}

bool IsLocalSetKey(const Ul& key) {
  return UlPrefixEquals(key, kLocalSetKeyPrefix, sizeof(kLocalSetKeyPrefix));
}

bool IsEssenceContainerElementKey(const Ul& key) {
  return key[8] == 0x0d && key[9] == 0x01 && key[10] == 0x03 && key[11] == 0x01;
}

uint64_t PartitionContentStart(MxfReader& reader, uint64_t partitionEnd, uint64_t limit) {
  limit = std::min(limit, reader.Size());
  uint64_t pos = partitionEnd;
  for (int guard = 0; guard < 8 && pos + 17 <= limit; ++guard) {  // a handful of Fill items at most
    if (!reader.Seek(pos)) return partitionEnd;
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv) || !LooksLikeKlvKey(klv.key)) return partitionEnd;
    if (!UlEquals(klv.key, kKlvFillKey)) return pos;
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > limit) return partitionEnd;
    pos = valueEnd;
  }
  return partitionEnd;
}

bool EndsHeaderMetadata(const Ul& key) {
  return !LooksLikeKlvKey(key) || UlEquals(key, kIndexTableSegmentKey) || IsPartitionPackKey(key, nullptr) ||
         UlEquals(key, kRandomIndexPackKey) || IsEssenceContainerElementKey(key);
}

uint64_t ScanHeaderMetadataEnd(MxfReader& reader, uint64_t start, uint64_t limit) {
  limit = std::min({limit, reader.Size(), start + kMaxHeaderMetadataSize});
  uint64_t pos = start;
  while (pos + 17 <= limit && reader.Seek(pos)) {
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv) || EndsHeaderMetadata(klv.key)) break;
    const uint64_t next = klv.valueOffset + klv.length;
    if (next > limit) break;
    pos = next;
  }
  return pos;
}

std::optional<PartitionPack> ReadPartitionPackValue(MxfReader& reader, const KlvHeader& klv) {
  PartitionPack pack;
  if (!IsPartitionPackKey(klv.key, &pack.kind)) return std::nullopt;

  if (!reader.ReadU16BE(pack.majorVersion)) return std::nullopt;
  if (!reader.ReadU16BE(pack.minorVersion)) return std::nullopt;
  if (!reader.ReadU32BE(pack.kagSize)) return std::nullopt;
  if (!reader.ReadU64BE(pack.thisPartition)) return std::nullopt;
  if (!reader.ReadU64BE(pack.previousPartition)) return std::nullopt;
  if (!reader.ReadU64BE(pack.footerPartition)) return std::nullopt;
  if (!reader.ReadU64BE(pack.headerByteCount)) return std::nullopt;
  if (!reader.ReadU64BE(pack.indexByteCount)) return std::nullopt;
  if (!reader.ReadU32BE(pack.indexSid)) return std::nullopt;
  if (!reader.ReadU64BE(pack.bodyOffset)) return std::nullopt;
  if (!reader.ReadU32BE(pack.bodySid)) return std::nullopt;
  if (!reader.ReadUl(pack.operationalPattern)) return std::nullopt;

  uint32_t count = 0, itemLen = 0;
  if (!reader.ReadU32BE(count)) return std::nullopt;
  if (!reader.ReadU32BE(itemLen)) return std::nullopt;
  if (itemLen != 16 && count != 0) return std::nullopt;  // EssenceContainers batch is always UL-sized
  pack.essenceContainers.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    Ul ul{};
    if (!reader.ReadUl(ul)) return std::nullopt;
    pack.essenceContainers.push_back(ul);
  }

  return pack;
}

std::optional<std::vector<RandomIndexPackEntry>> ReadRandomIndexPack(MxfReader& reader) {
  const uint64_t fileSize = reader.Size();
  // Smallest possible RIP: 16-byte key + 1-byte short-form BER length + one
  // 12-byte entry + 4-byte trailing length = 33 bytes.
  if (fileSize < 33) return std::nullopt;
  if (!reader.Seek(fileSize - 4)) return std::nullopt;

  uint32_t ripLength = 0;
  if (!reader.ReadU32BE(ripLength)) return std::nullopt;
  if (ripLength < 33 || ripLength > fileSize) return std::nullopt;

  const uint64_t ripStart = fileSize - ripLength;
  if (!reader.Seek(ripStart)) return std::nullopt;

  KlvHeader klv{};
  if (!ReadKlvHeader(reader, klv)) return std::nullopt;
  if (!UlEquals(klv.key, kRandomIndexPackKey)) return std::nullopt;
  if (klv.length < 4) return std::nullopt;

  const uint64_t valueEnd = klv.valueOffset + klv.length;
  const uint64_t entriesEnd = valueEnd - 4;  // last 4 bytes: redundant total-pack-length trailer

  std::vector<RandomIndexPackEntry> entries;
  while (reader.Tell() + 12 <= entriesEnd) {
    RandomIndexPackEntry entry;
    if (!reader.ReadU32BE(entry.bodySid)) return std::nullopt;
    if (!reader.ReadU64BE(entry.byteOffset)) return std::nullopt;
    entries.push_back(entry);
  }
  return entries;
}

}  // namespace mxfsrc
