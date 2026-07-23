#include "mxf/index_table.h"

#include <algorithm>

namespace mxfsrc {

std::optional<IndexTableSegment> ReadIndexTableSegmentValue(MxfReader& reader, const KlvHeader& klv) {
  if (!reader.Seek(klv.valueOffset)) return std::nullopt;
  LocalTagMap tags;
  if (!ReadLocalSet(reader, klv.length, tags)) return std::nullopt;

  IndexTableSegment seg;
  Rational editRate{};
  if (GetRational32(tags, 0x3F0B, editRate)) {
    seg.indexEditRateNum = static_cast<uint32_t>(editRate.num);
    seg.indexEditRateDen = static_cast<uint32_t>(editRate.den);
  }
  // IndexStartPosition/IndexDuration are the segment's core identity -- a
  // segment missing either is treated as malformed, not partially usable.
  if (!GetI64(tags, 0x3F0C, seg.indexStartPosition)) return std::nullopt;
  if (!GetI64(tags, 0x3F0D, seg.indexDuration)) return std::nullopt;
  GetU32(tags, 0x3F05, seg.editUnitByteCount);
  GetU32(tags, 0x3F06, seg.indexSid);
  GetU32(tags, 0x3F07, seg.bodySid);
  seg.entries = GetIndexEntryArray(tags, 0x3F0A);
  return seg;
}

std::vector<IndexTableEntry> GetIndexEntryArray(const LocalTagMap& set, uint16_t tag) {
  std::vector<IndexTableEntry> result;
  auto it = set.find(tag);
  if (it == set.end() || it->second.size() < 8) return result;
  const auto& b = it->second;

  const uint32_t count = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
                          (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  const uint32_t itemLen = (static_cast<uint32_t>(b[4]) << 24) | (static_cast<uint32_t>(b[5]) << 16) |
                           (static_cast<uint32_t>(b[6]) << 8) | static_cast<uint32_t>(b[7]);
  // 11 fixed bytes: TemporalOffset(1) + KeyFrameOffset(1) + Flags(1) + StreamOffset(8).
  if (itemLen < 11) return result;
  if (b.size() < 8 + static_cast<size_t>(count) * itemLen) return result;

  result.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const size_t base = 8 + static_cast<size_t>(i) * itemLen;
    IndexTableEntry e;
    // b[base+0]=TemporalOffset, b[base+1]=KeyFrameOffset -- both unused (see index_table.h).
    e.flags = b[base + 2];
    uint64_t offset = 0;
    for (int k = 0; k < 8; ++k) offset = (offset << 8) | b[base + 3 + k];
    e.streamOffset = offset;
    result.push_back(e);
    // Remaining itemLen-11 bytes (SliceOffset[]/PosTable[]) implicitly
    // skipped by the next iteration's stride -- never read individually.
  }
  return result;
}

namespace {

std::optional<IndexScanPartition> ReadOnePartitionForIndex(MxfReader& reader, uint64_t offset) {
  if (offset >= reader.Size()) return std::nullopt;
  if (!reader.Seek(offset)) return std::nullopt;

  KlvHeader klv{};
  if (!ReadKlvHeader(reader, klv)) return std::nullopt;
  PartitionKind kind{};
  if (!IsPartitionPackKey(klv.key, &kind)) return std::nullopt;
  std::optional<PartitionPack> pack = ReadPartitionPackValue(reader, klv);
  if (!pack) return std::nullopt;

  IndexScanPartition scan;
  scan.partitionOffset = offset;
  scan.bodySid = pack->bodySid;
  scan.indexSid = pack->indexSid;
  const uint64_t partitionEnd = klv.valueOffset + klv.length;
  scan.indexByteRangeStart = partitionEnd + pack->headerByteCount;
  scan.indexByteRangeEnd = scan.indexByteRangeStart + pack->indexByteCount;
  scan.essenceStartOffset = scan.indexByteRangeEnd;
  return scan;
}

}  // namespace

std::vector<IndexScanPartition> EnumeratePartitionsForIndex(MxfReader& reader) {
  std::vector<IndexScanPartition> result;
  std::vector<uint64_t> seenOffsets;

  auto addIfNew = [&](uint64_t offset) {
    for (uint64_t seen : seenOffsets) {
      if (seen == offset) return;
    }
    if (auto scan = ReadOnePartitionForIndex(reader, offset)) {
      seenOffsets.push_back(offset);
      result.push_back(*scan);
    }
  };

  addIfNew(0);  // Header Partition -- always present, always at offset 0.
  if (auto rip = ReadRandomIndexPack(reader)) {
    for (const auto& entry : *rip) addIfNew(entry.byteOffset);
  }

  return result;
}

std::vector<IndexTableSegment> ScanPartitionForIndexSegments(MxfReader& reader, const IndexScanPartition& partition) {
  std::vector<IndexTableSegment> result;
  if (partition.indexByteRangeEnd <= partition.indexByteRangeStart) return result;
  if (!reader.Seek(partition.indexByteRangeStart)) return result;

  const uint64_t rangeEnd = partition.indexByteRangeEnd;
  while (reader.Tell() + 16 + 1 <= rangeEnd && reader.Tell() < reader.Size()) {
    const uint64_t klvStart = reader.Tell();
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) break;

    if (!LooksLikeKlvKey(klv.key)) {
      // Some real recorders pad this region to a KAG boundary with raw
      // zero bytes instead of a spec-compliant KLV Fill Item (observed:
      // Atomos Shogun XAVC-Intra/XAVC-L recordings -- see klv.h's
      // LooksLikeKlvKey/ResyncToUlDesignator, added for EssenceWalker and
      // shared here) -- resync past it rather than trusting a length field
      // derived from noise. Bounded to rangeEnd so a resync can't wander
      // past this partition's own declared index region into unrelated
      // file content (e.g. essence bytes, which also start with the same
      // UL designator).
      if (!reader.Seek(klvStart + 1)) break;
      if (!ResyncToUlDesignator(reader) || reader.Tell() >= rangeEnd) break;
      continue;
    }

    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > rangeEnd) break;

    if (UlEquals(klv.key, kIndexTableSegmentKey)) {
      if (auto seg = ReadIndexTableSegmentValue(reader, klv)) {
        seg->essenceContainerBaseOffset = partition.essenceStartOffset;
        if (seg->bodySid == 0) seg->bodySid = partition.bodySid;
        if (seg->indexSid == 0) seg->indexSid = partition.indexSid;
        result.push_back(std::move(*seg));
      }
    }
    // KLV Fill, Primer Pack repeats, or anything else in this range: skip
    // without interpreting, same tolerant pattern as ReadMetadataSets.
    if (!reader.Seek(valueEnd)) break;
  }
  return result;
}

int64_t EssenceIndex::HighestCoveredEditUnit() const {
  if (entries_.empty()) return -1;
  return entries_.back().editUnit;
}

std::optional<EssenceIndex::SeekPoint> EssenceIndex::FindPrecedingKeyframe(int64_t targetEditUnit) const {
  if (entries_.empty()) return std::nullopt;
  auto it = std::upper_bound(entries_.begin(), entries_.end(), targetEditUnit,
                              [](int64_t t, const FlatIndexEntry& e) { return t < e.editUnit; });
  if (it == entries_.begin()) return std::nullopt;  // target before every covered edit unit
  --it;
  if (it->editUnit != targetEditUnit) return std::nullopt;  // target falls in an uncovered gap
  while (!it->isKeyframe) {
    if (it == entries_.begin()) return std::nullopt;  // no keyframe anywhere before target
    --it;
  }
  return SeekPoint{it->editUnit, it->byteOffset};
}

namespace {

// Appends one segment's edit units as absolute {editUnit, byteOffset,
// isKeyframe} rows. CBR iteration count is derived from how many edit
// units could possibly fit within the file's actual size (rather than
// trusting a possibly-corrupt/huge IndexDuration directly), so a malformed
// segment can't turn into an unbounded loop.
void FlattenSegmentInto(const IndexTableSegment& seg, uint64_t fileSize, std::vector<FlatIndexEntry>& out) {
  if (seg.editUnitByteCount != 0) {
    if (seg.indexDuration <= 0) return;
    if (seg.essenceContainerBaseOffset >= fileSize) return;
    const uint64_t maxIterationsThatFit =
        (fileSize - seg.essenceContainerBaseOffset) / seg.editUnitByteCount + 1;
    const int64_t iterations = std::min<int64_t>(seg.indexDuration, static_cast<int64_t>(maxIterationsThatFit));
    for (int64_t i = 0; i < iterations; ++i) {
      const uint64_t offset =
          seg.essenceContainerBaseOffset + static_cast<uint64_t>(seg.editUnitByteCount) * static_cast<uint64_t>(i);
      out.push_back({seg.indexStartPosition + i, offset, true});
    }
    return;
  }

  // VBR: one row per parsed IndexEntryArray entry, in order starting at
  // indexStartPosition. Each entry checked independently (a single bad
  // StreamOffset doesn't imply subsequent ones are also bad).
  for (size_t i = 0; i < seg.entries.size(); ++i) {
    const uint64_t offset = seg.essenceContainerBaseOffset + seg.entries[i].streamOffset;
    if (offset >= fileSize) continue;
    out.push_back({seg.indexStartPosition + static_cast<int64_t>(i), offset, IsKeyframeFlags(seg.entries[i].flags)});
  }
}

}  // namespace

std::optional<EssenceIndex> BuildEssenceIndex(MxfReader& reader, uint32_t videoBodySid) {
  std::vector<IndexTableSegment> segments;
  for (const auto& partition : EnumeratePartitionsForIndex(reader)) {
    for (auto& seg : ScanPartitionForIndexSegments(reader, partition)) {
      if (videoBodySid != 0 && seg.bodySid != 0 && seg.bodySid != videoBodySid) continue;
      segments.push_back(std::move(seg));
    }
  }
  if (segments.empty()) return std::nullopt;

  // Stable sorts throughout: on a genuine tie (overlapping/duplicate
  // segments sharing an IndexStartPosition, or two segments producing the
  // same editUnit), this makes "first-file-order/first-scanned-partition
  // wins" a well-defined guarantee rather than leaving it to an unspecified
  // sort implementation detail.
  std::stable_sort(segments.begin(), segments.end(), [](const IndexTableSegment& a, const IndexTableSegment& b) {
    return a.indexStartPosition < b.indexStartPosition;
  });

  const uint64_t fileSize = reader.Size();
  std::vector<FlatIndexEntry> flat;
  for (const auto& seg : segments) FlattenSegmentInto(seg, fileSize, flat);
  if (flat.empty()) return std::nullopt;

  std::stable_sort(flat.begin(), flat.end(),
                    [](const FlatIndexEntry& a, const FlatIndexEntry& b) { return a.editUnit < b.editUnit; });
  flat.erase(
      std::unique(flat.begin(), flat.end(),
                  [](const FlatIndexEntry& a, const FlatIndexEntry& b) { return a.editUnit == b.editUnit; }),
      flat.end());

  return EssenceIndex(std::move(flat));
}

}  // namespace mxfsrc
