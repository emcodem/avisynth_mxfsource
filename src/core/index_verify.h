#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mxf/header_parser.h"
#include "mxf/index_table.h"

namespace mxfsrc {

// verify=true (MxfSource and MxfExtractIndex): checks that every entry of a
// video essence index really points at its edit unit in this file, and
// repairs wrong entries from the essence itself. Neither the Index Table nor
// a fetched index (indexurl=) is otherwise compared against the essence, so
// a wrong StreamOffset only shows up later as a failing or wrong seek.
// Policy (strict refuses, non-strict repairs) lives in core/index_policy.h's
// ApplyIndexVerification; this file only checks and repairs.

struct IndexVerifyIssue {
  int64_t editUnit = 0;
  uint64_t byteOffset = 0;  // the entry's offset as found in the index
  std::string what;         // what is wrong there, for the log/error message
  size_t entryIndex = 0;    // position in EssenceIndex::GetEntries()
};

struct IndexVerifyResult {
  // Set when the check could not run at all (file could not be opened).
  std::optional<std::string> error;
  int64_t checked = 0;
  // Every bad entry, ascending by edit unit. Index problems: repairable.
  std::vector<IndexVerifyIssue> issues;
  // Damaged essence between an entry's picture element and the next entry
  // (the entries themselves are right): no KLV key where the previous value
  // ends, or a KLV running past the next entry's offset. Not repairable by
  // fixing the index; the edit unit named is the one before the damage.
  std::vector<IndexVerifyIssue> essenceDamage;
  // Per entry (same order as GetEntries()): file offset of the video
  // element the entry leads to, 0 for a bad entry. Used by the repair to
  // keep the layout of the good entries (an entry may point at the content
  // package's System Item or straight at the picture element).
  std::vector<uint64_t> videoElementOffsets;
  // Read requests made to the file (blocks with kStreaming), all readers.
  uint64_t readRequests = 0;
};

// Checks every entry: at its byteOffset a KLV must start, and walking on from
// there (headers only, no resync) the first picture element of the video
// track must come before the next entry's offset (else: `issues`). Offsets
// must increase and lie inside the file. From that picture element on, the
// KLV headers must chain without damage exactly to the next entry's offset
// (else: `essenceDamage`). One pass: each entry and the stretch up to the next
// one are checked together, in file order.
//
// One request per frame: everything the checks need between the end of one
// picture and the start of the next (the stretch to the next entry, that
// entry's KLV headers, the first bytes of its picture) is contiguous and
// fetched in one piece (RangeByteSource); the picture data itself is not
// read.
//
// `overlapping`: requests in flight at once -- that many threads, each with
// its own reader and its own runs of entries. 0 = auto: 16 over http(s), 4 on
// a network share or a local drive without seek penalty (IsLocalSolidState),
// 1 on any other local drive.
//
// `reads`: kRanges reads as above. kStreaming reads the whole file front to
// back in large blocks with one reader (StreamingFileByteSource): on a hard
// disk, rotating past the picture data costs about as much as reading it,
// and the drive streams. kAuto: kStreaming for a local file on a drive with
// seek penalty (or one that doesn't say) when `overlapping` comes to 1,
// kRanges otherwise.
enum class VerifyReads { kAuto, kRanges, kStreaming };

// Upper bound for `overlapping`.
inline constexpr int kMaxVerifyOverlapping = 64;

IndexVerifyResult VerifyEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info, int overlapping = 0,
                                     VerifyReads reads = VerifyReads::kAuto);

struct IndexRepairResult {
  EssenceIndex index;  // all entries, wrong ones replaced where possible
  int64_t repaired = 0;
  // Of `repaired`: entries that passed the checks but led to another edit
  // unit (inside an index shifted by whole edit units, by coincidence).
  int64_t repairedUndetected = 0;
  std::vector<IndexVerifyIssue> unrepaired;  // left unchanged, with the reason
};

// Repairs each problem region from the trusted entry before it: walks the
// essence from there (headers only, no resync -- a guess past damage could
// miscount edit units and produce a wrong offset) and compares every entry
// with the walked picture element of its edit unit -- also entries that
// passed the checks -- until 32 entries in a row agree. A wrong entry gets
// the offset with the same layout as the anchor entry, checked again like
// VerifyEssenceIndex does. Entries behind damaged essence, or with no valid
// entry before them, stay unchanged and are listed in `unrepaired`.
IndexRepairResult RepairEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info,
                                     const IndexVerifyResult& verified);

// "edit unit 646 at byte 193954816: no KLV key there (found 3a1f...); ..."
// for the first `maxListed` issues, plus "and N more".
std::string FormatVerifyIssues(const std::vector<IndexVerifyIssue>& issues, size_t maxListed = 10);

}  // namespace mxfsrc
