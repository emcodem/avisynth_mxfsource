// Manual diagnostic tool (not part of ctest, not linked into anything
// else): opens a .mxf file via SourceEngine directly (no AviSynth+/
// VapourSynth involved) and reads every frame, printing per-frame
// diagnostics. Useful for isolating whether a bug is in the core decode
// pipeline or in a host adapter -- e.g. confirmed the Phase 5 ring-buffer
// crash was adapter-side, not core-side, by reading the exact same file
// that crashed the real AviSynth+ host without issue here. Build with
// `cmake --build build --target read_test_mxf`.
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>

#include "core/source_engine.h"
#include "mxf/essence_walker.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"
#include "mxf/partition.h"

using namespace mxfsrc;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: read_test_mxf <file.mxf> [seek_target_frame | compare <frame>]" << std::endl;
    return 1;
  }

  // Raw KLV trace from an arbitrary offset, with the same resync-on-garbage
  // tolerance ScanPartitionForIndexSegments/EssenceWalker use -- prints
  // every key/length/offset it walks past, for diagnosing exactly where a
  // partition's own declared byte-count fields (HeaderByteCount,
  // IndexByteCount) disagree with what's actually there. args: <file>
  // klv-scan <startOffset> [count=30].
  if (argc >= 4 && std::string(argv[2]) == "klv-scan") {
    const uint64_t startOffset = std::stoull(argv[3]);
    const int count = argc >= 5 ? std::stoi(argv[4]) : 30;
    MxfReader reader(argv[1]);
    if (!reader.IsOpen()) {
      std::cerr << "failed to open" << std::endl;
      return 1;
    }
    if (!reader.Seek(startOffset)) {
      std::cerr << "seek failed" << std::endl;
      return 1;
    }
    for (int i = 0; i < count; ++i) {
      const uint64_t klvStart = reader.Tell();
      if (klvStart >= reader.Size()) {
        std::cout << "[" << i << "] EOF" << std::endl;
        break;
      }
      KlvHeader klv{};
      if (!ReadKlvHeader(reader, klv)) {
        std::cout << "[" << i << "] offset=" << klvStart << " ReadKlvHeader FAILED" << std::endl;
        break;
      }
      if (!LooksLikeKlvKey(klv.key)) {
        std::cout << "[" << i << "] offset=" << klvStart << " NOT-A-KLV-KEY, resyncing..." << std::endl;
        if (!reader.Seek(klvStart + 1) || !ResyncToUlDesignator(reader)) {
          std::cout << "  resync failed" << std::endl;
          break;
        }
        std::cout << "  resynced to offset=" << reader.Tell() << std::endl;
        continue;
      }
      std::cout << "[" << i << "] offset=" << klvStart << " key=";
      for (uint8_t b : klv.key) std::printf("%02x", b);
      const uint64_t valueEnd = klv.valueOffset + klv.length;
      std::cout << " length=" << klv.length << " valueOffset=" << klv.valueOffset << " valueEnd=" << valueEnd
                 << (UlEquals(klv.key, kIndexTableSegmentKey) ? "  <-- INDEX TABLE SEGMENT" : "")
                 << (IsEssenceElementKey(klv.key) ? "  <-- ESSENCE ELEMENT" : "") << std::endl;
      if (!reader.Seek(valueEnd)) {
        std::cout << "  seek past value failed" << std::endl;
        break;
      }
    }
    return 0;
  }

  // Content-correctness check: decode the same target frame two ways --
  // once via the fast index-assisted SeekAndDecode path (tiny buffer_frames
  // forces GetVideoFrame's forward-jump bypass), once via genuine
  // sequential decode from element 0 (huge buffer_frames disables that
  // bypass) -- and diff the actual pixel bytes. Isolates whether a
  // mid-file EssenceWalker/decoder start produces the *correct* frame, not
  // just *a* frame without erroring -- a status of kOk alone doesn't prove
  // the resolved byte offset actually landed on the right KLV.
  // Writes target frame's Y/U/V planes concatenated (matching ffmpeg's
  // yuv422p planar raw output) to `outpath`, for an external md5/hash
  // comparison against an independent reference (e.g. `ffmpeg -i file -vf
  // select=... -pix_fmt yuv422p -f rawvideo dump.raw`). Settles definitively
  // whether OUR notion of "host frame N" matches ffmpeg's own -- args:
  // <file> dump <target> <outpath> [seq|fast] (default seq).
  if (argc >= 5 && std::string(argv[2]) == "dump") {
    const int64_t target = std::stoll(argv[3]);
    const std::string outpath = argv[4];
    const bool useFast = argc >= 6 && std::string(argv[5]) == "fast";

    SourceEngine engine;
    OpenOptions opts;
    opts.buffer_frames = useFast ? 2 : static_cast<int>(target) + 10;
    OpenResult res = engine.Open(argv[1], opts);
    if (res.status != OpenStatus::kOk) {
      std::cerr << "Open failed: " << res.error << std::endl;
      return 1;
    }
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    if (engine.GetVideoFrame(target, frame, &err) != FetchStatus::kOk) {
      std::cerr << "GetVideoFrame failed: " << err << std::endl;
      return 1;
    }
    std::ofstream out(outpath, std::ios::binary | std::ios::trunc);
    for (int p = 0; p < 3; ++p) {
      if (frame->planeData[p].empty()) continue;
      out.write(reinterpret_cast<const char*>(frame->planeData[p].data()),
                static_cast<std::streamsize>(frame->planeData[p].size()));
    }
    std::cout << "wrote " << outpath << " (mode=" << (useFast ? "fast" : "seq") << ")" << std::endl;
    return 0;
  }

  if (argc >= 4 && std::string(argv[2]) == "compare-range") {
    // Off-by-N detector: decode `target` via the fast path once, then
    // decode a whole neighborhood around it sequentially and report which
    // (if any) sequential frame index the fast-path result actually
    // matches -- distinguishes "landed on the wrong nearby frame" (an
    // off-by-one/N counting bug) from "genuinely corrupted content".
    const int64_t target = std::stoll(argv[3]);
    const int64_t radius = argc >= 5 ? std::stoll(argv[4]) : 15;

    SourceEngine fastEngine;
    OpenOptions fastOpts;
    fastOpts.buffer_frames = 2;
    OpenResult fastRes = fastEngine.Open(argv[1], fastOpts);
    if (fastRes.status != OpenStatus::kOk) {
      std::cerr << "fast engine Open failed: " << fastRes.error << std::endl;
      return 1;
    }
    std::shared_ptr<NormalizedFrame> fastFrame;
    std::string fastErr;
    FetchStatus fastSt = fastEngine.GetVideoFrame(target, fastFrame, &fastErr);
    if (fastSt != FetchStatus::kOk) {
      std::cerr << "fast path failed: " << fastErr << std::endl;
      return 1;
    }

    SourceEngine seqEngine;
    OpenOptions seqOpts;
    seqOpts.buffer_frames = static_cast<int>(target + radius) + 10;
    OpenResult seqRes = seqEngine.Open(argv[1], seqOpts);
    if (seqRes.status != OpenStatus::kOk) {
      std::cerr << "sequential engine Open failed: " << seqRes.error << std::endl;
      return 1;
    }
    bool foundMatch = false;
    for (int64_t i = std::max<int64_t>(0, target - radius); i <= target + radius; ++i) {
      std::shared_ptr<NormalizedFrame> seqFrame;
      std::string seqErr;
      if (seqEngine.GetVideoFrame(i, seqFrame, &seqErr) != FetchStatus::kOk) continue;
      bool identical = fastFrame->format == seqFrame->format && fastFrame->width == seqFrame->width &&
                       fastFrame->height == seqFrame->height;
      for (int p = 0; identical && p < 3; ++p) identical = fastFrame->planeData[p] == seqFrame->planeData[p];
      if (identical) {
        std::cout << "fast-path frame " << target << " MATCHES sequential frame " << i << " (offset " << (i - target)
                   << ")" << std::endl;
        foundMatch = true;
      }
    }
    if (!foundMatch) {
      std::cout << "fast-path frame " << target << " matches NOTHING within +/-" << radius
                 << " of the sequential reference" << std::endl;
    }
    return 0;
  }

  if (argc >= 4 && std::string(argv[2]) == "compare") {
    const int64_t target = std::stoll(argv[3]);

    SourceEngine fastEngine;
    OpenOptions fastOpts;
    fastOpts.buffer_frames = 2;
    OpenResult fastRes = fastEngine.Open(argv[1], fastOpts);
    if (fastRes.status != OpenStatus::kOk) {
      std::cerr << "fast engine Open failed: " << fastRes.error << std::endl;
      return 1;
    }
    std::shared_ptr<NormalizedFrame> fastFrame;
    std::string fastErr;
    FetchStatus fastSt = fastEngine.GetVideoFrame(target, fastFrame, &fastErr);
    std::cout << "fast path: status=" << static_cast<int>(fastSt) << " indexAssistedSeeks="
               << fastEngine.IndexAssistedSeekCount() << (fastSt == FetchStatus::kOk ? "" : (" err=" + fastErr))
               << std::endl;

    SourceEngine seqEngine;
    OpenOptions seqOpts;
    seqOpts.buffer_frames = static_cast<int>(target) + 10;
    OpenResult seqRes = seqEngine.Open(argv[1], seqOpts);
    if (seqRes.status != OpenStatus::kOk) {
      std::cerr << "sequential engine Open failed: " << seqRes.error << std::endl;
      return 1;
    }
    std::shared_ptr<NormalizedFrame> seqFrame;
    std::string seqErr;
    FetchStatus seqSt = seqEngine.GetVideoFrame(target, seqFrame, &seqErr);
    std::cout << "sequential path: status=" << static_cast<int>(seqSt)
               << (seqSt == FetchStatus::kOk ? "" : (" err=" + seqErr)) << std::endl;

    if (fastSt != FetchStatus::kOk || seqSt != FetchStatus::kOk) return 1;

    bool identical = fastFrame->format == seqFrame->format && fastFrame->width == seqFrame->width &&
                     fastFrame->height == seqFrame->height;
    for (int p = 0; identical && p < 3; ++p) identical = fastFrame->planeData[p] == seqFrame->planeData[p];
    std::cout << "frame " << target << ": " << (identical ? "IDENTICAL" : "MISMATCH") << std::endl;
    if (!identical) {
      for (int p = 0; p < 3; ++p) {
        std::cout << "  plane[" << p << "] fastSize=" << fastFrame->planeData[p].size()
                   << " seqSize=" << seqFrame->planeData[p].size() << std::endl;
      }
    }
    return identical ? 0 : 1;
  }

  // Diagnostic: independently probe the RIP and Index Table Segment scan
  // that BuildEssenceIndex relies on -- useful for a file whose resulting
  // index unexpectedly only covers a fraction of it (e.g. a missing/
  // unreadable RIP means only the Header Partition itself ever gets
  // scanned for Index Table Segments).
  if (argc >= 3 && std::string(argv[2]) == "index-scan") {
    MxfReader diagReader(argv[1]);
    if (!diagReader.IsOpen()) {
      std::cerr << "failed to open" << std::endl;
      return 1;
    }
    auto rip = ReadRandomIndexPack(diagReader);
    std::cout << "RIP: " << (rip ? std::to_string(rip->size()) + " partition entries" : "absent/unreadable")
               << std::endl;
    auto scanParts = EnumeratePartitionsForIndex(diagReader);
    std::cout << "EnumeratePartitionsForIndex: " << scanParts.size() << " partitions scanned for index"
               << " (fileSize=" << diagReader.Size() << ")" << std::endl;
    size_t totalSegs = 0;
    for (size_t partIdx = 0; partIdx < scanParts.size(); ++partIdx) {
      const auto& part = scanParts[partIdx];
      auto segs = ScanPartitionForIndexSegments(diagReader, part);
      totalSegs += segs.size();
      std::cout << "  partition[" << partIdx << "] offset=" << part.partitionOffset << " bodySid=" << part.bodySid
                 << " indexSid=" << part.indexSid << " bodyOffset=" << part.bodyOffset
                 << " essenceStartOffset=" << part.essenceStartOffset << " indexByteRange=["
                 << part.indexByteRangeStart << "," << part.indexByteRangeEnd << ")" << " segments=" << segs.size()
                 << std::endl;
      for (const auto& seg : segs) {
        std::cout << "    segment: indexStartPosition=" << seg.indexStartPosition
                   << " indexDuration=" << seg.indexDuration << " editUnitByteCount=" << seg.editUnitByteCount
                   << " indexSid=" << seg.indexSid << " bodySid=" << seg.bodySid << " entries=" << seg.entries.size()
                   << " essenceContainerBaseOffset=" << seg.essenceContainerBaseOffset << std::endl;
      }
    }
    std::cout << "total index table segments found: " << totalSegs << std::endl;
    return 0;
  }

  SourceEngine engine;
  OpenOptions openOpts;
  // Optional 3rd arg: buffer_frames, e.g. large enough to cover the whole
  // file -- forces GetVideoFrame's forward-jump SeekAndDecode bypass to
  // never trigger, isolating whether a given target frame is reachable via
  // genuine sequential background-thread decode (same code path as normal
  // playback) versus only via the fast-seek path.
  if (argc >= 4) openOpts.buffer_frames = std::stoi(argv[3]);
  OpenResult res = engine.Open(argv[1], openOpts);
  if (res.status != OpenStatus::kOk) {
    std::cerr << "Open failed: " << static_cast<int>(res.status) << " " << res.error << std::endl;
    return 1;
  }
  const auto& info = engine.StreamInfo();
  std::cout << "opened: " << info.width << "x" << info.height << " numFrames=" << info.numFrames << std::endl;
  std::cout << "essenceIndex: built=" << engine.HasEssenceIndex()
             << " highestCoveredEditUnit=" << engine.EssenceIndexHighestEditUnit() << std::endl;

  // Fast-path diagnostic mode: a single direct seek, timed, with no full
  // sequential read beforehand -- isolates exactly how long GetVideoFrame()
  // takes to reach a single far-ahead frame from a cold engine, matching
  // real-world scrub behavior (AvsPmod jumping straight to a frame) instead
  // of the full-sequential-read-then-probe pattern below.
  if (argc >= 3) {
    const int64_t target = std::stoll(argv[2]);
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    FetchStatus st = engine.GetVideoFrame(target, frame, &err);
    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "direct seek to frame " << target << ": status=" << static_cast<int>(st) << " took=" << seconds
               << "s";
    if (st != FetchStatus::kOk) std::cout << " err=" << err;
    std::cout << std::endl;
    std::cout << "indexAssistedSeeks=" << engine.IndexAssistedSeekCount() << " nonBufferedFrames="
               << engine.NonBufferedServedFrames().size() << std::endl;
    return st == FetchStatus::kOk ? 0 : 1;
  }

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
