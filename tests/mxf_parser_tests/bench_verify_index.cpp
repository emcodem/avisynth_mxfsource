// Manual benchmark tool (not part of ctest): times verify=true's index check
// (core/index_verify.h) on a real file in each read mode, with the number of
// read requests, and says whether the drive counts as solid state. Usage:
//   bench_verify_index <sourcePath> [ranges:N|streaming|auto[:N] ...]
// (N = verify_overlapping). Default: ranges:4 streaming. Run each mode on a
// cold cache (e.g. after a reboot, or another large file read in between)
// for hard-disk numbers.
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/index_verify.h"
#include "mxf/byte_cache.h"
#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"

using namespace mxfsrc;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: bench_verify_index <sourcePath> [ranges:N|streaming|auto[:N] ...]" << std::endl;
    return 2;
  }
  const std::string path = argv[1];
  std::vector<std::string> modes;
  for (int i = 2; i < argc; ++i) modes.push_back(argv[i]);
  if (modes.empty()) modes = {"ranges:4", "streaming"};

  MxfReader reader(path);
  if (!reader.IsOpen()) {
    std::cerr << "failed to open " << path << std::endl;
    return 1;
  }
  std::string err;
  const auto info = ParseMxfHeader(reader, &err);
  if (!info) {
    std::cerr << "header parse failed: " << err << std::endl;
    return 1;
  }
  const auto index = BuildEssenceIndex(reader, info->bodySid);
  if (!index || index->Empty()) {
    std::cerr << "no index table in this file" << std::endl;
    return 1;
  }
  std::cout << "file " << reader.Size() << " bytes, " << index->GetEntries().size()
            << " entries; local solid state: " << (IsLocalSolidState(path) ? "yes" : "no") << std::endl;

  for (const std::string& mode : modes) {
    // "ranges:N" (N reads in flight), "streaming", "auto" or "auto:N".
    const size_t colon = mode.find(':');
    const std::string kind = mode.substr(0, colon);
    const int overlapping = colon == std::string::npos ? 0 : std::atoi(mode.c_str() + colon + 1);
    const VerifyReads reads = kind == "streaming" ? VerifyReads::kStreaming
                              : kind == "ranges"  ? VerifyReads::kRanges
                                                  : VerifyReads::kAuto;
    const auto start = std::chrono::steady_clock::now();
    const IndexVerifyResult r = VerifyEssenceIndex(path, "", *index, *info, overlapping, reads);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << mode << ": " << s << " s, " << r.readRequests << " requests, " << r.issues.size() << " issues, "
              << r.essenceDamage.size() << " damage" << (r.error ? " ERROR " + *r.error : "") << std::endl;
  }
  return 0;
}
