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
// 11-byte prefix (TemporalOffset, KeyFrameOffset, Flags, StreamOffset).
// TemporalOffset is still read-and-discarded (unused by any consumer here).
// KeyFrameOffset IS kept: per SMPTE 377-1, it's the offset in edit units
// (coded/stored order) from this entry to the anchor/key frame it actually
// references. Verified against a real Canon XAVC/AVC file: the two leading
// B pictures of a GOP starting at stored position 600 carry KeyFrameOffset
// -13/-14 -- pointing at stored position 588, the *previous* GOP's own
// keyframe (588 = 600 - 12, this file's GOP period), not the nearby
// keyframe at 600 a naive "-1 or -2" would expect. AVC has no bitstream-
// level closed_gop flag (unlike MPEG-2) to detect this any other way --
// see EssenceIndex::NeedsPreviousGop. Any trailing SliceOffset[]/PosTable[]
// bytes (itemLength-11) are skipped via stride arithmetic, never parsed
// individually -- same as FFmpeg.
struct IndexTableEntry {
  int8_t keyFrameOffset = 0;
  uint8_t flags = 0;
  uint64_t streamOffset = 0;  // relative to this segment's essenceContainerBaseOffset
};

// bit4 (backward-prediction-used) and bit5 (forward-prediction-used) both
// clear => I-picture => keyframe (mxfdec.c: `!(flags & 0x30)`). The only
// bit test needed for a "decode forward from here with no reference
// dependency on anything earlier" seek target.
inline bool IsKeyframeFlags(uint8_t flags) { return (flags & 0x30) == 0; }

// bit4 (backward-prediction-used) set at all => a picture displayed out of
// its coded/decode position => needs reorder-buffer accounting, whether or
// not bit5 (forward-prediction-used) is also set. A true P picture only
// ever sets bit5 alone (forward-only, decode position == display position);
// bit4-set-alone is NOT a P picture, it's a one-directional B picture some
// encoders emit for the frame(s) immediately following a keyframe (real
// Canon XAVC long-GOP file: every ordinary B picture in a GOP carries 0x33,
// but that GOP's first one or two carry 0x13 -- bit4 only -- instead;
// treating that as "not a B picture" undercounted
// EssenceIndex::EstimateReorderDelay's run length by exactly that many
// frames at every such GOP's boundary, landing SeekAndDecode on the wrong
// frame right after it). EstimateReorderDelay is the only consumer that
// cares about this distinction at all.
inline bool IsBFrameFlags(uint8_t flags) { return (flags & 0x10) != 0; }

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
  uint64_t essenceStartOffset = 0;   // == indexByteRangeEnd; where this partition's own essence bytes begin
  // A widen-only upper bound for ScanPartitionForIndexSegments's own
  // hardEnd, separate from essenceStartOffset above: the furthest position
  // reached by walking forward from indexByteRangeEnd through genuinely
  // parsed, real (non-essence) KLVs before giving up -- e.g. all the way to
  // true EOF for a Footer Partition, which never precedes essence at all
  // (see index_table.cpp's ResyncToActualEssenceStart). Deliberately NOT
  // folded into essenceStartOffset itself: that field also feeds
  // BuildEssenceIndex's bodyRanges/FlattenSegmentInto for resolving other
  // entries' absolute byte offsets, where a value that's merely "proven
  // safe to scan up to" but not "proven to be this partition's own genuine
  // essence start" would misresolve them once more than one partition
  // shares a BodySID.
  uint64_t scanHardEndBound = 0;
  // This partition's Partition Pack BodyOffset (SMPTE 377-1): this
  // partition's position within the *logical*, cross-partition essence
  // container for bodySid, i.e. the StreamOffset an Index Table Segment
  // entry would have if it pointed at the very first byte this partition
  // physically contributes. Needed to resolve a StreamOffset into an actual
  // file offset when the segment describing it isn't collocated with the
  // essence itself (see BuildEssenceIndex).
  uint64_t bodyOffset = 0;
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
  // Raw IndexTableEntry::flags this row was derived from (0 for CBR rows,
  // which are always synthesized as isKeyframe=true -- see
  // FlattenSegmentInto). Kept alongside the already-collapsed isKeyframe
  // bool so EssenceIndex::EstimateReorderDelay can distinguish a B picture
  // (bit4 AND bit5 both set) from a P picture (only one set) -- a
  // distinction IsKeyframeFlags's single bool deliberately throws away,
  // since nothing else needs it.
  uint8_t flags = 0;
  // SMPTE 377-1 KeyFrameOffset (0 for CBR rows, always intra-only per
  // FlattenSegmentInto's own doc comment, so never need this): the offset
  // in edit units (coded/stored order, the same axis `editUnit` itself is
  // on) from this row to the anchor/key frame it actually references.
  // `editUnit + keyFrameOffset` is that anchor's own absolute position.
  // Used by EssenceIndex::NeedsPreviousGop to detect an AVC open-GOP
  // boundary (no bitstream-level closed_gop flag to read, unlike MPEG-2)
  // -- see there.
  int8_t keyFrameOffset = 0;
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

  // Estimates the B-picture reordering pipeline depth a decoder restarted
  // fresh at `keyframeEditUnit` (which must itself be a keyframe -- normally
  // FindPrecedingKeyframe's own return value) will need before its Nth
  // output genuinely corresponds to input element N -- i.e. how many extra
  // outputs SourceEngine::SeekAndDecode's GOP branch needs to decode (and
  // discard) past its naive target before landing on the real one. Derived
  // straight from the index's own flags, not the decoder: counts the run of
  // consecutive B pictures (both backward- and forward-prediction bits set,
  // SMPTE 377-1 flags bits 4+5) immediately following the keyframe --
  // exactly the reordering depth a standard closed-GOP structure implies
  // (e.g. IBBP has a run of 2, matching a real long-GOP MPEG-2 camera
  // recording this was found against; AVCodecContext::delay/frame pts
  // weren't reliable enough to derive it from the decoder itself for that
  // file's codec). Returns 0 if `keyframeEditUnit` isn't covered/found, or
  // if the run is empty (no reordering, e.g. I/P-only GOPs).
  int EstimateReorderDelay(int64_t keyframeEditUnit) const;

  // True if a decoder restarted fresh at `keyframeEditUnit` cannot correctly
  // reconstruct this GOP alone -- an AVC open GOP, whose leading picture(s)
  // reference the *previous* GOP's own anchor frame for prediction, not
  // anything within this GOP. Unlike MPEG-2, AVC has no bitstream-level
  // closed_gop flag to read (and the MXF header carries nothing equivalent
  // either), so this is inferred the only way available: scanning forward
  // from the keyframe through this GOP (bounded by the next keyframe) for
  // any entry whose actual anchor (editUnit + keyFrameOffset, SMPTE 377-1)
  // falls before keyframeEditUnit itself -- verified against a real file
  // where the leading two B pictures' KeyFrameOffset (-13/-14) pointed at
  // the previous GOP's keyframe, 12 edit units back, not the -1/-2 a closed
  // GOP would carry. Returns false if keyframeEditUnit isn't covered/found,
  // or (CBR rows, always intra-only) keyFrameOffset is always 0.
  bool NeedsPreviousGop(int64_t keyframeEditUnit) const;

  // Returns all index entries. Used by BuildAudioSampleIndexFromIndexTable to
  // build audio indices directly from the video index without essence walking.
  const std::vector<FlatIndexEntry>& GetEntries() const { return entries_; }

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
