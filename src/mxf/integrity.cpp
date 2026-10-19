#include "mxf/integrity.h"

#include <algorithm>
#include <vector>

namespace mxfsrc {

namespace {

struct ValidatedPartition {
  PartitionPack pack;
  PartitionStatus status = PartitionStatus::kUnknown;
};

// Reads the Partition Pack at `offset` and accepts it only if its own
// ThisPartition field equals `offset` -- a pointer into essence or zero
// fill would have to reproduce both a Partition Pack key and its own
// absolute byte offset to pass.
std::optional<ValidatedPartition> ReadSelfReferencingPartitionPack(MxfReader& reader, uint64_t offset) {
  if (offset >= reader.Size() || !reader.Seek(offset)) return std::nullopt;
  KlvHeader klv{};
  if (!ReadKlvHeader(reader, klv)) return std::nullopt;
  if (!IsPartitionPackKey(klv.key, nullptr)) return std::nullopt;
  std::optional<PartitionPack> pack = ReadPartitionPackValue(reader, klv);
  if (!pack || pack->thisPartition != offset) return std::nullopt;
  return ValidatedPartition{*pack, ClassifyPartitionStatus(klv.key)};
}

bool IsClosedFooter(const std::optional<ValidatedPartition>& p) {
  return p && p->pack.kind == PartitionKind::kFooter && IsPartitionClosed(p->status);
}

// Reads [offset, offset+len) into `buf` and reports whether it is all zero.
// A failed read counts as "not all zero" so the probe never claims a zero
// tail it hasn't actually seen.
bool RangeIsAllZero(MxfReader& reader, uint64_t offset, size_t len, std::vector<uint8_t>& buf) {
  buf.resize(len);
  if (!reader.Seek(offset) || !reader.ReadBytes(buf.data(), len)) return false;
  return std::all_of(buf.begin(), buf.end(), [](uint8_t b) { return b == 0; });
}

// Measures the zero run at the end of the file in kMaxExpectedTrailingZeroBytes
// blocks, counted back from EOF: block i covers
// [size - (i+1)*B, size - i*B). Block 0 is already known to be zero. Binary
// search for the first non-zero block assumes the zeros are one contiguous
// run, which a zero-filled tail is; a zero block inside real data (e.g.
// silent clip-wrapped PCM) can only make the reported dataEnd too low,
// never turn a non-zero tail into a zero one -- the strict decision itself
// depends on block 0 alone.
void MeasureTrailingZeros(MxfReader& reader, FileIntegrity& out) {
  const uint64_t size = out.fileSize;
  const uint64_t block = kMaxExpectedTrailingZeroBytes;
  const uint64_t numBlocks = (size + block - 1) / block;
  std::vector<uint8_t> buf;
  auto blockStart = [&](uint64_t i) { return size > (i + 1) * block ? size - (i + 1) * block : 0; };
  auto blockIsZero = [&](uint64_t i) {
    const uint64_t start = blockStart(i);
    return RangeIsAllZero(reader, start, static_cast<size_t>(size - i * block - start), buf);
  };

  uint64_t lo = 0;          // known all-zero
  uint64_t hi = numBlocks;  // first non-zero block, or numBlocks if the whole file is zero
  while (hi - lo > 1) {
    const uint64_t mid = lo + (hi - lo) / 2;
    if (blockIsZero(mid)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  uint64_t dataEnd = 0;
  if (hi < numBlocks) {
    const uint64_t start = blockStart(hi);
    blockIsZero(hi);  // reload the non-zero block into buf
    auto lastNonZero = std::find_if(buf.rbegin(), buf.rend(), [](uint8_t b) { return b != 0; });
    dataEnd = start + static_cast<uint64_t>(buf.rend() - lastNonZero);
  }
  out.dataEnd = dataEnd;
  out.trailingZeroBytes = size - dataEnd;
}

}  // namespace

std::optional<FileIntegrity> ProbeFileIntegrity(MxfReader& reader) {
  FileIntegrity out;
  out.fileSize = reader.Size();
  out.dataEnd = out.fileSize;

  if (!reader.Seek(0)) return std::nullopt;
  KlvHeader headerKlv{};
  if (!ReadKlvHeader(reader, headerKlv)) return std::nullopt;
  PartitionKind kind{};
  if (!IsPartitionPackKey(headerKlv.key, &kind) || kind != PartitionKind::kHeader) return std::nullopt;
  std::optional<PartitionPack> header = ReadPartitionPackValue(reader, headerKlv);
  if (!header) return std::nullopt;

  out.headerStatus = ClassifyPartitionStatus(headerKlv.key);
  out.headerClosed = IsPartitionClosed(out.headerStatus);
  out.footerPartition = header->footerPartition;

  if (out.footerPartition != 0) {
    std::optional<ValidatedPartition> footer = ReadSelfReferencingPartitionPack(reader, out.footerPartition);
    out.footerValid = footer && footer->pack.kind == PartitionKind::kFooter;
    out.closedFooterFound = IsClosedFooter(footer);
  }

  if (auto rip = ReadRandomIndexPack(reader)) {
    if (!rip->empty() && rip->front().byteOffset == 0) {
      std::optional<ValidatedPartition> last = ReadSelfReferencingPartitionPack(reader, rip->back().byteOffset);
      out.ripValid = last.has_value();
      out.closedFooterFound = out.closedFooterFound || IsClosedFooter(last);
    }
  }

  const uint64_t tailLen = std::min<uint64_t>(out.fileSize, kMaxExpectedTrailingZeroBytes);
  std::vector<uint8_t> buf;
  if (tailLen > 0 && RangeIsAllZero(reader, out.fileSize - tailLen, static_cast<size_t>(tailLen), buf)) {
    out.trailingZerosUnexpected = true;
    MeasureTrailingZeros(reader, out);
  }
  return out;
}

std::optional<std::string> CheckStrictIntegrity(const FileIntegrity& integrity) {
  // Whatever the partition statuses say: a growing file preallocated with
  // zeros is as unusable for an automated pipeline as a broken one.
  if (integrity.trailingZerosUnexpected) {
    return "strict: the file ends with " + std::to_string(integrity.trailingZeroBytes) +
           " zero bytes (data ends at byte " + std::to_string(integrity.dataEnd) + " of " +
           std::to_string(integrity.fileSize) +
           "); the file is incomplete or damaged (disable strict to open it anyway)";
  }

  // A Closed header promises a Footer; an Open one (growing file) usually
  // doesn't know where it will be yet.
  if (integrity.headerClosed && !integrity.closedFooterFound && integrity.footerPartition != 0 &&
      !integrity.footerValid && !integrity.ripValid) {
    if (integrity.footerPartition >= integrity.fileSize) {
      return "strict: Header Partition is Closed and declares the Footer Partition at byte " +
             std::to_string(integrity.footerPartition) + ", but the file is only " +
             std::to_string(integrity.fileSize) +
             " bytes; the file is truncated (disable strict to open it anyway)";
    }
    return "strict: Header Partition is Closed and declares the Footer Partition at byte " +
           std::to_string(integrity.footerPartition) +
           ", but there is no valid Footer Partition Pack there and no Random Index Pack; the file is "
           "incomplete or damaged (disable strict to open it anyway)";
  }
  return std::nullopt;
}

}  // namespace mxfsrc
