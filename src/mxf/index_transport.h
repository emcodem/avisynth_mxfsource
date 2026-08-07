#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "mxf/index_table.h"

namespace mxfsrc {

// Publishes `index` as a JSON document (see mxf/index_serialization.h for
// the exact shape) to `target` -- an http:// or https:// URL (one-shot PUT,
// see net/http_put.h; intended for a presigned upload URL) or a local
// filesystem path (plain text file write), picked the same way MxfReader
// dispatches between HttpByteSource/FileByteSource for the source file
// itself (mxf/klv.h's IsHttpUrl). This is what the host-exposed
// MxfExtractIndex function calls. Returns false with a message in
// `errorOut` (if non-null) on any failure -- never silently swallowed.
bool PublishEssenceIndex(const std::string& target, const EssenceIndex& index, uint32_t bodySid,
                          std::string* errorOut);

// Fetches and validates an index published by PublishEssenceIndex from
// `source` (an http(s):// URL via one-shot GET, or a local path). Beyond the
// document's own version/bodySid checks (see DeserializeEssenceIndex), also
// rejects the index unless its last entry's byte offset is within
// kMaxTrailingBytesTolerance of `sourceFileSize` -- catches a slave applying
// an index built from a different or truncated copy of the file, while still
// tolerating the several-MB of repeated index tables large files can
// legitimately carry in their footer. Returns std::nullopt with a message in
// `errorOut` (if non-null) on any failure; callers (OpenOptions::index_url)
// must treat that as fatal, never fall back to a local rebuild.
std::optional<EssenceIndex> FetchEssenceIndex(const std::string& source, uint32_t expectedBodySid,
                                               uint64_t sourceFileSize, std::string* errorOut);

}  // namespace mxfsrc
