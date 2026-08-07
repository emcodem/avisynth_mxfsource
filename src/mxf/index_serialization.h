#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "mxf/index_table.h"

namespace mxfsrc {

// Current wire-format version, carried as the JSON document's own "version"
// field. Bump this whenever FlatIndexEntry's fields or their meaning change
// -- DeserializeEssenceIndex hard-rejects any other version rather than
// attempting to interpret it, since a slave silently misreading a stale/
// future format would corrupt seeks instead of failing visibly.
inline constexpr uint8_t kIndexBlobVersion = 1;

// Serializes `index` (as built by BuildEssenceIndex for `bodySid`) into a
// JSON document for cross-process index sharing -- see
// mxf/index_transport.h for how the document actually gets from one
// instance to another (MxfExtractIndex writes it; MxfSource's `indexurl`
// option reads it back). Human-readable/inspectable by design, per the
// user's own call over a packed binary format:
//   {"version": 1, "bodySid": 7, "entries": [{"editUnit": 0,
//   "byteOffset": 1000, "isKeyframe": true, "flags": 0,
//   "keyFrameOffset": 0}, ...]}
std::string SerializeEssenceIndex(const EssenceIndex& index, uint32_t bodySid);

// Inverse of SerializeEssenceIndex. Fails (returns std::nullopt with a
// message in `errorOut`, if non-null) on malformed JSON, a missing/wrong
// "version", a "bodySid" mismatch against `expectedBodySid`, or a missing/
// malformed "entries" array -- deliberately no partial/best-effort recovery,
// since a wrong index applied silently would corrupt seeks rather than fail
// visibly. Tolerant of unrecognized extra fields/keys (forward-compat) and
// of entries/top-level keys appearing in any order, since JSON doesn't
// guarantee either -- this is a small parser scoped exactly to this
// document shape, not a general-purpose JSON library.
std::optional<EssenceIndex> DeserializeEssenceIndex(const std::string& json, uint32_t expectedBodySid,
                                                     std::string* errorOut);

}  // namespace mxfsrc
