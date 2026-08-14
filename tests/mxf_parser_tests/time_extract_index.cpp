// Manual timing tool (not part of ctest): times ExtractAndPublishIndex
// end-to-end against a real source, e.g. an S3/localstack presigned URL, to
// verify the parallel HTTP index-extraction path (mxf/index_table.h's
// BuildEssenceIndexFromHttp) actually cuts wall-clock time on a real
// multi-partition file. Usage:
//   time_extract_index <sourcePathOrUrl> <targetPathOrUrl> [httpHeaders]
#include <chrono>
#include <iostream>

#include "core/index_extract.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: time_extract_index <sourcePathOrUrl> <targetPathOrUrl> [httpHeaders]" << std::endl;
    return 2;
  }
  const std::string source = argv[1];
  const std::string target = argv[2];
  const std::string headers = argc > 3 ? argv[3] : "";

  const auto start = std::chrono::steady_clock::now();
  const mxfsrc::ExtractIndexResult result = mxfsrc::ExtractAndPublishIndex(source, target, headers);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double seconds = std::chrono::duration<double>(elapsed).count();

  if (!result.ok) {
    std::cerr << "FAILED after " << seconds << "s: " << result.error << std::endl;
    return 1;
  }
  std::cout << "OK in " << seconds << "s -> " << target << std::endl;
  return 0;
}
