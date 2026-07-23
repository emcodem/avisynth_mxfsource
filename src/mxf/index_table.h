#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mxf/klv.h"
#include "mxf/metadata_sets.h"
#include "mxf/partition.h"

namespace mxfsrc {

// Index Table Segment key (SMPTE 377-1 SS10), verified against FFmpeg's
// libavformat/mxfdec.c mxf_metadata_read_table[] -- local-set-coded, shares
// kLocalSetKeyPrefix's 6-byte prefix (partition.h) with Header Metadata
// sets but must be matched as a full 16-byte exact constant, not confused
// with them.
inline constexpr Ul kIndexTableSegmentKey = {0x06, 0x0E, 0x2B, 0x34, 0x02, 0x53, 0x01, 0x01,
                                              0x0D, 0x01, 0x02, 0x01, 0x01, 0x10, 0x01, 0x00};

// One IndexEntryArray row (mxfdec.c mxf_read_index_entry_array): the fixed
// 11-byte prefix (TemporalOffset, KeyFrameOffset, Flags, StreamOffset) --
// TemporalOffset/KeyFrameOffset are read-and-discarded by FFmpeg's own
// decoder too (unused by any consumer for a "nearest preceding stored-order
// keyframe" lookup, only needed for PTS/display-order reordering, which
// this forward-only engine has no use for) so this project doesn't keep
// them either. Any trailing SliceOffset[]/PosTable[] bytes (itemLength-11)
// are skipped via stride arithmetic, never parsed individually -- same as
// FFmpeg.
struct IndexTableEntry {
  uint8_t flags = 0;
  uint64_t streamOffset = 0;  // relative to this segment's essenceContainerBaseOffset
};

// bit4 (backward-prediction-used) and bit5 (forward-prediction-used) both
// clear => I-picture => keyframe (mxfdec.c: `!(flags & 0x30)`). The only
// bit test needed for a "decode forward from here with no reference
// dependency on anything earlier" seek target.
inline bool IsKeyframeFlags(uint8_t flags) { return (flags & 0x30) == 0; }

// One parsed Index Table Segment. A segment covers edit units
// [indexStartPosition, indexStartPosition + indexDuration) for one IndexSID.
struct IndexTableSegment {
  uint32_t indexEditRateNum = 0;
  uint32_t indexEditRateDen = 1;
  int64_t indexStartPosition = 0;
  int64_t indexDuration = 0;
  // 0 => VBR, `entries` is authoritative (StreamOffset per edit unit).
  // Nonzero => CBR, byte offsets are computed from this directly and
  // `entries` is left empty (no IndexEntryArray needed for a CBR segment).
  uint32_t editUnitByteCount = 0;
  uint32_t indexSid = 0;
  uint32_t bodySid = 0;
  std::vector<IndexTableEntry> entries;

  // Byte offset this segment's edit-unit-0-relative StreamOffsets (or the
  // CBR formula) are added to -- not a field of the segment itself (SMPTE
  // leaves it implicit, derived from the essence container's start within
  // whichever partition the segment's BodySID maps to). Stamped in by
  // ScanPartitionForIndexSegments.
  uint64_t essenceContainerBaseOffset = 0;
};

// Parses one Index Table Segment's local tags (0x3F05 EditUnitByteCount,
// 0x3F06 IndexSID, 0x3F07 BodySID, 0x3F0A IndexEntryArray, 0x3F0B
// IndexEditRate, 0x3F0C IndexStartPosition, 0x3F0D IndexDuration) given its
// already-read KlvHeader (caller must have confirmed
// UlEquals(klv.key, kIndexTableSegmentKey) first, mirroring
// ReadPartitionPackValue's contract). Reuses ReadLocalSet/GetU32/GetI64/
// GetRational32 verbatim. Returns std::nullopt if IndexStartPosition or
// IndexDuration -- the segment's core identity -- can't be read; a segment
// missing either is treated as malformed rather than partially usable.
std::optional<IndexTableSegment> ReadIndexTableSegmentValue(MxfReader& reader, const KlvHeader& klv);

// IndexEntryArray batch reader (mxfdec.c mxf_read_index_entry_array):
// {4-byte count}{4-byte itemLength} header, then `count` entries of
// itemLength bytes each (itemLength >= 11; anything beyond the fixed
// 11-byte prefix is trailing SliceOffset[]/PosTable[] data, skipped via the
// per-entry stride, never parsed). NOT reusable from GetUlArray
// (metadata_sets.h), which hardcodes itemSize==16 (UL-array batches only).
// Returns an empty vector on any structural problem (absent tag, itemLength
// < 11, declared count*itemLength exceeding the buffered bytes).
std::vector<IndexTableEntry> GetIndexEntryArray(const LocalTagMap& set, uint16_t tag);

// One physical partition's Index Table geometry, as needed to locate and
// interpret any Index Table Segment(s) it carries.
struct IndexScanPartition {
  uint64_t partitionOffset = 0;
  uint32_t bodySid = 0;
  uint32_t indexSid = 0;
  uint64_t indexByteRangeStart = 0;  // partitionEnd + headerByteCount
  uint64_t indexByteRangeEnd = 0;    // indexByteRangeStart + indexByteCount
  uint64_t essenceStartOffset = 0;   // == indexByteRangeEnd; base for this partition's essence StreamOffsets
};

// Enumerates every partition worth scanning for Index Table Segments: the
// Header Partition (always at offset 0, always included even if the RIP is
// absent/unreadable -- covers real files, e.g. Atomos Shogun XAVC
// recordings, that embed essence+index directly in the Header Partition
// with no separate Body Partition at all) plus every partition
// ReadRandomIndexPack (partition.h) reports, deduplicated by
// partitionOffset. Each partition is re-read independently via its own
// Seek + ReadKlvHeader + ReadPartitionPackValue -- same "cheap to re-read,
// no shared state with ParseMxfHeader's own pass" pattern as
// header_parser.cpp's TryOverrideFromFooterPartition. A partition whose
// Partition Pack fails to parse is simply omitted, not a hard error.
std::vector<IndexScanPartition> EnumeratePartitionsForIndex(MxfReader& reader);

// Scans [indexByteRangeStart, indexByteRangeEnd) for Index Table Segment
// KLV(s) -- mirrors header_parser.cpp's ReadMetadataSets scan-a-byte-range
// pattern (tolerates KLV Fill padding the same way). There can legitimately
// be more than one Index Table Segment back-to-back in a single partition.
// Each successfully-parsed segment gets essenceContainerBaseOffset stamped
// from `partition.essenceStartOffset`. A malformed segment is dropped
// (skipped, scan continues) rather than aborting the whole scan.
std::vector<IndexTableSegment> ScanPartitionForIndexSegments(MxfReader& reader, const IndexScanPartition& partition);

// One flattened, absolute lookup row: `editUnit` is the video essence
// element index (the same space SourceEngine::SeekAndDecode's
// `essenceTarget` uses), `byteOffset` is the absolute file offset where
// that edit unit starts.
struct FlatIndexEntry {
  int64_t editUnit = 0;
  uint64_t byteOffset = 0;
  bool isKeyframe = false;
};

// A resolved, queryable index for one video track (one BodySID), built once
// and never mutated afterward.
class EssenceIndex {
 public:
  EssenceIndex() = default;
  // Constructs directly from a flattened, sorted-by-editUnit,
  // unique-by-editUnit entry list -- used by BuildEssenceIndex, and
  // directly by tests exercising FindPrecedingKeyframe's lookup logic
  // without needing to round-trip through a synthetic file.
  explicit EssenceIndex(std::vector<FlatIndexEntry> entries) : entries_(std::move(entries)) {}

  bool Empty() const { return entries_.empty(); }

  // Highest editUnit covered by any segment, or -1 if empty. Used to derive
  // a frame count when Header Metadata's own Duration is absent/unreliable.
  int64_t HighestCoveredEditUnit() const;

  struct SeekPoint {
    int64_t editUnit;
    uint64_t byteOffset;
  };

  // Locates the entry at exactly `targetEditUnit` (O(log n) via
  // std::upper_bound), then walks backward while !isKeyframe (bounded by
  // GOP length -- a handful to a few dozen entries, never O(n)). Returns
  // std::nullopt if `targetEditUnit` isn't covered by any parsed segment
  // (a gap, or before/after every covered range), or if no keyframe exists
  // anywhere at or before it within the covered range.
  std::optional<SeekPoint> FindPrecedingKeyframe(int64_t targetEditUnit) const;

 private:
  std::vector<FlatIndexEntry> entries_;  // sorted ascending & unique by editUnit
};

// Full pipeline: enumerate partitions -> scan each for Index Table
// Segment(s) matching `videoBodySid` -> sort matched segments by
// IndexStartPosition -> flatten each into FlatIndexEntry rows (VBR via the
// segment's own parsed entries; CBR via the O(1) formula, every synthesized
// row stamped isKeyframe=true, since every CBR essence class this project's
// ul_tables.h recognizes is already IsIntraOnlyCodec==true and doesn't need
// this index anyway -- this is a documented simplifying assumption, not a
// general truth about CBR streams) -> final sort+unique by editUnit
// (first-file-order wins on any overlap) -> drop any row whose byteOffset
// is out of the file's bounds. Returns std::nullopt (not an error) if
// nothing usable was found for `videoBodySid` -- callers must treat that as
// "index unavailable, use the legacy from-0 decode path", never a hard
// failure.
std::optional<EssenceIndex> BuildEssenceIndex(MxfReader& reader, uint32_t videoBodySid);

}  // namespace mxfsrc
