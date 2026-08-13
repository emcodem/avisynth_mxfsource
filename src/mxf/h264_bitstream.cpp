#include "mxf/h264_bitstream.h"

namespace mxfsrc {

namespace {

// Yields RBSP bytes from data[startByte, size), transparently dropping any
// emulation_prevention_three_byte (a literal 0x03 the encoder inserts right
// after two consecutive 0x00 bytes, per Annex B, to keep a real start code
// from accidentally appearing inside NAL payload data).
class RbspByteReader {
 public:
  RbspByteReader(const uint8_t* data, size_t size, size_t startByte) : data_(data), size_(size), pos_(startByte) {}

  bool NextByte(uint8_t* out) {
    if (pos_ >= size_) return false;
    uint8_t b = data_[pos_++];
    if (zeroRun_ >= 2 && b == 0x03) {
      zeroRun_ = 0;
      if (pos_ >= size_) return false;
      b = data_[pos_++];
    }
    zeroRun_ = (b == 0x00) ? zeroRun_ + 1 : 0;
    *out = b;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_;
  int zeroRun_ = 0;
};

// Bit-level reader over de-escaped RBSP bytes, with Exp-Golomb ue(v) support
// -- sufficient for first_mb_in_slice/slice_type, no CABAC involved.
class RbspBitReader {
 public:
  RbspBitReader(const uint8_t* data, size_t size, size_t startByte) : bytes_(data, size, startByte) {}

  bool ReadBit(uint32_t* bit) {
    if (bitPos_ == 0) {
      if (!bytes_.NextByte(&curByte_)) return false;
      bitPos_ = 8;
    }
    --bitPos_;
    *bit = (curByte_ >> bitPos_) & 0x01;
    return true;
  }

  bool ReadBits(int count, uint32_t* value) {
    uint32_t result = 0;
    for (int i = 0; i < count; ++i) {
      uint32_t bit = 0;
      if (!ReadBit(&bit)) return false;
      result = (result << 1) | bit;
    }
    *value = result;
    return true;
  }

  bool ReadUE(uint32_t* value) {
    int leadingZeroBits = 0;
    for (;;) {
      uint32_t bit = 0;
      if (!ReadBit(&bit)) return false;
      if (bit != 0) break;
      ++leadingZeroBits;
      if (leadingZeroBits > 31) return false;  // corrupt input guard, not a real Exp-Golomb limit
    }
    if (leadingZeroBits == 0) {
      *value = 0;
      return true;
    }
    uint32_t infoBits = 0;
    if (!ReadBits(leadingZeroBits, &infoBits)) return false;
    *value = (1u << leadingZeroBits) - 1 + infoBits;
    return true;
  }

 private:
  RbspByteReader bytes_;
  uint8_t curByte_ = 0;
  int bitPos_ = 0;
};

// Finds the next Annex-B start code (00 00 01, regardless of whether it was
// preceded by an extra leading 00 for a 4-byte start code) at or after
// `pos`. On success, *nalStart is the index of the NAL header byte itself.
bool FindNextStartCode(const uint8_t* data, size_t size, size_t pos, size_t* nalStart) {
  if (size < 3) return false;
  for (size_t i = pos; i + 2 < size; ++i) {
    if (data[i] == 0x00 && data[i + 1] == 0x00 && data[i + 2] == 0x01) {
      *nalStart = i + 3;
      return true;
    }
  }
  return false;
}

}  // namespace

H264FrameInfo ParseH264FrameHeader(const uint8_t* data, size_t size) {
  H264FrameInfo result;

  size_t pos = 0;
  size_t nalStart = 0;
  while (FindNextStartCode(data, size, pos, &nalStart)) {
    if (nalStart >= size) break;
    const uint8_t nalHeader = data[nalStart];
    const int nalUnitType = nalHeader & 0x1F;
    const size_t rbspStart = nalStart + 1;

    if (nalUnitType == 1 || nalUnitType == 5) {  // non-IDR or IDR slice -- the first VCL NAL we hit
      RbspBitReader reader(data, size, rbspStart);
      uint32_t firstMb = 0;
      uint32_t sliceType = 0;
      if (!reader.ReadUE(&firstMb) || !reader.ReadUE(&sliceType)) {
        return result;  // truncated mid-slice-header -- unrecognized
      }
      result.recognized = true;
      switch (sliceType % 5) {
        case 0:  // P
          result.flags = 0x20;
          break;
        case 1:  // B
          result.flags = 0x30;
          break;
        case 2:  // I
          result.flags = 0x00;
          break;
        default:  // SP/SI -- rare in MXF video, treated like P
          result.flags = 0x20;
          break;
      }
      result.isKeyframe = (nalUnitType == 5);
      return result;
    }

    // SPS/PPS/SEI/AUD/etc. -- skip structurally, keep scanning forward.
    pos = rbspStart;
  }
  return result;
}

}  // namespace mxfsrc
