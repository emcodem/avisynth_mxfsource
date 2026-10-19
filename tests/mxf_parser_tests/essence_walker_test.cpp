// Phase 2 unit tests for the sequential essence KLV walker (src/mxf/essence_walker.*).
// Builds on the synthetic OP1a Header Partition from synthetic_mxf.h and
// appends a Body Partition containing video essence elements interspersed
// with KLV Fill and an essence element from an untracked track, to verify
// EssenceWalker's forward-only skip logic.
#include <cstring>
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

// In-memory IByteSource that counts ReadBytes calls, to check that
// ResyncToUlDesignator reads in blocks instead of byte by byte.
class CountingByteSource : public IByteSource {
 public:
  explicit CountingByteSource(std::vector<uint8_t> data) : data_(std::move(data)) {}
  bool IsOpen() const override { return true; }
  uint64_t Tell() const override { return pos_; }
  bool Seek(uint64_t pos) override {
    if (pos > data_.size()) return false;
    pos_ = pos;
    return true;
  }
  uint64_t Size() const override { return data_.size(); }
  bool ReadBytes(void* dst, size_t n) override {
    ++reads_;
    if (pos_ + n > data_.size()) return false;
    std::memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  bool Skip(uint64_t n) override { return Seek(pos_ + n); }
  std::string LastError() const override { return {}; }
  int reads_ = 0;

 private:
  std::vector<uint8_t> data_;
  uint64_t pos_ = 0;
};

std::vector<uint8_t> ZerosWithDesignatorAt(size_t size, size_t at) {
  std::vector<uint8_t> data(size, 0);
  const uint8_t designator[4] = {0x06, 0x0e, 0x2b, 0x34};
  std::memcpy(data.data() + at, designator, 4);
  return data;
}

// Returns where ResyncToUlDesignator left the reader, or -1 if it failed.
int64_t ResyncFrom(std::vector<uint8_t> data, uint64_t start, uint64_t limit = UINT64_MAX, int* readsOut = nullptr) {
  auto source = std::make_unique<CountingByteSource>(std::move(data));
  CountingByteSource* counter = source.get();
  MxfReader reader(std::move(source));
  if (!reader.Seek(start)) return -2;
  const bool found = ResyncToUlDesignator(reader, limit);
  if (readsOut) *readsOut = counter->reads_;
  return found ? static_cast<int64_t>(reader.Tell()) : -1;
}

// A designator split across the first two read blocks (4 KiB, overlapping
// by 3 bytes) is found at every split point, as is one just before.
void TestResyncFindsDesignatorAcrossBlockBoundary() {
  for (size_t at : {size_t{4092}, size_t{4093}, size_t{4094}, size_t{4095}, size_t{4096}}) {
    CHECK(ResyncFrom(ZerosWithDesignatorAt(16384, at), 0) == static_cast<int64_t>(at));
  }
}

// The search starts at the current position, ignores earlier hits, and
// fails cleanly when nothing follows.
void TestResyncStartsAtCurrentPositionAndFailsAtEof() {
  std::vector<uint8_t> data = ZerosWithDesignatorAt(200, 5);
  const uint8_t designator[4] = {0x06, 0x0e, 0x2b, 0x34};
  std::memcpy(data.data() + 50, designator, 4);
  CHECK(ResyncFrom(data, 0) == 5);
  CHECK(ResyncFrom(data, 6) == 50);
  CHECK(ResyncFrom(data, 51) == -1);
  CHECK(ResyncFrom(std::vector<uint8_t>(3, 0x06), 0) == -1);  // shorter than a designator
  // A lone 0x06 0x0e 0x2b without the 0x34 is no match.
  std::vector<uint8_t> partial(100, 0);
  partial[40] = 0x06;
  partial[41] = 0x0e;
  partial[42] = 0x2b;
  CHECK(ResyncFrom(partial, 0) == -1);
}

// `limit`: a hit must start before it; one starting at limit-1 counts even
// though it extends past.
void TestResyncHonoursLimit() {
  std::vector<uint8_t> data = ZerosWithDesignatorAt(10000, 100);
  CHECK(ResyncFrom(data, 0, /*limit=*/100) == -1);
  CHECK(ResyncFrom(data, 0, /*limit=*/101) == 100);
  CHECK(ResyncFrom(data, 0, /*limit=*/9000) == 100);
}

// 3 MiB of zeros before the next key (real case: zero-filled tails and
// preallocated regions) costs a handful of block reads, not one per byte.
void TestResyncOverLongZeroRunUsesFewReads() {
  const size_t at = 3 * 1024 * 1024 + 7;
  int reads = 0;
  CHECK(ResyncFrom(ZerosWithDesignatorAt(at + 100, at), 0, UINT64_MAX, &reads) == static_cast<int64_t>(at));
  CHECK(reads <= 16);
}

int main() {
  TestResyncFindsDesignatorAcrossBlockBoundary();
  TestResyncStartsAtCurrentPositionAndFailsAtEof();
  TestResyncHonoursLimit();
  TestResyncOverLongZeroRunUsesFewReads();
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
