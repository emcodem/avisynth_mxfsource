// Manual diagnostic tool (not part of ctest, not linked into anything
// else): opens a .mxf file via SourceEngine directly (no AviSynth+/
// VapourSynth involved) and reads every frame, printing per-frame
// diagnostics. Useful for isolating whether a bug is in the core decode
// pipeline or in a host adapter -- e.g. confirmed the Phase 5 ring-buffer
// crash was adapter-side, not core-side, by reading the exact same file
// that crashed the real AviSynth+ host without issue here. Build with
// `cmake --build build --target read_test_mxf`.
#include <iostream>
#include <memory>

#include "core/source_engine.h"

using namespace mxfsrc;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: read_test_mxf <file.mxf>" << std::endl;
    return 1;
  }
  SourceEngine engine;
  OpenResult res = engine.Open(argv[1]);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open failed: " << static_cast<int>(res.status) << " " << res.error << std::endl;
    return 1;
  }
  const auto& info = engine.StreamInfo();
  std::cout << "opened: " << info.width << "x" << info.height << " numFrames=" << info.numFrames << std::endl;
  std::cout << "essenceIndex: built=" << engine.HasEssenceIndex()
             << " highestCoveredEditUnit=" << engine.EssenceIndexHighestEditUnit() << std::endl;

  for (int64_t i = 0; i < info.numFrames; ++i) {
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    FetchStatus st = engine.GetVideoFrame(i, frame, &err);
    if (st != FetchStatus::kOk) {
      std::cerr << "frame " << i << " failed: " << static_cast<int>(st) << " " << err << std::endl;
      return 1;
    }
    std::cout << "frame " << i << " ok, format=" << static_cast<int>(frame->format) << " w=" << frame->width
              << " h=" << frame->height << " plane0size=" << frame->planeData[0].size()
              << " firstByte=" << static_cast<int>(frame->planeData[0].empty() ? 0 : frame->planeData[0][0])
              << std::endl;
  }
  std::cout << "all frames read ok" << std::endl;

  // The sequential read above naturally evicted early frames from the ring
  // buffer once it walked far enough past them (default buffer_frames=300),
  // so re-requesting an early/mid frame now exercises SeekAndDecode's
  // eviction-triggered fallback for real -- a direct probe of whether the
  // Index Table Segment-assisted path (if this file has one) actually
  // engages, not just whether sequential playback works.
  if (info.numFrames > 10) {
    const int64_t probeTarget = info.numFrames / 3;
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    FetchStatus st = engine.GetVideoFrame(probeTarget, frame, &err);
    std::cout << "seek probe: requested frame " << probeTarget << " status=" << static_cast<int>(st);
    if (st == FetchStatus::kOk) {
      std::cout << " ok, firstByte=" << static_cast<int>(frame->planeData[0].empty() ? 0 : frame->planeData[0][0]);
    } else {
      std::cout << " err=" << err;
    }
    std::cout << std::endl;
  }

  std::cout << "indexAssistedSeeks=" << engine.IndexAssistedSeekCount() << std::endl;
  return 0;
}
