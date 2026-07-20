#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "mxf/klv.h"

namespace mxfsrc {

struct Rational {
  int32_t num = 0;
  int32_t den = 1;
};

// Raw local-tag -> value-bytes map for one local-set KLV Value (Preface,
// Package, Track, Sequence, SourceClip, Descriptor, ...). Well-known
// baseline-register tags (e.g. 0x3203 StoredWidth) are read directly by
// numeric value in header_parser.cpp, matching FFmpeg's mxfdec.c approach:
// in practice virtually all real-world files use the SMPTE static local tag
// register for these properties rather than Primer-Pack-assigned dynamic
// tags, so resolving through the Primer Pack isn't required for them.
using LocalTagMap = std::map<uint16_t, std::vector<uint8_t>>;

// Parses {2-byte tag}{2-byte length}{value} triples until `length` bytes of
// the parent local-set KLV Value have been consumed.
bool ReadLocalSet(MxfReader& reader, uint64_t length, LocalTagMap& out);

// InstanceUID (baseline tag 0x3C0A) is present on every metadata set and is
// how strong/weak references (also raw 16-byte UL values) are resolved
// between sets -- see FFmpeg mxfdec.c mxf_read_local_tags().
bool GetInstanceUid(const LocalTagMap& set, Ul& out);

bool GetUl(const LocalTagMap& set, uint16_t tag, Ul& out);
bool GetU32(const LocalTagMap& set, uint16_t tag, uint32_t& out);
bool GetI64(const LocalTagMap& set, uint16_t tag, int64_t& out);
bool GetRational32(const LocalTagMap& set, uint16_t tag, Rational& out);

// A StrongReference/WeakReference batch: {4-byte count}{4-byte item size=16}
// followed by count 16-byte ULs.
std::vector<Ul> GetUlArray(const LocalTagMap& set, uint16_t tag);

}  // namespace mxfsrc
