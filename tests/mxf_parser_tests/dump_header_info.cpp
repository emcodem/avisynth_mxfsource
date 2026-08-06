// Manual diagnostic tool (not part of ctest, not linked into anything
// else): parses just the Header Metadata via ParseMxfHeader (no essence
// decode) and prints every resolved MxfStreamInfo field, plus scans
// forward from headerMetadataEnd to report the first few KLV keys
// encountered (Partition Packs and essence element keys) -- used to debug
// real-world files where header parsing succeeds but essence walking/
// decoding fails, without needing a full decode to succeed first. Build
// with `cmake --build build --target dump_header_info`.
#include <cstdio>
#include <iostream>

#include "mxf/header_parser.h"
#include "mxf/klv.h"

using namespace mxfsrc;

namespace {
void PrintUl(const char* label, const Ul& ul) {
  std::printf("%s: ", label);
  for (uint8_t b : ul) std::printf("%02x ", b);
  std::printf("\n");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: dump_header_info <file.mxf>" << std::endl;
    return 1;
  }

  MxfReader reader(argv[1]);
  if (!reader.IsOpen()) {
    std::cerr << "failed to open: " << reader.LastError() << std::endl;
    return 1;
  }
  std::printf("file size: %llu\n", static_cast<unsigned long long>(reader.Size()));

  std::string err;
  auto info = ParseMxfHeader(reader, &err);
  if (!info) {
    std::cerr << "ParseMxfHeader failed: " << err << std::endl;
    return 1;
  }

  std::printf("hasVideo=%d width=%u height=%u editRate=%d/%d\n", info->hasVideo, info->width, info->height,
              info->editRate.num, info->editRate.den);
  std::printf("numFrames=%lld videoCodec=%d\n", static_cast<long long>(info->numFrames),
              static_cast<int>(info->videoCodec));
  PrintUl("pictureEssenceCoding", info->pictureEssenceCoding);
  PrintUl("videoTrackNumber(as UL-shaped 4 bytes)", Ul{info->videoTrackNumber[0], info->videoTrackNumber[1],
                                                        info->videoTrackNumber[2], info->videoTrackNumber[3]});
  std::printf("hasAudio=%d audioConfigSupported=%d audioSampleRate=%u audioChannels=%u audioBitsPerSample=%u\n",
              info->hasAudio, info->audioConfigSupported, info->audioSampleRate, info->audioChannels,
              info->audioBitsPerSample);
  if (info->hasAudio && !info->audioConfigSupported) {
    std::printf("audioUnsupportedReason: %s\n", info->audioUnsupportedReason.c_str());
  }
  std::printf("audioTracks (%zu):\n", info->audioTracks.size());
  for (const auto& t : info->audioTracks) {
    std::printf("  trackId=%u trackNumber=%02x%02x%02x%02x channels=%u sampleRate=%u bitsPerSample=%u codec=%d\n",
                t.trackId, t.trackNumber[0], t.trackNumber[1], t.trackNumber[2], t.trackNumber[3], t.channelCount,
                t.sampleRate, t.bitsPerSample, static_cast<int>(t.codec));
  }
  std::printf("headerMetadataEnd=%llu footerPartitionPresent=%d\n",
              static_cast<unsigned long long>(info->headerMetadataEnd), info->footerPartitionPresent);
  std::printf("origin=%lld sourcePackageDuration=%lld\n", static_cast<long long>(info->origin),
              static_cast<long long>(info->sourcePackageDuration));
  std::printf("durationReliable=%d\n", info->durationReliable);

  // Scan forward from headerMetadataEnd printing the first few top-level
  // KLV keys (Partition Packs, essence elements, KLVFill) to see what's
  // actually there, independent of EssenceWalker's TrackNumber matching.
  if (!reader.Seek(info->headerMetadataEnd)) {
    std::cerr << "seek to headerMetadataEnd failed" << std::endl;
    return 1;
  }
  std::printf("\n--- scanning forward from headerMetadataEnd=%llu ---\n",
              static_cast<unsigned long long>(info->headerMetadataEnd));
  for (int i = 0; i < 20; ++i) {
    const uint64_t pos = reader.Tell();
    if (pos >= reader.Size()) {
      std::printf("[%d] EOF at %llu\n", i, static_cast<unsigned long long>(pos));
      break;
    }
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) {
      std::printf("[%d] failed to read KLV header at %llu\n", i, static_cast<unsigned long long>(pos));
      break;
    }
    std::printf("[%d] offset=%llu length=%llu key=", i, static_cast<unsigned long long>(pos),
                static_cast<unsigned long long>(klv.length));
    for (uint8_t b : klv.key) std::printf("%02x", b);
    std::printf("\n");
    if (!reader.Seek(klv.valueOffset + klv.length)) {
      std::printf("  (seek past value failed, stopping)\n");
      break;
    }
  }

  return 0;
}
