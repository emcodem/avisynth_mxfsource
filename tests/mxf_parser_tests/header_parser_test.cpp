// Phase 1 unit tests for the MXF Header Metadata parser (src/mxf/). No real
// sample .mxf files are available in this environment, so these tests
// hand-craft a minimal, spec-shaped OP1a-style Header Partition + Header
// Metadata (see synthetic_mxf.h) and verify ParseMxfHeader() walks it
// correctly. This exercises the parsing logic thoroughly but is not a
// substitute for testing against real-world files (vendor quirks) -- see
// plan Phase 8 for that follow-up.
#include <filesystem>
#include <iostream>

#include "mxf/header_parser.h"
#include "synthetic_mxf.h"

using namespace mxfsrc;
using namespace mxfsrc_test;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

void TestHappyPath() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  cfg.includeFooterPartitionOffset = true;
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_happy.mxf");

  {
    MxfReader reader(path.string());
    CHECK(reader.IsOpen());

    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    CHECK(info.has_value());
    if (!info) {
      std::cerr << "ParseMxfHeader error: " << err << std::endl;
    } else {
      CHECK(info->hasVideo);
      CHECK(info->width == 320);
      CHECK(info->height == 240);
      CHECK(info->editRate.num == 25 && info->editRate.den == 1);
      CHECK(info->numFrames == 10);
      CHECK(info->videoCodec == CodecFamily::kRawVideo);
      CHECK(info->footerPartitionPresent);
      CHECK(info->durationReliable);
      CHECK(!info->hasAudio);
    }
  }  // reader closes its FILE* before we delete the file below

  std::filesystem::remove(path);
}

void TestUnknownDurationIsFlaggedUnreliable() {
  SyntheticFile cfg;
  cfg.sequenceDuration = -1;  // Header Metadata Duration unknown, per plan's fallback-chain step 5
  cfg.includeFooterPartitionOffset = false;
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_unreliable.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->hasVideo);
      CHECK(info->numFrames == -1);
      CHECK(!info->footerPartitionPresent);
      CHECK(!info->durationReliable);
    }
  }

  std::filesystem::remove(path);
}

void TestRejectsNonMxfFile() {
  std::vector<uint8_t> garbage = {'n', 'o', 't', ' ', 'm', 'x', 'f'};
  auto path = WriteTempFile(garbage, "mxfsrc_test_garbage.mxf");

  {
    MxfReader reader(path.string());
    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    CHECK(!info.has_value());
    CHECK(!err.empty());
  }

  std::filesystem::remove(path);
}

}  // namespace

int main() {
  TestHappyPath();
  TestUnknownDurationIsFlaggedUnreliable();
  TestRejectsNonMxfFile();

  if (g_failures == 0) {
    std::cout << "all mxf header_parser tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
