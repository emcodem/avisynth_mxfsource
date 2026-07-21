#pragma once

#include <memory>

#include "codec/decoder.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

// Picks RawUyvyDecoder for uncompressed essence (no avcodec needed) and
// AvcodecDecoder for everything else in ul_tables.h's CodecFamily. Returns
// nullptr if the codec is unrecognized or the underlying decoder failed to
// initialize (e.g. avcodec_find_decoder came back empty).
std::unique_ptr<Decoder> CreateVideoDecoder(CodecFamily family, int width, int height);

}  // namespace mxfsrc
