#include "mxf/index_transport.h"

#include <cstdio>

#include "mxf/file_open.h"
#include "mxf/index_serialization.h"
#include "mxf/klv.h"
#include "net/http_put.h"

namespace mxfsrc {

namespace {

// See FetchEssenceIndex: how far short of the source file's actual size the
// last index entry's byte offset is allowed to fall, to tolerate footer
// index-table duplication in large files without masking a genuinely
// mismatched/truncated source.
constexpr uint64_t kMaxTrailingBytesTolerance = 50ull * 1024 * 1024;

bool WriteLocalText(const std::string& path, const std::string& text, std::string* errorOut) {
  FILE* out = OpenFile(path, "wb");
  if (!out) {
    if (errorOut) *errorOut = "failed to open '" + path + "' for writing";
    return false;
  }
  const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
  if (std::fclose(out) != 0 || !ok) {
    if (errorOut) *errorOut = "failed writing index JSON to '" + path + "'";
    return false;
  }
  return true;
}

std::optional<std::string> ReadLocalText(const std::string& path, std::string* errorOut) {
  FILE* in = OpenFile(path, "rb");
  if (!in) {
    if (errorOut) *errorOut = "failed to open '" + path + "' for reading";
    return std::nullopt;
  }
  std::string text;
  char buf[64 * 1024];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) text.append(buf, n);
  const bool failed = std::ferror(in) != 0;
  std::fclose(in);
  if (failed) {
    if (errorOut) *errorOut = "failed reading index JSON from '" + path + "'";
    return std::nullopt;
  }
  return text;
}

std::vector<uint8_t> ToBytes(const std::string& text) { return std::vector<uint8_t>(text.begin(), text.end()); }

std::string ToText(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

}  // namespace

bool PublishEssenceIndex(const std::string& target, const EssenceIndex& index, uint32_t bodySid,
                          std::string* errorOut) {
  const std::string json = SerializeEssenceIndex(index, bodySid);
  if (IsHttpUrl(target)) return HttpPutBlob(target, ToBytes(json), errorOut);
  return WriteLocalText(target, json, errorOut);
}

std::optional<EssenceIndex> FetchEssenceIndex(const std::string& source, uint32_t expectedBodySid,
                                               uint64_t sourceFileSize, std::string* errorOut) {
  std::optional<std::string> json;
  if (IsHttpUrl(source)) {
    std::optional<std::vector<uint8_t>> bytes = HttpGetBlob(source, errorOut);
    if (bytes) json = ToText(*bytes);
  } else {
    json = ReadLocalText(source, errorOut);
  }
  if (!json) return std::nullopt;

  std::optional<EssenceIndex> index = DeserializeEssenceIndex(*json, expectedBodySid, errorOut);
  if (!index) return std::nullopt;

  const std::vector<FlatIndexEntry>& entries = index->GetEntries();
  if (entries.empty()) {
    if (errorOut) *errorOut = "index JSON: contains no entries";
    return std::nullopt;
  }
  const uint64_t lastByteOffset = entries.back().byteOffset;
  if (lastByteOffset > sourceFileSize || sourceFileSize - lastByteOffset > kMaxTrailingBytesTolerance) {
    if (errorOut) {
      *errorOut = "index JSON: last entry's byte offset (" + std::to_string(lastByteOffset) +
                  ") is inconsistent with this source file's size (" + std::to_string(sourceFileSize) +
                  ") -- index likely built from a different or truncated copy of the file";
    }
    return std::nullopt;
  }
  return index;
}

}  // namespace mxfsrc
