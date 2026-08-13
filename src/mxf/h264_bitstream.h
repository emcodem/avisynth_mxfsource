#pragma once

#include <cstddef>
#include <cstdint>

namespace mxfsrc {

// Result of inspecting one frame-wrapped H.264/AVC video essence element's
// leading bytes -- see mxf/bitstream_index.h, which uses this to build a
// fallback EssenceIndex without decoding when a file has no usable SMPTE
// Index Table Segment at all.
struct H264FrameInfo {
  bool recognized = false;  // a VCL NAL's slice header was found and parsed
  // True ONLY for a real IDR NAL (nal_unit_type==5) -- deliberately never
  // derived from slice_type alone. A non-IDR I-slice can still be an
  // open-GOP anchor that depends on the previous GOP (the exact bug
  // mxf/index_table.h's NeedsPreviousGop was written to catch on a real
  // Canon XAVC-L file, from real SMPTE Index Table KeyFrameOffset data);
  // this lightweight header-only parser has no way to rule that out for a
  // non-IDR picture, so it simply never trusts one as a seek entry point.
  bool isKeyframe = false;
  // Synthesized IndexTableEntry-style flags (mxf/index_table.h's
  // IsKeyframeFlags/IsBFrameFlags bit meanings), from slice_type: I=0x00,
  // P=0x20 (bit5), B=0x30 (bit4+bit5). Used only for EstimateReorderDelay's
  // B-run accounting, independent of isKeyframe.
  uint8_t flags = 0;
};

// Scans `data` (the leading bytes of one essence element -- not necessarily
// the whole frame) for Annex-B start codes, skips non-VCL NALs (SPS/PPS/SEI/
// AUD/etc.) structurally, and on the first VCL NAL (slice of a non-IDR or
// IDR picture) parses just `first_mb_in_slice`/`slice_type` (Exp-Golomb
// ue(v), no CABAC/macroblock decode) via an RBSP reader that transparently
// removes emulation-prevention bytes. Bounds-checked throughout; a short,
// garbage, or start-code-free buffer returns H264FrameInfo{}
// (recognized=false) rather than reading out of bounds.
H264FrameInfo ParseH264FrameHeader(const uint8_t* data, size_t size);

}  // namespace mxfsrc
