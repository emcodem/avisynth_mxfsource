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

}  // namespace mxfsrc
