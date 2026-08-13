#include "core/index_extract.h"

#include "mxf/bitstream_index.h"
#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/index_transport.h"
#include "mxf/klv.h"

namespace mxfsrc {

ExtractIndexResult ExtractAndPublishIndex(const std::string& sourcePath, const std::string& targetUrl,
                                           const std::string& httpHeaders) {
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
  if (!info->hasVideo) return {false, "no video track found in Header Metadata"};

  auto index = BuildEssenceIndex(reader, info->bodySid);
  if (!index || index->Empty()) {
    // No real SMPTE Index Table Segment (e.g. a truncated file with no
    // RIP/footer) -- fall back to walking the essence directly, see
    // mxf/bitstream_index.h for exactly what this can and can't recover.
    index = BuildEssenceIndexFromBitstream(reader, *info);
  }
  if (!index || index->Empty()) {
    return {false, "no usable Index Table Segment, and no recognizable bitstream keyframes found, for this "
                   "file's video BodySID " +
                       std::to_string(info->bodySid)};
  }

  std::string publishError;
  if (!PublishEssenceIndex(targetUrl, *index, info->bodySid, &publishError)) {
    return {false, "failed to publish index to '" + targetUrl + "': " + publishError};
  }
  return {true, ""};
}

}  // namespace mxfsrc
