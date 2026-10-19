#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"

namespace mxfsrc {

// How MxfSource (SourceEngine::Open) and MxfExtractIndex (core/
// index_extract.h) decide whether a file is usable and build its video
// essence index. Both go through these functions, in this order, so they
// accept and refuse exactly the same files and build exactly the same
// index: an index can never be published for a file that MxfSource with
// the same `strict` setting would refuse. See README "Strict mode" for the
// user-facing description of the checks.

// Strict checks 1 and 2 (mxf/integrity.h): unexpected zero tail, missing
// Footer behind a Closed header. Cheap, so run before any index work.
// Does nothing when `strict` is false.
struct IntegrityCheckResult {
  std::optional<std::string> error;  // set: refuse the file with this message
  // The writer declared the file finished (Closed Footer, or else Closed
  // header), so its Duration is final and the index must cover it (see
  // CheckIndexCoverage). Always false when `strict` is false.
  bool finished = false;
};
IntegrityCheckResult CheckIntegrityBeforeIndex(MxfReader& reader, bool strict);

// The video essence index: Index Table Segments from the Header Partition
// and every partition listed in the RIP -- fetched in parallel for a
// range-capable http(s):// source (BuildEssenceIndexFromHttp), otherwise
// read sequentially (BuildEssenceIndex); both give the same result. If there
// are none: with `strict`, strict check 3 fails; without, the full-file
// bitstream scan (BuildEssenceIndexFromBitstream) is tried.
struct VideoIndexResult {
  std::optional<EssenceIndex> index;  // nullopt or empty: no index at all
  bool fromBitstreamScan = false;
  std::optional<std::string> error;  // set: strict refusal
};
VideoIndexResult BuildVideoIndex(MxfReader& reader, const std::string& path, const std::string& httpHeaders,
                                 const MxfStreamInfo& info, bool strict);

// Edit units the file declares, in the coordinate space of the index: the
// SourcePackage Duration (which includes Origin pre-roll) or else the
// generic Duration. -1 if the Header Metadata declares none.
int64_t DeclaredEditUnits(const MxfStreamInfo& info);

// Strict check 4: for a finished file, the index must cover every declared
// edit unit. Does nothing unless `finished` (see IntegrityCheckResult) and
// the Duration is known.
std::optional<std::string> CheckIndexCoverage(const EssenceIndex& index, const MxfStreamInfo& info, bool finished);

}  // namespace mxfsrc
