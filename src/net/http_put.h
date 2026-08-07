#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mxfsrc {

// One-shot, whole-body HTTP(S) PUT via libcurl. Unlike HttpByteSource (a
// persistent streaming range-GET reader built for reading MXF source data),
// this is a single blocking request that uploads `data` in one call --
// intended for publishing a small serialized essence index blob (see
// mxf/index_transport.h) to a presigned upload URL. Returns false and fills
// `errorOut` (if non-null) on any failure, network or non-2xx HTTP status.
bool HttpPutBlob(const std::string& url, const std::vector<uint8_t>& data, std::string* errorOut);

// One-shot, whole-body HTTP(S) GET via libcurl, buffering the entire
// response in memory -- the index blobs this is used for are at most a few
// tens of MB (see mxf/index_transport.h), so unlike HttpByteSource there's
// no need for streaming/backpressure. Returns std::nullopt and fills
// `errorOut` (if non-null) on any failure.
std::optional<std::vector<uint8_t>> HttpGetBlob(const std::string& url, std::string* errorOut);

}  // namespace mxfsrc
