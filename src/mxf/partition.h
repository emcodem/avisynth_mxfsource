#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

// Key/prefix values verified against FFmpeg's libavformat/mxfdec.c and
// mxf.c (see plan: "cite FFmpeg mxfdec.c as correctness reference"), which
// in turn implement SMPTE 377-1/ST 336.

enum class PartitionKind { kHeader, kBody, kFooter };

// Partition Pack status (byte[14] of the key, see below): whether this
// partition's Header Metadata copy is still being appended to (Open) or
// finished (Closed), and whether all mandatory properties have been
// written yet (Complete) or not (Incomplete). Per SMPTE 377-1, a Closed
// partition's metadata takes precedence over an Open one -- "growing
// file" recorders (e.g. Omneon) commonly leave the Header Partition Open
// & Incomplete (Duration/Origin genuinely unresolved yet) until the
// recording finishes, only writing final values into the redundant Header
// Metadata copy carried by the (Closed & Complete) Footer Partition. See
// header_parser.cpp's footer-precedence handling.
//
// Value order verified against a real Omneon recording (byte[14]=0x01 on
// its Header Partition, whose Duration/Origin were genuinely unresolved,
// vs. byte[14]=0x04 on its Footer Partition, which carried the final
// values) -- status progresses monotonically from "least settled" to
// "most settled" as the numeric value increases, not alphabetically.
enum class PartitionStatus { kOpenIncomplete, kOpenComplete, kClosedIncomplete, kClosedComplete, kUnknown };

inline bool IsPartitionClosed(PartitionStatus status) {
  return status == PartitionStatus::kClosedComplete || status == PartitionStatus::kClosedIncomplete;
}

// Partition Pack keys share a 13-byte prefix; byte[13] (0-indexed) selects
// Header(2)/Body(3)/Footer(4), byte[14] the open/closed/complete status.
inline constexpr uint8_t kPartitionPackKeyPrefix[13] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01,
                                                          0x01, 0x0d, 0x01, 0x02, 0x01, 0x01};

bool IsPartitionPackKey(const Ul& key, PartitionKind* kindOut);

// Classifies byte[14] of a key already confirmed via IsPartitionPackKey.
// Values 0x01-0x04 per SMPTE 377-1's Partition Pack status byte; anything
// else (including pre-2004 Version 1 files, which leave this reserved as
// 0x00) is reported as kUnknown -- callers should treat kUnknown as "not
// safely Closed" rather than erroring, since the rest of the Partition
// Pack is still valid.
PartitionStatus ClassifyPartitionStatus(const Ul& key);

inline constexpr Ul kPrimerPackKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                                       0x0d, 0x01, 0x02, 0x01, 0x01, 0x05, 0x01, 0x00};

inline constexpr Ul kRandomIndexPackKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                                            0x0d, 0x01, 0x02, 0x01, 0x01, 0x11, 0x01, 0x00};

inline constexpr Ul kKlvFillKey = {0x06, 0x0E, 0x2B, 0x34, 0x01, 0x01, 0x01, 0x02,
                                    0x03, 0x01, 0x02, 0x10, 0x01, 0x00, 0x00, 0x00};

// Header Metadata local-set keys (Preface, ContentStorage, Package, ...)
// share this 5-byte prefix followed by 0x53 ('S') at byte[5], which is how
// mxfdec.c's mxf_parse_klv() distinguishes local-set-coded sets from
// fixed-layout packs (Partition Pack, Primer Pack).
inline constexpr uint8_t kLocalSetKeyPrefix[6] = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x53};
bool IsLocalSetKey(const Ul& key);

// Generic Container element keys -- picture/sound/data items and the
// system item's local sets alike -- all carry 0d 01 03 01 in bytes 8-11
// (SMPTE 379), whatever their other bytes.
bool IsEssenceContainerElementKey(const Ul& key);

// Where a partition's HeaderByteCount and IndexByteCount start counting:
// the first KLV after the KLV Fill items that pad the Partition Pack to the
// KAG, i.e. normally the Primer Pack. Measured on every real writer checked
// (D-10/IMX, K2 Summit, ffmpeg, Telestream; Fill of 340-388 bytes): counting
// from the end of the Partition Pack instead put the declared end of the
// Header Metadata and the start of the Index Table that many bytes too
// early. Skips at most a handful of consecutive Fill items and never reads
// past `limit`; returns `partitionEnd` when no Fill follows (or nothing
// parses there, e.g. raw zero padding).
uint64_t PartitionContentStart(MxfReader& reader, uint64_t partitionEnd, uint64_t limit);

// Hard bounds for every walk that looks for the end of the Header Metadata,
// so finding it never turns into a KLV walk through the file: at most
// kMaxHeaderMetadataOverrun past the declared end, and never more than
// kMaxHeaderMetadataSize from the start of the metadata, whatever the
// declared end says (metadata beyond it is not read). The stop rules
// (EndsHeaderMetadata) normally end a walk long before either bound.
inline constexpr uint64_t kMaxHeaderMetadataOverrun = 1024 * 1024;
inline constexpr uint64_t kMaxHeaderMetadataSize = 16 * 1024 * 1024;

// True for a KLV that can't be Header Metadata: an Index Table Segment, a
// Partition Pack, the RIP, an essence container element, or bytes that
// aren't a KLV key at all.
bool EndsHeaderMetadata(const Ul& key);

// Offset of the first KLV at or after `start` for which EndsHeaderMetadata
// is true, never past min(`limit`, start + kMaxHeaderMetadataSize). Only
// reads KLV headers, so it costs one small read per metadata set.
uint64_t ScanHeaderMetadataEnd(MxfReader& reader, uint64_t start, uint64_t limit);

struct PartitionPack {
  PartitionKind kind = PartitionKind::kHeader;
  uint16_t majorVersion = 0;
  uint16_t minorVersion = 0;
  uint32_t kagSize = 0;
  uint64_t thisPartition = 0;
  uint64_t previousPartition = 0;
  uint64_t footerPartition = 0;  // byte offset of the Footer Partition Pack, 0 if unknown
  uint64_t headerByteCount = 0;
  uint64_t indexByteCount = 0;
  uint32_t indexSid = 0;
  uint64_t bodyOffset = 0;
  uint32_t bodySid = 0;
  Ul operationalPattern{};
  std::vector<Ul> essenceContainers;
};

// Parses a Partition Pack Value given its already-read KlvHeader (caller
// must have confirmed IsPartitionPackKey(klv.key) first). Leaves the reader
// positioned wherever it ends up mid-Value; caller should Seek to
// klv.valueOffset + klv.length afterwards to continue.
std::optional<PartitionPack> ReadPartitionPackValue(MxfReader& reader, const KlvHeader& klv);

struct RandomIndexPackEntry {
  uint32_t bodySid = 0;
  uint64_t byteOffset = 0;
};

// Reads the Random Index Pack (RIP), if present: a fixed-format (not
// local-set-coded) table of every partition's byte offset, always the very
// last structure in a valid MXF file. It's the one MXF construct explicitly
// designed to be locatable without already knowing any partition's byte
// offset in advance -- its own total pack length is duplicated as the last
// 4 bytes of the file, letting a reader seek backward exactly that many
// bytes from EOF to find its Key.
//
// This is the fallback for locating the Footer Partition when the Header
// Partition Pack's own FooterPartition field is left 0/unknown -- observed
// on a real Omneon "growing file" recording that apparently never
// back-fills that reference even once the file is complete. Per SMPTE
// 377-1 the Footer Partition is always the last partition in the file, so
// entries.back().byteOffset (if entries is non-empty) is its offset.
std::optional<std::vector<RandomIndexPackEntry>> ReadRandomIndexPack(MxfReader& reader);

}  // namespace mxfsrc
