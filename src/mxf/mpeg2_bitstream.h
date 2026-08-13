#pragma once

#include <cstddef>
#include <cstdint>

namespace mxfsrc {

// Result of inspecting one frame-wrapped MPEG-2 video essence element's
// leading bytes -- see mxf/bitstream_index.h, which uses this to build a
// fallback EssenceIndex without decoding when a file has no usable SMPTE
// Index Table Segment at all.
struct Mpeg2FrameInfo {
  bool recognized = false;  // a picture_header was found and parsed
  // True only for an I-picture immediately preceded (within the same
  // prefix) by a group_start_code whose closed_gop bit is set. An I-picture
  // with no group header seen, or one whose group header has closed_gop==0
  // (open GOP), is deliberately never trusted as a seek entry point here --
  // an open GOP's leading B-pictures reference the *previous* GOP, so
  // restarting fresh at this I-picture alone would decode them wrong. This
  // mirrors the exact class of bug mxf/index_table.h's NeedsPreviousGop
  // exists to catch for real Index Table Segment data (found on a real
  // Canon XAVC-L file), just applied conservatively here since this parser
  // has no KeyFrameOffset-equivalent to fall back on.
  bool isKeyframe = false;
  // Synthesized IndexTableEntry-style flags (mxf/index_table.h's
  // IsKeyframeFlags/IsBFrameFlags bit meanings): I=0x00, P=0x20 (bit5,
  // forward-prediction-used), B=0x30 (bit4+bit5, backward+forward). Used
  // only for EstimateReorderDelay's B-run accounting, independent of
  // isKeyframe.
  uint8_t flags = 0;
};

// Scans `data` (the leading bytes of one essence element -- not necessarily
// the whole frame) for MPEG-2 start codes, tracking the most recently seen
// group_start_code's closed_gop bit and classifying the first picture_header
// found. Stops at the first picture header (one picture per frame-wrapped
// essence element). Bounds-checked throughout; a short, garbage, or
// start-code-free buffer returns Mpeg2FrameInfo{} (recognized=false) rather
// than reading out of bounds.
Mpeg2FrameInfo ParseMpeg2FrameHeader(const uint8_t* data, size_t size);

}  // namespace mxfsrc
