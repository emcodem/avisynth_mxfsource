#include "mxf/partition.h"

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

bool IsLocalSetKey(const Ul& key) {
  return UlPrefixEquals(key, kLocalSetKeyPrefix, sizeof(kLocalSetKeyPrefix));
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

}  // namespace mxfsrc
