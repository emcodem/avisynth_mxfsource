// Phase 2 unit tests for the sequential essence KLV walker (src/mxf/essence_walker.*).
// Builds on the synthetic OP1a Header Partition from synthetic_mxf.h and
// appends a Body Partition containing video essence elements interspersed
// with KLV Fill and an essence element from an untracked track, to verify
// EssenceWalker's forward-only skip logic.
#include <filesystem>
#include <iostream>

#include "mxf/essence_walker.h"
#include "mxf/header_parser.h"
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

constexpr uint32_t kVideoTrackNumber = 0x01010100;
constexpr uint32_t kOtherTrackNumber = 0x02020200;  // e.g. an audio/data track this walk doesn't request

void TestWalksVideoElementsInOrderSkippingOthers() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 4;
  cfg.videoTrackNumber = kVideoTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendKlvFill(bytes, 4);
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 0});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 1});
  AppendEssenceElement(bytes, kOtherTrackNumber, {0xBB, 0xBB, 0xBB});  // must be skipped
  AppendKlvFill(bytes, 2);
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 2});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 3});

  auto path = WriteTempFile(bytes, "mxfsrc_test_essence_walk.mxf");
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }
    CHECK(info->numFrames == 4);

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo,
                          info->audioTrackNumber, info->hasAudio);

    int64_t count = 0;
    EssenceElement element;
    while (walker.Next(element)) {
      CHECK(element.isVideo);
      CHECK(!element.isAudio);
      CHECK(element.data.size() == 2);
      CHECK(element.data[0] == 0xAA);
      CHECK(element.data[1] == static_cast<uint8_t>(count));  // confirms forward file order preserved
      ++count;
    }
    CHECK(count == info->numFrames);
  }

  std::filesystem::remove(path);
}

// Regression test for real Atomos Shogun XAVC-Intra/XAVC-L recordings: there
// is no separate Body Partition at all -- essence elements follow the
// Header Partition's own Header Metadata directly, and the Random Index
// Pack's only two entries are the Header (byteOffset 0) and the Footer.
// header_parser.cpp's Fixup 1 must not mistake the Footer's offset for
// "where Header Metadata truly ends" (see its comment) -- doing so would
// make EssenceWalker skip every essence element in the file.
void TestNoBodyPartitionEssenceFollowsHeaderDirectly() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 3;
  cfg.videoTrackNumber = kVideoTrackNumber;
  cfg.includeFooterPartitionOffset = false;  // force RIP-based footer discovery
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendEssenceElement(bytes, kVideoTrackNumber, {0xCC, 0});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xCC, 1});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xCC, 2});

  const uint64_t footerOffset = bytes.size();
  AppendPartitionPack(bytes, PartitionKind::kFooter);
  AppendRandomIndexPack(bytes, {{0, 0}, {0, footerOffset}});

  auto path = WriteTempFile(bytes, "mxfsrc_test_no_body_partition.mxf");
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }
    // The bug this guards against: Fixup 1 mistaking the Footer's offset
    // for the "next partition" and jumping headerMetadataEnd there.
    CHECK(info->headerMetadataEnd < footerOffset);

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo,
                          info->audioTrackNumber, info->hasAudio);
    int64_t count = 0;
    EssenceElement element;
    while (walker.Next(element)) {
      CHECK(element.isVideo);
      CHECK(element.data.size() == 2);
      CHECK(element.data[0] == 0xCC);
      CHECK(element.data[1] == static_cast<uint8_t>(count));
      ++count;
    }
    CHECK(count == 3);
  }

  std::filesystem::remove(path);
}

// Regression test for the same real Atomos Shogun files: they pad a
// partition out to a KAG boundary using raw zero bytes instead of a
// spec-compliant KLV Fill Item. A run length that isn't a multiple of 17
// (a zero-length KLV's header size) would desync a naive walk so it lands
// misaligned inside the real essence element and fails outright --
// EssenceWalker must resync past it instead (see essence_walker.cpp's
// ResyncToUlDesignator).
void TestSkipsNonCompliantZeroBytePadding() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 2;
  cfg.videoTrackNumber = kVideoTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  bytes.insert(bytes.end(), 37, 0);  // 37 raw zero bytes, not a multiple of 17
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xDD, 0});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xDD, 1});

  auto path = WriteTempFile(bytes, "mxfsrc_test_zero_padding.mxf");
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo,
                          info->audioTrackNumber, info->hasAudio);
    int64_t count = 0;
    EssenceElement element;
    while (walker.Next(element)) {
      CHECK(element.isVideo);
      CHECK(element.data.size() == 2);
      CHECK(element.data[0] == 0xDD);
      CHECK(element.data[1] == static_cast<uint8_t>(count));
      ++count;
    }
    CHECK(count == 2);
  }

  std::filesystem::remove(path);
}

void TestEmptyBodyPartitionYieldsNoElements() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 0;
  cfg.videoTrackNumber = kVideoTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendPartitionPack(bytes, PartitionKind::kFooter);

  auto path = WriteTempFile(bytes, "mxfsrc_test_essence_walk_empty.mxf");
  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo,
                          info->audioTrackNumber, info->hasAudio);
    EssenceElement element;
    CHECK(!walker.Next(element));
  }

  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestWalksVideoElementsInOrderSkippingOthers();
  TestNoBodyPartitionEssenceFollowsHeaderDirectly();
  TestSkipsNonCompliantZeroBytePadding();
  TestEmptyBodyPartitionYieldsNoElements();

  if (g_failures == 0) {
    std::cout << "all mxf essence_walker tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
