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
constexpr uint16_t kTrackOrigin = 0x4B02;
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
  // Whether each entry in `packages` is a SourcePackage (true) or a
  // MaterialPackage (false) -- needed to know which package's Track.Origin
  // is authoritative (only a SourcePackage's Origin is meaningful; a
  // MaterialPackage's must always be 0 per SMPTE 377M), independent of
  // whichever package ResolvePackage happens to resolve fields from first.
  std::map<Ul, bool> packageIsSource;
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

void ResolvePackage(const Ul& packageUid, const RawSets& sets, bool isSourcePackage, MxfStreamInfo& info) {
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

    // Origin/raw-duration are captured here (ahead of the "already
    // resolved" skip below) specifically because they must come from the
    // SourcePackage's video track regardless of whether some other
    // package's video track was resolved for editRate/trackNumber first --
    // see MxfStreamInfo::origin's comment.
    if (kind == EssenceKind::kVideo && isSourcePackage) {
      int64_t origin = 0;
      GetI64(track, tag::kTrackOrigin, origin);  // absent => 0, i.e. no trim
      info.origin = origin;
      int64_t sourceDuration = -1;
      if (GetI64(sequence, tag::kSequenceDuration, sourceDuration) && sourceDuration >= 0) {
        info.sourcePackageDuration = sourceDuration;
      }
    }

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

// Walks every local-set-coded metadata KLV in [partitionEnd, partitionEnd +
// byteCount) into `sets`. Shared between the Header Partition's Header
// Metadata and a Footer Partition's redundant copy of the same (see
// ParseMxfHeader's footer-precedence handling below) -- identical format,
// just a different partition's byte range.
bool ReadMetadataSets(MxfReader& reader, uint64_t partitionEnd, uint64_t byteCount, RawSets& sets) {
  const uint64_t metadataEnd = partitionEnd + byteCount;
  if (!reader.Seek(partitionEnd)) return false;

  while (reader.Tell() + 16 + 1 <= metadataEnd && reader.Tell() < reader.Size()) {
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) break;
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (valueEnd > metadataEnd) break;

    if (UlEquals(klv.key, kKlvFillKey) || UlEquals(klv.key, kPrimerPackKey)) {
      reader.Seek(valueEnd);
      continue;
    }
    if (!IsLocalSetKey(klv.key)) {
      reader.Seek(valueEnd);
      continue;
    }

    LocalTagMap parsed;
    if (!ReadLocalSet(reader, klv.length, parsed)) return false;

    const SetType type = ClassifySetType(klv.key);
    Ul instanceUid{};
    if (GetInstanceUid(parsed, instanceUid)) {
      if (type == SetType::kContentStorage) sets.contentStorages.push_back(instanceUid);
      if (type == SetType::kMaterialPackage || type == SetType::kSourcePackage) {
        sets.packages.push_back(instanceUid);
        sets.packageIsSource[instanceUid] = (type == SetType::kSourcePackage);
      }
      sets.byInstanceUid[instanceUid] = std::move(parsed);
    }
    reader.Seek(valueEnd);
  }
  return true;
}

// Resolves a full RawSets (as produced by ReadMetadataSets) into a fresh
// MxfStreamInfo -- shared between the Header Partition's pass and a Closed
// Footer Partition's override pass.
MxfStreamInfo ResolveStreamInfo(const RawSets& sets) {
  MxfStreamInfo info;

  std::vector<Ul> packageUids;
  if (!sets.contentStorages.empty()) {
    packageUids = GetUlArray(sets.byInstanceUid.at(sets.contentStorages.front()), tag::kContentStoragePackages);
  }
  if (packageUids.empty()) packageUids = sets.packages;  // fallback for malformed/unusual files

  for (const Ul& packageUid : packageUids) {
    auto isSourceIt = sets.packageIsSource.find(packageUid);
    const bool isSourcePackage = isSourceIt != sets.packageIsSource.end() && isSourceIt->second;
    ResolvePackage(packageUid, sets, isSourcePackage, info);
    if (info.hasVideo && info.hasAudio) break;
  }
  return info;
}

// Attempts to override `info` from the Footer Partition's own redundant
// Header Metadata copy, given a candidate byte offset for it (either the
// Header Partition Pack's own FooterPartition field, or -- when that's
// left 0/unknown, as observed on a real Omneon recording -- the last entry
// of the Random Index Pack; see ParseMxfHeader). No-ops (returns false)
// unless `candidateOffset` genuinely points at a Closed Footer Partition
// whose Header Metadata copy resolves a video track, so trying a
// less-certain second candidate can never regress an already-successful
// override, nor corrupt `info` with a partial/malformed read.
bool TryOverrideFromFooterPartition(MxfReader& reader, uint64_t candidateOffset, MxfStreamInfo& info) {
  if (candidateOffset >= reader.Size()) return false;
  if (!reader.Seek(candidateOffset)) return false;

  KlvHeader footerKlv{};
  PartitionKind footerKind{};
  if (!ReadKlvHeader(reader, footerKlv)) return false;
  if (!IsPartitionPackKey(footerKlv.key, &footerKind) || footerKind != PartitionKind::kFooter) return false;
  if (!IsPartitionClosed(ClassifyPartitionStatus(footerKlv.key))) return false;

  std::optional<PartitionPack> footerPartition = ReadPartitionPackValue(reader, footerKlv);
  if (!footerPartition) return false;

  const uint64_t footerPartitionEnd = footerKlv.valueOffset + footerKlv.length;
  RawSets footerSets;
  if (!ReadMetadataSets(reader, footerPartitionEnd, footerPartition->headerByteCount, footerSets)) return false;

  MxfStreamInfo footerInfo = ResolveStreamInfo(footerSets);
  if (!footerInfo.hasVideo) return false;

  const uint64_t savedHeaderMetadataEnd = info.headerMetadataEnd;
  const bool savedFooterPartitionPresent = info.footerPartitionPresent;
  info = footerInfo;
  info.headerMetadataEnd = savedHeaderMetadataEnd;
  info.footerPartitionPresent = savedFooterPartitionPresent;
  return true;
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

  RawSets headerSets;
  if (!ReadMetadataSets(reader, partitionEnd, partition->headerByteCount, headerSets)) {
    return fail("malformed local-set metadata KLV");
  }

  MxfStreamInfo info = ResolveStreamInfo(headerSets);
  info.headerMetadataEnd = headerMetadataEnd;
  info.footerPartitionPresent = partition->footerPartition != 0;

  // The Random Index Pack (see mxf/partition.h), if present, is read once
  // and used for two independent fixups below -- both observed on a real
  // Omneon "growing file" recording, so neither is hypothetical.
  std::optional<std::vector<RandomIndexPackEntry>> rip = ReadRandomIndexPack(reader);

  // Fixup 1: HeaderByteCount can itself undercount the metadata actually
  // written (observed: a trailing Identification set whose value genuinely
  // extends past partitionEnd+headerByteCount). ReadMetadataSets above
  // tolerates this silently -- a KLV whose declared value would cross that
  // boundary is treated as "no more metadata to read" rather than a parse
  // failure -- but that leaves headerMetadataEnd (EssenceWalker's scan
  // start point) landing mid-way through real metadata bytes instead of at
  // the true start of the next partition, which EssenceWalker then reads
  // as garbage and fails to find any essence in. The RIP is authoritative
  // here: its entries record every partition's real byte offset in file
  // order, so the entry right after the Header Partition's own (always
  // byteOffset 0) entry is exactly where the Header Partition truly ends --
  // *if* that entry is a separate Body Partition. Some real files (observed:
  // Atomos Shogun XAVC-Intra/XAVC-L recordings) have no Body Partition at
  // all -- the Header Partition itself carries the Index Table Segment and
  // then all essence directly after its Header Metadata, and RIP[1] is the
  // Footer Partition instead. Blindly trusting RIP[1] there would jump
  // straight to the Footer and make EssenceWalker skip every essence
  // element in the file, so this fixup only applies when RIP[1] actually
  // resolves to a Body Partition Pack.
  if (rip && rip->size() >= 2 && (*rip)[0].byteOffset == 0) {
    const uint64_t nextPartitionOffset = (*rip)[1].byteOffset;
    if (nextPartitionOffset > info.headerMetadataEnd && nextPartitionOffset < reader.Size() &&
        reader.Seek(nextPartitionOffset)) {
      KlvHeader nextKlv{};
      PartitionKind nextKind{};
      if (ReadKlvHeader(reader, nextKlv) && IsPartitionPackKey(nextKlv.key, &nextKind) &&
          nextKind == PartitionKind::kBody) {
        info.headerMetadataEnd = nextPartitionOffset;
      }
    }
  }

  // Fixup 2 (footer precedence, SMPTE 377-1: a Closed partition's metadata
  // takes precedence over an Open one): some recorders leave the Header
  // Partition Open/Incomplete -- Duration/Origin genuinely unresolved --
  // until the recording finishes, writing final values only into the
  // Footer Partition's own redundant Header Metadata copy.
  bool overridden = false;
  if (partition->footerPartition != 0) {
    overridden = TryOverrideFromFooterPartition(reader, partition->footerPartition, info);
  }
  if (!overridden && rip && !rip->empty()) {
    // The Header Partition Pack's FooterPartition back-reference can be
    // left 0/unknown even in an otherwise-complete file -- the RIP's last
    // entry is always the Footer Partition's offset, giving a reader with
    // no prior knowledge of any partition's byte offset a way to find it
    // anyway.
    overridden = TryOverrideFromFooterPartition(reader, rip->back().byteOffset, info);
    if (overridden) info.footerPartitionPresent = true;
  }

  info.durationReliable = info.hasVideo && info.numFrames >= 0 && info.footerPartitionPresent;
  return info;
}

}  // namespace mxfsrc
