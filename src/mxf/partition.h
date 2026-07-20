#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Key/prefix values verified against FFmpeg's libavformat/mxfdec.c and
// mxf.c (see plan: "cite FFmpeg mxfdec.c as correctness reference"), which
// in turn implement SMPTE 377-1/ST 336.

enum class PartitionKind { kHeader, kBody, kFooter };

// Partition Pack keys share a 13-byte prefix; byte[13] (0-indexed) selects
// Header(2)/Body(3)/Footer(4), byte[14] the open/closed/complete status
// (not needed for MVP parsing, kept available for future use).
inline constexpr uint8_t kPartitionPackKeyPrefix[13] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01,
                                                          0x01, 0x0d, 0x01, 0x02, 0x01, 0x01};

bool IsPartitionPackKey(const Ul& key, PartitionKind* kindOut);

inline constexpr Ul kPrimerPackKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                                       0x0d, 0x01, 0x02, 0x01, 0x01, 0x05, 0x01, 0x00};

inline constexpr Ul kRandomIndexPackKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                                            0x0d, 0x01, 0x02, 0x01, 0x01, 0x11, 0x01, 0x00};

inline constexpr Ul kKlvFillKey = {0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x02,
                                    0x03, 0x01, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00};

// Header Metadata local-set keys (Preface, ContentStorage, Package, ...)
// share this 5-byte prefix followed by 0x53 ('S') at byte[5], which is how
// mxfdec.c's mxf_parse_klv() distinguishes local-set-coded sets from
// fixed-layout packs (Partition Pack, Primer Pack).
inline constexpr uint8_t kLocalSetKeyPrefix[6] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53};
bool IsLocalSetKey(const Ul& key);

struct PartitionPack {
  PartitionKind kind = PartitionKind::kHeader;
  uint16_t majorVersion = 0;
  uint16_t minorVersion = 0;
  uint32_t kagSize = 0;
  uint64_t thisPartition = 0;
  uint64_t previousPartition = 0;
  uint64_t footerPartition = 0;  // byte offset of the Footer Partition Pack, 0 if unknown
  uint64_t headerByteCount = 0;
  uint64_t indexByteCount = 0;
  uint32_t indexSid = 0;
  uint64_t bodyOffset = 0;
  uint32_t bodySid = 0;
  Ul operationalPattern{};
  std::vector<Ul> essenceContainers;
};

// Parses a Partition Pack Value given its already-read KlvHeader (caller
// must have confirmed IsPartitionPackKey(klv.key) first). Leaves the reader
// positioned wherever it ends up mid-Value; caller should Seek to
// klv.valueOffset + klv.length afterwards to continue.
std::optional<PartitionPack> ReadPartitionPackValue(MxfReader& reader, const KlvHeader& klv);

}  // namespace mxfsrc
