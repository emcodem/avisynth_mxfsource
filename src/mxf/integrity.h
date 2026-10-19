#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "mxf/klv.h"
#include "mxf/partition.h"

namespace mxfsrc {

// A valid MXF file ends with a Random Index Pack or a Footer Partition,
// never with a long run of zero bytes. Raw-zero KAG padding seen on real
// recorders (Atomos Shogun, XDCAM) stays well below 1 KiB, so a tail block
// this large that is entirely zero means the file was preallocated/zero-
// filled and never finished (observed: XDCAM recordings with a zero-filled
// tail where the Footer Partition should be).
inline constexpr uint64_t kMaxExpectedTrailingZeroBytes = 64 * 1024;

// Read-only structural checks on an opened file, used by
// OpenOptions::strict. Costs a handful of small reads (Header Partition
// Pack, RIP, Footer Partition Pack, and ~log2(size/64 KiB) 64 KiB reads only
// when the tail is zero) and never changes how the file is parsed.
struct FileIntegrity {
  uint64_t fileSize = 0;

  // Header Partition Pack status (key byte 14). kUnknown (e.g. pre-2004
  // Version 1 files with 0x00 there) counts as not Closed.
  PartitionStatus headerStatus = PartitionStatus::kUnknown;
  bool headerClosed = false;

  // The Header Partition Pack's FooterPartition field, 0 if left unknown.
  uint64_t footerPartition = 0;
  // footerPartition != 0 and a Footer Partition Pack whose ThisPartition
  // equals its own offset sits there.
  bool footerValid = false;
  // A RIP parses at EOF, its first entry is offset 0 and its last entry
  // holds a Partition Pack whose ThisPartition equals that offset.
  bool ripValid = false;

  // Number of zero bytes at the end of the file, and where the non-zero data
  // ends (fileSize - trailingZeroBytes). Only measured when the last
  // kMaxExpectedTrailingZeroBytes are all zero; 0 / fileSize otherwise.
  uint64_t trailingZeroBytes = 0;
  uint64_t dataEnd = 0;
  bool trailingZerosUnexpected = false;
};

// Returns nullopt only if the Header Partition Pack itself can't be read
// (ParseMxfHeader already fails on such a file first).
std::optional<FileIntegrity> ProbeFileIntegrity(MxfReader& reader);

// The strict-mode checks that need nothing but FileIntegrity: an
// unexpected zero tail, or a Footer Partition the Closed header promises but
// that isn't there (with no RIP to say otherwise). Returns the error message
// for the first failing check, nullopt if the file passes. Never fails a file
// whose Header Partition is not Closed -- an Open header is a growing or
// aborted recording, expected to be incomplete.
std::optional<std::string> CheckStrictIntegrity(const FileIntegrity& integrity);

}  // namespace mxfsrc
