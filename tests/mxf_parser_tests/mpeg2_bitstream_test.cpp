// Unit tests for src/mxf/mpeg2_bitstream.* -- hand-crafted MPEG-2
// elementary-stream byte sequences (group_of_pictures_header +
// picture_header), no synthetic MXF wrapping needed since the parser
// operates on raw bitstream bytes directly.
#include <iostream>
#include <vector>

#include "mxf/mpeg2_bitstream.h"

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

// group_start_code (00 00 01 B8) + group_of_pictures_header with time_code=0
// and the given closed_gop bit (25 bits in).
std::vector<uint8_t> GroupHeader(bool closedGop) {
  return {0x00, 0x00, 0x01, 0xB8, 0x00, 0x00, 0x00, static_cast<uint8_t>(closedGop ? 0x40 : 0x00)};
}

// picture_start_code (00 00 01 00) + picture_header with temporal_reference=0
// and the given picture_coding_type (1=I, 2=P, 3=B).
std::vector<uint8_t> PictureHeader(uint8_t codingType) {
  return {0x00, 0x00, 0x01, 0x00, 0x00, static_cast<uint8_t>(codingType << 3)};
}

std::vector<uint8_t> Concat(std::vector<uint8_t> a, const std::vector<uint8_t>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

void TestClosedGopIPictureIsKeyframe() {
  const auto bytes = Concat(GroupHeader(/*closedGop=*/true), PictureHeader(1));
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(info.isKeyframe);
  CHECK(info.flags == 0x00);
}

void TestOpenGopIPictureIsNotKeyframe() {
  const auto bytes = Concat(GroupHeader(/*closedGop=*/false), PictureHeader(1));
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x00);
}

// No group_start_code seen at all -- conservative: never trust an I-picture
// as a keyframe without an explicit closed_gop confirmation.
void TestIPictureWithNoGroupHeaderIsNotKeyframe() {
  const auto bytes = PictureHeader(1);
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
}

void TestPPictureFlags() {
  const auto bytes = PictureHeader(2);
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x20);
}

void TestBPictureFlags() {
  const auto bytes = PictureHeader(3);
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
  CHECK(info.flags == 0x30);
}

void TestGarbageIsUnrecognized() {
  const std::vector<uint8_t> bytes = {0x12, 0x34, 0x56, 0x78, 0x9A};
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(!info.recognized);
}

void TestEmptyBufferIsUnrecognized() {
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(nullptr, 0);
  CHECK(!info.recognized);
}

// Only 1 byte follows the picture_start_code -- not enough for the 13-bit
// temporal_reference+picture_coding_type field. Must fail gracefully, never
// read out of bounds.
void TestTruncatedMidPictureHeaderIsUnrecognized() {
  const std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0x00, 0x00};
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(!info.recognized);
}

// Truncated mid-group-header: closed_gop is never confirmed, so a following
// (fully present) I-picture must still not be trusted as a keyframe.
void TestTruncatedGroupHeaderDoesNotConfirmClosedGop() {
  std::vector<uint8_t> bytes = {0x00, 0x00, 0x01, 0xB8, 0x00};  // group header cut short
  const auto picture = PictureHeader(1);
  bytes.insert(bytes.end(), picture.begin(), picture.end());
  const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(bytes.data(), bytes.size());
  CHECK(info.recognized);
  CHECK(!info.isKeyframe);
}

}  // namespace

int main() {
  TestClosedGopIPictureIsKeyframe();
  TestOpenGopIPictureIsNotKeyframe();
  TestIPictureWithNoGroupHeaderIsNotKeyframe();
  TestPPictureFlags();
  TestBPictureFlags();
  TestGarbageIsUnrecognized();
  TestEmptyBufferIsUnrecognized();
  TestTruncatedMidPictureHeaderIsUnrecognized();
  TestTruncatedGroupHeaderDoesNotConfirmClosedGop();

  if (g_failures == 0) {
    std::cout << "all mxf mpeg2_bitstream tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
