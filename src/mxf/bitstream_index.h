#pragma once

#include <optional>

#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"

namespace mxfsrc {

// Builds a fallback EssenceIndex by walking essence elements directly --
// for files with no usable SMPTE Index Table Segment at all (e.g. a
// truncated file missing its RIP/footer), where mxf/index_table.h's
// BuildEssenceIndex has nothing to read. Deliberately never decodes: for
// intra-only codecs (ul_tables.h's IsIntraOnlyCodec) every essence element
// is already its own safe entry point by construction, so byte offsets
// alone (no payload bytes read) are enough for a full, always-correct
// index. For MPEG-2 and H.264/AVC, the leading bytes of each element's
// compressed payload are parsed directly (mxf/mpeg2_bitstream.h,
// mxf/h264_bitstream.h) to find genuinely safe keyframe entry points --
// MPEG-2 via its GOP header's closed_gop bit, H.264 via true IDR NALs only
// -- never treating a plain I-picture/non-IDR slice as a keyframe, since
// this parser has no way to rule out an open-GOP dependency on the
// previous GOP the way a real Index Table Segment's KeyFrameOffset does.
//
// Returns std::nullopt if `streamInfo.videoCodec` isn't one this fallback
// supports, or if no video essence elements were found at all -- callers
// must treat that exactly like BuildEssenceIndex's own std::nullopt, never
// a hard failure.
std::optional<EssenceIndex> BuildEssenceIndexFromBitstream(MxfReader& reader, const MxfStreamInfo& streamInfo);

}  // namespace mxfsrc
