#include "mxf/essence_walker.h"

#include <cstring>

namespace mxfsrc {

bool IsEssenceElementKey(const Ul& key) {
  return UlPrefixEquals(key, kEssenceElementKeyPrefix, sizeof(kEssenceElementKeyPrefix));
}

EssenceWalker::EssenceWalker(MxfReader& reader, uint64_t startOffset, std::array<uint8_t, 4> videoTrackNumber,
                              bool hasVideo, std::vector<std::array<uint8_t, 4>> audioTrackNumbers)
    : reader_(reader),
      videoTrackNumber_(videoTrackNumber),
      hasVideo_(hasVideo),
      audioTrackNumbers_(std::move(audioTrackNumbers)) {
  reader_.Seek(startOffset);
}

bool EssenceWalker::Next(EssenceElement& out, bool wantData) {
  while (reader_.Tell() + 17 <= reader_.Size()) {
    const uint64_t klvStart = reader_.Tell();
    KlvHeader klv{};
    if (!ReadKlvHeader(reader_, klv)) return false;

    if (!LooksLikeKlvKey(klv.key)) {
      if (!reader_.Seek(klvStart + 1) || !ResyncToUlDesignator(reader_)) return false;
      continue;
    }

    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > reader_.Size()) return false;  // truncated file

    if (IsEssenceElementKey(klv.key)) {
      const bool matchesVideo = hasVideo_ && std::memcmp(&klv.key[12], videoTrackNumber_.data(), 4) == 0;
      int audioTrackIndex = -1;
      if (!matchesVideo) {
        for (size_t i = 0; i < audioTrackNumbers_.size(); ++i) {
          if (std::memcmp(&klv.key[12], audioTrackNumbers_[i].data(), 4) == 0) {
            audioTrackIndex = static_cast<int>(i);
            break;
          }
        }
      }
      if (matchesVideo || audioTrackIndex >= 0) {
        out.isVideo = matchesVideo;
        out.isAudio = audioTrackIndex >= 0;
        out.audioTrackIndex = audioTrackIndex;
        out.filePosition = klvStart;
        out.length = klv.length;
        if (!wantData) {
          out.data.clear();
          if (!reader_.Skip(klv.length)) return false;
          return true;
        }
        out.data.resize(klv.length);
        if (klv.length > 0 && !reader_.ReadBytes(out.data.data(), klv.length)) return false;
        return true;
      }
    }

    // Partition packs, Primer Pack repeats, Index Table Segments, System
    // Items, KLV Fill, essence elements from untracked tracks, etc. -- all
    // skipped the same way: move past the Value without interpreting it.
    if (!reader_.Skip(klv.length)) return false;
  }
  return false;
}

}  // namespace mxfsrc
