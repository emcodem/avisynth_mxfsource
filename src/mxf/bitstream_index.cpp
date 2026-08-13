#include "mxf/bitstream_index.h"

#include "mxf/essence_walker.h"
#include "mxf/h264_bitstream.h"
#include "mxf/mpeg2_bitstream.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

namespace {

// How many of each essence element's leading bytes to read when peeking for
// MPEG-2/H.264 headers -- comfortably larger than any real picture/slice
// header plus whatever SPS/PPS/SEI precede it, while still tiny compared to
// a full compressed frame (hundreds of KB to several MB), so this fallback
// stays cheap even over HTTP.
constexpr size_t kBitstreamPrefixBytes = 8192;

}  // namespace

std::optional<EssenceIndex> BuildEssenceIndexFromBitstream(MxfReader& reader, const MxfStreamInfo& streamInfo) {
  if (!streamInfo.hasVideo) return std::nullopt;

  const bool intraOnly = IsIntraOnlyCodec(streamInfo.videoCodec);
  const bool mpeg2 = streamInfo.videoCodec == CodecFamily::kMpeg2Video;
  const bool h264 = streamInfo.videoCodec == CodecFamily::kH264 || streamInfo.videoCodec == CodecFamily::kH264LongGop;
  if (!intraOnly && !mpeg2 && !h264) return std::nullopt;

  EssenceWalker walker(reader, streamInfo.headerMetadataEnd, streamInfo.videoTrackNumber, /*hasVideo=*/true, {});

  std::vector<FlatIndexEntry> entries;
  int64_t editUnit = 0;
  EssenceElement element;
  for (;;) {
    const bool ok = intraOnly ? walker.Next(element, /*wantData=*/false)
                               : walker.Next(element, /*wantData=*/true, kBitstreamPrefixBytes);
    if (!ok) break;
    if (!element.isVideo) continue;

    FlatIndexEntry entry;
    entry.editUnit = editUnit++;
    entry.byteOffset = element.filePosition;

    if (intraOnly) {
      entry.isKeyframe = true;
    } else if (mpeg2) {
      const Mpeg2FrameInfo info = ParseMpeg2FrameHeader(element.data.data(), element.data.size());
      if (info.recognized) {
        entry.isKeyframe = info.isKeyframe;
        entry.flags = info.flags;
      }
    } else {  // h264
      const H264FrameInfo info = ParseH264FrameHeader(element.data.data(), element.data.size());
      if (info.recognized) {
        entry.isKeyframe = info.isKeyframe;
        entry.flags = info.flags;
      }
    }
    // keyFrameOffset stays 0: only genuinely self-sufficient anchors are
    // ever marked isKeyframe here, so NeedsPreviousGop can never fire for
    // one of these entries (it only fires when editUnit+keyFrameOffset
    // lands before the candidate keyframe, which can't happen at 0).
    entries.push_back(entry);
  }

  if (entries.empty()) return std::nullopt;
  return EssenceIndex(std::move(entries));
}

}  // namespace mxfsrc
