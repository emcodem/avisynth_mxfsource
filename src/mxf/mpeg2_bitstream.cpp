#include "mxf/mpeg2_bitstream.h"

namespace mxfsrc {

namespace {

// Reads MSB-first bits from data[startByte, size) -- ReadBits returns false
// (leaving *value unspecified) the instant a read would run past `size`,
// rather than reading out of bounds.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size, size_t startByte) : data_(data), size_(size), bytePos_(startByte) {}

  bool ReadBits(int count, uint32_t* value) {
    uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
      if (bytePos_ >= size_) return false;
      const uint8_t bit = (data_[bytePos_] >> (7 - bitPos_)) & 0x01;
      result = (result << 1) | bit;
      ++bitPos_;
      if (bitPos_ == 8) {
        bitPos_ = 0;
        ++bytePos_;
      }
    }
    *value = result;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t bytePos_;
  int bitPos_ = 0;
};

// Finds the next MPEG-2 start code (00 00 01 XX) at or after `pos`. On
// success, *codeBytePos is the index of the start_code_value byte itself
// (always a valid index into `data`).
bool FindNextStartCode(const uint8_t* data, size_t size, size_t pos, size_t* codeBytePos) {
  if (size < 4) return false;
  for (size_t i = pos; i + 3 < size; ++i) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      *codeBytePos = i + 3;
      return true;
    }
  }
  return false;
}

constexpr uint8_t kPictureStartCode = 0x00;
constexpr uint8_t kGroupStartCode = 0xB8;

}  // namespace

Mpeg2FrameInfo ParseMpeg2FrameHeader(const uint8_t* data, size_t size) {
  Mpeg2FrameInfo result;
  bool haveClosedGop = false;
  bool closedGop = false;

  size_t pos = 0;
  size_t codeBytePos = 0;
  while (FindNextStartCode(data, size, pos, &codeBytePos)) {
    const uint8_t code = data[codeBytePos];
    const size_t afterCode = codeBytePos + 1;

    if (code == kGroupStartCode) {
      // group_of_pictures_header: 25-bit time_code, then 1-bit closed_gop.
      BitReader reader(data, size, afterCode);
      uint32_t timeCode = 0;
      uint32_t closedGopBit = 0;
      if (reader.ReadBits(25, &timeCode) && reader.ReadBits(1, &closedGopBit)) {
        haveClosedGop = true;
        closedGop = closedGopBit != 0;
      }
    } else if (code == kPictureStartCode) {
      // picture_header: 10-bit temporal_reference, then 3-bit picture_coding_type.
      BitReader reader(data, size, afterCode);
      uint32_t temporalReference = 0;
      uint32_t codingType = 0;
      if (!reader.ReadBits(10, &temporalReference) || !reader.ReadBits(3, &codingType)) {
        return result;  // truncated mid-header -- unrecognized
      }
      result.recognized = true;
      switch (codingType) {
        case 1:  // I
          result.flags = 0x00;
          result.isKeyframe = haveClosedGop && closedGop;
          break;
        case 2:  // P
          result.flags = 0x20;
          break;
        case 3:  // B
          result.flags = 0x30;
          break;
        default:  // reserved/D-picture -- not something we classify
          result.recognized = false;
          break;
      }
      return result;
    }

    pos = afterCode;
  }
  return result;
}

}  // namespace mxfsrc
