#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Essence Element KLV keys share this 12-byte prefix (verified against
// FFmpeg's libavformat/mxfdec.c mxf_essence_element_key); the trailing 4
// bytes of the key equal the owning Track's TrackNumber (baseline tag
// 0x4804, see header_parser's MxfStreamInfo::videoTrackNumber/audioTrackNumber).
inline constexpr uint8_t kEssenceElementKeyPrefix[12] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02,
                                                          0x01, 0x01, 0x0d, 0x01, 0x03, 0x01};

bool IsEssenceElementKey(const Ul& key);

struct EssenceElement {
  bool isVideo = false;
  bool isAudio = false;
  uint64_t filePosition = 0;  // KLV key offset, useful for diagnostics
  std::vector<uint8_t> data;  // raw KLV Value bytes -- Phase 3 hands this to avcodec as-is
};

// Walks forward through Body Partition(s) starting at `startOffset` (Phase
// 1's MxfStreamInfo::headerMetadataEnd), returning one EssenceElement per
// call to Next() for whichever tracked Track (video or audio TrackNumber)
// is encountered next, skipping over partition packs, index tables, system
// items, KLV fill, and any other unrelated KLVs in between. Never seeks
// backward -- matches the plan's forward-only decode design; a GOP-based
// codec's dependency on an earlier frame is automatically satisfied because
// the walk always starts from file position 0 (via Phase 1) and only moves
// forward. MVP scope: frame-wrapped OP1a only (see plan Phase 7 for
// clip-wrapped / OP-Atom).
class EssenceWalker {
 public:
  EssenceWalker(MxfReader& reader, uint64_t startOffset, std::array<uint8_t, 4> videoTrackNumber,
                bool hasVideo, std::array<uint8_t, 4> audioTrackNumber, bool hasAudio);

  // Advances to and returns the next matching essence element. Returns
  // false at EOF or on a malformed KLV (out.data is left empty in that case).
  bool Next(EssenceElement& out);

  uint64_t Position() const { return reader_.Tell(); }

 private:
  MxfReader& reader_;
  std::array<uint8_t, 4> videoTrackNumber_;
  bool hasVideo_;
  std::array<uint8_t, 4> audioTrackNumber_;
  bool hasAudio_;
};

}  // namespace mxfsrc
