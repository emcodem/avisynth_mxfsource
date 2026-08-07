// Unit tests for src/mxf/index_serialization.* (wire format) and
// src/mxf/index_transport.* (local-file publish/fetch dispatch + the
// byte-offset-vs-file-size plausibility check) -- see mxf_source_engine's
// distributed-indexing feature: one instance builds an EssenceIndex and
// publishes it so other instances can fetch it instead of re-scanning the
// file themselves.
#include <cctype>
#include <filesystem>
#include <iostream>

#include "mxf/index_serialization.h"
#include "mxf/index_transport.h"
#include "synthetic_mxf.h"

using namespace mxfsrc;
using namespace mxfsrc_test;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

EssenceIndex MakeSampleIndex() {
  std::vector<FlatIndexEntry> entries;
  FlatIndexEntry a;
  a.editUnit = 0;
  a.byteOffset = 1000;
  a.isKeyframe = true;
  a.flags = 0x00;
  a.keyFrameOffset = 0;
  entries.push_back(a);
  FlatIndexEntry b;
  b.editUnit = 1;
  b.byteOffset = 2000;
  b.isKeyframe = false;
  b.flags = 0x33;
  b.keyFrameOffset = -1;
  entries.push_back(b);
  FlatIndexEntry c;
  c.editUnit = 2;
  c.byteOffset = 3000;
  c.isKeyframe = false;
  c.flags = 0x13;
  c.keyFrameOffset = -13;
  entries.push_back(c);
  return EssenceIndex(std::move(entries));
}

void TestSerializeDeserializeRoundTrip() {
  const EssenceIndex index = MakeSampleIndex();
  const std::string json = SerializeEssenceIndex(index, /*bodySid=*/7);

  std::string error;
  auto result = DeserializeEssenceIndex(json, /*expectedBodySid=*/7, &error);
  CHECK(result.has_value());
  if (!result) return;

  const auto& original = index.GetEntries();
  const auto& roundTripped = result->GetEntries();
  CHECK(roundTripped.size() == original.size());
  for (size_t i = 0; i < original.size() && i < roundTripped.size(); ++i) {
    CHECK(roundTripped[i].editUnit == original[i].editUnit);
    CHECK(roundTripped[i].byteOffset == original[i].byteOffset);
    CHECK(roundTripped[i].isKeyframe == original[i].isKeyframe);
    CHECK(roundTripped[i].flags == original[i].flags);
    CHECK(roundTripped[i].keyFrameOffset == original[i].keyFrameOffset);
  }
}

// Replaces a `"key": <number>` occurrence's number with `replacement` --
// used to corrupt a single field in an otherwise well-formed document
// without hand-building JSON text.
std::string ReplaceNumberField(std::string json, const std::string& key, const std::string& replacement) {
  const std::string needle = "\"" + key + "\": ";
  const size_t keyPos = json.find(needle);
  if (keyPos == std::string::npos) return json;
  const size_t valueStart = keyPos + needle.size();
  size_t valueEnd = valueStart;
  while (valueEnd < json.size() && (std::isdigit(static_cast<unsigned char>(json[valueEnd])) || json[valueEnd] == '-')) {
    ++valueEnd;
  }
  return json.substr(0, valueStart) + replacement + json.substr(valueEnd);
}

void TestDeserializeRejectsMalformedJson() {
  std::string json = SerializeEssenceIndex(MakeSampleIndex(), 7);
  json[0] = 'X';  // no longer starts with '{'
  std::string error;
  auto result = DeserializeEssenceIndex(json, 7, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

void TestDeserializeRejectsWrongVersion() {
  const std::string json = ReplaceNumberField(SerializeEssenceIndex(MakeSampleIndex(), 7), "version",
                                               std::to_string(kIndexBlobVersion + 1));
  std::string error;
  auto result = DeserializeEssenceIndex(json, 7, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

void TestDeserializeRejectsBodySidMismatch() {
  const std::string json = SerializeEssenceIndex(MakeSampleIndex(), 7);
  std::string error;
  auto result = DeserializeEssenceIndex(json, /*expectedBodySid=*/8, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

void TestDeserializeRejectsTruncatedJson() {
  std::string json = SerializeEssenceIndex(MakeSampleIndex(), 7);
  json.resize(json.size() - 10);  // cut off mid-document, no closing braces
  std::string error;
  auto result = DeserializeEssenceIndex(json, 7, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

void TestDeserializeRejectsMissingEntries() {
  const std::string json = "{\"version\": " + std::to_string(kIndexBlobVersion) + ", \"bodySid\": 7}";
  std::string error;
  auto result = DeserializeEssenceIndex(json, 7, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

// Extra/unrecognized fields (top-level and per-entry) must not break
// parsing -- forward-compat with a future version adding fields, guarded
// instead by the explicit version check.
void TestDeserializeToleratesUnknownFields() {
  const std::string json =
      "{\"version\": " + std::to_string(kIndexBlobVersion) +
      ", \"bodySid\": 7, \"generatedBy\": \"test\", \"entries\": ["
      "{\"editUnit\": 0, \"byteOffset\": 1000, \"isKeyframe\": true, \"flags\": 0, \"keyFrameOffset\": 0, "
      "\"note\": \"first\"}"
      "]}";
  std::string error;
  auto result = DeserializeEssenceIndex(json, 7, &error);
  CHECK(result.has_value());
  if (result) CHECK(result->GetEntries().size() == 1);
}

// End-to-end via the local-file transport path (no network needed): publish
// then fetch, with the fetch's own file-size plausibility check exercised
// against a size consistent with the sample index's last entry (byteOffset
// 3000).
void TestPublishFetchLocalFileRoundTrip() {
  const EssenceIndex index = MakeSampleIndex();
  const auto path = WriteTempFile({}, "mxfsrc_test_index_transport_roundtrip.bin");
  std::filesystem::remove(path);  // WriteTempFile only reserves the name; PublishEssenceIndex creates the real file

  std::string error;
  CHECK(PublishEssenceIndex(path.string(), index, /*bodySid=*/7, &error));

  auto fetched = FetchEssenceIndex(path.string(), /*expectedBodySid=*/7, /*sourceFileSize=*/3100, &error);
  CHECK(fetched.has_value());
  if (fetched) CHECK(fetched->GetEntries().size() == index.GetEntries().size());

  std::filesystem::remove(path);
}

// The plausibility check (see index_transport.cpp's kMaxTrailingBytesTolerance)
// must fail loud -- no fallback -- when the source file's actual size is
// wildly inconsistent with the index's last entry, e.g. a stale index
// fetched against a truncated or entirely different file.
void TestFetchRejectsImplausibleFileSize() {
  const EssenceIndex index = MakeSampleIndex();  // last entry byteOffset == 3000
  const auto path = WriteTempFile({}, "mxfsrc_test_index_transport_implausible.bin");
  std::filesystem::remove(path);

  std::string error;
  CHECK(PublishEssenceIndex(path.string(), index, 7, &error));

  // Source file reported as far smaller than the last entry's own offset.
  auto tooSmall = FetchEssenceIndex(path.string(), 7, /*sourceFileSize=*/100, &error);
  CHECK(!tooSmall.has_value());
  CHECK(!error.empty());

  // Source file far larger than the index's last entry plus tolerance.
  error.clear();
  auto tooLarge = FetchEssenceIndex(path.string(), 7, /*sourceFileSize=*/3000ull + 200ull * 1024 * 1024, &error);
  CHECK(!tooLarge.has_value());
  CHECK(!error.empty());

  std::filesystem::remove(path);
}

void TestFetchNonexistentSourceFails() {
  std::string error;
  auto result = FetchEssenceIndex("mxfsrc_test_index_transport_does_not_exist.bin", 7, 3100, &error);
  CHECK(!result.has_value());
  CHECK(!error.empty());
}

}  // namespace

int main() {
  TestSerializeDeserializeRoundTrip();
  TestDeserializeRejectsMalformedJson();
  TestDeserializeRejectsWrongVersion();
  TestDeserializeRejectsBodySidMismatch();
  TestDeserializeRejectsTruncatedJson();
  TestDeserializeRejectsMissingEntries();
  TestDeserializeToleratesUnknownFields();
  TestPublishFetchLocalFileRoundTrip();
  TestFetchRejectsImplausibleFileSize();
  TestFetchNonexistentSourceFails();

  if (g_failures == 0) {
    std::cout << "all mxf index_serialization/index_transport tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
