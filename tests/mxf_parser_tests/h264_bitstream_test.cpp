// Unit tests for src/mxf/h264_bitstream.* -- hand-crafted Annex-B byte
// sequences (NAL start codes + slice header Exp-Golomb fields), no
// synthetic MXF wrapping needed since the parser operates on raw bitstream
// bytes directly.
#include <iostream>
#include <vector>

#include "mxf/h264_bitstream.h"

using namespace mxfsrc;

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

// MSB-first bit packer, used to hand-build slice header Exp-Golomb fields
// without manual byte arithmetic.
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
  // Exp-Golomb ue(v): codeNum+1 in binary, with (bitlength-1) leading zeros.
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

// Builds one Annex-B NAL unit: 00 00 01, a NAL header byte for `nalUnitType`,
// then a minimal slice header (first_mb_in_slice=0, the given slice_type).
std::vector<uint8_t> SliceNal(int nalUnitType, uint32_t sliceType) {
  std::vector<uint8_t> nal = {0x00, 0x00, 0x01};
  constexpr uint8_t kNalRefIdc = 3;  // arbitrary nonzero, irrelevant to parsing
  nal.push_back(static_cast<uint8_t>((kNalRefIdc << 5) | (nalUnitType & 0x1F)));
  BitWriter writer;
  writer.WriteUE(0);         // first_mb_in_slice
  writer.WriteUE(sliceType);
  writer.WriteBits(0, 3);    // a little padding so the field isn't byte-exact-fragile
  const auto rbsp = writer.Finish();
  nal.insert(nal.end(), rbsp.begin(), rbsp.end());
  return nal;
}

std::vector<uint8_t> Concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

void TestIdrSliceIsKeyframe() {
  const auto bytes = SliceNal(/*nalUnitType=*/5, /*sliceType=*/2);  // IDR, I-slice
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(info.isKeyframe);
  CHECK(info.flags == 0x00);
}

void TestNonIdrISliceIsNotKeyframe() {
  const auto bytes = SliceNal(/*nalUnitType=*/1, /*sliceType=*/2);  // non-IDR, I-slice
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);  // deliberately never trusted -- see header comment
  CHECK(info.flags == 0x00);
}

void TestPSliceFlags() {
  const auto bytes = SliceNal(/*nalUnitType=*/1, /*sliceType=*/0);
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x20);
}

void TestBSliceFlags() {
  const auto bytes = SliceNal(/*nalUnitType=*/1, /*sliceType=*/1);
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x30);
}

// SPS (type 7) and PPS (type 8) NALs, with arbitrary/dummy payload bytes
// that are never parsed, must be skipped structurally so the following IDR
// slice is still found and classified correctly.
void TestSpsPpsPrecedingSliceAreSkipped() {
  std::vector<uint8_t> sps = {0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0xAB, 0xCD};
  std::vector<uint8_t> pps = {0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80};
  auto bytes = Concat(Concat(sps, pps), SliceNal(/*nalUnitType=*/5, /*sliceType=*/2));

  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(info.isKeyframe);
  CHECK(info.flags == 0x00);
}

// A slice header whose bits, once escaped per Annex B, genuinely require
// removing an emulation_prevention_three_byte (0x03 immediately after two
// 0x00 bytes) to recover the true first_mb_in_slice/slice_type values.
// Hand-derived: the logical (de-escaped) RBSP bytes are
// [0x00, 0x00, 0x80, 0x00, 0x40], which decode to first_mb_in_slice=65535
// (16 leading zero bits, terminator at bit 16, 16 zero info bits) followed
// immediately by slice_type=0 (a lone '1' bit at bit 33 -> P). The raw
// (escaped) bytes insert 0x03 between the two leading 0x00 bytes and the
// 0x80 that follows them: [0x00, 0x00, 0x03, 0x80, 0x00, 0x40]. If the
// escape byte were not stripped, the bit sequence (and therefore
// slice_type) would come out completely different.
void TestEmulationPreventionByteIsUnescaped() {
  std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0x41 /* nal_ref_idc=2, type=1 (non-IDR) */,
                                 0x00, 0x00, 0x03, 0x80, 0x00, 0x40};
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x20);  // slice_type decodes to 0 (P)
}

void TestGarbageIsUnrecognized() {
  const std::vector<uint8_t> bytes = {0x12, 0x34, 0x56, 0x78, 0x9A};
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(!info.recognized);
}

void TestEmptyBufferIsUnrecognized() {
  const H264FrameInfo info = ParseH264FrameHeader(nullptr, 0);
  CHECK(!info.recognized);
}

// A start code and NAL header with nothing else -- no room for even the
// first Exp-Golomb bit. Must fail gracefully, never read out of bounds.
void TestTruncatedRightAfterNalHeaderIsUnrecognized() {
  const std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0x65};  // IDR slice NAL header, no payload
  const H264FrameInfo info = ParseH264FrameHeader(bytes.data(), bytes.size());
  CHECK(!info.recognized);
}

}  // namespace

int main() {
  TestIdrSliceIsKeyframe();
  TestNonIdrISliceIsNotKeyframe();
  TestPSliceFlags();
  TestBSliceFlags();
  TestSpsPpsPrecedingSliceAreSkipped();
  TestEmulationPreventionByteIsUnescaped();
  TestGarbageIsUnrecognized();
  TestEmptyBufferIsUnrecognized();
  TestTruncatedRightAfterNalHeaderIsUnrecognized();

  if (g_failures == 0) {
    std::cout << "all mxf h264_bitstream tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
