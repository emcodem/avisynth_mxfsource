#include "mxf/audio_sample_index.h"

#include <algorithm>

namespace mxfsrc {

uint64_t AudioSampleIndex::ByteOffsetOf(int64_t editUnit) const {
  if (editUnit < 0 || editUnit >= NumEditUnits()) return 0;
  return editUnitByteOffset_[static_cast<size_t>(editUnit)];
}

int64_t AudioSampleIndex::SamplesInEditUnit(int64_t editUnit) const {
  if (editUnit < 0 || editUnit >= NumEditUnits()) return 0;
  return cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(editUnit) + 1] -
         cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(editUnit)];
}

int64_t AudioSampleIndex::CumulativeSamplesBefore(int64_t editUnit) const {
  if (editUnit <= 0) return 0;
  if (editUnit >= static_cast<int64_t>(cumulativeSamplesBeforeEditUnit_.size())) return totalSamples_;
  return cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(editUnit)];
}

std::optional<AudioSampleIndex::Location> AudioSampleIndex::Locate(int64_t samplePos) const {
  if (samplePos < 0 || samplePos >= totalSamples_) return std::nullopt;
  auto it =
      std::upper_bound(cumulativeSamplesBeforeEditUnit_.begin(), cumulativeSamplesBeforeEditUnit_.end(), samplePos);
  const int64_t editUnit = static_cast<int64_t>(it - cumulativeSamplesBeforeEditUnit_.begin()) - 1;
  if (editUnit < 0 || editUnit >= NumEditUnits()) return std::nullopt;

  Location loc;
  loc.editUnit = editUnit;
  loc.sampleOffsetInEditUnit = samplePos - cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(editUnit)];
  loc.byteOffset = editUnitByteOffset_[static_cast<size_t>(editUnit)];
  return loc;
}

std::optional<AudioSampleIndex> BuildAudioSampleIndexFromIndexTable(const EssenceIndex& videoIndex,
                                                                     const MxfStreamInfo& streamInfo,
                                                                     int64_t rawFrameCount) {
  if (rawFrameCount <= 0) return std::nullopt;
  if (streamInfo.editRate.num <= 0) return std::nullopt;
  if (streamInfo.audioSampleRate == 0) return std::nullopt;
  if (videoIndex.Empty()) return std::nullopt;

  const std::vector<FlatIndexEntry>& entries = videoIndex.GetEntries();
  // Entries are sorted ascending & unique by editUnit (EssenceIndex's own
  // invariant) -- so matching both the count and the first/last editUnit
  // against [0, rawFrameCount) alone proves full contiguous coverage, no
  // gaps, with no per-element loop needed.
  if (static_cast<int64_t>(entries.size()) != rawFrameCount) return std::nullopt;
  if (entries.front().editUnit != 0 || entries.back().editUnit != rawFrameCount - 1) return std::nullopt;

  AudioSampleIndex index;
  index.editUnitByteOffset_.resize(static_cast<size_t>(rawFrameCount));
  index.cumulativeSamplesBeforeEditUnit_.resize(static_cast<size_t>(rawFrameCount) + 1);

  const int64_t sampleRate = static_cast<int64_t>(streamInfo.audioSampleRate);
  const int64_t den = static_cast<int64_t>(streamInfo.editRate.den);
  const int64_t num = static_cast<int64_t>(streamInfo.editRate.num);
  for (int64_t i = 0; i < rawFrameCount; ++i) {
    index.editUnitByteOffset_[static_cast<size_t>(i)] = entries[static_cast<size_t>(i)].byteOffset;
    // Closed-form cumulative sample count -- floor(i * sampleRate * den /
    // num) -- reproduces a constant per-edit-unit count when the ratio
    // divides evenly (e.g. 1920 for 25fps/48kHz), and the correct
    // variable-length cadence otherwise (e.g. NTSC's 1601/1602 pattern for
    // editRate={30000,1001}/48kHz). See BuildAudioSampleIndexFromIndexTable's
    // doc comment in the header for the full rationale.
    index.cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(i)] = (i * sampleRate * den) / num;
  }
  index.cumulativeSamplesBeforeEditUnit_[static_cast<size_t>(rawFrameCount)] =
      (rawFrameCount * sampleRate * den) / num;
  index.totalSamples_ = index.cumulativeSamplesBeforeEditUnit_.back();

  return index;
}

}  // namespace mxfsrc
