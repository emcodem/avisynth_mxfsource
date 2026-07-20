#include "mxf/essence_walker.h"

#include <cstring>

namespace mxfsrc {

bool IsEssenceElementKey(const Ul& key) {
  return UlPrefixEquals(key, kEssenceElementKeyPrefix, sizeof(kEssenceElementKeyPrefix));
}

EssenceWalker::EssenceWalker(MxfReader& reader, uint64_t startOffset, std::array<uint8_t, 4> videoTrackNumber,
                              bool hasVideo, std::array<uint8_t, 4> audioTrackNumber, bool hasAudio)
    : reader_(reader),
      videoTrackNumber_(videoTrackNumber),
      hasVideo_(hasVideo),
      audioTrackNumber_(audioTrackNumber),
      hasAudio_(hasAudio) {
  reader_.Seek(startOffset);
}

bool EssenceWalker::Next(EssenceElement& out) {
  while (reader_.Tell() + 17 <= reader_.Size()) {
    const uint64_t klvStart = reader_.Tell();
    KlvHeader klv{};
    if (!ReadKlvHeader(reader_, klv)) return false;
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > reader_.Size()) return false;  // truncated file

    if (IsEssenceElementKey(klv.key)) {
      const bool matchesVideo = hasVideo_ && std::memcmp(&klv.key[12], videoTrackNumber_.data(), 4) == 0;
      const bool matchesAudio = hasAudio_ && std::memcmp(&klv.key[12], audioTrackNumber_.data(), 4) == 0;
      if (matchesVideo || matchesAudio) {
        out.isVideo = matchesVideo;
        out.isAudio = matchesAudio && !matchesVideo;
        out.filePosition = klvStart;
        out.data.resize(klv.length);
        if (klv.length > 0 && !reader_.ReadBytes(out.data.data(), klv.length)) return false;
        return true;
      }
    }

    // Partition packs, Primer Pack repeats, Index Table Segments, System
    // Items, KLV Fill, essence elements from untracked tracks, etc. -- all
    // skipped the same way: move past the Value without interpreting it.
    if (!reader_.Seek(valueEnd)) return false;
  }
  return false;
}

}  // namespace mxfsrc
