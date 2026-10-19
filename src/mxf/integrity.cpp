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

}  // namespace

std::optional<FileIntegrity> ProbeFileIntegrity(MxfReader& reader) {
  FileIntegrity out;
  out.fileSize = reader.Size();

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
    out.zeroTailOffset = out.fileSize - tailLen;
  }
  return out;
}

std::optional<std::string> CheckStrictIntegrity(const FileIntegrity& integrity) {
  // Whatever the partition statuses say: a growing file preallocated with
  // zeros is as unusable for an automated pipeline as a broken one.
  if (integrity.trailingZerosUnexpected) {
    return "strict: unexpected zero bytes at offset " + std::to_string(integrity.zeroTailOffset) +
           " (verified from there to the end of the file at " + std::to_string(integrity.fileSize) +
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
