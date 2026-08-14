// Tests for BuildEssenceIndexFromHttp (src/mxf/index_table.h) -- the
// parallel, chunked-download index-extraction path used when
// core/index_extract.cpp's ExtractAndPublishIndex is pointed at an HTTP(S)
// source with confirmed byte-range support (see index_table.cpp's own
// FetchAndParsePartitionForIndex doc comment for the growth strategy this
// exercises). Every test compares the parallel path's result against
// BuildEssenceIndex run locally against byte-identical content: since both
// paths call the exact same ReadOnePartitionForIndex/
// ScanPartitionForIndexSegments parsing functions, an exact match proves the
// parallel path's chunked-download/grow-and-retry machinery converged to a
// fully-downloaded, correctly-parsed result rather than a silently truncated
// one -- without needing to hand-verify byte offsets through
// ResyncToActualEssenceStart's own resync logic (already covered by
// index_table_test.cpp's local-file tests).
#include <filesystem>
#include <iostream>

#include "mini_http_server.h"
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

bool EntriesEqual(const std::vector<FlatIndexEntry>& a, const std::vector<FlatIndexEntry>& b) {
  if (a.size() != b.size()) {
    std::cerr << "entry count mismatch: " << a.size() << " vs " << b.size() << std::endl;
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].editUnit != b[i].editUnit || a[i].byteOffset != b[i].byteOffset ||
        a[i].isKeyframe != b[i].isKeyframe || a[i].flags != b[i].flags ||
        a[i].keyFrameOffset != b[i].keyFrameOffset) {
      std::cerr << "entry " << i << " mismatch: editUnit=" << a[i].editUnit << "/" << b[i].editUnit
                << " byteOffset=" << a[i].byteOffset << "/" << b[i].byteOffset << std::endl;
      return false;
    }
  }
  return true;
}

// Verifies BuildEssenceIndexFromHttp against BuildEssenceIndex run locally
// on the exact same bytes, given a set of (unsorted, possibly-duplicated)
// partition offsets exactly as ExtractAndPublishIndex would gather them from
// a RIP.
void CheckMatchesLocal(const std::vector<uint8_t>& file, std::vector<uint64_t> partitionOffsets,
                        const char* label) {
  MiniHttpServer server(file);
  auto httpIndex = BuildEssenceIndexFromHttp(server.BaseUrl() + "/test.mxf", "", /*videoBodySid=*/1, file.size(),
                                              partitionOffsets);
  CHECK(httpIndex.has_value());

  auto path = WriteTempFile(file, label);
  std::optional<EssenceIndex> localIndex;
  {
    MxfReader reader(path.string());
    localIndex = BuildEssenceIndex(reader, /*videoBodySid=*/1);
  }
  std::filesystem::remove(path);

  CHECK(localIndex.has_value());
  if (httpIndex && localIndex) {
    CHECK(EntriesEqual(httpIndex->GetEntries(), localIndex->GetEntries()));
  }
}

// Mirrors index_table_test.cpp's own TestMultiSegmentStitching file layout:
// two partitions (Header + one Body), each with a small VBR segment, tied
// together by a Random Index Pack -- the common, everyday case, well under
// any chunk-growth threshold. `partitionOffsets` deliberately unsorted with
// offset 0 repeated, exactly as ExtractAndPublishIndex would pass
// [0] + every RIP entry verbatim.
void TestHttpTwoPartitionsMatchesLocal() {
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
  constexpr uint64_t kPartitionAEssenceBytes = 400;
  file.insert(file.end(), kPartitionAEssenceBytes, 0);

  const uint64_t partition2Offset = file.size();
  IndexTableSegmentSpec segB;
  segB.indexStartPosition = 3;
  segB.indexDuration = 3;
  segB.indexSid = 1;
  segB.bodySid = 1;
  segB.entries = {{0x00, kPartitionAEssenceBytes + 50},
                  {0x30, kPartitionAEssenceBytes + 150},
                  {0x30, kPartitionAEssenceBytes + 250}};
  std::vector<uint8_t> segBBytes;
  AppendIndexTableSegment(segBBytes, segB);
  AppendPartitionPackWithFields(file, PartitionKind::kBody, TestPartitionStatus::kClosedComplete, partition2Offset, 0,
                                 64, segBBytes.size(), 1, kPartitionAEssenceBytes);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segBBytes.begin(), segBBytes.end());
  file.insert(file.end(), 400, 0);

  AppendRandomIndexPack(file, {{1, 0}, {1, partition2Offset}});

  // Unsorted, with offset 0 duplicated -- BuildEssenceIndexFromHttp must
  // sort+dedupe exactly like ExtractAndPublishIndex's real caller pattern
  // ([0] followed by whatever the RIP itself reports, which conventionally
  // includes the Header Partition's own offset too).
  CheckMatchesLocal(file, {partition2Offset, 0, 0}, "mxfsrc_test_http_two_partitions.mxf");
}

// A single partition whose combined Index Table Segments exceed
// kIndexFetchInitialChunk (100KB) -- forces FetchAndParsePartitionForIndex
// through at least one grow-and-retry round (the buffer's first 100KB isn't
// enough to read all three segments) before it can fully resolve the
// partition. IndexByteCount is declared accurately here (see the separate
// undercount test below for that dimension) -- this test is specifically
// about the download-size/grow-loop mechanism: if growth were broken (e.g.
// stopped after the first HitBufferLimit, or never grew far enough), this
// would come back with far fewer than the expected 12000 entries, or fail to
// match the local reference build entirely.
void TestHttpGrowthForLargeMultiSegmentPartition() {
  constexpr int kEntriesPerSegment = 4000;  // 4000*11 = 44000 bytes/segment; 3 segments ~132KB total, over 100KB
  constexpr int kSegmentCount = 3;

  std::vector<uint8_t> allSegmentBytes;
  for (int s = 0; s < kSegmentCount; ++s) {
    IndexTableSegmentSpec seg;
    seg.indexStartPosition = s * kEntriesPerSegment;
    seg.indexDuration = kEntriesPerSegment;
    seg.indexSid = 1;
    seg.bodySid = 1;
    seg.entries.reserve(kEntriesPerSegment);
    for (int i = 0; i < kEntriesPerSegment; ++i) {
      const int64_t editUnit = s * kEntriesPerSegment + i;
      seg.entries.push_back({0x00, static_cast<uint64_t>(editUnit)});  // keyframe, offset == editUnit
    }
    AppendIndexTableSegment(allSegmentBytes, seg);
  }

  std::vector<uint8_t> file;
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0,
                                 /*headerByteCount=*/64, allSegmentBytes.size(), /*indexSid=*/1);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), allSegmentBytes.begin(), allSegmentBytes.end());
  // Essence trailer must cover the largest streamOffset used above
  // (kSegmentCount*kEntriesPerSegment - 1).
  file.insert(file.end(), static_cast<size_t>(kSegmentCount * kEntriesPerSegment) + 100, 0);

  CheckMatchesLocal(file, {0}, "mxfsrc_test_http_growth_large.mxf");
}

// A partition whose declared IndexByteCount undercounts its one Index Table
// Segment's real length (mirrors the real cmc_30min.mxf quirk noted in
// project memory) -- exercises FetchAndParsePartitionForIndex's growth loop
// on the *inaccurate-hint* dimension specifically, as opposed to the
// large-content dimension above. A real trailing Footer Partition Pack (not
// just zero-padding) follows the essence so ResyncToActualEssenceStart has a
// genuine KLV boundary to walk forward onto and correctly widen hardEnd past
// the undercounted candidate -- exactly what a real file gives it (a
// zero-padded synthetic trailer with no real KLV at all, as most of this
// file's other tests use, would make even the local reference build fail
// here, since there'd be nothing for the resync to find either).
void TestHttpUndercountedIndexByteCountWithRealTrailingPartition() {
  constexpr int kEntryCount = 600;  // 600*11 = 6600 bytes -- comfortably under kIndexFetchInitialChunk on its own
  IndexTableSegmentSpec seg;
  seg.indexStartPosition = 0;
  seg.indexDuration = kEntryCount;
  seg.indexSid = 1;
  seg.bodySid = 1;
  seg.entries.reserve(kEntryCount);
  for (int i = 0; i < kEntryCount; ++i) seg.entries.push_back({0x00, static_cast<uint64_t>(i)});
  std::vector<uint8_t> segBytes;
  AppendIndexTableSegment(segBytes, seg);

  std::vector<uint8_t> file;
  // Undercounts by ~1000 bytes -- lands the naive candidate inside the
  // entry array itself, not at a real KLV boundary.
  const uint64_t declaredIndexByteCount = segBytes.size() > 1000 ? segBytes.size() - 1000 : 0;
  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0,
                                 /*headerByteCount=*/64, declaredIndexByteCount, /*indexSid=*/1);
  file.insert(file.end(), 64, 0);
  file.insert(file.end(), segBytes.begin(), segBytes.end());
  file.insert(file.end(), static_cast<size_t>(kEntryCount) + 100, 0);  // essence trailer

  AppendPartitionPackWithFields(file, PartitionKind::kFooter, TestPartitionStatus::kClosedComplete, file.size(),
                                 file.size(), /*headerByteCount=*/0);

  CheckMatchesLocal(file, {0}, "mxfsrc_test_http_undercount.mxf");
}

// Several small Body Partitions (more than the 20-way concurrency cap) all
// enumerated via one Random Index Pack -- exercises the worker-pool fan-out
// itself (partition count > kMaxConcurrentPartitionFetches) rather than any
// single partition's own growth logic.
void TestHttpManyPartitionsAllResolved() {
  constexpr int kPartitionCount = 25;
  constexpr uint64_t kEssencePerPartition = 200;

  std::vector<uint8_t> file;
  std::vector<std::pair<uint32_t, uint64_t>> ripEntries;

  AppendPartitionPackWithFields(file, PartitionKind::kHeader, TestPartitionStatus::kClosedComplete, 0, 0,
                                 /*headerByteCount=*/64, /*indexByteCount=*/0, /*indexSid=*/0);
  file.insert(file.end(), 64, 0);
  ripEntries.push_back({1, 0});

  for (int p = 0; p < kPartitionCount; ++p) {
    const uint64_t partitionOffset = file.size();
    IndexTableSegmentSpec seg;
    seg.indexStartPosition = p;  // one edit unit per partition, CBR-free (VBR, single entry)
    seg.indexDuration = 1;
    seg.indexSid = 1;
    seg.bodySid = 1;
    seg.entries = {{0x00, 10}};  // same relative offset in every partition's own essence range
    std::vector<uint8_t> segBytes;
    AppendIndexTableSegment(segBytes, seg);
    AppendPartitionPackWithFields(file, PartitionKind::kBody, TestPartitionStatus::kClosedComplete, partitionOffset,
                                   0, 64, segBytes.size(), 1, static_cast<uint64_t>(p) * kEssencePerPartition);
    file.insert(file.end(), 64, 0);
    file.insert(file.end(), segBytes.begin(), segBytes.end());
    file.insert(file.end(), kEssencePerPartition, 0);
    ripEntries.push_back({1, partitionOffset});
  }

  AppendRandomIndexPack(file, ripEntries);

  std::vector<uint64_t> offsets;
  for (auto& e : ripEntries) offsets.push_back(e.second);
  CheckMatchesLocal(file, offsets, "mxfsrc_test_http_many_partitions.mxf");

  // Also confirm the expected shape directly (every partition contributed
  // exactly one edit unit, in order) rather than only relying on equality
  // with the local build -- guards against both paths being wrong the same
  // way.
  MiniHttpServer server(file);
  auto index = BuildEssenceIndexFromHttp(server.BaseUrl() + "/test.mxf", "", 1, file.size(), offsets);
  CHECK(index.has_value());
  if (index) CHECK(index->HighestCoveredEditUnit() == kPartitionCount - 1);
}

}  // namespace

int main() {
  TestHttpTwoPartitionsMatchesLocal();
  TestHttpGrowthForLargeMultiSegmentPartition();
  TestHttpUndercountedIndexByteCountWithRealTrailingPartition();
  TestHttpManyPartitionsAllResolved();

  if (g_failures == 0) {
    std::cout << "all index_table_http tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
