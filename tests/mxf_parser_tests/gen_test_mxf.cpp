// Manual verification tool (not part of ctest, not linked into anything
// else): writes a small real .mxf file with uncompressed UYVY essence (and
// optionally one or more synthetic PCM audio tracks) to disk, given a
// path/frame-count/dimensions on argv, so the actual
// mxfsource.dll/.so/mxfsource_vs.dll can be exercised end to end through
// ffmpeg/vspipe like a real user would. No real sample .mxf files are
// available in this environment -- this is how Phase 5/6 (and the
// multi-track audio pass) were actually verified against the real
// AviSynth+/VapourSynth hosts. Build with
// `cmake --build build --target gen_test_mxf`.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "synthetic_mxf.h"

using namespace mxfsrc_test;

namespace {

std::vector<uint32_t> ParseChannelsCsv(const std::string& csv) {
  std::vector<uint32_t> out;
  std::stringstream ss(csv);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.push_back(static_cast<uint32_t>(std::atoi(item.c_str())));
  }
  return out;
}

// Deterministic, byte-exact-verifiable PCM content: for edit unit `e`,
// track `t`, channel `c`, sample `s` (all 0-indexed), value =
// e*10000 + t*1000 + c*100 + s, packed little-endian into `bytesPerSample`
// bytes -- easy for an external script (see samples/ or a one-off Python
// vapoursynth check) to recompute independently and diff against.
std::vector<uint8_t> MakeAudioEditUnitPayload(int editUnit, int track, uint32_t channels, uint32_t bytesPerSample,
                                                int samplesPerEditUnit) {
  std::vector<uint8_t> data;
  data.reserve(static_cast<size_t>(samplesPerEditUnit) * channels * bytesPerSample);
  for (int s = 0; s < samplesPerEditUnit; ++s) {
    for (uint32_t c = 0; c < channels; ++c) {
      uint32_t value = static_cast<uint32_t>(editUnit) * 10000 + static_cast<uint32_t>(track) * 1000 + c * 100 +
                       static_cast<uint32_t>(s);
      for (uint32_t b = 0; b < bytesPerSample; ++b) {
        data.push_back(static_cast<uint8_t>((value >> (8 * b)) & 0xFF));
      }
    }
  }
  return data;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: gen_test_mxf <output.mxf> <num_frames> [width] [height] "
                 "[audio_channels_csv, e.g. 1,2] [sample_rate] [bits_per_sample] [samples_per_edit_unit]"
              << std::endl;
    return 1;
  }
  const char* outPath = argv[1];
  const int numFrames = std::atoi(argv[2]);
  const int width = argc > 3 ? std::atoi(argv[3]) : 32;
  const int height = argc > 4 ? std::atoi(argv[4]) : 16;
  const std::string audioChannelsCsv = argc > 5 ? argv[5] : "";
  const uint32_t sampleRate = argc > 6 ? static_cast<uint32_t>(std::atoi(argv[6])) : 48000;
  const uint32_t bitsPerSample = argc > 7 ? static_cast<uint32_t>(std::atoi(argv[7])) : 24;
  const int samplesPerEditUnit = argc > 8 ? std::atoi(argv[8]) : static_cast<int>(sampleRate / 25);
  constexpr uint32_t kVideoTrackNumber = 0x01010100;
  constexpr uint32_t kAudioTrackNumberBase = 0x02020200;

  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = static_cast<uint32_t>(width);
  cfg.height = static_cast<uint32_t>(height);
  cfg.videoTrackNumber = kVideoTrackNumber;

  const std::vector<uint32_t> audioChannels = ParseChannelsCsv(audioChannelsCsv);
  const uint32_t bytesPerSample = bitsPerSample / 8;
  for (size_t t = 0; t < audioChannels.size(); ++t) {
    SyntheticAudioTrack at;
    at.trackNumber = kAudioTrackNumberBase + static_cast<uint32_t>(t) * 0x100;
    at.trackId = 100 + static_cast<uint32_t>(t);
    at.channelCount = audioChannels[t];
    at.sampleRate = sampleRate;
    at.bitsPerSample = bitsPerSample;
    cfg.audioTracks.push_back(at);
  }

  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, mxfsrc::PartitionKind::kBody);
  for (int i = 0; i < numFrames; ++i) {
    std::vector<uint8_t> payload(static_cast<size_t>(width) * height * 2);
    // Visibly distinct per-frame luma-ish value so a human can sanity-check
    // frame-to-frame variation when previewing the output.
    for (size_t b = 0; b < payload.size(); ++b) {
      payload[b] = static_cast<uint8_t>((b % 2 == 1) ? (16 + i * 8) : 128);  // UYVY: even=chroma, odd=luma
    }
    AppendEssenceElement(bytes, kVideoTrackNumber, payload);

    for (size_t t = 0; t < audioChannels.size(); ++t) {
      AppendEssenceElement(bytes, cfg.audioTracks[t].trackNumber,
                            MakeAudioEditUnitPayload(i, static_cast<int>(t), audioChannels[t], bytesPerSample,
                                                      samplesPerEditUnit));
    }
  }

  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "failed to open " << outPath << " for writing" << std::endl;
    return 1;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();

  std::cout << "wrote " << bytes.size() << " bytes, " << numFrames << " frames, " << width << "x" << height;
  if (!audioChannels.empty()) {
    std::cout << ", " << audioChannels.size() << " audio track(s), " << bitsPerSample << "-bit @ " << sampleRate
               << "Hz, " << samplesPerEditUnit << " samples/edit unit";
  }
  std::cout << " to " << outPath << std::endl;
  return 0;
}
