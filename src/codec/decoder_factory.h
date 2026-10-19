#pragma once

#include <cstdint>
#include <memory>

#include "codec/decoder.h"
#include "mxf/klv.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

// Picks RawUyvyDecoder for uncompressed essence (no avcodec needed) and
// AvcodecDecoder for everything else in ul_tables.h's CodecFamily. Returns
// nullptr if the codec is unrecognized or the underlying decoder failed to
// initialize (e.g. avcodec_find_decoder came back empty).
// `pictureEssenceCoding` is the Descriptor's PictureEssenceCoding UL. For
// ProRes it selects the codec tag (see ProresCodecTag), which FFmpeg's
// decoder needs to output 4444 profiles at their full 12 bits.
std::unique_ptr<Decoder> CreateVideoDecoder(CodecFamily family, int width, int height,
                                            const Ul& pictureEssenceCoding = {});

// FFmpeg codec tag ('apco' ... 'ap4x') for a ProRes PictureEssenceCoding UL
// (SMPTE RDD 44, byte 14 = profile: 01 422 Proxy, 02 422 LT, 03 422,
// 04 422 HQ, 05 4444, 06 4444 XQ), the same mapping as FFmpeg's MXF
// demuxer. 0 for anything else.
uint32_t ProresCodecTag(const Ul& pictureEssenceCoding);

}  // namespace mxfsrc
