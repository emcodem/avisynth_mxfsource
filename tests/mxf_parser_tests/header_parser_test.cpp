// Phase 1 unit tests for the MXF Header Metadata parser (src/mxf/). No real
// sample .mxf files are available in this environment, so these tests
// hand-craft a minimal, spec-shaped OP1a-style Header Partition + Header
// Metadata (see synthetic_mxf.h) and verify ParseMxfHeader() walks it
// correctly. This exercises the parsing logic thoroughly but is not a
// substitute for testing against real-world files (vendor quirks) -- see
// plan Phase 8 for that follow-up.
#include <filesystem>
#include <iostream>

#include "mxf/essence_walker.h"
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

// Regression test for the UL Version Number byte (ST 336 byte 8, index 7)
// being part of the comparison: real files on the sample share carry
// DNxHR coding ULs at version 0x0a and KLV Fill at 0x01, where our tables
// hold 0x01/0x02, so every DNxHR file failed as an unknown codec. Version
// must be ignored. Also covers SMPTE382MDefaultUncompressedSoundCoding,
// the PCM label 640 real files use, which was missing from the table and
// made their audio drop as "non-PCM".
void TestUlVersionByteIgnored() {
  // DNxHR HQX, verbatim from "TALK-UHD HDR DNxHR HQX.mxf".
  CHECK(ClassifyCodecUl({0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x0a,
                         0x04, 0x01, 0x02, 0x02, 0x71, 0x25, 0x00, 0x00}) == CodecFamily::kDnxhd);
  // Uncompressed PCM, verbatim from "DJ OETZI UHD HDR XAVC CBG300.mxf".
  CHECK(ClassifyCodecUl({0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x0a,
                         0x04, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00}) == CodecFamily::kPcm);
  // KLV Fill at version 0x01 (table key is 0x02).
  CHECK(UlEquals({0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x01,
                  0x03, 0x01, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00}, kKlvFillKey));
  // Any other byte still distinguishes.
  CHECK(ClassifyCodecUl({0x06, 0x0e, 0x2b, 0x34, 0x04, 0x01, 0x01, 0x0a,
                         0x04, 0x01, 0x02, 0x02, 0x72, 0x25, 0x00, 0x00}) == CodecFamily::kUnknown);
  CHECK(!UlEquals({0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x01,
                   0x03, 0x01, 0x02, 0x10, 0x02, 0x00, 0x00, 0x00}, kKlvFillKey));
}

// Header Partition Pack's HeaderByteCount field: 16-byte key + 1-byte BER
// length, then MajorVersion(2) MinorVersion(2) KAGSize(4) ThisPartition(8)
// PreviousPartition(8) FooterPartition(8).
void SetHeaderByteCount(std::vector<uint8_t>& file, uint64_t headerByteCount) {
  for (int i = 0; i < 8; ++i) file[49 + i] = static_cast<uint8_t>(headerByteCount >> (56 - 8 * i));
}

// Fixup 1's original purpose: HeaderByteCount undercounts the metadata, and
// the RIP's Body Partition entry shows where the Header Partition really
// ends. Without the RIP the sets beyond HeaderByteCount are lost.
void TestUndercountedHeaderByteCountRecoveredViaRip() {
  SyntheticFile cfg;
  const size_t metadataSize = BuildHeaderMetadataBlock(cfg).size();
  for (bool withRip : {true, false}) {
    std::vector<uint8_t> file = BuildSyntheticHeader(cfg);
    SetHeaderByteCount(file, metadataSize / 2);
    const uint64_t bodyOffset = file.size();
    AppendPartitionPack(file, PartitionKind::kBody);
    AppendEssenceElement(file, cfg.videoTrackNumber, std::vector<uint8_t>(16, 0x11));
    if (withRip) AppendRandomIndexPack(file, {{0, 0}, {1, bodyOffset}});
    auto path = WriteTempFile(file, "mxfsrc_test_hbc_undercount.mxf");
    {
      MxfReader reader(path.string());
      auto info = ParseMxfHeader(reader);
      if (withRip) {
        CHECK(info.has_value());
        if (info) {
          CHECK(info->width == cfg.width && info->height == cfg.height);
          CHECK(info->headerMetadataEnd == bodyOffset);
        }
      } else {
        CHECK(!info || info->width != cfg.width);  // the test really cuts into needed sets
      }
    }
    std::filesystem::remove(path);
  }
}

// Real layout (67 Harmonic/AJA UHD H.264 files on the sample share): the
// Header Partition carries the essence after its Index Table Segment, and
// the only Body Partition (RIP[1]) holds the last part of the essence near
// EOF. Fixup 1 used to take RIP[1] as the end of the Header Metadata, which
// made the metadata scan walk the whole essence and EssenceWalker skip it.
// Covered with an exact and an undercounted HeaderByteCount.
void TestEssenceInHeaderPartitionNotTreatedAsMetadata() {
  SyntheticFile cfg;
  const size_t metadataSize = BuildHeaderMetadataBlock(cfg).size();
  for (size_t headerByteCount : {metadataSize, metadataSize / 2}) {
    std::vector<uint8_t> file = BuildSyntheticHeader(cfg);
    SetHeaderByteCount(file, headerByteCount);
    const uint64_t metadataEnd = file.size();
    IndexTableSegmentSpec seg;
    seg.editUnitByteCount = 16 + 1 + 16;
    AppendIndexTableSegment(file, seg);
    for (int i = 0; i < 3; ++i) AppendEssenceElement(file, cfg.videoTrackNumber, std::vector<uint8_t>(16, 0x20));
    const uint64_t bodyOffset = file.size();
    AppendPartitionPack(file, PartitionKind::kBody);
    for (int i = 0; i < 2; ++i) AppendEssenceElement(file, cfg.videoTrackNumber, std::vector<uint8_t>(16, 0x30));
    AppendRandomIndexPack(file, {{1, 0}, {1, bodyOffset}});
    auto path = WriteTempFile(file, "mxfsrc_test_essence_in_header.mxf");
    {
      MxfReader reader(path.string());
      auto info = ParseMxfHeader(reader);
      CHECK(info.has_value());
      if (info) {
        CHECK(info->width == cfg.width && info->height == cfg.height);
        CHECK(info->headerMetadataEnd == metadataEnd);

        EssenceWalker walker(reader, info->headerMetadataEnd, info->videoTrackNumber, true, {});
        EssenceElement element;
        int videoElements = 0;
        while (walker.Next(element, /*wantData=*/false)) videoElements += element.isVideo ? 1 : 0;
        CHECK(videoElements == 5);
      }
    }
    std::filesystem::remove(path);
  }
}

// Single audio track, via a MultipleDescriptor wrapping the CDCI Descriptor
// + one Sound Descriptor -- the common real-world case. Binds directly
// (sole-candidate fallback) even though LinkedTrackID/TrackID default to 0
// here, matching a lazy writer that omits LinkedTrackID entirely when
// there's nothing ambiguous to resolve.
void TestSingleAudioTrackResolved() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  SyntheticAudioTrack audio;
  audio.channelCount = 2;
  audio.sampleRate = 48000;
  audio.bitsPerSample = 24;
  cfg.audioTracks = {audio};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_single_audio.mxf");

  {
    MxfReader reader(path.string());
    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    CHECK(info.has_value());
    if (!info) {
      std::cerr << "ParseMxfHeader error: " << err << std::endl;
    } else {
      CHECK(info->hasVideo);
      CHECK(info->hasAudio);
      CHECK(info->audioTracks.size() == 1);
      CHECK(info->audioConfigSupported);
      CHECK(info->audioChannels == 2);
      CHECK(info->audioSampleRate == 48000);
      CHECK(info->audioBitsPerSample == 24);
      CHECK(info->audioCodec == CodecFamily::kPcm);
      CHECK(info->audioUnsupportedReason.empty());
    }
  }

  std::filesystem::remove(path);
}

// D-10/IMX sound (SDTI-CP sound item, TrackNumber item type 0x06) is AES3
// (SMPTE 331), not plain PCM, even though its descriptor says PCM. Reading
// it as PCM failed on the first audio request on real files, so it must be
// reported unsupported instead of exposed.
void TestD10Aes3AudioMarkedUnsupported() {
  SyntheticFile cfg;
  SyntheticAudioTrack audio;
  audio.trackNumber = 0x06010110;  // CP sound item, element type AES3 8-channel
  audio.channelCount = 8;
  cfg.audioTracks = {audio};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_d10_audio.mxf");
  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->hasAudio);
      CHECK(!info->audioConfigSupported);
      CHECK(info->audioUnsupportedReason.find("D-10/IMX AES3") != std::string::npos);
    }
  }
  std::filesystem::remove(path);
}

// Two audio tracks (e.g. two mono language tracks), each carrying a
// distinct TrackID/LinkedTrackID pair -- exercises the LinkedTrackID-based
// sub-descriptor binding (see header_parser.cpp's ResolveDescriptor) rather
// than the single-sound-descriptor sole-candidate fallback. Total
// audioChannels must be the sum across both tracks.
void TestMultiAudioTrackSummedAndBoundByLinkedTrackId() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  SyntheticAudioTrack a;
  a.trackNumber = 0x02020200;
  a.trackId = 101;
  a.channelCount = 1;
  a.sampleRate = 48000;
  a.bitsPerSample = 16;
  SyntheticAudioTrack b;
  b.trackNumber = 0x02020300;
  b.trackId = 102;
  b.channelCount = 1;
  b.sampleRate = 48000;
  b.bitsPerSample = 16;
  cfg.audioTracks = {a, b};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_multi_audio.mxf");

  {
    MxfReader reader(path.string());
    std::string err;
    auto info = ParseMxfHeader(reader, &err);
    CHECK(info.has_value());
    if (!info) {
      std::cerr << "ParseMxfHeader error: " << err << std::endl;
    } else {
      CHECK(info->hasAudio);
      CHECK(info->audioTracks.size() == 2);
      CHECK(info->audioConfigSupported);
      CHECK(info->audioChannels == 2);  // 1 + 1 summed
      CHECK(info->audioBitsPerSample == 16);
      // Each resolved track kept its own distinct TrackNumber -- confirms
      // LinkedTrackID bound each Sound Descriptor to the correct Track
      // rather than both collapsing onto the same one.
      auto trackNumberBytes = [](uint32_t n) {
        return std::array<uint8_t, 4>{static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                                       static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
      };
      const auto aBytes = trackNumberBytes(a.trackNumber);
      const auto bBytes = trackNumberBytes(b.trackNumber);
      bool sawA = false, sawB = false;
      for (const auto& t : info->audioTracks) {
        if (t.trackNumber == aBytes) sawA = true;
        if (t.trackNumber == bBytes) sawB = true;
      }
      CHECK(sawA);
      CHECK(sawB);
    }
  }

  std::filesystem::remove(path);
}

// Mismatched bit depth across audio tracks must NOT fail ParseMxfHeader
// itself -- it's flagged via audioConfigSupported/audioUnsupportedReason,
// with the playback policy (degrade vs. hard-fail) decided later by
// SourceEngine::Open via UseAudioMode.
void TestMismatchedBitDepthMarksAudioUnsupported() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  SyntheticAudioTrack a;
  a.trackNumber = 0x02020200;
  a.trackId = 101;
  a.bitsPerSample = 16;
  SyntheticAudioTrack b;
  b.trackNumber = 0x02020300;
  b.trackId = 102;
  b.bitsPerSample = 24;
  cfg.audioTracks = {a, b};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_mismatched_bitdepth.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->hasAudio);
      CHECK(info->audioTracks.size() == 2);
      CHECK(!info->audioConfigSupported);
      CHECK(!info->audioUnsupportedReason.empty());
    }
  }

  std::filesystem::remove(path);
}

// A non-PCM audio codec (AC-3) must likewise flag audioConfigSupported=false
// with a descriptive reason, not fail ParseMxfHeader.
void TestNonPcmCodecMarksAudioUnsupported() {
  SyntheticFile cfg;
  cfg.sequenceDuration = 10;
  SyntheticAudioTrack a;
  a.soundEssenceCoding = kAc3Coding;
  cfg.audioTracks = {a};
  auto path = WriteTempFile(BuildSyntheticHeader(cfg), "mxfsrc_test_ac3_unsupported.mxf");

  {
    MxfReader reader(path.string());
    auto info = ParseMxfHeader(reader);
    CHECK(info.has_value());
    if (info) {
      CHECK(info->hasAudio);
      CHECK(!info->audioConfigSupported);
      CHECK(!info->audioUnsupportedReason.empty());
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
  TestOriginParsedFromSourcePackage();
  TestNoSourcePackageLeavesOriginFieldsAtDefault();
  TestFooterPrecedenceOverridesOpenIncompleteHeader();
  TestRipFallbackLocatesFooterWhenHeaderFieldIsZero();
  TestNoFooterPartitionLeavesUnknownDurationUnresolved();
  TestXavcLongGopUlClassification();
  TestUlVersionByteIgnored();
  TestUndercountedHeaderByteCountRecoveredViaRip();
  TestEssenceInHeaderPartitionNotTreatedAsMetadata();
  TestSingleAudioTrackResolved();
  TestD10Aes3AudioMarkedUnsupported();
  TestMultiAudioTrackSummedAndBoundByLinkedTrackId();
  TestMismatchedBitDepthMarksAudioUnsupported();
  TestNonPcmCodecMarksAudioUnsupported();
  TestRejectsNonMxfFile();

  if (g_failures == 0) {
    std::cout << "all mxf header_parser tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
