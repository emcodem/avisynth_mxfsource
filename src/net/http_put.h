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

// Bounded ranged GET: fetches [offset, offset+length) via libcurl into
// memory, in one blocking call. Unlike HttpByteSource (a persistent
// streaming reader, one connection per instance, meant to be driven
// sequentially by a single thread), this creates and tears down its own
// short-lived easy handle every call -- deliberately, so index_table.h's
// parallel HTTP index-extraction path can call this concurrently from many
// worker threads at once (each call's easy handle is only ever touched by
// the thread that made it, which is all libcurl requires for thread safety).
// `httpHeaders` is the same raw "Name: value\r\n..." block HttpByteSource
// accepts. Retries a couple of times on transient failures (connection
// resets, 5xx) with the same backoff policy as HttpByteSource, since a
// fan-out of many concurrent requests hitting the same endpoint is more
// likely to see transient blips than a single sequential stream is. Returns
// std::nullopt and fills `errorOut` (if non-null) on final failure.
std::optional<std::vector<uint8_t>> HttpGetRange(const std::string& url, const std::string& httpHeaders,
                                                  uint64_t offset, uint64_t length, std::string* errorOut);

}  // namespace mxfsrc
