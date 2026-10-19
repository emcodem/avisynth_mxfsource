#pragma once

#include <string>

namespace mxfsrc {

struct ExtractIndexResult {
  bool ok = false;
  std::string error;
};

// Backing implementation for the host-exposed MxfExtractIndex function:
// opens `sourcePath` (local path or http(s):// URL, `httpHeaders` applied
// only for the latter -- same rules as OpenOptions::http_headers), parses
// just enough Header Metadata to know the video BodySID, builds the essence
// index (mxf/index_table.h's BuildEssenceIndex), and publishes it as JSON to
// `targetUrl` (mxf/index_transport.h's PublishEssenceIndex -- local path or
// http(s):// URL). Deliberately lighter weight than SourceEngine::Open():
// no codec/decode-thread/ring-buffer setup, since none of that is needed
// just to extract and publish an index. Any failure (file open, header
// parse, no usable index for this file, or the publish itself) is reported
// via the returned `error`, never partially written.
//
// `strict` has exactly the meaning of OpenOptions::strict (default on, like
// there): the file is checked and its index built by the same functions
// SourceEngine::Open uses (core/index_policy.h), so a file MxfSource would
// refuse never gets an index published.
ExtractIndexResult ExtractAndPublishIndex(const std::string& sourcePath, const std::string& targetUrl,
                                           const std::string& httpHeaders = {}, bool strict = true);

}  // namespace mxfsrc
