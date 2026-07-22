#include "mxf/essence_walker.h"

#include <cstring>

namespace mxfsrc {

namespace {

// Every legitimate KLV key begins with the SMPTE UL registry designator
// (06.0E.2B.34, ST 298). Some real-world recorders (observed: Atomos Shogun
// XAVC-Intra/XAVC-L recordings) pad a partition out to a KAG boundary using
// raw zero bytes instead of a spec-compliant KLV Fill Item -- such a run of
// zeros parses as a spurious zero-length "KLV" with an all-zero key, and
// naively trusting its (bogus) length field walks forward 17 bytes at a
// time, which is generally not a multiple of the actual pad length. That
// desyncs the walk so it lands misaligned inside real KLV data afterward
// and fails outright instead of just skipping the padding.
constexpr uint8_t kUlDesignator[4] = {0x06, 0x0e, 0x2b, 0x34};

bool LooksLikeKlvKey(const Ul& key) { return std::memcmp(key.data(), kUlDesignator, sizeof(kUlDesignator)) == 0; }

// Scans forward byte-by-byte from the reader's current position for the
// next occurrence of the UL designator, leaving the reader positioned
// exactly at its first byte so the caller can retry ReadKlvHeader there.
bool ResyncToUlDesignator(MxfReader& reader) {
  uint8_t window[4] = {};
  size_t have = 0;
  while (reader.Tell() < reader.Size()) {
    uint8_t b = 0;
    if (!reader.ReadU8(b)) return false;
    if (have < 4) {
      window[have++] = b;
    } else {
      window[0] = window[1];
      window[1] = window[2];
      window[2] = window[3];
      window[3] = b;
    }
    if (have == 4 && std::memcmp(window, kUlDesignator, 4) == 0) return reader.Seek(reader.Tell() - 4);
  }
  return false;
}

}  // namespace

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

    if (!LooksLikeKlvKey(klv.key)) {
      if (!reader_.Seek(klvStart + 1) || !ResyncToUlDesignator(reader_)) return false;
      continue;
    }

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
