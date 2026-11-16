#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mxf/klv.h"
#include "mxf/metadata_sets.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

// One discovered audio Track. Unlike video, real files can carry more than
// one (stereo pairs, per-language tracks) -- all are summed into one
// interleaved output stream when every track agrees on format (see
// MxfStreamInfo::audioConfigSupported).
struct AudioTrackInfo {
  // Track.TrackNumber (baseline tag 0x4804) -- same role as
  // MxfStreamInfo::videoTrackNumber, see its comment.
  std::array<uint8_t, 4> trackNumber{};
  // Track.TrackID (baseline tag 0x4801), used to bind this track to its
  // MultipleDescriptor sub-descriptor via LinkedTrackID when a file has more
  // than one audio track (see header_parser.cpp's ResolveDescriptor).
  uint32_t trackId = 0;
  uint32_t channelCount = 0;
  uint32_t sampleRate = 0;
  uint32_t bitsPerSample = 0;
  CodecFamily codec = CodecFamily::kUnknown;
  // The track's essence is the D-10/IMX "8-Ch AES3 Element" (SMPTE 386 /
  // SMPTE 331, key 06 0E 2B 34 01 02 01 01 0D 01 03 01 06 01 10 00 --
  // SDTI-CP sound item 0x06, element type 0x10): a 4-byte element header,
  // then per sample frame 8 channel slots of one 32-bit little-endian word
  // each, with the PCM sample in bits 4-27 (channel number below, V/U/C/P
  // bits above). See codec/pcm_interleave.h.
  bool eightChAes3Element = false;
};

struct MxfStreamInfo {
  bool hasVideo = false;
  uint32_t width = 0;
  uint32_t height = 0;
  Rational editRate{25, 1};
  int64_t numFrames = -1;  // -1 = unknown after Header Metadata alone
  CodecFamily videoCodec = CodecFamily::kUnknown;
  Ul pictureEssenceCoding{};
  // Picture descriptor FrameLayout (tag 0x320C, SMPTE 377-1: 0 FullFrame,
  // 1 SeparateFields, 2 OneField, 3 MixedFields, 4 SegmentedFrame) and
  // FieldDominance (0x3212: which field comes first in time), -1 if absent.
  // Only the fallback for the field order when the decoder doesn't report it
  // (see SourceEngine::FieldOrderOf).
  int frameLayout = -1;
  int fieldDominance = -1;
  // Colour description from the picture descriptor, as ITU-T H.273 codes
  // (the values of the _Matrix/_Primaries/_Transfer frame properties); -1 if
  // the tag is absent or its label isn't mapped (see ul_tables.h's
  // ClassifyColorUl). colorMatrix comes from CodingEquations (0x321A),
  // colorPrimaries from ColorPrimaries (0x3219), colorTransfer from
  // TransferCharacteristic (0x3210). colorRange uses the _ColorRange
  // convention (0 full, 1 limited) and is derived from the CDCI
  // BlackRefLevel/WhiteReflevel pair (0x3304/0x3305).
  int colorMatrix = -1;
  int colorPrimaries = -1;
  int colorTransfer = -1;
  int colorRange = -1;
  // Picture geometry from the picture descriptor, in descriptor units (for
  // FrameLayout SeparateFields / MixedFields the heights and Y offsets count
  // the lines of one field). aspectRatio is AspectRatio (0x320E), the display
  // aspect ratio of the display rectangle; {0, 0} if absent. The sampled
  // rectangle (0x3205 width, 0x3204 height, 0x3206/0x3207 offsets from the
  // stored rectangle) and the display rectangle (0x3209 width, 0x3208
  // height, 0x320A/0x320B offsets from the sampled rectangle) are -1 if
  // absent (offsets 0). See ComputePictureGeometry.
  Rational aspectRatio{0, 0};
  int64_t sampledWidth = -1;
  int64_t sampledHeight = -1;
  int64_t sampledXOffset = 0;
  int64_t sampledYOffset = 0;
  int64_t displayWidth = -1;
  int64_t displayHeight = -1;
  int64_t displayXOffset = 0;
  int64_t displayYOffset = 0;
  // Track.TrackNumber (baseline tag 0x4804): the last 4 bytes of an Essence
  // Element KLV key identify which Track it belongs to (see FFmpeg
  // mxfdec.c's mxf_essence_element_key matching) -- Phase 2's essence
  // walker uses this to pick video/audio KLVs out of the Body Partition.
  std::array<uint8_t, 4> videoTrackNumber{};

  bool hasAudio = false;
  // Resolved, validated, uniform values actually used for output -- only
  // meaningful when audioConfigSupported is true (see ParseMxfHeader's
  // validation pass and SourceEngine::Open's UseAudioMode handling).
  uint32_t audioSampleRate = 0;
  uint32_t audioChannels = 0;  // sum of every audioTracks[].channelCount
  uint32_t audioBitsPerSample = 0;
  CodecFamily audioCodec = CodecFamily::kUnknown;

  // Every discovered audio Track, in TrackID order.
  std::vector<AudioTrackInfo> audioTracks;
  // True when every entry in audioTracks agrees on bit depth and sample
  // rate, and all are uncompressed PCM -- the only configuration this
  // plugin can output audio for today (compressed codecs need a separate
  // avcodec decode wrapper, not yet implemented).
  bool audioConfigSupported = false;
  // Human-readable reason audioConfigSupported is false; empty otherwise.
  std::string audioUnsupportedReason;

  uint64_t headerMetadataEnd = 0;  // byte offset where Header Metadata ends; Phase 2's essence
                                    // walk starts scanning for the first Body Partition from here.
  bool footerPartitionPresent = false;

  // Header Partition Pack's own BodySID field (see mxf/partition.h's
  // PartitionPack) -- identifies which essence container this file's video
  // belongs to, used to correlate against an Index Table Segment's own
  // BodySID when building mxf/index_table.h's EssenceIndex.
  uint32_t bodySid = 0;

  // SourcePackage video Track.Origin (baseline tag 0x4B02): how many frames
  // of pre-roll/trim sit at the start of the essence container before the
  // MaterialPackage's intended playable range begins. 0 if the file has no
  // SourcePackage Origin (or it's genuinely 0 -- the common case). Per
  // SMPTE 377M, MaterialPackage.Origin must always be 0, so this is only
  // ever read off a set classified as a SourcePackage; see
  // \the mxf_origin README for the full
  // SP/MP Origin/Duration relationship this plugin honors.
  int64_t origin = 0;

  // SourcePackage video Sequence.Duration (baseline tag 0x0202): the raw,
  // untrimmed essence frame count, i.e. MP Duration + SP Origin. -1 if no
  // SourcePackage video track was found (e.g. malformed/unusual file) --
  // callers should fall back to numFrames as resolved below in that case.
  // Kept separate from numFrames so SourceEngine can apply/ignore the
  // Origin trim (OpenOptions::handle_origin) without re-parsing metadata.
  int64_t sourcePackageDuration = -1;

  // Per plan's Duration fallback chain: true once (1) Sequence.Duration (or
  // Descriptor.ContainerDuration) resolved AND (2) the Header Partition
  // Pack's FooterPartition offset is nonzero (file is closed/complete).
  // False means Phase 2's essence-KLV-count pre-pass fallback is needed
  // before trusting numFrames.
  bool durationReliable = false;
};

// Parses just the Header Partition Pack + Header Metadata (Preface(implicit)
// -> ContentStorage -> Package -> Track -> Sequence -> Descriptor chain) to
// produce MxfStreamInfo. No essence decode, no Index Table/RIP dependency --
// see plan Phase 1.
std::optional<MxfStreamInfo> ParseMxfHeader(MxfReader& reader, std::string* errorOut = nullptr);

// Sample aspect ratio and display window of the decoded frame
// (decodedWidth x decodedHeight), from MxfStreamInfo's descriptor geometry.
// The display rectangle (else the sampled one) is converted to frame lines
// (x2 for FrameLayout SeparateFields / MixedFields); when it doesn't fit in
// the decoded frame, the whole frame is used instead. SAR = AspectRatio x
// rectangle height / rectangle width, reduced; e.g. D-10 PAL 720x608 with
// a 720x576 display rectangle at line 32 and AspectRatio 16/9 gives 64:45
// and the window (0, 32, 720, 576). Only the known ratios 4:3 and 16:9 are
// used (within 1 %, snapped to the exact value), and for rectangles of 720
// lines and more only 16:9; any other AspectRatio is a defective label and
// gives no SAR (rejectedAspect says why).
struct PictureGeometry {
  int64_t sarNum = 0;  // 0: no usable AspectRatio in the descriptor
  int64_t sarDen = 0;
  std::string rejectedAspect;  // non-empty: the descriptor's AspectRatio was not used
  // The display rectangle in frame pixels; set only when it is valid and
  // smaller than the decoded frame (e.g. the VBI lines of D-10).
  bool hasWindow = false;
  int64_t windowLeft = 0;
  int64_t windowTop = 0;
  int64_t windowWidth = 0;
  int64_t windowHeight = 0;
};
PictureGeometry ComputePictureGeometry(const MxfStreamInfo& info, int decodedWidth, int decodedHeight);

}  // namespace mxfsrc
