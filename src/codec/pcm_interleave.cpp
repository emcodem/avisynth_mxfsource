#include "codec/pcm_interleave.h"

#include <cstring>

namespace mxfsrc {

namespace {

constexpr size_t kAes3ElementHeaderBytes = 4;
constexpr size_t kAes3SlotsPerSampleFrame = 8;
constexpr size_t kAes3BytesPerSampleFrame = kAes3SlotsPerSampleFrame * 4;

// Converts one SMPTE 331 AES3 element into plain interleaved little-endian
// PCM (channelCount channels of bitsPerSample bits). Returns false if the
// element isn't a whole number of 8-slot sample frames.
bool UnpackAes3Element(const std::vector<uint8_t>& element, const AudioTrackInfo& track, std::vector<uint8_t>& pcm) {
  if (element.size() < kAes3ElementHeaderBytes) return false;
  const size_t payload = element.size() - kAes3ElementHeaderBytes;
  if (payload % kAes3BytesPerSampleFrame != 0) return false;
  const size_t sampleFrames = payload / kAes3BytesPerSampleFrame;
  const size_t bytesPerSample = track.bitsPerSample / 8;
  const int shift = track.bitsPerSample == 24 ? 4 : 12;

  pcm.resize(sampleFrames * track.channelCount * bytesPerSample);
  uint8_t* dst = pcm.data();
  const uint8_t* src = element.data() + kAes3ElementHeaderBytes;
  for (size_t s = 0; s < sampleFrames; ++s, src += kAes3BytesPerSampleFrame) {
    for (uint32_t ch = 0; ch < track.channelCount; ++ch) {
      const uint8_t* w = src + ch * 4;
      const uint32_t word = static_cast<uint32_t>(w[0]) | (static_cast<uint32_t>(w[1]) << 8) |
                            (static_cast<uint32_t>(w[2]) << 16) | (static_cast<uint32_t>(w[3]) << 24);
      const uint32_t sample = word >> shift;
      for (size_t b = 0; b < bytesPerSample; ++b) *dst++ = static_cast<uint8_t>(sample >> (8 * b));
    }
  }
  return true;
}

}  // namespace

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
  // Plain PCM per track: the element's own data, or its AES3 unpacking.
  std::vector<const std::vector<uint8_t>*> trackPcm(tracks.size());
  std::vector<std::vector<uint8_t>> unpacked(tracks.size());
  int64_t sampleCount = -1;

  for (size_t i = 0; i < tracks.size(); ++i) {
    const uint32_t bytesPerSample = tracks[i].bitsPerSample / 8;
    frameBytes[i] = tracks[i].channelCount * bytesPerSample;
    if (frameBytes[i] == 0) return fail("audio track has zero channel count or bit depth");

    trackPcm[i] = &perTrackElements[i].data;
    if (tracks[i].aes3Packed) {
      if (!UnpackAes3Element(perTrackElements[i].data, tracks[i], unpacked[i])) {
        return fail("D-10/IMX AES3 audio element is not a whole number of 8-channel sample frames");
      }
      trackPcm[i] = &unpacked[i];
    }

    const size_t dataSize = trackPcm[i]->size();
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
      const uint8_t* src = trackPcm[i]->data() + static_cast<size_t>(s) * frameBytes[i];
      std::memcpy(dst, src, frameBytes[i]);
      dst += frameBytes[i];
    }
  }
  return true;
}

}  // namespace mxfsrc
