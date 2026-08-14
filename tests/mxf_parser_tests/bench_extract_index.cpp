// Manual benchmark tool (not part of ctest): times the old sequential
// BuildEssenceIndex path against the new parallel BuildEssenceIndexFromHttp
// path on the exact same real source, so the improvement can be measured
// head-to-head rather than compared across separate anecdotal runs. Usage:
//   bench_extract_index <sourceUrl> [repeats] [httpHeaders]
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"
#include "mxf/partition.h"

using namespace mxfsrc;

namespace {

double TimeIt(const std::function<void()>& fn) {
  const auto start = std::chrono::steady_clock::now();
  fn();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: bench_extract_index <sourceUrl> [repeats] [httpHeaders]" << std::endl;
    return 2;
  }
  const std::string url = argv[1];
  const int repeats = argc > 2 ? std::atoi(argv[2]) : 1;
  const std::string headers = argc > 3 ? argv[3] : "";

  // Header parse once, up front (identical cost for both paths, not part of
  // what this change touches) -- just need bodySid/fileSize/RIP offsets.
  uint64_t fileSize = 0;
  uint32_t bodySid = 0;
  std::vector<uint64_t> offsets;
  {
    MxfReader reader(url, headers);
    if (!reader.IsOpen()) {
      std::cerr << "failed to open: " << reader.LastError() << std::endl;
      return 1;
    }
    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    if (!info) {
      std::cerr << "header parse failed: " << err << std::endl;
      return 1;
    }
    bodySid = info->bodySid;
    fileSize = reader.Size();
    if (auto rip = ReadRandomIndexPack(reader)) {
      offsets.push_back(0);
      for (auto& e : *rip) offsets.push_back(e.byteOffset);
    }
    std::cout << "fileSize=" << fileSize << " bodySid=" << bodySid << " partitions(incl. header)=" << offsets.size()
              << std::endl;
  }
  if (offsets.empty()) {
    std::cerr << "no RIP found -- can't compare, this file wouldn't use the parallel path anyway" << std::endl;
    return 1;
  }

  size_t sequentialEntries = 0, parallelEntries = 0;
  std::vector<double> sequentialTimes, parallelTimes;

  for (int i = 0; i < repeats; ++i) {
    const double t = TimeIt([&] {
      MxfReader reader(url, headers);
      auto index = BuildEssenceIndex(reader, bodySid);
      sequentialEntries = index ? index->GetEntries().size() : 0;
    });
    sequentialTimes.push_back(t);
    std::cout << "sequential run " << i << ": " << t << "s (" << sequentialEntries << " entries)" << std::endl;
  }

  for (int i = 0; i < repeats; ++i) {
    const double t = TimeIt([&] {
      auto index = BuildEssenceIndexFromHttp(url, headers, bodySid, fileSize, offsets);
      parallelEntries = index ? index->GetEntries().size() : 0;
    });
    parallelTimes.push_back(t);
    std::cout << "parallel   run " << i << ": " << t << "s (" << parallelEntries << " entries)" << std::endl;
  }

  auto minOf = [](const std::vector<double>& v) { return *std::min_element(v.begin(), v.end()); };
  std::cout << "--- summary ---" << std::endl;
  std::cout << "sequential best: " << minOf(sequentialTimes) << "s" << std::endl;
  std::cout << "parallel   best: " << minOf(parallelTimes) << "s" << std::endl;
  std::cout << "entries match: " << (sequentialEntries == parallelEntries ? "yes" : "NO -- MISMATCH") << " ("
            << sequentialEntries << " vs " << parallelEntries << ")" << std::endl;
  std::cout << "speedup: " << (minOf(sequentialTimes) / minOf(parallelTimes)) << "x" << std::endl;
  return 0;
}
