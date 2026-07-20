#include "mxf/header_parser.h"

#include <cstring>
#include <map>
#include <vector>

#include "mxf/partition.h"

namespace mxfsrc {

namespace {

// Local-set type keys share the 14-byte prefix below; the last two bytes
// (key[14],key[15]) select the set type. Values verified against FFmpeg's
// libavformat/mxfdec.c mxf_metadata_read_table (see plan Phase 1 notes).
constexpr uint8_t kSetTypePrefix[14] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53, 0x01,
                                         0x01, 0x0d, 0x01, 0x01, 0x01, 0x01, 0x01};

enum class SetType {
  kOther,
  kPreface,
  kContentStorage,
  kMaterialPackage,
  kSourcePackage,
  kSequence,
  kMultipleDescriptor,
  kSoundDescriptor,
  kCdciDescriptor,
  kRgbaDescriptor,
  kWaveDescriptor,
  kAes3Descriptor,
  kMpeg2VideoDescriptor,
  kMpeg2AudioDescriptor,
};

SetType ClassifySetType(const Ul& key) {
  if (std::memcmp(key.data(), kSetTypePrefix, sizeof(kSetTypePrefix)) != 0) return SetType::kOther;
  const uint16_t code = (static_cast<uint16_t>(key[14]) << 8) | key[15];
  switch (code) {
    case 0x2F00: return SetType::kPreface;
    case 0x1800: return SetType::kContentStorage;
    case 0x3600: return SetType::kMaterialPackage;
    case 0x3700: return SetType::kSourcePackage;
    case 0x0F00: return SetType::kSequence;
    case 0x4400: return SetType::kMultipleDescriptor;
    case 0x4200: return SetType::kSoundDescriptor;
    case 0x2800: return SetType::kCdciDescriptor;
    case 0x2900: return SetType::kRgbaDescriptor;
    case 0x4800: return SetType::kWaveDescriptor;
    case 0x4700: return SetType::kAes3Descriptor;
    case 0x5100: return SetType::kMpeg2VideoDescriptor;
    case 0x5E00: return SetType::kMpeg2AudioDescriptor;
    default: return SetType::kOther;
  }
}

// Local tags, per SMPTE 377-1's static/baseline register, verified against
// FFmpeg's libavformat/mxfdec.c per-type read functions (mxf_read_package,
// mxf_read_track, mxf_read_sequence, mxf_read_source_clip,
// mxf_read_generic_descriptor, mxf_read_content_storage).
namespace tag {
constexpr uint16_t kPackageTracks = 0x4403;
constexpr uint16_t kPackageDescriptor = 0x4701;
constexpr uint16_t kContentStoragePackages = 0x1901;
constexpr uint16_t kTrackEditRate = 0x4B01;
constexpr uint16_t kTrackSequenceRef = 0x4803;
constexpr uint16_t kTrackNumber = 0x4804;
constexpr uint16_t kSequenceDuration = 0x0202;
constexpr uint16_t kSequenceDataDefinition = 0x0201;
constexpr uint16_t kDescriptorContainerDuration = 0x3002;
constexpr uint16_t kDescriptorPictureEssenceCoding = 0x3201;
constexpr uint16_t kDescriptorSoundEssenceCompression = 0x3D06;
constexpr uint16_t kDescriptorStoredWidth = 0x3203;
constexpr uint16_t kDescriptorStoredHeight = 0x3202;
constexpr uint16_t kDescriptorSampleRate = 0x3D03;
constexpr uint16_t kDescriptorChannelCount = 0x3D07;
constexpr uint16_t kDescriptorQuantizationBits = 0x3D01;
constexpr uint16_t kMultipleDescriptorFileDescriptors = 0x3F01;
}  // namespace tag

struct RawSets {
  std::map<Ul, LocalTagMap> byInstanceUid;
  std::vector<Ul> contentStorages;
  std::vector<Ul> packages;  // Material and Source, in file order
};

bool ExtractPictureParams(const LocalTagMap& d, MxfStreamInfo& info) {
  uint32_t width = 0, height = 0;
  if (!GetU32(d, tag::kDescriptorStoredWidth, width)) return false;
  if (!GetU32(d, tag::kDescriptorStoredHeight, height)) return false;
  if (width == 0 || height == 0) return false;
  info.width = width;
  info.height = height;
  Ul coding{};
  if (GetUl(d, tag::kDescriptorPictureEssenceCoding, coding)) {
    info.pictureEssenceCoding = coding;
    info.videoCodec = ClassifyCodecUl(coding);
  }
  int64_t containerDuration = 0;
  if (info.numFrames < 0 && GetI64(d, tag::kDescriptorContainerDuration, containerDuration)) {
    info.numFrames = containerDuration;
  }
  return true;
}

bool ExtractSoundParams(const LocalTagMap& d, MxfStreamInfo& info) {
  uint32_t channels = 0;
  if (!GetU32(d, tag::kDescriptorChannelCount, channels)) return false;
  if (channels == 0) return false;
  info.audioChannels = channels;
  Rational sr{};
  if (GetRational32(d, tag::kDescriptorSampleRate, sr) && sr.den > 0) {
    info.audioSampleRate = static_cast<uint32_t>(sr.num / sr.den);
  }
  GetU32(d, tag::kDescriptorQuantizationBits, info.audioBitsPerSample);
  Ul coding{};
  if (GetUl(d, tag::kDescriptorSoundEssenceCompression, coding)) {
    info.audioCodec = ClassifyCodecUl(coding);
  }
  return true;
}

// Resolves a Package's DescriptorRef -- possibly a MultipleDescriptor
// wrapping one sub-descriptor per essence track -- and fills in whichever of
// info.hasVideo/hasAudio isn't set yet. MVP matches sub-descriptors by shape
// (has StoredWidth => picture, has ChannelCount => sound) rather than by
// LinkedTrackID; see Phase 7 for exact per-track matching.
void ResolveDescriptor(const Ul& descriptorRef, const RawSets& sets, MxfStreamInfo& info) {
  auto it = sets.byInstanceUid.find(descriptorRef);
  if (it == sets.byInstanceUid.end()) return;
  const LocalTagMap& d = it->second;

  std::vector<Ul> subRefs = GetUlArray(d, tag::kMultipleDescriptorFileDescriptors);
  if (subRefs.empty()) {
    if (!info.hasVideo) info.hasVideo = ExtractPictureParams(d, info);
    if (!info.hasAudio) info.hasAudio = ExtractSoundParams(d, info);
    return;
  }
  for (const Ul& sub : subRefs) {
    auto sit = sets.byInstanceUid.find(sub);
    if (sit == sets.byInstanceUid.end()) continue;
    if (!info.hasVideo) info.hasVideo = ExtractPictureParams(sit->second, info);
    if (!info.hasAudio) info.hasAudio = ExtractSoundParams(sit->second, info);
  }
}

void ResolvePackage(const Ul& packageUid, const RawSets& sets, MxfStreamInfo& info) {
  auto pit = sets.byInstanceUid.find(packageUid);
  if (pit == sets.byInstanceUid.end()) return;
  const LocalTagMap& pkg = pit->second;

  Ul descriptorRef{};
  const bool haveDescriptor = GetUl(pkg, tag::kPackageDescriptor, descriptorRef);

  for (const Ul& trackUid : GetUlArray(pkg, tag::kPackageTracks)) {
    auto tit = sets.byInstanceUid.find(trackUid);
    if (tit == sets.byInstanceUid.end()) continue;
    const LocalTagMap& track = tit->second;

    Ul sequenceRef{};
    if (!GetUl(track, tag::kTrackSequenceRef, sequenceRef)) continue;
    auto sit = sets.byInstanceUid.find(sequenceRef);
    if (sit == sets.byInstanceUid.end()) continue;
    const LocalTagMap& sequence = sit->second;

    Ul dataDefinition{};
    if (!GetUl(sequence, tag::kSequenceDataDefinition, dataDefinition)) continue;
    const EssenceKind kind = ClassifyDataDefinition(dataDefinition);
    if (kind != EssenceKind::kVideo && kind != EssenceKind::kAudio) continue;
    if ((kind == EssenceKind::kVideo && info.hasVideo) || (kind == EssenceKind::kAudio && info.hasAudio)) {
      continue;  // already resolved this kind from an earlier package/track
    }

    Rational editRate{};
    const bool haveEditRate = GetRational32(track, tag::kTrackEditRate, editRate);
    int64_t duration = -1;
    GetI64(sequence, tag::kSequenceDuration, duration);
    uint32_t trackNumber = 0;
    const bool haveTrackNumber = GetU32(track, tag::kTrackNumber, trackNumber);
    const auto trackNumberBytes = [trackNumber]() {
      std::array<uint8_t, 4> b{};
      b[0] = static_cast<uint8_t>(trackNumber >> 24);
      b[1] = static_cast<uint8_t>(trackNumber >> 16);
      b[2] = static_cast<uint8_t>(trackNumber >> 8);
      b[3] = static_cast<uint8_t>(trackNumber);
      return b;
    }();

    if (kind == EssenceKind::kVideo) {
      if (haveEditRate && editRate.den != 0) info.editRate = editRate;
      if (duration >= 0) info.numFrames = duration;
      if (haveTrackNumber) info.videoTrackNumber = trackNumberBytes;
    } else {
      if (haveTrackNumber) info.audioTrackNumber = trackNumberBytes;
    }
    if (haveDescriptor) ResolveDescriptor(descriptorRef, sets, info);
  }
}

}  // namespace

std::optional<MxfStreamInfo> ParseMxfHeader(MxfReader& reader, std::string* errorOut) {
  auto fail = [&](const char* msg) -> std::optional<MxfStreamInfo> {
    if (errorOut) *errorOut = msg;
    return std::nullopt;
  };

  if (!reader.IsOpen()) return fail("file not open");
  if (!reader.Seek(0)) return fail("seek to start failed");

  KlvHeader partitionKlv{};
  if (!ReadKlvHeader(reader, partitionKlv)) return fail("failed to read Header Partition Pack KLV header");
  PartitionKind kind{};
  if (!IsPartitionPackKey(partitionKlv.key, &kind) || kind != PartitionKind::kHeader) {
    return fail("file does not start with a Header Partition Pack");
  }
  std::optional<PartitionPack> partition = ReadPartitionPackValue(reader, partitionKlv);
  if (!partition) return fail("failed to parse Header Partition Pack");

  const uint64_t partitionEnd = partitionKlv.valueOffset + partitionKlv.length;
  const uint64_t headerMetadataEnd = partitionEnd + partition->headerByteCount;
  if (!reader.Seek(partitionEnd)) return fail("seek past Header Partition Pack failed");

  RawSets sets;
  while (reader.Tell() + 16 + 1 <= headerMetadataEnd && reader.Tell() < reader.Size()) {
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) break;
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > headerMetadataEnd) break;

    if (UlEquals(klv.key, kKlvFillKey) || UlEquals(klv.key, kPrimerPackKey)) {
      reader.Seek(valueEnd);
      continue;
    }
    if (!IsLocalSetKey(klv.key)) {
      reader.Seek(valueEnd);
      continue;
    }

    LocalTagMap parsed;
    if (!ReadLocalSet(reader, klv.length, parsed)) return fail("malformed local-set metadata KLV");

    const SetType type = ClassifySetType(klv.key);
    Ul instanceUid{};
    if (GetInstanceUid(parsed, instanceUid)) {
      if (type == SetType::kContentStorage) sets.contentStorages.push_back(instanceUid);
      if (type == SetType::kMaterialPackage || type == SetType::kSourcePackage) sets.packages.push_back(instanceUid);
      sets.byInstanceUid[instanceUid] = std::move(parsed);
    }
    reader.Seek(valueEnd);
  }

  MxfStreamInfo info;
  info.headerMetadataEnd = headerMetadataEnd;
  info.footerPartitionPresent = partition->footerPartition != 0;

  std::vector<Ul> packageUids;
  if (!sets.contentStorages.empty()) {
    packageUids = GetUlArray(sets.byInstanceUid.at(sets.contentStorages.front()), tag::kContentStoragePackages);
  }
  if (packageUids.empty()) packageUids = sets.packages;  // fallback for malformed/unusual files

  for (const Ul& packageUid : packageUids) {
    ResolvePackage(packageUid, sets, info);
    if (info.hasVideo && info.hasAudio) break;
  }

  info.durationReliable = info.hasVideo && info.numFrames >= 0 && info.footerPartitionPresent;
  return info;
}

}  // namespace mxfsrc
