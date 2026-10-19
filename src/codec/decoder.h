#pragma once

#include <cstdint>
#include <vector>

namespace mxfsrc {

enum class PixelFormat {
  kUnknown,
  kYuv420p8,
  kYuv422p8,
  kYuv420p10,
  kYuv422p10,
  kYuv422p12,  // DNxHR HQX 12-bit
  kYuv444p10,  // DNxHR 444 10-bit
  kYuv444p12,  // DNxHR 444 12-bit, ProRes 4444 (XQ) without alpha
  kUyvy422,  // packed, single plane -- e.g. raw uncompressed essence
};

// A decoded video frame in a host-agnostic layout. Plane 0 only is used for
// packed formats (kUyvy422); planes 0/1/2 = Y/U/V for planar formats.
struct NormalizedFrame {
  PixelFormat format = PixelFormat::kUnknown;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> planeData[3];
  int stride[3] = {0, 0, 0};
  int planeHeight[3] = {0, 0, 0};
};

// One Decoder instance per essence track. Fed essence elements strictly in
// file order (as EssenceWalker yields them) -- this matches how every
// implementation here works: avcodec's own internal reorder queue expects
// exactly this decode-order input, and forward-only playback needs no
// keyframe seeking (see plan Context).
class Decoder {
 public:
  virtual ~Decoder() = default;

  // Returns true and fills `out` with a decoded frame if the codec produced
  // one in response; returns false if the codec needs more input first
  // (e.g. B-frame/reorder latency -- MPEG-2 has this even for a lone
  // I-frame) or on decode error -- caller should keep feeding subsequent
  // elements and calling again. Pass an empty `essenceData` to flush/drain:
  // this asks the codec for any frame it's still holding once there is no
  // more input (end of stream, or synchronously right after the last
  // element when the caller needs that frame immediately rather than
  // waiting for a "next" element that doesn't exist).
  virtual bool SendAndReceive(const std::vector<uint8_t>& essenceData, NormalizedFrame& out) = 0;

  virtual bool IsValid() const = 0;
};

}  // namespace mxfsrc
