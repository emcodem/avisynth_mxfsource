#include "codec/decoder_factory.h"

#include "codec/avcodec_decoder.h"
#include "codec/raw_unpack.h"

namespace mxfsrc {

std::unique_ptr<Decoder> CreateVideoDecoder(CodecFamily family, int width, int height) {
  if (family == CodecFamily::kRawVideo) {
    auto decoder = std::make_unique<RawUyvyDecoder>(width, height);
    if (!decoder->IsValid()) return nullptr;
    return decoder;
  }

  auto decoder = std::make_unique<AvcodecDecoder>(family, width, height);
  if (!decoder->IsValid()) return nullptr;
  return decoder;
}

}  // namespace mxfsrc
