#pragma once

#include <memory>

#include "codec/decoder.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

// libavcodec-backed decoder for the compressed codecs in ul_tables.h's
// CodecFamily (MPEG-2, DNxHD, ProRes, H.264/AVC-Intra, ...). One instance
// per essence track; essenceData is handed to avcodec_send_packet as-is
// per element, in the same file order EssenceWalker produced it -- forward
// decode order is exactly what avcodec's internal B-frame reorder queue
// expects (see plan Context and codec/ notes), no extra work needed here.
//
// PIMPL: keeps <libavcodec/avcodec.h> out of this header so consumers
// (avs_adapter.cpp, vs_adapter.cpp, core/) don't need FFmpeg's include path.
class AvcodecDecoder final : public Decoder {
 public:
  AvcodecDecoder(CodecFamily family, int width, int height);
  ~AvcodecDecoder() override;

  bool SendAndReceive(const std::vector<uint8_t>& essenceData, NormalizedFrame& out) override;
  bool IsValid() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mxfsrc
