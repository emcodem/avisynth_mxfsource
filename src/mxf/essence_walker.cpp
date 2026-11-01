#include "mxf/essence_walker.h"

#include <algorithm>
#include <cstring>

namespace mxfsrc {

bool IsEssenceElementKey(const Ul& key) {
  return UlPrefixEquals(key, kEssenceElementKeyPrefix, sizeof(kEssenceElementKeyPrefix));
}

EssenceWalker::EssenceWalker(MxfReader& reader, uint64_t startOffset, std::array<uint8_t, 4> videoTrackNumber,
                              bool hasVideo, std::vector<std::array<uint8_t, 4>> audioTrackNumbers,
                              EssenceWalkerOptions options)
    : reader_(reader),
      videoTrackNumber_(videoTrackNumber),
      hasVideo_(hasVideo),
      audioTrackNumbers_(std::move(audioTrackNumbers)),
      options_(std::move(options)) {
  reader_.Seek(startOffset);
}

bool EssenceWalker::HandleDamage(uint64_t offset) {
  WalkDamage damage;
  damage.offset = offset;
  if (options_.maxResyncBytes > 0 && reader_.Seek(offset + 1) &&
      ResyncToUlDesignator(reader_, offset + 1 + options_.maxResyncBytes)) {
    damage.resumed = true;
    damage.resumedAt = reader_.Tell();
  }
  if (options_.onDamage) options_.onDamage(damage);
  return damage.resumed;
}

bool EssenceWalker::Next(EssenceElement& out, bool wantData, size_t maxBytes) {
  while (reader_.Tell() + 17 <= reader_.Size()) {
    const uint64_t klvStart = reader_.Tell();
    KlvHeader klv{};
    if (!ReadKlvHeader(reader_, klv)) {
      // A damaged length field: nothing to resync on reliably, the walk ends.
      if (options_.onDamage) options_.onDamage(WalkDamage{klvStart, /*badLength=*/true, false, 0});
      return false;
    }

    // No key where the previous value ended: the file is damaged here (see
    // LooksLikeKlvKey). Resync only as far as options_ allow -- not at all
    // in strict mode.
    if (!LooksLikeKlvKey(klv.key)) {
      if (!HandleDamage(klvStart)) return false;
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
        const uint64_t readLen = std::min<uint64_t>(klv.length, maxBytes);
        out.data.resize(readLen);
        if (readLen > 0 && !reader_.ReadBytes(out.data.data(), readLen)) return false;
        if (!reader_.Seek(valueEnd)) return false;
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
