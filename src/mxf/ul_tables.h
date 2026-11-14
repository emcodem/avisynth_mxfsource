#pragma once

#include "mxf/klv.h"

// UL tables verified against FFmpeg's libavformat/mxf.c (ff_mxf_data_definition_uls,
// ff_mxf_codec_uls), which SMPTE 377-1/RP224 implementations treat as the
// de facto correctness reference (see plan Phase 1 notes). Each entry's
// matchLen mirrors mxf.c: some UL families are only defined/matched up to a
// prefix shorter than 16 bytes.

namespace mxfsrc {

enum class EssenceKind { kUnknown, kVideo, kAudio, kData };

struct DataDefinitionUl {
  Ul ul;
  size_t matchLen;
  EssenceKind kind;
};

// SMPTE RP224 data definitions, as carried by Sequence.DataDefinition
// (local tag 0x0201) -- distinguishes a picture track from a sound track.
inline constexpr DataDefinitionUl kDataDefinitionUls[] = {
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00},
     13,
     EssenceKind::kVideo},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x02, 0x02, 0x00, 0x00, 0x00},
     13,
     EssenceKind::kAudio},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x02, 0x03, 0x00, 0x00, 0x00},
     13,
     EssenceKind::kData},
    // Avid Media Composer legacy Picture/Sound data definitions.
    {{0x80, 0x7D, 0x00, 0x60, 0x08, 0x14, 0x3E, 0x6F, 0x6F, 0x3C, 0x8C, 0xE1, 0x6C, 0xEF, 0x11, 0xD2},
     16,
     EssenceKind::kVideo},
    {{0x80, 0x7D, 0x00, 0x60, 0x08, 0x14, 0x3E, 0x6F, 0x78, 0xE1, 0xEB, 0xE1, 0x6C, 0xEF, 0x11, 0xD2},
     16,
     EssenceKind::kAudio},
};

inline EssenceKind ClassifyDataDefinition(const Ul& ul) {
  for (const auto& entry : kDataDefinitionUls) {
    if (UlPrefixEquals(ul, entry.ul.data(), entry.matchLen)) return entry.kind;
  }
  return EssenceKind::kUnknown;
}

enum class CodecFamily {
  kUnknown,
  kMpeg2Video,
  kMpeg4,
  kDvVideo,
  kJpeg2000,
  kVc1,
  kRawVideo,
  kV210,
  kDnxhd,
  kH264,
  kH264LongGop,
  kProres,
  kFfv1,
  kAac,
  kPcm,
  kAc3,
  kMp2,
};

struct CodecUl {
  Ul ul;
  size_t matchLen;
  CodecFamily family;
};

// PictureEssenceCoding (descriptor local tag 0x3201) and SoundEssenceCompression
// (0x3D06) values. Subset of ff_mxf_codec_uls covering the codecs named in
// the plan's Phase 3/7 scope; extend here as coverage grows.
inline constexpr CodecUl kCodecUls[] = {
    // PictureEssenceCoding
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03, 0x04, 0x01, 0x02, 0x02, 0x01, 0x01, 0x11, 0x00},
     14,
     CodecFamily::kMpeg2Video},  // MP@ML Long GoP (XDCAM)
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x01, 0x02, 0x02, 0x01, 0x02, 0x01, 0x01},
     14,
     CodecFamily::kMpeg2Video},  // D-10 50Mbps PAL (IMX)
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03, 0x04, 0x01, 0x02, 0x02, 0x01, 0x03, 0x03, 0x00},
     14,
     CodecFamily::kMpeg2Video},  // MP@HL (13 files in the 2026-10-19 scan, 1080/1440/720x608 4:2:0)
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03, 0x04, 0x01, 0x02, 0x02, 0x01, 0x04, 0x02, 0x00},
     14,
     CodecFamily::kMpeg2Video},  // 422P@HL I-Frame
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x03, 0x04, 0x01, 0x02, 0x02, 0x01, 0x05, 0x03, 0x00},
     14,
     CodecFamily::kMpeg2Video},  // MP@H-14 (1 file in the 2026-10-19 scan)
    // Byte 13 is the MPEG-2 profile/level; each one is listed on purpose
    // rather than matching the 13-byte prefix: that prefix is shared with
    // MPEG-4 Visual (01 20) and H.264 (01 31, 01 32).
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x01, 0x02, 0x02, 0x02, 0x01, 0x02, 0x00},
     13,
     CodecFamily::kDvVideo},  // DV25 IEC PAL
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x07, 0x04, 0x01, 0x02, 0x02, 0x03, 0x01, 0x01, 0x00},
     14,
     CodecFamily::kJpeg2000},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x01, 0x02, 0x01, 0x7F, 0x00, 0x00, 0x00},
     13,
     CodecFamily::kRawVideo},  // uncompressed
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0A, 0x04, 0x01, 0x02, 0x01, 0x01, 0x02, 0x01, 0x00},
     15,
     CodecFamily::kRawVideo},  // uncompressed 4:2:2 8-bit
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x01, 0x02, 0x02, 0x71, 0x00, 0x00, 0x00},
     13,
     CodecFamily::kDnxhd},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x01, 0x02, 0x02, 0x03, 0x02, 0x00, 0x00},
     14,
     CodecFamily::kDnxhd},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0A, 0x04, 0x01, 0x02, 0x02, 0x01, 0x32, 0x00, 0x00},
     14,
     CodecFamily::kH264},  // AVC-Intra (also matches Sony XAVC-Intra, verified against a real
                            // Atomos Shogun XAVC-I file -- same registered coding UL family)
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D, 0x04, 0x01, 0x02, 0x02, 0x01, 0x31, 0x00, 0x00},
     14,
     CodecFamily::kH264LongGop},  // Sony XAVC long-GOP (XAVC-L/XAVC-HD Long GOP), verified
                                   // against a real Atomos Shogun XAVC-L file -- distinct
                                   // CodecFamily from plain kH264 since this is genuinely
                                   // long-GOP (B-frames), unlike every other UL mapped to kH264.
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0A, 0x04, 0x01, 0x02, 0x01, 0x01, 0x02, 0x02, 0x01},
     16,
     CodecFamily::kV210},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D, 0x04, 0x01, 0x02, 0x02, 0x03, 0x06, 0x00, 0x00},
     14,
     CodecFamily::kProres},  // Apple ProRes
    {{0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x0D, 0x04, 0x01, 0x02, 0x02, 0x03, 0x09, 0x01, 0x00},
     15,
     CodecFamily::kFfv1},
    // SoundEssenceCompression
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00},
     13,
     CodecFamily::kPcm},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0A, 0x04, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00},
     13,
     CodecFamily::kPcm},  // SMPTE382MDefaultUncompressedSoundCoding -- the most common PCM label on
                          // real broadcast files (640 of 1479 on the sample share)
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x02, 0x02, 0x01, 0x7F, 0x00, 0x00, 0x00},
     13,
     CodecFamily::kPcm},
    // Not mapped: AIFFUncompressedCoding (...04 02 02 01 7E) is big-endian PCM, and the
    // audio path assumes little-endian samples.
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x02, 0x02, 0x02, 0x03, 0x02, 0x01, 0x00},
     15,
     CodecFamily::kAc3},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x01, 0x04, 0x02, 0x02, 0x02, 0x03, 0x02, 0x05, 0x00},
     15,
     CodecFamily::kMp2},
    {{0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D, 0x04, 0x02, 0x02, 0x02, 0x04, 0x03, 0x00, 0x00},
     14,
     CodecFamily::kAac},
};

inline CodecFamily ClassifyCodecUl(const Ul& ul) {
  for (const auto& entry : kCodecUls) {
    if (UlPrefixEquals(ul, entry.ul.data(), entry.matchLen)) return entry.family;
  }
  return CodecFamily::kUnknown;
}

// Picture descriptor colour labels (SMPTE RP224 / ST 2067-21), mapped to the
// ITU-T H.273 code points that AviSynth+ and VapourSynth use for _Primaries,
// _Transfer and _Matrix. All three share the prefix
// 06 0E 2B 34 04 01 01 xx 04 01 01 01 and differ in byte 12 (01 transfer,
// 02 coding equations, 03 primaries) and byte 13 (the value). Labels with no
// unambiguous H.273 equivalent (e.g. "ITU-R BT.470" transfer) map to -1.
struct ColorUl {
  uint8_t group;  // byte 12
  uint8_t item;   // byte 13
  int h273;
};

inline constexpr uint8_t kColorUlPrefix[] = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01,
                                             0x00, 0x04, 0x01, 0x01, 0x01};
inline constexpr uint8_t kColorGroupTransfer = 0x01;
inline constexpr uint8_t kColorGroupCodingEquations = 0x02;
inline constexpr uint8_t kColorGroupPrimaries = 0x03;

inline constexpr ColorUl kColorUls[] = {
    // TransferCharacteristic (descriptor tag 0x3210)
    {0x01, 0x02, 1},   // ITU-R BT.709
    {0x01, 0x03, 7},   // SMPTE 240M
    {0x01, 0x04, 1},   // SMPTE 274M/296M (same curve as BT.709)
    {0x01, 0x05, 12},  // ITU-R BT.1361 extended colour gamut
    {0x01, 0x06, 8},   // linear
    {0x01, 0x07, 17},  // SMPTE DC28 DCDM (ST 428-1)
    {0x01, 0x08, 11},  // IEC 61966-2-4 (xvYCC)
    {0x01, 0x09, 14},  // ITU-R BT.2020
    {0x01, 0x0A, 16},  // SMPTE ST 2084 (PQ)
    {0x01, 0x0B, 18},  // Hybrid Log-Gamma (ARIB STD-B67 / BT.2100 HLG)
    // CodingEquations (0x321A)
    {0x02, 0x01, 5},  // ITU-R BT.601
    {0x02, 0x02, 1},  // ITU-R BT.709
    {0x02, 0x03, 7},  // SMPTE 240M
    {0x02, 0x04, 8},  // YCgCo
    {0x02, 0x05, 0},  // GBR (identity)
    {0x02, 0x06, 9},  // ITU-R BT.2020 non-constant luminance
    // ColorPrimaries (0x3219)
    {0x03, 0x01, 6},   // SMPTE 170M (BT.601 525)
    {0x03, 0x02, 5},   // ITU-R BT.470 BG (BT.601 625)
    {0x03, 0x03, 1},   // ITU-R BT.709
    {0x03, 0x04, 9},   // ITU-R BT.2020
    {0x03, 0x05, 10},  // SMPTE DC28 DCDM (XYZ, ST 428-1)
    {0x03, 0x06, 12},  // P3 D65 (SMPTE EG 432-1)
};

// H.273 code for a colour label of the given group, or -1 if the UL is not
// such a label or not one we map. The version byte (7) is ignored.
inline int ClassifyColorUl(const Ul& ul, uint8_t group) {
  if (!UlPrefixEquals(ul, kColorUlPrefix, sizeof(kColorUlPrefix))) return -1;
  if (ul[12] != group) return -1;
  for (const auto& entry : kColorUls) {
    if (entry.group == group && entry.item == ul[13]) return entry.h273;
  }
  return -1;
}

// True for codecs where every essence element decodes independently (no
// inter-frame prediction) -- these have no "anchor"/keyframe concept at
// all, so a forward seek can skip straight to the target frame's essence
// element without decoding (or needing to decode) anything before it.
// Used by SourceEngine::SeekAndDecode's fast path.
//
// Conservatively false (assume inter-frame dependencies may exist) for
// anything not verified intra-only: kMpeg2Video covers both intra-only
// IMX/D-10 *and* long-GOP XDCAM HD under the same UL-classified family
// (see kCodecUls above -- we don't yet distinguish the profile), and a
// naive skip-without-decode would produce garbage for the long-GOP case
// since P/B frames need actual decoded reference pictures the decoder
// never saw. Likewise kMpeg4/kVc1 are not verified intra-only in the MXF
// profiles we've seen. kH264 is safe here because our UL table
// (kCodecUls) maps only AVC-Intra-shaped coding ULs to it (Panasonic
// AVC-Intra and Sony XAVC-Intra, both verified I-frame-only against real
// files); genuine long-GOP H.264 (Sony XAVC-L) is classified as the
// separate CodecFamily::kH264LongGop specifically so it does NOT hit this
// fast path -- it has real B-frame reference dependencies like any other
// long-GOP codec here.
inline bool IsIntraOnlyCodec(CodecFamily family) {
  switch (family) {
    case CodecFamily::kRawVideo:
    case CodecFamily::kV210:
    case CodecFamily::kDnxhd:
    case CodecFamily::kProres:
    case CodecFamily::kJpeg2000:
    case CodecFamily::kFfv1:
    case CodecFamily::kDvVideo:
    case CodecFamily::kH264:
      return true;
    default:
      return false;
  }
}

}  // namespace mxfsrc
