#include "core/index_policy.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "mxf/bitstream_index.h"
#include "mxf/essence_walker.h"
#include "mxf/integrity.h"
#include "mxf/partition.h"

namespace mxfsrc {

IntegrityCheckResult CheckIntegrityBeforeIndex(MxfReader& reader, bool strict) {
  IntegrityCheckResult result;
  if (!strict) return result;
  if (std::optional<FileIntegrity> integrity = ProbeFileIntegrity(reader)) {
    result.error = CheckStrictIntegrity(*integrity);
    result.finished = integrity->Finished();
  }
  return result;
}

VideoIndexResult BuildVideoIndex(MxfReader& reader, const std::string& path, const std::string& httpHeaders,
                                 const MxfStreamInfo& info, bool strict) {
  VideoIndexResult result;
  if (IsHttpUrl(path) && reader.SupportsRandomAccess()) {
    // A network source with confirmed byte-range support: BuildEssenceIndex's
    // sequential walk would pay one full HTTP round trip per partition,
    // serially -- see mxf/index_table.h's BuildEssenceIndexFromHttp doc
    // comment for why that's the actual bottleneck on a real
    // multi-partition file. Read the RIP once more via the already-open
    // `reader` purely to learn every partition's byte offset, then fan out
    // concurrent bounded fetches instead of walking them one at a time.
    if (auto rip = ReadRandomIndexPack(reader)) {
      std::vector<uint64_t> offsets;
      offsets.reserve(rip->size() + 1);
      offsets.push_back(0);
      for (const auto& entry : *rip) offsets.push_back(entry.byteOffset);
      result.index = BuildEssenceIndexFromHttp(path, httpHeaders, info.bodySid, reader.Size(), std::move(offsets));
    }
  }
  if (!result.index || result.index->Empty()) {
    // Either not eligible for the parallel path, or it came back empty
    // (e.g. no RIP found) -- the same reader, still fully valid regardless
    // of how far the attempt above moved it.
    result.index = BuildEssenceIndex(reader, info.bodySid);
  }
  if (result.index && !result.index->Empty()) return result;

  if (strict) {
    // Strict never builds an index by walking the whole file, finished or
    // not: a growing file opens only if its index is reachable directly
    // (Header Partition, e.g. a CBR/CBG segment, or partitions listed in
    // the RIP).
    result.error =
        "strict: no Index Table Segment found in the Header Partition or any partition listed in a Random Index "
        "Pack; opening the file would need a full-file bitstream scan (disable strict to open it anyway)";
    return result;
  }
  // No real SMPTE Index Table Segment at all (e.g. a truncated file with no
  // RIP/footer) -- try recovering an index straight from the essence's own
  // bitstream headers (mxf/bitstream_index.h). An unsupported codec or no
  // video elements at all leaves it unset.
  result.index = BuildEssenceIndexFromBitstream(reader, info);
  result.fromBitstreamScan = result.index && !result.index->Empty();
  return result;
}

int64_t DeclaredEditUnits(const MxfStreamInfo& info) {
  return info.sourcePackageDuration >= 0 ? info.sourcePackageDuration : info.numFrames;
}

std::optional<std::string> CheckIndexCoverage(const EssenceIndex& index, const MxfStreamInfo& info, bool finished) {
  const int64_t declared = DeclaredEditUnits(info);
  if (!finished || declared < 0) return std::nullopt;
  const int64_t covered = index.HighestCoveredEditUnit() + 1;
  if (covered >= declared) return std::nullopt;
  return "strict: the file is finalized (Closed Partition) and declares " + std::to_string(declared) +
         " edit units, but the Index Table covers only " + std::to_string(covered) +
         "; the file is incomplete or damaged (disable strict to open it anyway)";
}

EssenceEnd EssenceEndsAfter(MxfReader& reader, uint64_t lastEditUnitOffset,
                            const std::array<uint8_t, 4>& videoTrackNumber,
                            const std::vector<std::array<uint8_t, 4>>& audioTrackNumbers) {
  // One content package has a handful of elements (system item, picture,
  // up to 16 sound, data, fill); anything longer is not "right after".
  constexpr int kMaxKlvs = 64;
  const uint64_t size = reader.Size();
  uint64_t pos = lastEditUnitOffset;
  bool seenLastPicture = false;
  std::vector<bool> seenAudio(audioTrackNumbers.size(), false);
  auto ended = [&]() {
    EssenceEnd result;
    result.ends = true;
    result.lastPackageComplete =
        seenLastPicture && std::all_of(seenAudio.begin(), seenAudio.end(), [](bool seen) { return seen; });
    return result;
  };
  for (int i = 0; i < kMaxKlvs; ++i) {
    if (pos >= size) return ended();  // EOF
    if (!reader.Seek(pos)) return {};
    KlvHeader klv{};
    if (!ReadKlvHeader(reader, klv)) return pos + 17 > size ? ended() : EssenceEnd{};  // only a cut-off tail left
    if (!LooksLikeKlvKey(klv.key)) {
      // Raw zero fill instead of a Footer counts as the end; anything else
      // is unknown territory, so don't conclude anything.
      uint8_t probe[16] = {};
      const size_t probeLen = static_cast<size_t>(std::min<uint64_t>(sizeof(probe), size - pos));
      if (!reader.Seek(pos) || !reader.ReadBytes(probe, probeLen)) return {};
      const bool zeros = std::all_of(std::begin(probe), std::end(probe), [](uint8_t b) { return b == 0; });
      return zeros ? ended() : EssenceEnd{};
    }
    PartitionKind kind{};
    if (IsPartitionPackKey(klv.key, &kind)) return kind == PartitionKind::kFooter ? ended() : EssenceEnd{};
    if (UlEquals(klv.key, kRandomIndexPackKey)) return ended();
    const uint64_t valueEnd = klv.valueOffset + klv.length;
    if (IsEssenceElementKey(klv.key)) {
      if (std::memcmp(klv.key.data() + 12, videoTrackNumber.data(), 4) == 0) {
        if (seenLastPicture) return {};  // a further frame follows
        seenLastPicture = true;
      } else if (valueEnd <= size) {  // a sound element cut off at EOF doesn't count
        for (size_t t = 0; t < audioTrackNumbers.size(); ++t) {
          if (std::memcmp(klv.key.data() + 12, audioTrackNumbers[t].data(), 4) == 0) seenAudio[t] = true;
        }
      }
    }
    if (valueEnd >= size) return ended();  // the last element reaches (or is cut off at) EOF
    pos = valueEnd;
  }
  return {};
}

}  // namespace mxfsrc
