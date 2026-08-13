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

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, {});

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

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, {});
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

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, {});
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

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, {});
    EssenceElement element;
    CHECK(!walker.Next(element));
  }

  std::filesystem::remove(path);
}

// Two audio tracks summed into one walk: verifies each matched audio
// element's audioTrackIndex correctly identifies which of the two
// TrackNumbers it came from, and that video/audio interleave in file order
// is preserved regardless of which track's element appears first per edit
// unit.
constexpr uint32_t kAudioTrackNumberA = 0x02020200;
constexpr uint32_t kAudioTrackNumberB = 0x02020300;

void TestMultiTrackAudioFiltering() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 2;
  cfg.videoTrackNumber = kVideoTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  // Edit unit 0: video, then track A, then track B.
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 0});
  AppendEssenceElement(bytes, kAudioTrackNumberA, {0x0A, 0});
  AppendEssenceElement(bytes, kAudioTrackNumberB, {0x0B, 0});
  // Edit unit 1: same order.
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 1});
  AppendEssenceElement(bytes, kAudioTrackNumberA, {0x0A, 1});
  AppendEssenceElement(bytes, kAudioTrackNumberB, {0x0B, 1});

  auto path = WriteTempFile(bytes, "mxfsrc_test_essence_walk_multi_audio.mxf");
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }

    auto trackNumberBytes = [](uint32_t n) {
      return std::array<uint8_t, 4>{static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                                     static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
    };
    std::vector<std::array<uint8_t, 4>> audioTrackNumbers = {trackNumberBytes(kAudioTrackNumberA),
                                                               trackNumberBytes(kAudioTrackNumberB)};

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, audioTrackNumbers);

    int videoCount = 0, audioACount = 0, audioBCount = 0;
    EssenceElement element;
    while (walker.Next(element)) {
      if (element.isVideo) {
        CHECK(element.data[0] == 0xAA);
        CHECK(element.data[1] == static_cast<uint8_t>(videoCount));
        ++videoCount;
      } else {
        CHECK(element.isAudio);
        if (element.audioTrackIndex == 0) {
          CHECK(element.data[0] == 0x0A);
          CHECK(element.data[1] == static_cast<uint8_t>(audioACount));
          ++audioACount;
        } else {
          CHECK(element.audioTrackIndex == 1);
          CHECK(element.data[0] == 0x0B);
          CHECK(element.data[1] == static_cast<uint8_t>(audioBCount));
          ++audioBCount;
        }
      }
    }
    CHECK(videoCount == 2);
    CHECK(audioACount == 2);
    CHECK(audioBCount == 2);
  }

  std::filesystem::remove(path);
}

// Regression/feature test for mxf/bitstream_index.h's use of Next()'s
// bounded-prefix-read mode: a maxBytes smaller than an element's true
// length must return only that many bytes (while still reporting the full
// `length`), and the reader must still land correctly on the *next*
// element afterward -- proving the un-read remainder was actually skipped,
// not left for the next ReadKlvHeader to misparse.
void TestBoundedPrefixReadSkipsRemainder() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 2;
  cfg.videoTrackNumber = kVideoTrackNumber;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  std::vector<uint8_t> firstPayload = {0xEE, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9};  // 11 bytes
  AppendEssenceElement(bytes, kVideoTrackNumber, firstPayload);
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xEE, 1});

  auto path = WriteTempFile(bytes, "mxfsrc_test_essence_walk_bounded_prefix.mxf");
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (!info) {
      std::filesystem::remove(path);
      return;
    }

    EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, info->hasVideo, {});

    EssenceElement first;
    CHECK(walker.Next(first, /*wantData=*/true, /*maxBytes=*/4));
    CHECK(first.isVideo);
    CHECK(first.length == firstPayload.size());  // true size still reported...
    CHECK(first.data.size() == 4);               // ...even though only the prefix was read
    CHECK(first.data[0] == 0xEE);
    CHECK(first.data[1] == 0);
    CHECK(first.data[2] == 1);
    CHECK(first.data[3] == 2);

    EssenceElement second;
    CHECK(walker.Next(second));  // default wantData/maxBytes -- must land cleanly on element 2
    CHECK(second.isVideo);
    CHECK(second.data.size() == 2);
    CHECK(second.data[0] == 0xEE);
    CHECK(second.data[1] == 1);

    CHECK(!walker.Next(second));  // EOF
  }

  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestWalksVideoElementsInOrderSkippingOthers();
  TestNoBodyPartitionEssenceFollowsHeaderDirectly();
  TestSkipsNonCompliantZeroBytePadding();
  TestEmptyBodyPartitionYieldsNoElements();
  TestMultiTrackAudioFiltering();
  TestBoundedPrefixReadSkipsRemainder();

  if (g_failures == 0) {
    std::cout << "all mxf essence_walker tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
