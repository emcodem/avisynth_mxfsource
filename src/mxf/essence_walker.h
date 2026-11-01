#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Essence Element KLV keys share this 12-byte prefix (verified against
// FFmpeg's libavformat/mxfdec.c mxf_essence_element_key); the trailing 4
// bytes of the key equal the owning Track's TrackNumber (baseline tag
// 0x4804, see header_parser's MxfStreamInfo::videoTrackNumber/AudioTrackInfo::trackNumber).
inline constexpr uint8_t kEssenceElementKeyPrefix[12] = {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x02,
                                                          0x01, 0x01, 0x0d, 0x01, 0x03, 0x01};

bool IsEssenceElementKey(const Ul& key);

struct EssenceElement {
  bool isVideo = false;
  bool isAudio = false;
  // Index into the audioTrackNumbers list passed to EssenceWalker's
  // constructor, valid iff isAudio -- lets a caller with several summed
  // audio tracks place this element correctly without re-matching bytes.
  // -1 otherwise.
  int audioTrackIndex = -1;
  uint64_t filePosition = 0;  // KLV key offset, useful for diagnostics
  // The KLV Value's byte length -- always set, even when Next() was called
  // with wantData=false (data left empty in that case). Lets a caller that
  // only needs sizes (e.g. AudioSampleIndex's header-only prescan) avoid
  // paying for the payload read entirely.
  uint64_t length = 0;
  std::vector<uint8_t> data;  // raw KLV Value bytes -- handed to a decoder/interleaver as-is
};

// A spot where no valid KLV starts right after the previous value -- never
// payload (values are skipped by their length, never looked into), so in an
// intact file this doesn't happen: the file is damaged at `offset`.
struct WalkDamage {
  uint64_t offset = 0;
  // A key was there but its length field is invalid; never resynced.
  bool badLength = false;
  // True if the walk went on: a resync found the next key at `resumedAt`.
  // False if the walk ended at `offset`.
  bool resumed = false;
  uint64_t resumedAt = 0;
};

struct EssenceWalkerOptions {
  // How far past a damaged spot the walk may search for the next key (see
  // ResyncToUlDesignator). 0: never -- the walk ends at the damage, so the
  // frames after it are unreachable and fail (strict mode: no guessing past
  // damage). A damaged length field (a BER length of more than 8 bytes)
  // always ends the walk.
  uint64_t maxResyncBytes = kMaxResyncBytes;
  // Called for every damaged spot met, whether the walk went on or not.
  std::function<void(const WalkDamage&)> onDamage;
};

// Walks forward through Body Partition(s) starting at `startOffset` (Phase
// 1's MxfStreamInfo::headerMetadataEnd), returning one EssenceElement per
// call to Next() for whichever tracked Track (video or one of the audio
// TrackNumbers) is encountered next, skipping over partition packs, index
// tables, system items, KLV fill, and any other unrelated KLVs in between.
// Never seeks backward -- matches the plan's forward-only decode design; a
// GOP-based codec's dependency on an earlier frame is automatically
// satisfied because the walk always starts from file position 0 (via Phase
// 1) and only moves forward. MVP scope: frame-wrapped OP1a only (see plan
// Phase 7 for clip-wrapped / OP-Atom).
class EssenceWalker {
 public:
  EssenceWalker(MxfReader& reader, uint64_t startOffset, std::array<uint8_t, 4> videoTrackNumber,
                bool hasVideo, std::vector<std::array<uint8_t, 4>> audioTrackNumbers,
                EssenceWalkerOptions options = {});

  // Advances to and returns the next matching essence element. Returns
  // false at EOF or on a malformed KLV (out.data is left empty in that case).
  //
  // `wantData=false` skips reading the matched element's payload bytes off
  // disk/network entirely (just seeks past its Value) -- out.data is left
  // empty, but isVideo/isAudio/audioTrackIndex/filePosition are still valid.
  // For a caller that only needs to count/locate elements (e.g.
  // SourceEngine::SeekAndDecode's intra-only-codec skip-to-target loop, or
  // AudioSampleIndex's KLV-length-only prescan), this is the difference
  // between a cheap seek and actually transferring every intervening
  // frame's full payload -- real bug found on a real 93GB XAVC-Intra file
  // over an SMB share: seeking to frame 5000 dragged several GB of essence
  // data across the network only to discard all of it, because Next()
  // unconditionally read each skipped video element's payload even though
  // nothing was ever going to look at it.
  //
  // `maxBytes` (only consulted when wantData=true) bounds how many of the
  // matched element's Value bytes actually get read into out.data -- the
  // remainder is Seek()'d past, never transferred. out.data.size() can
  // therefore be smaller than out.length when this is used; out.length is
  // always the element's true, full size regardless. For
  // mxf/bitstream_index.h's fallback keyframe detection, which only needs
  // to see a frame's leading MPEG-2/H.264 headers (a few KB at most) to
  // classify it, this avoids paying for every frame's full compressed
  // payload just to peek at its first bytes -- same rationale as
  // wantData=false, just for callers that need *some* leading bytes rather
  // than none.
  bool Next(EssenceElement& out, bool wantData = true, size_t maxBytes = SIZE_MAX);

  uint64_t Position() const { return reader_.Tell(); }

 private:
  // Handles a damaged spot at `offset` per options_: reports it, and returns
  // true with the reader on the next key if a resync found one in range.
  bool HandleDamage(uint64_t offset);

  MxfReader& reader_;
  std::array<uint8_t, 4> videoTrackNumber_;
  bool hasVideo_;
  std::vector<std::array<uint8_t, 4>> audioTrackNumbers_;
  EssenceWalkerOptions options_;
};

}  // namespace mxfsrc
