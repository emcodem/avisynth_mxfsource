#include "codec/pcm_interleave.h"

#include <cstring>

namespace mxfsrc {

bool InterleaveAudioEditUnit(const std::vector<EssenceElement>& perTrackElements,
                              const std::vector<AudioTrackInfo>& tracks, AudioBlock& out, std::string* errorOut) {
  auto fail = [&](const char* msg) {
    if (errorOut) *errorOut = msg;
    return false;
  };

  if (tracks.empty()) return fail("no audio tracks");
  if (perTrackElements.size() != tracks.size()) {
    return fail("audio element count does not match track count for this edit unit");
  }

  std::vector<uint32_t> frameBytes(tracks.size());  // one track's full sample frame (all its channels)
  int64_t sampleCount = -1;

  for (size_t i = 0; i < tracks.size(); ++i) {
    const uint32_t bytesPerSample = tracks[i].bitsPerSample / 8;
    frameBytes[i] = tracks[i].channelCount * bytesPerSample;
    if (frameBytes[i] == 0) return fail("audio track has zero channel count or bit depth");

    const size_t dataSize = perTrackElements[i].data.size();
    if (dataSize % frameBytes[i] != 0) {
      return fail("audio element byte length is not a multiple of the track's sample frame size");
    }
    const int64_t trackSampleCount = static_cast<int64_t>(dataSize / frameBytes[i]);
    if (sampleCount < 0) {
      sampleCount = trackSampleCount;
    } else if (trackSampleCount != sampleCount) {
      return fail("audio tracks disagree on sample count for this edit unit");
    }
  }

  out.sampleCount = sampleCount;
  uint32_t totalFrameBytes = 0;
  for (uint32_t f : frameBytes) totalFrameBytes += f;
  out.data.resize(static_cast<size_t>(sampleCount) * totalFrameBytes);

  uint8_t* dst = out.data.data();
  for (int64_t s = 0; s < sampleCount; ++s) {
    for (size_t i = 0; i < tracks.size(); ++i) {
      const uint8_t* src = perTrackElements[i].data.data() + static_cast<size_t>(s) * frameBytes[i];
      std::memcpy(dst, src, frameBytes[i]);
      dst += frameBytes[i];
    }
  }
  return true;
}

}  // namespace mxfsrc
