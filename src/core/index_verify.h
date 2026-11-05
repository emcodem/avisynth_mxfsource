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
};

// Checks every entry: at its byteOffset a KLV must start, and walking on from
// there (headers only, no resync) the first picture element of the video
// track must come before the next entry's offset (else: `issues`). Offsets
// must increase and lie inside the file. From that picture element on, the
// KLV headers must chain without damage exactly to the next entry's offset
// (else: `essenceDamage`). Costs a few small reads per entry; entries are
// checked on several threads, each with its own reader (one range request
// per read over http(s), so this is slow on remote files).
IndexVerifyResult VerifyEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info);

struct IndexRepairResult {
  EssenceIndex index;  // all entries, wrong ones replaced where possible
  int64_t repaired = 0;
  std::vector<IndexVerifyIssue> unrepaired;  // left unchanged, with the reason
};

// Repairs each run of bad entries from the nearest preceding good entry:
// walks the essence from there (headers only, no resync -- a guess past
// damage could miscount edit units and produce a wrong offset), takes the
// picture element of each bad edit unit, and derives the entry offset with
// the same layout as that good entry. Each repaired offset is checked again
// like VerifyEssenceIndex does. Entries behind damaged essence, or with no
// good entry before them, stay unchanged and are listed in `unrepaired`.
IndexRepairResult RepairEssenceIndex(const std::string& path, const std::string& httpHeaders,
                                     const EssenceIndex& index, const MxfStreamInfo& info,
                                     const IndexVerifyResult& verified);

// "edit unit 646 at byte 193954816: no KLV key there (found 3a1f...); ..."
// for the first `maxListed` issues, plus "and N more".
std::string FormatVerifyIssues(const std::vector<IndexVerifyIssue>& issues, size_t maxListed = 10);

}  // namespace mxfsrc
