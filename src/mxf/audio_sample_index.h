#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"

namespace mxfsrc {

// A resolved, queryable index of audio sample counts and byte offsets per
// edit unit, built once (see BuildAudioSampleIndexFromIndexTable below) and
// never mutated afterward.
//
// Unlike mxf/index_table.h's EssenceIndex (keyframe-granular -- fine for
// video, where a seek only ever needs to land on the nearest anchor frame),
// this records every edit unit's byte offset and cumulative sample count
// directly, since PCM has no keyframe concept and
// SourceEngine::GetAudioSamples needs to resolve *any* arbitrary sample
// position, not just the nearest anchor frame.
class AudioSampleIndex {
 public:
  AudioSampleIndex() = default;

  bool Empty() const { return editUnitByteOffset_.empty(); }
  int64_t TotalSamples() const { return totalSamples_; }
  int64_t NumEditUnits() const { return static_cast<int64_t>(editUnitByteOffset_.size()); }

  // Resolved location of sample position `samplePos` (must be in
  // [0, TotalSamples())): which edit unit covers it, the sample offset
  // within that edit unit's own decoded block, and that edit unit's byte
  // offset (for a fresh-reader seek, see SourceEngine::GetAudioSamples).
  struct Location {
    int64_t editUnit = 0;
    int64_t sampleOffsetInEditUnit = 0;
    uint64_t byteOffset = 0;
  };

  // O(log n) via binary search. std::nullopt if samplePos is out of range.
  std::optional<Location> Locate(int64_t samplePos) const;

  // Samples-per-channel in edit unit `editUnit` (0 if out of range).
  int64_t SamplesInEditUnit(int64_t editUnit) const;

  // Total samples-per-channel in edit units [0, editUnit) -- clamped to 0
  // below editUnit 0 and to TotalSamples() at/above NumEditUnits(). Used by
  // SourceEngine to translate MxfStreamInfo::origin's essence-edit-unit-
  // space pre-roll trim into a sample count, the same way SeekAndDecode
  // translates it via `originSkip_ + target` for video.
  int64_t CumulativeSamplesBefore(int64_t editUnit) const;

  // Byte offset of edit unit `editUnit`'s earliest audio essence element in
  // file order (0 if out of range -- callers should check editUnit against
  // NumEditUnits() first).
  uint64_t ByteOffsetOf(int64_t editUnit) const;

 private:
  friend std::optional<AudioSampleIndex> BuildAudioSampleIndexFromIndexTable(const EssenceIndex&,
                                                                              const MxfStreamInfo&, int64_t);

  // cumulativeSamplesBeforeEditUnit_[i] = total samples in edit units
  // [0, i); size NumEditUnits()+1, so SamplesInEditUnit(i) ==
  // cumulative[i+1] - cumulative[i]. editUnitByteOffset_[i] is edit unit
  // i's own byte offset; size NumEditUnits().
  std::vector<int64_t> cumulativeSamplesBeforeEditUnit_{0};
  std::vector<uint64_t> editUnitByteOffset_;
  int64_t totalSamples_ = 0;
};

// Builds the audio sample index directly from the already-parsed video
// Index Table (`videoIndex`, see mxf/index_table.h's BuildEssenceIndex) --
// zero essence walking, zero file I/O. Relies on this plugin's supported
// audio envelope: PCM only, no mid-file frame-rate/channel/track-config
// change, and audio+video essence always sharing one BodySID covered by
// that BodySID's own Index Table (a file where this doesn't hold simply
// doesn't get audio -- same graceful degrade as any other unsupported
// config, see SourceEngine::Open's UseAudioMode handling).
//
// Under those guarantees, every edit unit (content package) carries exactly
// the number of audio samples implied by streamInfo.audioSampleRate and the
// video streamInfo.editRate, computable in closed form -- this reproduces a
// constant per-edit-unit count when the ratio divides evenly (e.g. 1920
// samples/edit-unit for 25fps/48kHz), and the correct variable-length
// cadence otherwise (e.g. the classic NTSC 1601/1602 pattern for
// editRate={30000,1001}/48kHz). `videoIndex`'s own per-edit-unit byteOffset
// (the content package's start, per FlatIndexEntry's doc comment) is reused
// directly for SourceEngine::FreshReadAudioBlock's existing byte-offset-only
// read path.
//
// `rawFrameCount` is the raw, untrimmed essence-edit-unit-space frame count
// (SourceEngine::Open's originSkip_ + streamInfo.numFrames) -- the same
// coordinate space videoIndex's entries themselves live in.
//
// Returns std::nullopt (never a hard failure -- callers treat this exactly
// like any other unsupported-audio condition) unless ALL of the following
// hold:
//   - videoIndex is non-empty
//   - videoIndex.GetEntries().size() == rawFrameCount, with the first
//     entry's editUnit == 0 and the last's == rawFrameCount-1 (entries are
//     sorted-ascending-and-unique-by-editUnit, so this alone proves full
//     contiguous coverage of [0, rawFrameCount) with no gaps)
//   - streamInfo.editRate.num > 0 and streamInfo.audioSampleRate > 0
std::optional<AudioSampleIndex> BuildAudioSampleIndexFromIndexTable(const EssenceIndex& videoIndex,
                                                                     const MxfStreamInfo& streamInfo,
                                                                     int64_t rawFrameCount);

}  // namespace mxfsrc
