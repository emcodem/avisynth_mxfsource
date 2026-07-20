#pragma once

#include "codec/decoder.h"

namespace mxfsrc {

// Uncompressed essence needs no codec at all -- the KLV Value already *is*
// the pixel data (see plan Phase 2/3 notes: "uncompressed needs no avcodec
// decoder, a raw unpack path directly from StoredWidth/Height"). This
// decoder covers UYVY 4:2:2 8-bit (ul_tables.h CodecFamily::kRawVideo,
// SMPTE 377-1's "uncompressed 4:2:2 8-bit" essence coding UL), the simplest
// and cheapest case, and the natural first codec to validate the Decoder
// interface against before adding avcodec-backed ones.
class RawUyvyDecoder final : public Decoder {
 public:
  RawUyvyDecoder(int width, int height);

  bool SendAndReceive(const std::vector<uint8_t>& essenceData, NormalizedFrame& out) override;
  bool IsValid() const override;

 private:
  int width_;
  int height_;
};

}  // namespace mxfsrc
