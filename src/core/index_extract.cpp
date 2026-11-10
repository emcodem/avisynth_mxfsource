#include "core/index_extract.h"

#include <cstdio>

#include "core/index_policy.h"
#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/index_transport.h"
#include "mxf/klv.h"

namespace mxfsrc {

ExtractIndexResult ExtractAndPublishIndex(const std::string& sourcePath, const std::string& targetUrl,
                                           const std::string& httpHeaders, bool strict, bool verify,
                                           int verifyOverlapping) {
  MxfReader reader(sourcePath, httpHeaders);
  if (!reader.IsOpen()) {
    std::string error = "failed to open file: " + sourcePath;
    const std::string detail = reader.LastError();
    if (!detail.empty()) error += " (" + detail + ")";
    return {false, error};
  }

  std::string parseError;
  auto info = ParseMxfHeader(reader, &parseError);
  if (!info) {
    const std::string detail = reader.LastError();
    if (!detail.empty()) parseError += " (" + detail + ")";
    return {false, parseError};
  }

  // Exactly the checks and index build SourceEngine::Open uses
  // (core/index_policy.h), so an index is only ever published for a file
  // MxfSource with the same `strict` setting would open.
  const IntegrityCheckResult integrity = CheckIntegrityBeforeIndex(reader, strict);
  if (integrity.error) return {false, *integrity.error};
  if (!info->hasVideo) return {false, "no video track found in Header Metadata"};

  VideoIndexResult built = BuildVideoIndex(reader, sourcePath, httpHeaders, *info, strict);
  if (built.error) return {false, *built.error};
  if (!built.index || built.index->Empty()) {
    return {false, "no usable Index Table Segment, and no recognizable bitstream keyframes found, for this "
                   "file's video BodySID " +
                       std::to_string(info->bodySid)};
  }
  EssenceIndex& index = *built.index;
  if (std::optional<std::string> error = CheckIndexCoverage(index, *info, integrity.finished)) {
    return {false, *error};
  }
  if (verify) {
    const IndexVerification verification = ApplyIndexVerification(sourcePath, httpHeaders, *info, strict, index, verifyOverlapping);
    if (verification.error) return {false, *verification.error};
    for (const std::string& warning : verification.warnings) {
      std::fprintf(stderr, "[MxfExtractIndex] %s: %s\n", sourcePath.c_str(), warning.c_str());
    }
    std::fflush(stderr);
  }

  std::string publishError;
  if (!PublishEssenceIndex(targetUrl, index, info->bodySid, &publishError)) {
    return {false, "failed to publish index to '" + targetUrl + "': " + publishError};
  }
  return {true, ""};
}

}  // namespace mxfsrc
