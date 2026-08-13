// End-to-end tests for src/mxf/bitstream_index.* against synthetic files
// with NO Index Table Segment at all (simulating a truncated file missing
// its RIP/footer) -- verifies BuildEssenceIndexFromBitstream recovers a
// usable fallback index straight from essence content for MPEG-2, H.264
// long-GOP, and intra-only codecs.
#include <filesystem>
#include <iostream>

#include "mxf/bitstream_index.h"
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

// PictureEssenceCoding ULs, copied verbatim from ul_tables.h's kCodecUls.
const Ul kMpeg2LongGopCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03,
                                 0x04, 0x01, 0x02, 0x02, 0x01, 0x01, 0x11, 0x00};
const Ul kH264LongGopCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D,
                                0x04, 0x01, 0x02, 0x02, 0x01, 0x31, 0x00, 0x00};
const Ul kProresCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D,
                           0x04, 0x01, 0x02, 0x02, 0x03, 0x06, 0x00, 0x00};

// -- MPEG-2 elementary-stream fragment builders (mirrors mpeg2_bitstream_test.cpp) --
std::vector<uint8_t> Mpeg2GroupHeader(bool closedGop) {
  return {0x00, 0x00, 0x01, 0xB8, 0x00, 0x00, 0x00, static_cast<uint8_t>(closedGop ? 0x40 : 0x00)};
}
std::vector<uint8_t> Mpeg2PictureHeader(uint8_t codingType) {
  return {0x00, 0x00, 0x01, 0x00, 0x00, static_cast<uint8_t>(codingType << 3)};
}
std::vector<uint8_t> Concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// -- H.264 slice NAL builder (mirrors h264_bitstream_test.cpp) --
class BitWriter {
 public:
  void WriteBit(int bit) {
    curByte_ = static_cast<uint8_t>((curByte_ << 1) | (bit & 1));
    if (++bitCount_ == 8) {
      bytes_.push_back(curByte_);
      curByte_ = 0;
      bitCount_ = 0;
    }
  }
  void WriteBits(uint32_t value, int count) {
    for (int i = count - 1; i >= 0; --i) WriteBit(static_cast<int>((value >> i) & 1));
  }
  void WriteUE(uint32_t value) {
    const uint32_t v = value + 1;
    int numBits = 0;
    for (uint32_t t = v; t != 0; t >>= 1) ++numBits;
    for (int i = 0; i < numBits - 1; ++i) WriteBit(0);
    WriteBits(v, numBits);
  }
  std::vector<uint8_t> Finish() {
    if (bitCount_ > 0) {
      curByte_ = static_cast<uint8_t>(curByte_ << (8 - bitCount_));
      bytes_.push_back(curByte_);
      bitCount_ = 0;
    }
    return bytes_;
  }

 private:
  std::vector<uint8_t> bytes_;
  uint8_t curByte_ = 0;
  int bitCount_ = 0;
};

std::vector<uint8_t> H264SliceNal(int nalUnitType, uint32_t sliceType) {
  std::vector<uint8_t> nal = {0x00, 0x00, 0x01};
  constexpr uint8_t kNalRefIdc = 3;
  nal.push_back(static_cast<uint8_t>((kNalRefIdc << 5) | (nalUnitType & 0x1F)));
  BitWriter writer;
  writer.WriteUE(0);
  writer.WriteUE(sliceType);
  writer.WriteBits(0, 3);
  const auto rbsp = writer.Finish();
  nal.insert(nal.end(), rbsp.begin(), rbsp.end());
  return nal;
}

// Parses `bytes` (a synthetic file with no Index Table Segment) and returns
// its BuildEssenceIndexFromBitstream result, or nullopt if header parsing
// itself failed (test aborts via CHECK in that case).
std::optional<EssenceIndex> RunBitstreamIndex(const std::vector<uint8_t>& bytes, const char* fileName) {
  auto path = WriteTempFile(bytes, fileName);
  std::optional<EssenceIndex> result;
  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) result = BuildEssenceIndexFromBitstream(reader, *info);
  }
  std::filesystem::remove(path);
  return result;
}

void TestMpeg2FallbackIndex() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 4;
  cfg.videoTrackNumber = kVideoTrackNumber;
  cfg.pictureEssenceCoding = kMpeg2LongGopCoding;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendEssenceElement(bytes, kVideoTrackNumber, Concat(Mpeg2GroupHeader(/*closedGop=*/true), Mpeg2PictureHeader(1)));
  AppendEssenceElement(bytes, kVideoTrackNumber, Mpeg2PictureHeader(2));  // P
  AppendEssenceElement(bytes, kVideoTrackNumber, Mpeg2PictureHeader(3));  // B
  AppendEssenceElement(bytes, kVideoTrackNumber,
                        Concat(Mpeg2GroupHeader(/*closedGop=*/false), Mpeg2PictureHeader(1)));  // open-GOP I

  auto index = RunBitstreamIndex(bytes, "mxfsrc_test_bitstream_index_mpeg2.mxf");
  CHECK(index.has_value());
  if (!index) return;
  const auto& entries = index->GetEntries();
  CHECK(entries.size() == 4);
  if (entries.size() == 4) {
    CHECK(entries[0].editUnit == 0 && entries[0].isKeyframe);        // closed-GOP I
    CHECK(entries[1].editUnit == 1 && !entries[1].isKeyframe);       // P
    CHECK(entries[2].editUnit == 2 && !entries[2].isKeyframe);       // B
    CHECK(entries[3].editUnit == 3 && !entries[3].isKeyframe);       // open-GOP I -- never trusted
  }
}

void TestH264LongGopFallbackIndex() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 3;
  cfg.videoTrackNumber = kVideoTrackNumber;
  cfg.pictureEssenceCoding = kH264LongGopCoding;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendEssenceElement(bytes, kVideoTrackNumber, H264SliceNal(/*nalUnitType=*/5, /*sliceType=*/2));  // IDR
  AppendEssenceElement(bytes, kVideoTrackNumber, H264SliceNal(/*nalUnitType=*/1, /*sliceType=*/1));  // B
  AppendEssenceElement(bytes, kVideoTrackNumber, H264SliceNal(/*nalUnitType=*/1, /*sliceType=*/2));  // non-IDR I

  auto index = RunBitstreamIndex(bytes, "mxfsrc_test_bitstream_index_h264.mxf");
  CHECK(index.has_value());
  if (!index) return;
  const auto& entries = index->GetEntries();
  CHECK(entries.size() == 3);
  if (entries.size() == 3) {
    CHECK(entries[0].editUnit == 0 && entries[0].isKeyframe);   // IDR
    CHECK(entries[1].editUnit == 1 && !entries[1].isKeyframe);  // B
    CHECK(entries[2].editUnit == 2 && !entries[2].isKeyframe);  // non-IDR I -- never trusted
  }
}

// Intra-only codec: every essence element is its own safe entry point by
// construction, so this path needs no bitstream parsing at all -- content
// is arbitrary placeholder bytes, never inspected.
void TestIntraOnlyCodecFallbackIndex() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 3;
  cfg.videoTrackNumber = kVideoTrackNumber;
  cfg.pictureEssenceCoding = kProresCoding;
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);

  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xAA, 0xBB, 0xCC});  // never parsed
  AppendEssenceElement(bytes, kVideoTrackNumber, {0xDD, 0xEE, 0xFF});
  AppendEssenceElement(bytes, kVideoTrackNumber, {0x11, 0x22, 0x33});

  auto index = RunBitstreamIndex(bytes, "mxfsrc_test_bitstream_index_intra.mxf");
  CHECK(index.has_value());
  if (!index) return;
  const auto& entries = index->GetEntries();
  CHECK(entries.size() == 3);
  for (size_t i = 0; i < entries.size(); ++i) {
    CHECK(entries[i].editUnit == static_cast<int64_t>(i));
    CHECK(entries[i].isKeyframe);
  }
}

// A codec this fallback doesn't support (plain raw/uncompressed, which is
// already intra-only and handled by the branch above -- use an unmapped/
// unclassified coding UL instead to force CodecFamily::kUnknown) must
// return std::nullopt rather than attempting anything.
void TestUnsupportedCodecReturnsNullopt() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 1;
  cfg.videoTrackNumber = kVideoTrackNumber;
  cfg.pictureEssenceCoding = Ul{0x06, 0x0E, 0x2B, 0x34, 0x99, 0x99, 0x99, 0x99,
                                 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99, 0x99};  // unrecognized UL
  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, PartitionKind::kBody);
  AppendEssenceElement(bytes, kVideoTrackNumber, {0x00});

  auto index = RunBitstreamIndex(bytes, "mxfsrc_test_bitstream_index_unsupported.mxf");
  CHECK(!index.has_value());
}

}  // namespace

int main() {
  TestMpeg2FallbackIndex();
  TestH264LongGopFallbackIndex();
  TestIntraOnlyCodecFallbackIndex();
  TestUnsupportedCodecReturnsNullopt();

  if (g_failures == 0) {
    std::cout << "all mxf bitstream_index tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
