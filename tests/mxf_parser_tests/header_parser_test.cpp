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

// Origin (baseline tag 0x4B02) and its raw/untrimmed duration are only ever
// read off a SourcePackage's video track (see MxfStreamInfo::origin) --
// this verifies they're captured correctly even though the MaterialPackage
// (with its own, different Duration) is resolved first in file order.
void TestOriginParsedFromSourcePackage() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;  // MaterialPackage's (trimmed) Duration
  cfg.includeSourcePackage = true;
  cfg.sourcePackageDuration = 30;  // SourcePackage's raw/untrimmed Duration
  cfg.sourcePackageOrigin = 20;
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_origin.mxf");

  {
    MxfReader reader(path.string());
    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->hasVideo);
      CHECK(info->numFrames == 10);  // MaterialPackage's Duration, untouched by Origin parsing itself
      CHECK(info->origin == 20);
      CHECK(info->sourcePackageDuration == 30);
    } else {
      std::cerr << "ParseMxfHeader error: " << err << std::endl;
    }
  }

  std::filesystem::remove(path);
}

void TestNoSourcePackageLeavesOriginFieldsAtDefault() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  cfg.includeSourcePackage = false;
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_no_origin.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->origin == 0);
      CHECK(info->sourcePackageDuration == -1);
    }
  }

  std::filesystem::remove(path);
}

// Regression test for real Omneon MXF files: the Header Partition is
// marked Open & Incomplete and its Header Metadata copy genuinely has no
// resolvable Duration, while the Footer Partition is Closed & Complete and
// carries its own full, correct Header Metadata copy (Duration, Origin).
// Per SMPTE 377-1, a Closed partition's metadata takes precedence over an
// Open one -- ParseMxfHeader must fall through to the Footer's copy rather
// than failing with "unable to determine frame count".
void TestFooterPrecedenceOverridesOpenIncompleteHeader() {
  SyntheticFile headerCfg;
  headerCfg.sequenceDuration = -1;  // genuinely unresolved in the (open/incomplete) header

  SyntheticFile footerCfg;
  footerCfg.sequenceDuration = 60;  // MaterialPackage's trimmed Duration (e.g. duration_1m0s0f)
  footerCfg.includeSourcePackage = true;
  footerCfg.sourcePackageDuration = 65;  // SourcePackage's raw/untrimmed Duration
  footerCfg.sourcePackageOrigin = 5;     // e.g. origin5

  auto path =
      WriteTempFile(BuildFooterPrecedenceTestFile(headerCfg, footerCfg), "mxfsrc_test_footer_precedence.mxf");

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
      CHECK(info->numFrames == 60);  // from the Footer's MaterialPackage, not the Header's unresolved -1
      CHECK(info->origin == 5);
      CHECK(info->sourcePackageDuration == 65);
      CHECK(info->durationReliable);
    }
  }

  std::filesystem::remove(path);
}

// A Header Partition that's Open & Incomplete but has NO Footer Partition
// at all must still fail with the existing "unable to determine frame
// count" behavior -- there's nothing to fall back to.
// Regression test for the actual real-world Omneon file this was found on:
// the Header Partition's own FooterPartition field is left 0 (unknown)
// even though a Closed & Complete Footer Partition exists -- the Footer
// can only be located via the Random Index Pack at the end of the file.
void TestRipFallbackLocatesFooterWhenHeaderFieldIsZero() {
  SyntheticFile headerCfg;
  headerCfg.sequenceDuration = -1;

  SyntheticFile footerCfg;
  footerCfg.sequenceDuration = 1500;  // 1m0s0f @ 25fps
  footerCfg.includeSourcePackage = true;
  footerCfg.sourcePackageDuration = 1505;
  footerCfg.sourcePackageOrigin = 5;

  auto path =
      WriteTempFile(BuildRipFooterFallbackTestFile(headerCfg, footerCfg), "mxfsrc_test_rip_fallback.mxf");

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
      CHECK(info->numFrames == 1500);
      CHECK(info->origin == 5);
      CHECK(info->sourcePackageDuration == 1505);
      CHECK(info->footerPartitionPresent);
      CHECK(info->durationReliable);
    }
  }

  std::filesystem::remove(path);
}

void TestNoFooterPartitionLeavesUnknownDurationUnresolved() {
  SyntheticFile cfg;
  cfg.sequenceDuration = -1;
  cfg.includeFooterPartitionOffset = false;
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_no_footer_fallback.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->numFrames == -1);
      CHECK(!info->durationReliable);
    }
  }

  std::filesystem::remove(path);
}

// Regression test for real Atomos Shogun XAVC-L files: their
// PictureEssenceCoding UL (registered under the 0x0D family, distinct from
// AVC-Intra's 0x0A family) must classify as CodecFamily::kH264LongGop, not
// kUnknown -- and that family must NOT be treated as intra-only (it's
// genuine long-GOP H.264 with B-frame reference dependencies), unlike
// kH264 which covers only verified I-frame-only AVC-Intra/XAVC-Intra ULs.
void TestXavcLongGopUlClassification() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  cfg.pictureEssenceCoding = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D,
                              0x04, 0x01, 0x02, 0x02, 0x01, 0x31, 0x60, 0x01};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_xavc_longgop.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->videoCodec == CodecFamily::kH264LongGop);
    }
  }

  std::filesystem::remove(path);

  CHECK(!IsIntraOnlyCodec(CodecFamily::kH264LongGop));
  CHECK(IsIntraOnlyCodec(CodecFamily::kH264));
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
  TestOriginParsedFromSourcePackage();
  TestNoSourcePackageLeavesOriginFieldsAtDefault();
  TestFooterPrecedenceOverridesOpenIncompleteHeader();
  TestRipFallbackLocatesFooterWhenHeaderFieldIsZero();
  TestNoFooterPartitionLeavesUnknownDurationUnresolved();
  TestXavcLongGopUlClassification();
  TestRejectsNonMxfFile();

  if (g_failures == 0) {
    std::cout << "all mxf header_parser tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
