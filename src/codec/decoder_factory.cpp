#include "codec/decoder_factory.h"

#include "codec/avcodec_decoder.h"
#include "codec/raw_unpack.h"

namespace mxfsrc {

namespace {

constexpr uint32_t MakeTag(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint8_t kProresCodingPrefix[13] = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D,
                                             0x04, 0x01, 0x02, 0x02, 0x03};

}  // namespace

uint32_t ProresCodecTag(const Ul& pictureEssenceCoding) {
  if (!UlPrefixEquals(pictureEssenceCoding, kProresCodingPrefix, sizeof(kProresCodingPrefix)) ||
      pictureEssenceCoding[13] != 0x06) {
    return 0;
  }
  switch (pictureEssenceCoding[14]) {
    case 0x01: return MakeTag('a', 'p', 'c', 'o');
    case 0x02: return MakeTag('a', 'p', 'c', 's');
    case 0x03: return MakeTag('a', 'p', 'c', 'n');
    case 0x04: return MakeTag('a', 'p', 'c', 'h');
    case 0x05: return MakeTag('a', 'p', '4', 'h');
    case 0x06: return MakeTag('a', 'p', '4', 'x');
    default: return 0;
  }
}

std::unique_ptr<Decoder> CreateVideoDecoder(CodecFamily family, int width, int height,
                                            const Ul& pictureEssenceCoding) {
  if (family == CodecFamily::kRawVideo) {
    auto decoder = std::make_unique<RawUyvyDecoder>(width, height);
    if (!decoder->IsValid()) return nullptr;
    return decoder;
  }

  const uint32_t codecTag = family == CodecFamily::kProres ? ProresCodecTag(pictureEssenceCoding) : 0;
  auto decoder = std::make_unique<AvcodecDecoder>(family, width, height, codecTag);
  if (!decoder->IsValid()) return nullptr;
  return decoder;
}

}  // namespace mxfsrc
