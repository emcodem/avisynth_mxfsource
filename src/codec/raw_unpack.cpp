#include "codec/raw_unpack.h"

#include <cstddef>

namespace mxfsrc {

RawUyvyDecoder::RawUyvyDecoder(int width, int height) : width_(width), height_(height) {}

bool RawUyvyDecoder::IsValid() const { return width_ > 0 && height_ > 0; }

bool RawUyvyDecoder::SendAndReceive(const std::vector<uint8_t>& essenceData, NormalizedFrame& out) {
  if (!IsValid()) return false;
  const size_t expected = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 2;
  if (essenceData.size() != expected) return false;  // malformed/truncated essence element

  out.format = PixelFormat::kUyvy422;
  out.width = width_;
  out.height = height_;
  out.stride[0] = width_ * 2;
  out.planeHeight[0] = height_;
  out.planeData[0] = essenceData;
  out.planeData[1].clear();
  out.planeData[2].clear();
  return true;
}

}  // namespace mxfsrc
