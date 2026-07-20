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
  TestEmptyBodyPartitionYieldsNoElements();

  if (g_failures == 0) {
    std::cout << "all mxf essence_walker tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
