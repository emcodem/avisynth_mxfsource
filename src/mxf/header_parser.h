#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "mxf/klv.h"
#include "mxf/metadata_sets.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

struct MxfStreamInfo {
  bool hasVideo = false;
  uint32_t width = 0;
  uint32_t height = 0;
  Rational editRate{25, 1};
  int64_t numFrames = -1;  // -1 = unknown after Header Metadata alone
  CodecFamily videoCodec = CodecFamily::kUnknown;
  Ul pictureEssenceCoding{};
  // Track.TrackNumber (baseline tag 0x4804): the last 4 bytes of an Essence
  // Element KLV key identify which Track it belongs to (see FFmpeg
  // mxfdec.c's mxf_essence_element_key matching) -- Phase 2's essence
  // walker uses this to pick video/audio KLVs out of the Body Partition.
  std::array<uint8_t, 4> videoTrackNumber{};

  bool hasAudio = false;
  uint32_t audioSampleRate = 0;
  uint32_t audioChannels = 0;
  uint32_t audioBitsPerSample = 0;
  CodecFamily audioCodec = CodecFamily::kUnknown;
  std::array<uint8_t, 4> audioTrackNumber{};

  uint64_t headerMetadataEnd = 0;  // byte offset where Header Metadata ends; Phase 2's essence
                                    // walk starts scanning for the first Body Partition from here.
  bool footerPartitionPresent = false;

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
