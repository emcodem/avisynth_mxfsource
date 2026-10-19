#include "mxf/index_table.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

#include "mxf/essence_walker.h"
#include "mxf/memory_byte_source.h"
#include "net/http_put.h"

namespace mxfsrc {

namespace {

struct EssenceResyncResult {
  // Same semantics this whole function always had: `candidate` unchanged
  // unless an actual essence element key is found, never inferred from
  // merely "walked to EOF without finding one". This is what
  // BuildEssenceIndex's bodyRanges/FlattenSegmentInto uses to resolve
  // *other* entries' absolute byte offsets (see ResolveLogicalEssenceOffset)
  // -- a value that's merely "proven safe to scan up to" but not "proven to
  // be this partition's own genuine essence start" would misresolve those
  // once more than one partition shares a BodySID (verified: loosening this
  // field broke TestMultiSegmentStitching/TestMalformedSegmentIsSkippedNotFatal,
  // both of which need a *tight* essenceStartOffset for a real, non-footer
  // partition to correctly bound bodyRanges).
  uint64_t essenceStartOffset;
  // Furthest position reached by walking forward from `candidate` through
  // genuinely parsed, real (non-essence) KLVs before giving up -- widen-only,
  // safe to use *purely* as ScanPartitionForIndexSegments's own hardEnd (see
  // scanHardEndBound's own comment in index_table.h for why it's kept
  // separate from essenceStartOffset above).
  uint64_t scanHardEndBound;
};

// A partition's own declared HeaderByteCount+IndexByteCount is supposed to
// delineate exactly where its essence begins, but some real recorders don't
// include a trailing KAG-alignment KLV Fill in that count -- the same class
// of "undercounted byte-count field" quirk this project already works
// around for HeaderByteCount on real Omneon files (see header_parser.cpp),
// just a different field (observed: a real multi-partition camera
// recording whose IndexByteCount undercounts by a constant 868 bytes in
// every Body Partition, silently shifting BuildEssenceIndex's resolved
// offsets short by exactly that much and landing SeekAndDecode on the
// wrong frame's content without any decode error). Resyncs forward from
// `candidate` past any KLV Fill/other non-essence KLV (mirrors
// EssenceWalker::Next()'s own tolerant skip-and-resync) until landing on an
// actual essence element -- of any track, video or audio, since that's
// enough to prove "essence genuinely starts here" regardless of which
// track this partition's essence belongs to.
EssenceResyncResult ResyncToActualEssenceStart(MxfReader& reader, uint64_t candidate) {
  uint64_t pos = candidate;
  for (int guard = 0; guard < 8; ++guard) {  // a handful of Fill items at most, never unbounded
    // True EOF, reached only via real, successfully-parsed KLVs: proves
    // there's no essence anywhere between `candidate` and end of file, which
    // is exactly the situation a real Footer Partition is always in (SMPTE
    // 377-1 -- footer partitions never precede essence) -- but this says
    // nothing about whether `pos` is *this partition's own* essence start
    // (it may have walked through Fill, other partitions' Partition Packs,
    // the RIP, anything non-essence in between), so it's only trustworthy as
    // `scanHardEndBound`, never as `essenceStartOffset` (see
    // EssenceResyncResult's own comment above). Observed on a real file:
    // the old always-`return candidate` behavior here silently kept the
    // Footer Partition's own Index Table Segment (covering the file's true
    // final edit units) undercounted-and-rejected by
    // ScanPartitionForIndexSegments's hardEnd check.
    if (pos >= reader.Size()) return {candidate, pos};
    if (!reader.Seek(pos)) return {candidate, candidate};
    KlvHeader klv{};
    // A failed parse here isn't just "no valid key at pos" -- since
    // `candidate` is itself only an estimate (see the undercount note
    // above), it commonly lands *inside* a still-undiscovered segment's own
    // raw entry array, where BER-decoding random binary content as a length
    // field can fail outright (e.g. a length-byte count of 0 or >8) rather
    // than merely producing a key that fails LooksLikeKlvKey. Both cases
    // mean "not a real KLV boundary here" and get the identical resync
    // recovery -- an outright ReadKlvHeader failure must not be treated as
    // fatal, or a real file with this undercount can silently drop this
    // partition's Index Table Segment for essentially arbitrary reasons
    // (whichever way that specific binary content happens to BER-decode).
    if (!ReadKlvHeader(reader, klv) || !LooksLikeKlvKey(klv.key)) {
      if (!reader.Seek(pos + 1) || !ResyncToUlDesignator(reader)) return {candidate, candidate};
      pos = reader.Tell();
      continue;
    }
    if (IsEssenceElementKey(klv.key)) return {pos, pos};
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > reader.Size()) return {candidate, candidate};
    pos = valueEnd;
  }
  return {candidate, pos};
}

}  // namespace

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
    // b[base+0]=TemporalOffset -- unused (see index_table.h).
    e.keyFrameOffset = static_cast<int8_t>(b[base + 1]);
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
  const EssenceResyncResult resync = ResyncToActualEssenceStart(reader, scan.indexByteRangeEnd);
  scan.essenceStartOffset = resync.essenceStartOffset;
  scan.scanHardEndBound = resync.scanHardEndBound;
  scan.bodyOffset = pack->bodyOffset;
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

  // `rangeEnd` is only a rough guide -- derived from the partition's own
  // HeaderByteCount/IndexByteCount fields, which can undercount reality
  // (the same "byte-count field undercounts real content" quirk this
  // project has hit before, see ResyncToActualEssenceStart above) -- for
  // when to stop *looking for more* segments. It must never be used to
  // reject a segment already found: once its key matches
  // kIndexTableSegmentKey, its own KLV length is self-describing and
  // authoritative, so its value is read in full even if that runs past
  // this rough estimate (real bug found on a real XAVC-Intra file: a
  // genuine, well-formed Index Table Segment's value ran 81 bytes past the
  // partition's own undercounted IndexByteCount, so the old strict `>
  // rangeEnd` check silently discarded it entirely -- meaning this
  // scanner's very first checked entry was the ONLY thing standing between
  // an O(1) CBR-indexed seek and reading every frame's payload en route to
  // the target, the actual complaint that motivated finding this).
  // `essenceStartOffset` (already resync-corrected, see above) is one bound
  // that's always safe to enforce: nothing legitimately part of the index
  // can be at or past where essence itself is confirmed to start.
  // `scanHardEndBound` is the other -- a widen-only bound proven safe purely
  // for "is there room to keep scanning/accept an already-found segment
  // here" (never used as this partition's own essence-resolution base, see
  // its own comment in index_table.h) -- needed for e.g. a Footer Partition,
  // which never precedes essence at all, so essenceStartOffset alone can
  // stay exactly at the (possibly undercounted) rangeEnd.
  const uint64_t rangeEnd = partition.indexByteRangeEnd;
  const uint64_t hardEnd = std::max({rangeEnd, partition.essenceStartOffset, partition.scanHardEndBound});
  while (reader.Tell() + 16 + 1 <= hardEnd && reader.Tell() < reader.Size()) {
    if (reader.Tell() >= rangeEnd) break;  // rough guide: stop looking for a *new* segment past here
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
      if (!ResyncToUlDesignator(reader, rangeEnd)) break;
      continue;
    }

    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > hardEnd) break;

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

int EssenceIndex::EstimateReorderDelay(int64_t keyframeEditUnit) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), keyframeEditUnit,
                              [](const FlatIndexEntry& e, int64_t t) { return e.editUnit < t; });
  if (it == entries_.end() || it->editUnit != keyframeEditUnit) return 0;
  int run = 0;
  uint64_t prevByteOffset = it->byteOffset;
  bool monotonic = true;
  for (++it; it != entries_.end() && IsBFrameFlags(it->flags); ++it) {
    ++run;
    if (it->byteOffset <= prevByteOffset) monotonic = false;
    prevByteOffset = it->byteOffset;
  }
  // "Low-delay" B-pictures (bi-predictive for prediction quality, but
  // referencing only earlier-displayed frames -- no genuine temporal
  // reordering) have decode order already equal to display order: this
  // run's own byteOffsets increase monotonically right along with editUnit,
  // exactly like the keyframe's own position, confirmed against a real
  // broadcast MXF file (D:\Vistek_DMX_-1ms.mxf) whose Index Table's
  // StreamOffsets increase strictly with editUnit for its entire length --
  // see this file's own handoff.md for the earlier, independent discovery
  // of this exact distinction on a different real file. In that case, no
  // reorder-delay compensation is needed or correct: DecodeThreadMain's
  // `pending`/`pendingAudio` position-shifting (this function's return
  // value, downstream) would otherwise unnecessarily shift both audio and
  // video by `run` positions for the file's *entire* length, not just a
  // leading warm-up window -- confirmed empirically: for the Vistek file,
  // this repeats-forever bug looked exactly like a single ring index
  // getting an earlier index's stale content, at every reorderDelay-many
  // positions, not just near the start. A genuine B-pyramid (a *later*
  // editUnit's essence physically stored *before* an earlier one --
  // byteOffset breaks monotonicity somewhere in the run) still needs this
  // function's original behavior; that's `TestReorderDelayCompensationMatchesReferenceWithRealBFrames`'s
  // own already-known, already-documented harder case, unaffected by this
  // check since monotonic is false there.
  return monotonic ? 0 : run;
}

bool EssenceIndex::NeedsPreviousGop(int64_t keyframeEditUnit) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), keyframeEditUnit,
                              [](const FlatIndexEntry& e, int64_t t) { return e.editUnit < t; });
  if (it == entries_.end() || it->editUnit != keyframeEditUnit) return false;
  for (++it; it != entries_.end() && !it->isKeyframe; ++it) {
    if (it->editUnit + static_cast<int64_t>(it->keyFrameOffset) < keyframeEditUnit) return true;
  }
  return false;
}

int EssenceIndex::CountUndecodableLeadingPictures(int64_t keyframeEditUnit) const {
  auto it = std::lower_bound(entries_.begin(), entries_.end(), keyframeEditUnit,
                              [](const FlatIndexEntry& e, int64_t t) { return e.editUnit < t; });
  if (it == entries_.end() || it->editUnit != keyframeEditUnit) return 0;
  int count = 0;
  for (++it; it != entries_.end() && !it->isKeyframe; ++it) {
    if (it->editUnit + static_cast<int64_t>(it->keyFrameOffset) < keyframeEditUnit) ++count;
  }
  return count;
}

namespace {

struct BodyRange {
  uint64_t bodyOffset;
  uint64_t essenceStartOffset;
};

// Resolves a StreamOffset -- always defined (SMPTE 377-1) relative to the
// *logical*, cross-partition essence container for one BodySID -- into an
// actual physical file offset, using each contributing Body Partition's own
// BodyOffset (its position within that logical container) and
// essenceStartOffset (where its own essence bytes physically begin).
// Necessary because Index Table Segments are sometimes all gathered into a
// single partition (e.g. the Footer) describing essence physically stored
// across many earlier, unrelated Body Partitions -- naively adding
// StreamOffset to *that* partition's own essenceStartOffset (as if index
// and essence were always collocated) silently produces offsets far beyond
// the file's actual size for every edit unit but the first, which
// BuildEssenceIndex's own out-of-bounds check then quietly drops (real bug:
// a 7400-frame camera recording whose Footer Partition consolidates 31
// Index Table Segments for 31 earlier Body Partitions ended up with an
// index covering only edit unit 0). `ranges` sorted ascending by
// bodyOffset. Falls back to `fallbackBase + logicalOffset` (the old
// collocated-index assumption) when `ranges` is empty -- no BodyOffset
// information available at all, e.g. a file with only one partition ever
// carrying this BodySID's essence.
uint64_t ResolveLogicalEssenceOffset(uint64_t logicalOffset, const std::vector<BodyRange>& ranges,
                                     uint64_t fallbackBase) {
  if (ranges.empty()) return fallbackBase + logicalOffset;
  auto it = std::upper_bound(ranges.begin(), ranges.end(), logicalOffset,
                              [](uint64_t off, const BodyRange& r) { return off < r.bodyOffset; });
  if (it == ranges.begin()) {
    // At or before the very first range's own BodyOffset -- conventionally
    // 0 for the first essence-bearing partition, so this is the normal
    // case, not a fallback.
    return ranges.front().essenceStartOffset + logicalOffset;
  }
  --it;
  return it->essenceStartOffset + (logicalOffset - it->bodyOffset);
}

// True if `offset` can be the start of an edit unit: the closest known
// partition at or before it carries essence (BodySID != 0, so never a
// Footer or an index-only partition) and its essence has started there.
// With no partition at or before it, there's nothing to check against.
// `partitionsByOffset` is sorted ascending by partitionOffset.
bool LiesInPartitionEssence(uint64_t offset, const std::vector<IndexScanPartition>& partitionsByOffset) {
  auto it = std::upper_bound(partitionsByOffset.begin(), partitionsByOffset.end(), offset,
                              [](uint64_t off, const IndexScanPartition& p) { return off < p.partitionOffset; });
  if (it == partitionsByOffset.begin()) return true;
  --it;
  return it->bodySid != 0 && offset >= it->essenceStartOffset;
}

// Appends one segment's edit units as absolute {editUnit, byteOffset,
// isKeyframe} rows. CBR iteration count is derived from how many edit
// units could possibly fit within the file's actual size (rather than
// trusting a possibly-corrupt/huge IndexDuration directly), so a malformed
// segment can't turn into an unbounded loop -- also the *only* count
// available when IndexDuration is left at 0 (real bug found on a real
// XAVC-Intra file: a perfectly valid, correctly CBR-flagged Index Table
// Segment left IndexDuration unspecified/0, which used to make this whole
// segment -- despite its otherwise-usable EditUnitByteCount -- get
// silently discarded rather than falling back to this same file-size-based
// cap). That cap counts to the end of the file, so the edit units it
// adds past the essence -- e.g. one landing on the Footer Partition Pack
// or the RIP, observed on 238 real files with IndexDuration 0 -- are
// dropped via LiesInPartitionEssence.
void FlattenSegmentInto(const IndexTableSegment& seg, uint64_t fileSize, const std::vector<BodyRange>& ranges,
                        const std::vector<IndexScanPartition>& partitionsByOffset, std::vector<FlatIndexEntry>& out) {
  if (seg.editUnitByteCount != 0) {
    if (seg.essenceContainerBaseOffset >= fileSize) return;
    const uint64_t maxIterationsThatFit =
        (fileSize - seg.essenceContainerBaseOffset) / seg.editUnitByteCount + 1;
    const int64_t iterations = seg.indexDuration > 0
                                    ? std::min<int64_t>(seg.indexDuration, static_cast<int64_t>(maxIterationsThatFit))
                                    : static_cast<int64_t>(maxIterationsThatFit);
    for (int64_t i = 0; i < iterations; ++i) {
      const int64_t editUnit = seg.indexStartPosition + i;
      // CBR StreamOffsets aren't stored explicitly (unlike VBR's
      // IndexEntryArray) -- by convention they're `editUnit *
      // editUnitByteCount`, uniform from edit unit 0 of the logical
      // essence container.
      const uint64_t logicalOffset = static_cast<uint64_t>(editUnit) * static_cast<uint64_t>(seg.editUnitByteCount);
      const uint64_t offset = ResolveLogicalEssenceOffset(logicalOffset, ranges, seg.essenceContainerBaseOffset);
      if (offset >= fileSize || !LiesInPartitionEssence(offset, partitionsByOffset)) continue;
      out.push_back({editUnit, offset, true, 0});
    }
    return;
  }

  // VBR: one row per parsed IndexEntryArray entry, in order starting at
  // indexStartPosition. Each entry checked independently (a single bad
  // StreamOffset doesn't imply subsequent ones are also bad).
  for (size_t i = 0; i < seg.entries.size(); ++i) {
    const uint64_t offset =
        ResolveLogicalEssenceOffset(seg.entries[i].streamOffset, ranges, seg.essenceContainerBaseOffset);
    if (offset >= fileSize) continue;
    out.push_back({seg.indexStartPosition + static_cast<int64_t>(i), offset, IsKeyframeFlags(seg.entries[i].flags),
                   seg.entries[i].flags, seg.entries[i].keyFrameOffset});
  }
}

}  // namespace

namespace {

// The reader-independent tail shared by both BuildEssenceIndex (sequential,
// works for local files and as the network fallback) and
// BuildEssenceIndexFromHttp (parallel, HTTP-only, see below): given every
// scanned partition and every already-bodySid-filtered segment found across
// them, resolves absolute byte offsets and flattens into the final,
// queryable EssenceIndex. Pulled out verbatim from BuildEssenceIndex's own
// former second half so both gathering strategies produce identical results
// from identical (scanPartitions, segments) input.
std::optional<EssenceIndex> FinishBuildingIndex(const std::vector<IndexScanPartition>& scanPartitions,
                                                 std::vector<IndexTableSegment> segments, uint64_t fileSize,
                                                 uint32_t videoBodySid) {
  if (segments.empty()) return std::nullopt;

  // Every essence-bearing partition for videoBodySid, sorted by BodyOffset
  // (its position within the logical, cross-partition essence container) --
  // see ResolveLogicalEssenceOffset. A partition only actually carries
  // BodySID's essence if its own Partition Pack claims that exact BodySID
  // (Header/Footer Partitions that merely host Index Table Segments report
  // BodySID 0 or an unrelated value and are naturally excluded).
  //
  // Stable, like segments/flat below: a well-formed file never has two
  // partitions genuinely sharing one BodyOffset, but a malformed one might
  // (or ResyncToActualEssenceStart guessing wrong for one of them) -- stable
  // sort at least makes "first-enumerated-partition wins" well-defined
  // instead of leaving a tie to std::sort's unspecified order.
  std::vector<BodyRange> bodyRanges;
  for (const auto& partition : scanPartitions) {
    if (partition.bodySid == 0) continue;  // never a partition's own essence, only an index/metadata host
    if (videoBodySid != 0 && partition.bodySid != videoBodySid) continue;
    bodyRanges.push_back({partition.bodyOffset, partition.essenceStartOffset});
  }
  std::stable_sort(bodyRanges.begin(), bodyRanges.end(),
                   [](const BodyRange& a, const BodyRange& b) { return a.bodyOffset < b.bodyOffset; });

  // Stable sorts throughout: on a genuine tie (overlapping/duplicate
  // segments sharing an IndexStartPosition, or two segments producing the
  // same editUnit), this makes "first-file-order/first-scanned-partition
  // wins" a well-defined guarantee rather than leaving it to an unspecified
  // sort implementation detail.
  std::stable_sort(segments.begin(), segments.end(), [](const IndexTableSegment& a, const IndexTableSegment& b) {
    return a.indexStartPosition < b.indexStartPosition;
  });

  std::vector<IndexScanPartition> partitionsByOffset = scanPartitions;
  std::sort(partitionsByOffset.begin(), partitionsByOffset.end(),
            [](const IndexScanPartition& a, const IndexScanPartition& b) { return a.partitionOffset < b.partitionOffset; });

  std::vector<FlatIndexEntry> flat;
  for (const auto& seg : segments) FlattenSegmentInto(seg, fileSize, bodyRanges, partitionsByOffset, flat);
  if (flat.empty()) return std::nullopt;

  std::stable_sort(flat.begin(), flat.end(),
                    [](const FlatIndexEntry& a, const FlatIndexEntry& b) { return a.editUnit < b.editUnit; });
  flat.erase(
      std::unique(flat.begin(), flat.end(),
                  [](const FlatIndexEntry& a, const FlatIndexEntry& b) { return a.editUnit == b.editUnit; }),
      flat.end());

  return EssenceIndex(std::move(flat));
}

}  // namespace

std::optional<EssenceIndex> BuildEssenceIndex(MxfReader& reader, uint32_t videoBodySid) {
  std::vector<IndexScanPartition> scanPartitions = EnumeratePartitionsForIndex(reader);

  std::vector<IndexTableSegment> segments;
  for (const auto& partition : scanPartitions) {
    for (auto& seg : ScanPartitionForIndexSegments(reader, partition)) {
      if (videoBodySid != 0 && seg.bodySid != 0 && seg.bodySid != videoBodySid) continue;
      segments.push_back(std::move(seg));
    }
  }
  return FinishBuildingIndex(scanPartitions, std::move(segments), reader.Size(), videoBodySid);
}

namespace {

// First guess at how much of a partition's Index Table data to download in
// one shot -- covers the common case (a handful of Index Table Segments) in
// a single request. See FetchAndParsePartitionForIndex's growth strategy
// below for what happens when it isn't enough.
constexpr uint64_t kIndexFetchInitialChunk = 100 * 1024;
// Step size for blind growth (used before the Partition Pack itself has even
// been parsed, so IndexByteCount isn't known yet) and the minimum step once
// it has -- paired with doubling below so a handful of retries can still
// reach a multi-MB gap without needing dozens of round trips for it.
constexpr uint64_t kIndexFetchGrowChunk = 100 * 1024;
// Doubling converges to even a many-MB gap (e.g. a Footer Partition whose
// own redundant Header Metadata copy + RIP sit between its Index Table
// Segment and EOF) within a handful of steps; this bounds the pathological
// case (a badly malformed partition whose own declared sizes are wildly
// wrong) from growing forever. Growth is separately capped at `hardCap`
// (the next partition's own offset, or file size for the last partition)
// regardless of this limit -- a partition's content can never legitimately
// extend past that point.
constexpr int kIndexFetchMaxGrowSteps = 24;

struct PartitionFetchResult {
  IndexScanPartition partition;
  std::vector<IndexTableSegment> segments;
};

// Downloads and parses one partition's worth of Index Table data, growing
// the download as needed: first `kIndexFetchInitialChunk` bytes from
// `offset` (per the CMC/Omneon-quirk precedent this project has already hit
// more than once, see index_table.h's own BuildEssenceIndexFromHttp comment
// -- a partition's own declared IndexByteCount can itself undercount reality,
// so it's a hint to grow *toward*, never a guarantee to stop at). After each
// attempt, MemoryRangeByteSource::HitBufferLimit() says whether the parse
// (ReadOnePartitionForIndex + ScanPartitionForIndexSegments, run completely
// unmodified against the in-memory buffer) ran out of downloaded bytes
// somewhere -- if so, grow and re-parse from scratch (cheap: pure in-memory
// CPU work) rather than trusting a result that might be silently truncated.
// `hardCap` bounds how far this will ever grow (see kIndexFetchMaxGrowSteps).
// Returns std::nullopt only if the very first fetch fails outright or the
// downloaded bytes never resolve to a valid Partition Pack at `offset` --
// mirrors ReadOnePartitionForIndex's own "simply omitted, not a hard error"
// contract for a partition that fails to parse.
std::optional<PartitionFetchResult> FetchAndParsePartitionForIndex(
    const std::function<std::optional<std::vector<uint8_t>>(uint64_t, uint64_t)>& fetchRange, uint64_t offset,
    uint64_t hardCap, uint64_t fileSize) {
  if (offset >= fileSize || hardCap <= offset) return std::nullopt;
  const uint64_t cap = std::min(hardCap, fileSize);
  std::vector<uint8_t> buffer;

  auto growTo = [&](uint64_t targetSize) -> bool {
    targetSize = std::min(targetSize, cap - offset);
    if (targetSize <= buffer.size()) return false;
    auto more = fetchRange(offset + buffer.size(), targetSize - buffer.size());
    if (!more || more->empty()) return false;
    buffer.insert(buffer.end(), more->begin(), more->end());
    return true;
  };

  if (!growTo(kIndexFetchInitialChunk)) return std::nullopt;

  for (int step = 0; step <= kIndexFetchMaxGrowSteps; ++step) {
    auto source = std::make_unique<MemoryRangeByteSource>(offset, buffer, fileSize);
    MemoryRangeByteSource* sourcePtr = source.get();
    MxfReader memReader(std::move(source));
    auto scan = ReadOnePartitionForIndex(memReader, offset);
    std::vector<IndexTableSegment> segs;
    if (scan && !sourcePtr->HitBufferLimit()) segs = ScanPartitionForIndexSegments(memReader, *scan);
    if (!sourcePtr->HitBufferLimit()) {
      if (!scan) return std::nullopt;  // genuinely not a valid Partition Pack here, not a buffering issue
      return PartitionFetchResult{*scan, std::move(segs)};
    }

    uint64_t nextTarget = std::max<uint64_t>(buffer.size() + kIndexFetchGrowChunk, buffer.size() * 2);
    if (scan) nextTarget = std::max<uint64_t>(nextTarget, scan->indexByteRangeEnd - offset);
    if (!growTo(nextTarget)) break;  // fetch failed, or already at `cap` -- stop, use what's parseable below
  }

  // Ran out of grow steps (or hit `cap`) still short of a complete parse --
  // fall back to whatever's parseable from the buffer we do have, same
  // tolerant "a malformed/truncated segment is dropped, not fatal"
  // philosophy as the rest of this file, rather than losing the whole
  // partition.
  auto source = std::make_unique<MemoryRangeByteSource>(offset, buffer, fileSize);
  MxfReader memReader(std::move(source));
  auto scan = ReadOnePartitionForIndex(memReader, offset);
  if (!scan) return std::nullopt;
  return PartitionFetchResult{*scan, ScanPartitionForIndexSegments(memReader, *scan)};
}

// Concurrent partition fetches capped at this many in flight -- comfortably
// past what one round trip's latency alone would otherwise force this whole
// build to pay for serially (see BuildEssenceIndexFromHttp's own comment),
// without opening an unreasonable number of simultaneous connections against
// the source.
constexpr size_t kMaxConcurrentPartitionFetches = 20;

}  // namespace

std::optional<EssenceIndex> BuildEssenceIndexFromHttp(const std::string& url, const std::string& httpHeaders,
                                                       uint32_t videoBodySid, uint64_t fileSize,
                                                       std::vector<uint64_t> partitionOffsets) {
  std::sort(partitionOffsets.begin(), partitionOffsets.end());
  partitionOffsets.erase(std::unique(partitionOffsets.begin(), partitionOffsets.end()), partitionOffsets.end());
  if (partitionOffsets.empty() || partitionOffsets.front() != 0) partitionOffsets.insert(partitionOffsets.begin(), 0);

  auto fetchRange = [&](uint64_t off, uint64_t len) -> std::optional<std::vector<uint8_t>> {
    std::string err;
    return HttpGetRange(url, httpHeaders, off, len, &err);
  };

  const size_t n = partitionOffsets.size();
  std::vector<std::optional<PartitionFetchResult>> results(n);
  std::atomic<size_t> nextIndex{0};
  const size_t workerCount = std::min(kMaxConcurrentPartitionFetches, n);
  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t w = 0; w < workerCount; ++w) {
    workers.emplace_back([&] {
      while (true) {
        const size_t i = nextIndex.fetch_add(1);
        if (i >= n) break;
        const uint64_t hardCap = (i + 1 < n) ? partitionOffsets[i + 1] : fileSize;
        results[i] = FetchAndParsePartitionForIndex(fetchRange, partitionOffsets[i], hardCap, fileSize);
      }
    });
  }
  for (auto& t : workers) t.join();

  std::vector<IndexScanPartition> scanPartitions;
  std::vector<IndexTableSegment> segments;
  for (auto& r : results) {
    if (!r) continue;
    scanPartitions.push_back(r->partition);
    for (auto& seg : r->segments) {
      if (videoBodySid != 0 && seg.bodySid != 0 && seg.bodySid != videoBodySid) continue;
      segments.push_back(std::move(seg));
    }
  }
  return FinishBuildingIndex(scanPartitions, std::move(segments), fileSize, videoBodySid);
}

}  // namespace mxfsrc
