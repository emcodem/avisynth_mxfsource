#include "mxf/header_parser.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "mxf/index_table.h"
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
  if (!UlPrefixEquals(key, kSetTypePrefix, sizeof(kSetTypePrefix))) return SetType::kOther;
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
constexpr uint16_t kTrackId = 0x4801;
// GenericDescriptor.LinkedTrackID: binds one sub-descriptor of a
// MultipleDescriptor to a specific Track when a file has more than one
// audio track (see ResolveDescriptor).
constexpr uint16_t kLinkedTrackID = 0x3006;
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

bool ExtractSoundParams(const LocalTagMap& d, AudioTrackInfo& out) {
  uint32_t channels = 0;
  if (!GetU32(d, tag::kDescriptorChannelCount, channels)) return false;
  if (channels == 0) return false;
  out.channelCount = channels;
  Rational sr{};
  if (GetRational32(d, tag::kDescriptorSampleRate, sr) && sr.den > 0) {
    out.sampleRate = static_cast<uint32_t>(sr.num / sr.den);
  }
  GetU32(d, tag::kDescriptorQuantizationBits, out.bitsPerSample);
  Ul coding{};
  if (GetUl(d, tag::kDescriptorSoundEssenceCompression, coding)) {
    out.codec = ClassifyCodecUl(coding);
  } else {
    // SoundEssenceCompression is optional per SMPTE 377-1/382: a compressed
    // format (AC-3/MP2/AAC) has no other way to signal what scheme was used
    // and always carries it, so its absence reliably means uncompressed
    // PCM in practice -- confirmed against a real Canon camera file whose
    // AES3PCMDescriptor sets (SMPTE 382, AES3-wrapped linear PCM) omit this
    // tag entirely. Defaulting to kUnknown here (as an earlier version of
    // this code did) misclassified that real file's perfectly valid PCM
    // audio as an unsupported codec.
    out.codec = CodecFamily::kPcm;
  }
  return true;
}

// Resolves a Package's DescriptorRef -- possibly a MultipleDescriptor
// wrapping one sub-descriptor per essence track -- for the Track currently
// being visited in ResolvePackage's loop (identified by `kind`/`trackId`/
// `trackNumberBytes`). Gated by `kind` so a video Track's call can only ever
// resolve video and an audio Track's call can only ever resolve audio --
// without this, since a single-audio-track file's MultipleDescriptor has
// exactly one sound-shaped sub-descriptor, the *video* Track's call (which
// runs first, per ResolvePackage's Track order) would otherwise immediately
// claim it via the sole-candidate fallback below, attributing it to the
// video Track's TrackNumber instead of the real audio Track's.
//
// Audio sub-descriptor binding: a file with exactly one sound-shaped
// sub-descriptor (the common single-audio-track case) binds it directly --
// zero behavior change from the original shape-only matching. A file with
// more than one requires LinkedTrackID (baseline tag 0x3006) to match the
// current Track's TrackID; if none matches (e.g. an older/lenient writer
// that omits LinkedTrackID on a multi-audio-track file), this call simply
// resolves no audio for this Track rather than risk binding the wrong
// channels to it.
void ResolveDescriptor(const Ul& descriptorRef, const RawSets& sets, EssenceKind kind, uint32_t trackId,
                       const std::array<uint8_t, 4>& trackNumberBytes, MxfStreamInfo& info) {
  auto it = sets.byInstanceUid.find(descriptorRef);
  if (it == sets.byInstanceUid.end()) return;
  const LocalTagMap& d = it->second;

  std::vector<Ul> subRefs = GetUlArray(d, tag::kMultipleDescriptorFileDescriptors);
  if (subRefs.empty()) {
    if (kind == EssenceKind::kVideo && !info.hasVideo) info.hasVideo = ExtractPictureParams(d, info);
    if (kind == EssenceKind::kAudio) {
      AudioTrackInfo audio;
      audio.trackNumber = trackNumberBytes;
      audio.trackId = trackId;
      if (ExtractSoundParams(d, audio)) info.audioTracks.push_back(audio);
    }
    return;
  }

  if (kind == EssenceKind::kVideo && !info.hasVideo) {
    for (const Ul& sub : subRefs) {
      auto sit = sets.byInstanceUid.find(sub);
      if (sit == sets.byInstanceUid.end()) continue;
      if (ExtractPictureParams(sit->second, info)) {
        info.hasVideo = true;
        break;
      }
    }
  }

  if (kind == EssenceKind::kAudio) {
    const LocalTagMap* chosen = nullptr;
    const LocalTagMap* soleCandidate = nullptr;
    int soundCount = 0;
    for (const Ul& sub : subRefs) {
      auto sit = sets.byInstanceUid.find(sub);
      if (sit == sets.byInstanceUid.end()) continue;
      uint32_t probeChannels = 0;
      if (!GetU32(sit->second, tag::kDescriptorChannelCount, probeChannels) || probeChannels == 0) continue;
      ++soundCount;
      soleCandidate = &sit->second;
      uint32_t linkedId = 0;
      if (GetU32(sit->second, tag::kLinkedTrackID, linkedId) && linkedId == trackId) {
        chosen = &sit->second;
      }
    }
    if (!chosen && soundCount == 1) chosen = soleCandidate;

    if (chosen) {
      AudioTrackInfo audio;
      audio.trackNumber = trackNumberBytes;
      audio.trackId = trackId;
      if (ExtractSoundParams(*chosen, audio)) info.audioTracks.push_back(audio);
    }
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

    // Video stays single-track: once resolved, later video Tracks are
    // skipped (multi-video-track detection is separate, not-yet-done work,
    // see TODO.md Phase 8 item 6). Audio is never skipped here -- every
    // audio Track is visited so all of them can be summed into one output.
    if (kind == EssenceKind::kVideo && info.hasVideo) continue;

    Rational editRate{};
    const bool haveEditRate = GetRational32(track, tag::kTrackEditRate, editRate);
    int64_t duration = -1;
    GetI64(sequence, tag::kSequenceDuration, duration);
    uint32_t trackNumber = 0;
    const bool haveTrackNumber = GetU32(track, tag::kTrackNumber, trackNumber);
    uint32_t trackId = 0;
    GetU32(track, tag::kTrackId, trackId);
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
    }
    if (haveDescriptor) ResolveDescriptor(descriptorRef, sets, kind, trackId, trackNumberBytes, info);
  }
}

// Generic Container element keys -- picture/sound/data items and the
// system item's local sets alike -- all carry 0d 01 03 01 in bytes 8-11
// (SMPTE 379), whatever their other bytes.
bool IsEssenceContainerElementKey(const Ul& key) {
  return key[8] == 0x0d && key[9] == 0x01 && key[10] == 0x03 && key[11] == 0x01;
}

// Offset of the first KLV at or after `start` that can't be Header Metadata
// -- an Index Table Segment, an essence container element, a Partition Pack,
// or bytes that aren't a KLV key at all -- never past `limit`. Only reads
// KLV headers, so it costs one small read per metadata set.
uint64_t ScanHeaderMetadataEnd(MxfReader& reader, uint64_t start, uint64_t limit) {
  uint64_t pos = start;
  while (pos + 17 <= limit && reader.Seek(pos)) {
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv) || !LooksLikeKlvKey(klv.key)) break;
    if (UlEquals(klv.key, kIndexTableSegmentKey) || IsPartitionPackKey(klv.key, nullptr) ||
        IsEssenceContainerElementKey(klv.key)) {
      break;
    }
    const uint64_t next = klv.valueOffset + klv.length;
    if (next > limit) break;
    pos = next;
  }
  return pos;
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
    // No early-exit here (unlike the old single-video/single-audio version):
    // audio tracks can be spread across multiple Tracks within a package (or
    // even across packages), so every package must be scanned to collect
    // all of them. Cost is trivial -- real files have at most a handful of
    // packages (MaterialPackage + one or more SourcePackages).
  }
  return info;
}

// Sums audioTracks' channel counts and validates every track agrees on bit
// depth, sample rate, and codec family (uncompressed PCM only) -- the only
// configuration this plugin can output audio for today. Sets info.hasAudio
// and either audioConfigSupported (populating the singular
// audioSampleRate/audioBitsPerSample/audioCodec/audioChannels summary
// fields used for output) or audioUnsupportedReason. Never fails
// ParseMxfHeader itself -- what to do about unsupported audio (degrade vs.
// hard-fail) is a playback-policy decision made later by
// SourceEngine::Open via UseAudioMode, not a parse error.
void ValidateAndSummarizeAudio(MxfStreamInfo& info) {
  info.hasAudio = !info.audioTracks.empty();
  if (!info.hasAudio) return;

  // TrackNumber = the essence element key's last 4 bytes (SMPTE 379): item
  // type, element count, element type, element number. Item type 0x06 is
  // an SDTI-CP sound item (D-10/IMX); its element type 0x10 is the
  // "8-Ch AES3 Element" (see AudioTrackInfo::eightChAes3Element), the only
  // one supported. It carries 16 or 24 bit in at most 8 channels.
  for (AudioTrackInfo& t : info.audioTracks) {
    if (t.trackNumber[0] != 0x06) continue;
    if (t.trackNumber[2] != 0x10) {
      info.audioUnsupportedReason = "SDTI-CP sound element other than the 8-Ch AES3 Element not supported";
      return;
    }
    t.eightChAes3Element = true;
    if ((t.bitsPerSample != 16 && t.bitsPerSample != 24) || t.channelCount > 8) {
      info.audioUnsupportedReason = "8-Ch AES3 Element with unexpected bit depth or more than 8 channels";
      return;
    }
  }

  const AudioTrackInfo& first = info.audioTracks.front();
  uint32_t totalChannels = 0;
  for (const AudioTrackInfo& t : info.audioTracks) {
    totalChannels += t.channelCount;
    if (t.codec != CodecFamily::kPcm) {
      info.audioUnsupportedReason = "non-PCM audio codec not supported yet";
      return;
    }
    if (t.bitsPerSample != first.bitsPerSample) {
      info.audioUnsupportedReason = "audio tracks have mismatched bit depth";
      return;
    }
    if (t.sampleRate != first.sampleRate) {
      info.audioUnsupportedReason = "audio tracks have mismatched sample rate";
      return;
    }
  }
  info.audioConfigSupported = true;
  info.audioChannels = totalChannels;
  info.audioSampleRate = first.sampleRate;
  info.audioBitsPerSample = first.bitsPerSample;
  info.audioCodec = CodecFamily::kPcm;
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
  const uint32_t savedBodySid = info.bodySid;
  info = footerInfo;
  info.headerMetadataEnd = savedHeaderMetadataEnd;
  info.footerPartitionPresent = savedFooterPartitionPresent;
  info.bodySid = savedBodySid;
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
  uint64_t headerMetadataEnd = partitionEnd + partition->headerByteCount;

  // The Random Index Pack (see mxf/partition.h), if present, is read once
  // and used for two independent fixups below -- both observed on a real
  // Omneon "growing file" recording, so neither is hypothetical.
  std::optional<std::vector<RandomIndexPackEntry>> rip = ReadRandomIndexPack(reader);

  // Fixup 1: HeaderByteCount can itself undercount the metadata actually
  // written (observed: a trailing Identification set, or -- on another real
  // file -- the video Descriptor set itself, whose value genuinely extends
  // past partitionEnd+headerByteCount). Applied *before* the metadata read
  // below, not after: ReadMetadataSets tolerates the undercount silently (a
  // KLV whose declared value would cross that boundary is treated as "no
  // more metadata to read" rather than a parse failure), so scanning with
  // the raw, too-small headerByteCount silently drops whichever sets
  // happen to live in the undercounted gap -- fatal when that's the
  // Descriptor carrying StoredWidth/StoredHeight, not just cosmetic like a
  // dropped Identification set. The RIP is authoritative here: its entries
  // record every partition's real byte offset in file order, so the entry
  // right after the Header Partition's own (always byteOffset 0) entry is
  // exactly where the Header Partition truly ends -- *if* that entry is a
  // separate Body Partition. Some real files (observed: Atomos Shogun
  // XAVC-Intra/XAVC-L recordings) have no Body Partition at all -- the
  // Header Partition itself carries the Index Table Segment and then all
  // essence directly after its Header Metadata, and RIP[1] is the Footer
  // Partition instead. Blindly trusting RIP[1] there would jump straight to
  // the Footer and make both this metadata scan and EssenceWalker skip
  // every essence element in the file, so this fixup only applies when
  // RIP[1] actually resolves to a Body Partition Pack.
  //
  // RIP[1] is only an upper bound, though: the real end is found by walking
  // KLV headers up to the first one that isn't metadata. Other real files
  // (67 on the sample share, Harmonic/AJA UHD H.264) carry the essence in
  // the Header Partition itself, after its Index Table Segments, and their
  // only Body Partition sits near EOF -- jumping straight to RIP[1] there
  // made this metadata scan walk tens of GB of essence KLV by KLV, and
  // EssenceWalker skip all of it.
  if (rip && rip->size() >= 2 && (*rip)[0].byteOffset == 0) {
    const uint64_t nextPartitionOffset = (*rip)[1].byteOffset;
    if (nextPartitionOffset > headerMetadataEnd && nextPartitionOffset < reader.Size() &&
        reader.Seek(nextPartitionOffset)) {
      KlvHeader nextKlv{};
      PartitionKind nextKind{};
      if (ReadKlvHeader(reader, nextKlv) && IsPartitionPackKey(nextKlv.key, &nextKind) &&
          nextKind == PartitionKind::kBody) {
        headerMetadataEnd =
            std::max(headerMetadataEnd, ScanHeaderMetadataEnd(reader, partitionEnd, nextPartitionOffset));
      }
    }
  }

  RawSets headerSets;
  if (!ReadMetadataSets(reader, partitionEnd, headerMetadataEnd - partitionEnd, headerSets)) {
    return fail("malformed local-set metadata KLV");
  }

  MxfStreamInfo info = ResolveStreamInfo(headerSets);
  info.headerMetadataEnd = headerMetadataEnd;
  info.footerPartitionPresent = partition->footerPartition != 0;
  info.bodySid = partition->bodySid;

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
  ValidateAndSummarizeAudio(info);
  return info;
}

}  // namespace mxfsrc
