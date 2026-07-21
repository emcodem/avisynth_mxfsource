// Manual verification tool (not part of ctest, not linked into anything
// else): writes a small real .mxf file with uncompressed UYVY essence to
// disk, given a path/frame-count/dimensions on argv, so the actual
// mxfsource.dll/.so can be exercised end to end through ffmpeg/vspipe like
// a real user would. No real sample .mxf files are available in this
// environment -- this is how Phase 5/6 were actually verified against the
// real AviSynth+/VapourSynth hosts, and it already caught three real bugs
// (BER length truncation in the test data builder, and two ring-buffer
// eviction races) that the pure in-process unit tests missed. Build with
// `cmake --build build --target gen_test_mxf`.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>

#include "synthetic_mxf.h"

using namespace mxfsrc_test;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: gen_test_mxf <output.mxf> <num_frames> [width] [height]" << std::endl;
    return 1;
  }
  const char* outPath = argv[1];
  const int numFrames = std::atoi(argv[2]);
  const int width = argc > 3 ? std::atoi(argv[3]) : 32;
  const int height = argc > 4 ? std::atoi(argv[4]) : 16;
  constexpr uint32_t kTrackNumber = 0x01010100;

  SyntheticFile cfg;
  cfg.sequenceDuration = numFrames;
  cfg.width = static_cast<uint32_t>(width);
  cfg.height = static_cast<uint32_t>(height);
  cfg.videoTrackNumber = kTrackNumber;

  std::vector<uint8_t> bytes = BuildSyntheticHeader(cfg);
  AppendPartitionPack(bytes, mxfsrc::PartitionKind::kBody);
  for (int i = 0; i < numFrames; ++i) {
    std::vector<uint8_t> payload(static_cast<size_t>(width) * height * 2);
    // Visibly distinct per-frame luma-ish value so a human can sanity-check
    // frame-to-frame variation when previewing the output.
    for (size_t b = 0; b < payload.size(); ++b) {
      payload[b] = static_cast<uint8_t>((b % 2 == 1) ? (16 + i * 8) : 128);  // UYVY: even=chroma, odd=luma
    }
    AppendEssenceElement(bytes, kTrackNumber, payload);
  }

  std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "failed to open " << outPath << " for writing" << std::endl;
    return 1;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  out.close();

  std::cout << "wrote " << bytes.size() << " bytes, " << numFrames << " frames, " << width << "x" << height
            << " to " << outPath << std::endl;
  return 0;
}
