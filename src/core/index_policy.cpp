#include "core/index_policy.h"

#include <vector>

#include "mxf/bitstream_index.h"
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

}  // namespace mxfsrc
