// Phase 8 item 2 unit tests for src/mxf/index_table.*: parsing an Index
// Table Segment's local tags and IndexEntryArray batch, the flattened
// EssenceIndex's nearest-preceding-keyframe lookup, and the full
// BuildEssenceIndex pipeline (partition enumeration + segment scan +
// CBR/VBR flatten + multi-segment stitching), against hand-crafted
// synthetic files -- see synthetic_mxf.h's AppendIndexTableSegment et al.
#include <filesystem>
#include <iostream>

#include "mxf/index_table.h"
#include "synthetic_mxf.h"

using namespace mxfsrc;
using namespace mxfsrc_test;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

// Writes a single Index Table Segment KLV (no enclosing partition) and
// reads it back via ReadKlvHeader + ReadIndexTableSegmentValue -- exercises
// the low-level parser directly, independent of partition scanning.
std::optional<IndexTableSegment> RoundTripSegment(const IndexTableSegmentSpec& spec, const char* fileName) {
  std::vector<uint8_t> bytes;
  AppendIndexTableSegment(bytes, spec);
  auto path = WriteTempFile(bytes, fileName);

  std::optional<IndexTableSegment> result;
  {
    MxfReader reader(path.string());
    KlvHeader klv{};
    if (ReadKlvHeader(reader, klv) && UlEquals(klv.key, kIndexTableSegmentKey)) {
      result = ReadIndexTableSegmentValue(reader, klv);
    }
  }
  std::filesystem::remove(path);
  return result;
}

void TestVbrSegmentRoundTrip() {
  IndexTableSegmentSpec spec;
  spec.indexEditRateNum = 25;
  spec.indexEditRateDen = 1;
  spec.indexStartPosition = 100;
  spec.indexDuration = 3;
  spec.editUnitByteCount = 0;
  spec.indexSid = 5;
  spec.bodySid = 7;
  spec.entries = {{0x00, 1000}, {0x22, 2000}, {0x00, 3000}};

  auto seg = RoundTripSegment(spec, "mxfsrc_test_its_vbr_roundtrip.mxf");
  CHECK(seg.has_value());
  if (!seg) return;
  CHECK(seg->indexEditRateNum == 25 && seg->indexEditRateDen == 1);
  CHECK(seg->indexStartPosition == 100);
  CHECK(seg->indexDuration == 3);
  CHECK(seg->editUnitByteCount == 0);
  CHECK(seg->indexSid == 5);
  CHECK(seg->bodySid == 7);
  CHECK(seg->entries.size() == 3);
  if (seg->entries.size() == 3) {
    CHECK(seg->entries[0].flags == 0x00 && seg->entries[0].streamOffset == 1000);
    CHECK(seg->entries[1].flags == 0x22 && seg->entries[1].streamOffset == 2000);
    CHECK(seg->entries[2].flags == 0x00 && seg->entries[2].streamOffset == 3000);
  }
}

// itemLength=15 (11 fixed + a 4-byte SliceOffset) -- proves the batch reader
// skips the trailing bytes via stride arithmetic without desyncing the next
// entry's fields.
void TestIndexEntryArrayItemLength15SkipsTrailingBytes() {
  IndexTableSegmentSpec spec;
  spec.indexStartPosition = 0;
  spec.indexDuration = 2;
  spec.editUnitByteCount = 0;
  spec.indexEntryItemLength = 15;
  spec.entries = {{0x00, 500}, {0x30, 600}};

  auto seg = RoundTripSegment(spec, "mxfsrc_test_its_itemlen15.mxf");
  CHECK(seg.has_value());
  if (!seg) return;
  CHECK(seg->entries.size() == 2);
  if (seg->entries.size() == 2) {
    CHECK(seg->entries[0].flags == 0x00 && seg->entries[0].streamOffset == 500);
    CHECK(seg->entries[1].flags == 0x30 && seg->entries[1].streamOffset == 600);
  }
}

void TestIsKeyframeFlagsTruthTable() {
  CHECK(IsKeyframeFlags(0x00));
  CHECK(!IsKeyframeFlags(0x10));
  CHECK(!IsKeyframeFlags(0x20));
  CHECK(!IsKeyframeFlags(0x30));
  CHECK(IsKeyframeFlags(0x80));  // random-access marker bit, unrelated to the 0x30 prediction bits
}

// A real Canon XAVC long-GOP file flags the first one or two B pictures
// immediately after a keyframe as backward-prediction-only (0x13: bit4 set,
// bit5 clear) rather than the 0x33 (both bits set) every other B picture in
// the same GOP carries. EstimateReorderDelay's run-length count must still
// include these -- they're still displayed out of decode order and held in
// the reorder buffer, just referencing one direction -- or it silently
// undercounts the GOP's true reorder depth and SeekAndDecode lands on the
// wrong frame right after this GOP's keyframe.
void TestEstimateReorderDelayCountsBackwardOnlyBFrames() {
  // byteOffsets for 101/102 deliberately NOT monotonic with editUnit (102
  // physically stored *before* 101) -- this is what actually makes this a
  // genuine-reordering scenario needing EstimateReorderDelay's real,
  // non-zero return: monotonically-increasing byteOffsets (matching
  // editUnit order) are the exact signal for "low-delay" B-pictures that
  // need NO reorder-delay compensation at all (see EstimateReorderDelay's
  // own doc comment, root-caused against a real broadcast file whose Index
  // Table byte offsets increase strictly with editUnit throughout).
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {100, 1000, true, 0x40},   // I
      {101, 1200, false, 0x13},  // backward-only B (the real file's anomaly) -- physically after 102
      {102, 1100, false, 0x13},  // backward-only B -- physically before 101 (non-monotonic -> genuine reorder)
      {103, 1300, false, 0x26},  // P
      {104, 1400, false, 0x33},  // ordinary B
      {105, 1500, false, 0x33},  // ordinary B
      {106, 1600, false, 0x26},  // P
  });
  CHECK(index.EstimateReorderDelay(100) == 2);
}

// Regression test: a real broadcast MXF file (D:\Vistek_DMX_-1ms.mxf,
// MPEG-2 long-GOP) has B pictures whose Index Table byteOffsets increase
// strictly with editUnit for the file's entire length -- confirmed via
// read_test_mxf's index-scan tool directly against the real file -- meaning
// decode order already equals display order (a "low-delay" bi-predictive
// structure, same class of file as the Canon XAVC repro documented in
// handoff.md's Handoff 1/2). EstimateReorderDelay's original run-length
// count (purely flag-based, blind to actual physical ordering) returned a
// nonzero delay for this file anyway, causing DecodeThreadMain's
// `pending`/`pendingAudio` position-shifting to misalign BOTH audio and
// video for the file's *entire* length, not just its leading edge --
// confirmed via byte-comparison against ffmpeg's own independent MXF demux.
void TestEstimateReorderDelayReturnsZeroForLowDelayMonotonicByteOffsets() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {200, 2000, true, 0x40},   // I
      {201, 2100, false, 0x33},  // B -- physically stored in display order (monotonic)
      {202, 2200, false, 0x33},  // B -- still monotonic
      {203, 2300, false, 0x26},  // P
  });
  CHECK(index.EstimateReorderDelay(200) == 0);
}

// Regression test for the real-file tail-EOF bug (2026-08-14 handoff):
// D:\Vistek_DMX_-1ms.mxf's every GOP boundary carries this exact open-GOP
// pattern (verified via read_test_mxf's index-scan tool against the real
// file, absolute edit units 7380-7383) -- the keyframe's own two leading B
// pictures reference the *previous* GOP's keyframe (13/14 edit units back,
// this file's GOP period), not this GOP's own keyframe. NeedsPreviousGop
// only detects this (bool); CountUndecodableLeadingPictures must count
// exactly how many, since SeekAndDecode's restart-one-GOP-back workaround
// still never decodes whatever those leading pictures actually reference,
// silently costing that many missing outputs that must be added back into
// SeekAndDecode's iteration count (see ResolveSeekStart's reorderDelay).
void TestCountUndecodableLeadingPicturesMatchesRealFileOpenGopPattern() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {7368, 100000, true, 0x40, 0},     // I (previous GOP's own keyframe)
      {7380, 200000, true, 0x40, 0},     // I (this GOP's keyframe)
      {7381, 200100, false, 0x33, -13},  // B, anchor=7381-13=7368 -- previous GOP
      {7382, 200200, false, 0x33, -14},  // B, anchor=7382-14=7368 -- previous GOP
      {7383, 200300, false, 0x22, -3},   // P, anchor=7383-3=7380 -- this GOP, fine
      {7384, 200400, false, 0x33, -4},   // B, anchor=7384-4=7380 -- this GOP, fine
  });
  CHECK(index.NeedsPreviousGop(7380));
  CHECK(index.CountUndecodableLeadingPictures(7380) == 2);
  CHECK(index.CountUndecodableLeadingPictures(7368) == 0);  // closed GOP: nothing to count
}

void TestFindPrecedingKeyframeTargetIsAlreadyKeyframe() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {10, 1000, true},
      {11, 1100, false},
      {12, 1200, false},
  });
  auto point = index.FindPrecedingKeyframe(10);
  CHECK(point.has_value());
  if (point) {
    CHECK(point->editUnit == 10);
    CHECK(point->byteOffset == 1000);
  }
}

void TestFindPrecedingKeyframeWalksBackToEarlierKeyframe() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {10, 1000, true},
      {11, 1100, false},
      {12, 1200, false},
  });
  auto point = index.FindPrecedingKeyframe(12);
  CHECK(point.has_value());
  if (point) {
    CHECK(point->editUnit == 10);
    CHECK(point->byteOffset == 1000);
  }
}

void TestFindPrecedingKeyframeNoKeyframeBeforeTarget() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {10, 1000, false},
      {11, 1100, false},
  });
  CHECK(!index.FindPrecedingKeyframe(11).has_value());
}

void TestFindPrecedingKeyframeOutsideCoveredRange() {
  EssenceIndex index(std::vector<FlatIndexEntry>{
      {10, 1000, true},
      {11, 1100, false},
  });
  CHECK(!index.FindPrecedingKeyframe(5).has_value());   // before first covered edit unit
  CHECK(!index.FindPrecedingKeyframe(20).has_value());  // after last covered edit unit
  // A gap in the middle (12..14 uncovered) also resolves to nullopt rather
  // than silently snapping to the nearest earlier entry outside its range.
  EssenceIndex withGap(std::vector<FlatIndexEntry>{
      {10, 1000, true},
      {15, 1500, false},
  });
  CHECK(!withGap.FindPrecedingKeyframe(12).has_value());
}

// Builds a minimal file with one Partition Pack whose declared
// headerByteCount/indexByteCount regions are exactly where BuildEssenceIndex
// expects Header Metadata (unread -- just skipped) and Index Table
// Segment(s) (read) to live, mirroring the real [partitionEnd,
// partitionEnd+headerByteCount) / [.., ..+indexByteCount) layout. BodySID is
// always 1 (AppendPartitionPackWithFields hardcodes it), so tests
// consistently pass videoBodySid=1 to BuildEssenceIndex.
std::vector<uint8_t> BuildSingleIndexPartitionFile(const std::vector<uint8_t>& indexRegionBytes,
                                                     uint64_t headerByteCount, uint32_t indexSid,
                                                     size_t essenceTrailerSize) {
  std::vector<uint8_t> file;
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete,
                                  /*thisPartition=*/0, /*footerPartitionOffset=*/0, headerByteCount,
                                  indexRegionBytes.size(), indexSid);
  file.insert(file.end(), headerByteCount, 0);
  file.insert(file.end(), indexRegionBytes.begin(), indexRegionBytes.end());
  file.insert(file.end(), essenceTrailerSize, 0);
  return file;
}

void TestCbrSegmentFlattenFormula() {
  IndexTableSegmentSpec spec;
  spec.indexStartPosition = 0;
  spec.indexDuration = 5;
  spec.editUnitByteCount = 100;  // CBR
  spec.indexSid = 1;
  spec.bodySid = 1;

  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, spec);
  auto bytes = BuildSingleIndexPartitionFile(segBytes, /*headerByteCount=*/64, /*indexSid=*/1,
                                               /*essenceTrailerSize=*/1000);
  auto path = WriteTempFile(bytes, "mxfsrc_test_its_cbr.mxf");

  {
    MxfReader reader(path.string());
    auto index = BuildEssenceIndex(reader, /*videoBodySid=*/1);
    CHECK(index.has_value());
    if (index) {
      CHECK(index->HighestCoveredEditUnit() == 4);
      for (int64_t i = 0; i <= 4; ++i) {
        auto point = index->FindPrecedingKeyframe(i);
        CHECK(point.has_value());
        if (point) CHECK(point->editUnit == i);  // every CBR row is a keyframe -- resolves to itself
      }
    }
  }
  std::filesystem::remove(path);
}

void TestNoIndexTableAnywhereReturnsNullopt() {
  std::vector<uint8_t> file;
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0,
                                  /*headerByteCount=*/64, /*indexByteCount=*/0, /*indexSid=*/0);
  file.insert(file.end(), 64, 0);
  auto path = WriteTempFile(file, "mxfsrc_test_its_none.mxf");

  {
    MxfReader reader(path.string());
    CHECK(!BuildEssenceIndex(reader, 1).has_value());
  }
  std::filesystem::remove(path);
}

// A segment missing IndexStartPosition/IndexDuration (its core identity
// fields) is malformed -- ReadIndexTableSegmentValue must reject it, and
// BuildEssenceIndex must treat that as "no usable segment" rather than
// aborting or crashing.
void TestMalformedSegmentIsSkippedNotFatal() {
  std::vector<uint8_t> malformed;
  {
    std::vector<uint8_t> entries;
    AppendTagU32(entries, 0x3F05, 0);  // EditUnitByteCount only -- no IndexStartPosition/IndexDuration
    AppendKlvHeader(malformed, kIndexTableSegmentKey, entries.size());
    malformed.insert(malformed.end(), entries.begin(), entries.end());
  }

  auto path1 = WriteTempFile(BuildSingleIndexPartitionFile(malformed, 64, 1, 100), "mxfsrc_test_its_malformed_only.mxf");
  {
    // Sub-case A: only segment present, malformed -> nullopt overall.
    MxfReader reader(path1.string());
    CHECK(!BuildEssenceIndex(reader, 1).has_value());
  }
  std::filesystem::remove(path1);

  {
    // Sub-case B: malformed segment in partition 1, a valid VBR segment in
    // partition 2 -- the malformed one is dropped, the valid one still used.
    std::vector<uint8_t> file;
    AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0, 64,
                                    malformed.size(), 1);
    file.insert(file.end(), 64, 0);
    file.insert(file.end(), malformed.begin(), malformed.end());
    file.insert(file.end(), 100, 0);

    const uint64_t partition2Offset = file.size();
    IndexTableSegmentSpec goodSpec;
    goodSpec.indexStartPosition = 0;
    goodSpec.indexDuration = 2;
    goodSpec.indexSid = 1;
    goodSpec.bodySid = 1;
    goodSpec.entries = {{0x00, 50}, {0x00, 150}};
    std::vector<uint8_t> goodBytes;
    AppendIndexTableSegment(goodBytes, goodSpec);
    AppendPartitionPackWithFields(file, PartitionKind::kBody, TestPartitionStatus::kClosedComplete, partition2Offset,
                                    0, 64, goodBytes.size(), 1);
    file.insert(file.end(), 64, 0);
    file.insert(file.end(), goodBytes.begin(), goodBytes.end());
    file.insert(file.end(), 400, 0);  // must exceed the largest streamOffset (150) used above

    AppendRandomIndexPack(file, {{1, 0}, {1, partition2Offset}});

    auto path = WriteTempFile(file, "mxfsrc_test_its_malformed_one_of_two.mxf");
    {
      MxfReader reader(path.string());
      auto index = BuildEssenceIndex(reader, 1);
      CHECK(index.has_value());
      if (index) {
        CHECK(index->HighestCoveredEditUnit() == 1);
        CHECK(index->FindPrecedingKeyframe(0).has_value());
        CHECK(index->FindPrecedingKeyframe(1).has_value());
      }
    }
    std::filesystem::remove(path);
  }
}

// One entry's resolved byte offset (essenceContainerBaseOffset +
// streamOffset) lands past the file's actual size -- dropped at build time,
// the other (in-bounds) entries remain usable.
void TestOutOfBoundsOffsetIsDropped() {
  IndexTableSegmentSpec spec;
  spec.indexStartPosition = 0;
  spec.indexDuration = 2;
  spec.indexSid = 1;
  spec.bodySid = 1;
  spec.entries = {{0x00, 50}, {0x00, 10000000}};  // second entry's offset will exceed the tiny test file's size

  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, spec);
  auto bytes = BuildSingleIndexPartitionFile(segBytes, 64, 1, /*essenceTrailerSize=*/100);
  auto path = WriteTempFile(bytes, "mxfsrc_test_its_oob_offset.mxf");

  {
    MxfReader reader(path.string());
    auto index = BuildEssenceIndex(reader, 1);
    CHECK(index.has_value());
    if (index) {
      CHECK(index->FindPrecedingKeyframe(0).has_value());   // in-bounds entry kept
      CHECK(!index->FindPrecedingKeyframe(1).has_value());  // out-of-bounds entry dropped
    }
  }
  std::filesystem::remove(path);
}

// Two Index Table Segments in two different partitions, covering disjoint
// IndexStartPosition ranges (0..2 and 3..5) -- a target in the second
// segment's range must resolve using its own offsets, not incorrectly via
// the first segment's.
void TestMultiSegmentStitching() {
  std::vector<uint8_t> file;

  IndexTableSegmentSpec segA;
  segA.indexStartPosition = 0;
  segA.indexDuration = 3;
  segA.indexSid = 1;
  segA.bodySid = 1;
  segA.entries = {{0x00, 100}, {0x30, 200}, {0x30, 300}};
  std::vector<uint8_t> segABytes;
  AppendIndexTableSegment(segABytes, segA);
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0, 64,
                                  segABytes.size(), 1);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segABytes.begin(), segABytes.end());
  const uint64_t essenceBaseA = file.size();  // == segment A's essenceContainerBaseOffset once parsed
  constexpr uint64_t kPartitionAEssenceBytes = 400;
  file.insert(file.end(), kPartitionAEssenceBytes, 0);  // essence trailer covering segment A's offsets

  const uint64_t partition2Offset = file.size();
  IndexTableSegmentSpec segB;
  segB.indexStartPosition = 3;
  segB.indexDuration = 3;
  segB.indexSid = 1;
  segB.bodySid = 1;
  // StreamOffset is defined relative to the whole logical, cross-partition
  // essence container (SMPTE 377-1), not to this partition's own essence --
  // so these continue on from where partition A's own BodyOffset range
  // (0..kPartitionAEssenceBytes) leaves off, landing at
  // essenceBaseB+50/150/250 once resolved via BodyOffset.
  segB.entries = {{0x00, kPartitionAEssenceBytes + 50},
                   {0x30, kPartitionAEssenceBytes + 150},
                   {0x30, kPartitionAEssenceBytes + 250}};
  std::vector<uint8_t> segBBytes;
  AppendIndexTableSegment(segBBytes, segB);
  // BodyOffset: partition A's own essence byte count, i.e. where partition B's
  // essence genuinely begins within the logical, cross-partition essence
  // container -- must be distinct from partition A's own BodyOffset (0) or
  // BuildEssenceIndex's bodyRanges sort can't tell the two partitions apart
  // (a real bug this test previously tripped over: AppendPartitionPackWithFields
  // used to hardcode BodyOffset=0 for every partition it built, so this and
  // TestOverlappingSegmentsDeterministicFirstWins both ended up with two
  // BodyOffset=0 entries and the resolved offset silently depended on
  // std::sort's unspecified tie-breaking between them).
  AppendPartitionPackWithFields(file, PartitionKind::kBody, TestPartitionStatus::kClosedComplete, partition2Offset, 0,
                                  64, segBBytes.size(), 1, kPartitionAEssenceBytes);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segBBytes.begin(), segBBytes.end());
  const uint64_t essenceBaseB = file.size();  // == this segment's essenceContainerBaseOffset once parsed
  file.insert(file.end(), 400, 0);

  AppendRandomIndexPack(file, {{1, 0}, {1, partition2Offset}});

  auto path = WriteTempFile(file, "mxfsrc_test_its_multi_segment.mxf");
  {
    MxfReader reader(path.string());
    auto index = BuildEssenceIndex(reader, 1);
    CHECK(index.has_value());
    if (index) {
      CHECK(index->HighestCoveredEditUnit() == 5);
      // editUnit 5 (in segment B) must walk back to segment B's own
      // keyframe at editUnit 3, not incorrectly resolve into segment A.
      auto point = index->FindPrecedingKeyframe(5);
      CHECK(point.has_value());
      if (point) {
        CHECK(point->editUnit == 3);
        CHECK(point->byteOffset == essenceBaseB + 50);
      }
      // editUnit 2 (in segment A) resolves within segment A, against
      // partition A's own essence base -- not partition B's.
      auto pointA = index->FindPrecedingKeyframe(2);
      CHECK(pointA.has_value());
      if (pointA) {
        CHECK(pointA->editUnit == 0);
        CHECK(pointA->byteOffset == essenceBaseA + 100);
      }
    }
  }
  std::filesystem::remove(path);
}

// Two segments deliberately overlapping the same IndexStartPosition range --
// verifies deterministic first-file-order-wins dedupe, no crash.
void TestOverlappingSegmentsDeterministicFirstWins() {
  std::vector<uint8_t> file;

  IndexTableSegmentSpec segA;
  segA.indexStartPosition = 0;
  segA.indexDuration = 2;
  segA.indexSid = 1;
  segA.bodySid = 1;
  segA.entries = {{0x00, 111}, {0x30, 222}};  // "first" -- should win
  std::vector<uint8_t> segABytes;
  AppendIndexTableSegment(segABytes, segA);
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0, 64,
                                  segABytes.size(), 1);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segABytes.begin(), segABytes.end());
  const uint64_t essenceBaseA = file.size();  // == segment A's essenceContainerBaseOffset once parsed
  constexpr uint64_t kPartitionAEssenceBytes = 400;
  file.insert(file.end(), kPartitionAEssenceBytes, 0);

  const uint64_t partition2Offset = file.size();
  IndexTableSegmentSpec segB;
  segB.indexStartPosition = 0;  // deliberately the same range as segA
  segB.indexDuration = 2;
  segB.indexSid = 1;
  segB.bodySid = 1;
  segB.entries = {{0x00, 999}, {0x30, 888}};  // "second" -- should lose
  std::vector<uint8_t> segBBytes;
  AppendIndexTableSegment(segBBytes, segB);
  // BodyOffset must be distinct from partition A's (0) -- see the identical
  // comment in TestMultiSegmentStitching for why.
  AppendPartitionPackWithFields(file, PartitionKind::kBody, TestPartitionStatus::kClosedComplete, partition2Offset, 0,
                                  64, segBBytes.size(), 1, kPartitionAEssenceBytes);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segBBytes.begin(), segBBytes.end());
  file.insert(file.end(), 1400, 0);  // large enough that segB's own (losing) offsets would also be in-bounds

  AppendRandomIndexPack(file, {{1, 0}, {1, partition2Offset}});

  auto path = WriteTempFile(file, "mxfsrc_test_its_overlap.mxf");
  {
    MxfReader reader(path.string());
    auto index = BuildEssenceIndex(reader, 1);
    CHECK(index.has_value());
    if (index) {
      auto point = index->FindPrecedingKeyframe(0);
      CHECK(point.has_value());
      // Deterministic: the Header Partition (offset 0, segment A) is always
      // enumerated first, so segment A's offset must win over segment B's.
      if (point) CHECK(point->byteOffset == essenceBaseA + 111);
    }
  }
  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestVbrSegmentRoundTrip();
  TestIndexEntryArrayItemLength15SkipsTrailingBytes();
  TestIsKeyframeFlagsTruthTable();
  TestEstimateReorderDelayCountsBackwardOnlyBFrames();
  TestEstimateReorderDelayReturnsZeroForLowDelayMonotonicByteOffsets();
  TestCountUndecodableLeadingPicturesMatchesRealFileOpenGopPattern();
  TestFindPrecedingKeyframeTargetIsAlreadyKeyframe();
  TestFindPrecedingKeyframeWalksBackToEarlierKeyframe();
  TestFindPrecedingKeyframeNoKeyframeBeforeTarget();
  TestFindPrecedingKeyframeOutsideCoveredRange();
  TestCbrSegmentFlattenFormula();
  TestNoIndexTableAnywhereReturnsNullopt();
  TestMalformedSegmentIsSkippedNotFatal();
  TestOutOfBoundsOffsetIsDropped();
  TestMultiSegmentStitching();
  TestOverlappingSegmentsDeterministicFirstWins();

  if (g_failures == 0) {
    std::cout << "all mxf index_table tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
