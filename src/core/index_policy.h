#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

// Whether the essence demonstrably ends with the edit unit whose content
// package starts at `lastEditUnitOffset` (the last index entry): walking on
// from there, the Footer Partition, the RIP, the end of the file or zero fill
// comes before any further picture element of `videoTrackNumber` or any
// Body/Header Partition. Then frames the Header Metadata declares beyond it
// don't exist (real case: Duration 714, 706 frames of essence, Footer) --
// unlike a file whose index merely stops early while its essence goes on.
// `lastPackageComplete`: that last content package also holds a complete
// element of every one of `audioTrackNumbers` (real case: a recording cut
// off in zero fill after its last picture and 5 of 8 sound elements).
// Costs the KLV header reads of about one content package.
struct EssenceEnd {
  bool ends = false;
  bool lastPackageComplete = false;
};
EssenceEnd EssenceEndsAfter(MxfReader& reader, uint64_t lastEditUnitOffset,
                            const std::array<uint8_t, 4>& videoTrackNumber,
                            const std::vector<std::array<uint8_t, 4>>& audioTrackNumbers);

// verify=true: checks every entry of `index` against the essence
// (core/index_verify.h). All entries fine: nothing changes. Bad entries:
// with `strict`, `error` is set (refuse the file / don't publish); without,
// the entries are repaired in `index` where the essence allows, and
// `warnings` says what was wrong and what could not be repaired. `summary`
// is an informational one-liner either way.
struct IndexVerification {
  std::optional<std::string> error;
  std::vector<std::string> warnings;
  std::string summary;
};
IndexVerification ApplyIndexVerification(const std::string& path, const std::string& httpHeaders,
                                         const MxfStreamInfo& info, bool strict, EssenceIndex& index);

}  // namespace mxfsrc
