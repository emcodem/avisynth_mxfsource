// Unit tests for src/core/index_verify.* and ApplyIndexVerification
// (core/index_policy.h): verify=true's check of every index entry against the
// essence, the essence-damage chain check between entries, and the repair of
// wrong entries from the essence. Synthetic content packages (System Item,
// picture element, sound element) in both index layouts real writers use:
// entries pointing at the picture element, or at the package's System Item.
#include <filesystem>
#include <iostream>

#include "core/index_policy.h"
#include "core/index_verify.h"
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

constexpr uint32_t kVideoTrack = 0x15010501;
constexpr uint32_t kAudioTrack = 0x16010101;
constexpr int kEditUnits = 40;
const Ul kSystemItemKey = {0x06, 0x0e, 0x2b, 0x34, 0x02, 0x05, 0x01, 0x01,
                           0x0d, 0x01, 0x03, 0x01, 0x04, 0x01, 0x01, 0x00};

struct Stream {
  std::vector<uint8_t> bytes;
  std::vector<uint64_t> systemItem;  // per edit unit: offset of its System Item
  std::vector<uint64_t> picture;     // per edit unit: offset of its picture element
};

// kEditUnits content packages, after a 64-byte KLV Fill so offset 0 is not an
// edit unit. Payload bytes never contain the UL designator.
Stream BuildStream() {
  Stream s;
  AppendKlvHeader(s.bytes, {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01, 0x02, 0x10, 0x01, 0, 0, 0},
                  64);
  s.bytes.resize(s.bytes.size() + 64, 0);
  for (int i = 0; i < kEditUnits; ++i) {
    s.systemItem.push_back(s.bytes.size());
    AppendKlvHeader(s.bytes, kSystemItemKey, 28);
    s.bytes.resize(s.bytes.size() + 28, 0x22);
    s.picture.push_back(s.bytes.size());
    AppendEssenceElement(s.bytes, kVideoTrack, std::vector<uint8_t>(1000 + 37 * (i % 5), 0x11));
    AppendEssenceElement(s.bytes, kAudioTrack, std::vector<uint8_t>(192, 0x33));
  }
  return s;
}

EssenceIndex IndexFrom(const std::vector<uint64_t>& offsets) {
  std::vector<FlatIndexEntry> entries;
  for (size_t i = 0; i < offsets.size(); ++i) {
    FlatIndexEntry e;
    e.editUnit = static_cast<int64_t>(i);
    e.byteOffset = offsets[i];
    e.isKeyframe = i % 10 == 0;
    e.flags = e.isKeyframe ? 0xc0 : 0x33;
    entries.push_back(e);
  }
  return EssenceIndex(std::move(entries));
}

MxfStreamInfo Info() {
  MxfStreamInfo info;
  info.hasVideo = true;
  info.videoTrackNumber = {0x15, 0x01, 0x05, 0x01};
  return info;
}

std::vector<uint64_t> Offsets(const EssenceIndex& index) {
  std::vector<uint64_t> out;
  for (const FlatIndexEntry& e : index.GetEntries()) out.push_back(e.byteOffset);
  return out;
}

void TestCleanBothLayouts() {
  const Stream s = BuildStream();
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_clean.mxf");
  for (const auto* offsets : {&s.picture, &s.systemItem}) {
    const IndexVerifyResult r = VerifyEssenceIndex(path.string(), "", IndexFrom(*offsets), Info());
    CHECK(!r.error);
    CHECK(r.checked == kEditUnits);
    CHECK(r.issues.empty());
    CHECK(r.essenceDamage.empty());
    CHECK(r.videoElementOffsets[5] == s.picture[5]);
  }
  std::filesystem::remove(path);
}

// A run of shifted entries (like the 3c mock) is found and repaired back to
// the exact original offsets, in both layouts.
void TestShiftedRunIsRepaired() {
  const Stream s = BuildStream();
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_shift.mxf");
  for (const auto* original : {&s.picture, &s.systemItem}) {
    std::vector<uint64_t> wrong = *original;
    for (int i = 12; i <= 16; ++i) wrong[i] += 100;  // inside the picture payload
    const EssenceIndex index = IndexFrom(wrong);
    const IndexVerifyResult r = VerifyEssenceIndex(path.string(), "", index, Info());
    CHECK(r.issues.size() == 5);
    CHECK(!r.issues.empty() && r.issues.front().editUnit == 12 && r.issues.back().editUnit == 16);
    CHECK(r.essenceDamage.empty());

    const IndexRepairResult repaired = RepairEssenceIndex(path.string(), "", index, Info(), r);
    CHECK(repaired.repaired == 5);
    CHECK(repaired.unrepaired.empty());
    CHECK(Offsets(repaired.index) == *original);
    CHECK(VerifyEssenceIndex(path.string(), "", repaired.index, Info()).issues.empty());
  }
  std::filesystem::remove(path);
}

// Damaged essence between two correct entries (like the 3b mock: the System
// Item key of edit unit 20 overwritten) is essence damage, not an index issue.
void TestEssenceDamageBetweenEntries() {
  Stream s = BuildStream();
  std::fill_n(s.bytes.begin() + static_cast<std::ptrdiff_t>(s.systemItem[20]), 16, 0xff);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_damage.mxf");
  const IndexVerifyResult r = VerifyEssenceIndex(path.string(), "", IndexFrom(s.picture), Info());
  CHECK(r.issues.empty());
  CHECK(r.essenceDamage.size() == 1);
  CHECK(!r.essenceDamage.empty() && r.essenceDamage[0].editUnit == 19);
  std::filesystem::remove(path);
}

// Bad entries behind damaged essence can't be repaired (no resync guessing),
// and neither can a bad first entry (nothing before it to walk from).
void TestUnrepairable() {
  Stream s = BuildStream();
  std::fill_n(s.bytes.begin() + static_cast<std::ptrdiff_t>(s.systemItem[20]), 16, 0xff);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_unrepairable.mxf");
  std::vector<uint64_t> wrong = s.picture;
  wrong[0] += 100;
  for (int i = 20; i <= 22; ++i) wrong[i] += 100;
  const EssenceIndex index = IndexFrom(wrong);
  const IndexVerifyResult r = VerifyEssenceIndex(path.string(), "", index, Info());
  CHECK(r.issues.size() == 4);
  const IndexRepairResult repaired = RepairEssenceIndex(path.string(), "", index, Info(), r);
  CHECK(repaired.repaired == 0);
  CHECK(repaired.unrepaired.size() == 4);
  CHECK(Offsets(repaired.index) == wrong);  // left unchanged
  std::filesystem::remove(path);
}

void TestNonIncreasingOffset() {
  const Stream s = BuildStream();
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_order.mxf");
  std::vector<uint64_t> wrong = s.picture;
  wrong[8] = wrong[7];
  const IndexVerifyResult r = VerifyEssenceIndex(path.string(), "", IndexFrom(wrong), Info());
  bool flagged8 = false;
  for (const IndexVerifyIssue& issue : r.issues) flagged8 |= issue.editUnit == 8;
  CHECK(flagged8);
  std::filesystem::remove(path);
}

// Policy: strict refuses and leaves the index alone, non-strict repairs.
void TestApplyIndexVerificationPolicy() {
  const Stream s = BuildStream();
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_policy.mxf");
  std::vector<uint64_t> wrong = s.picture;
  wrong[30] += 100;

  EssenceIndex strictIndex = IndexFrom(wrong);
  const IndexVerification strict = ApplyIndexVerification(path.string(), "", Info(), true, strictIndex);
  CHECK(strict.error.has_value());
  CHECK(strict.error && strict.error->find("edit unit 30") != std::string::npos);
  CHECK(Offsets(strictIndex) == wrong);

  EssenceIndex lenientIndex = IndexFrom(wrong);
  const IndexVerification lenient = ApplyIndexVerification(path.string(), "", Info(), false, lenientIndex);
  CHECK(!lenient.error);
  CHECK(!lenient.warnings.empty());
  CHECK(Offsets(lenientIndex) == s.picture);

  EssenceIndex cleanIndex = IndexFrom(s.picture);
  const IndexVerification clean = ApplyIndexVerification(path.string(), "", Info(), true, cleanIndex);
  CHECK(!clean.error);
  CHECK(clean.warnings.empty());
  std::filesystem::remove(path);
}

void TestFormatListsFirstTen() {
  std::vector<IndexVerifyIssue> issues;
  for (int i = 0; i < 13; ++i) issues.push_back({i, 1000u + i, "x", static_cast<size_t>(i)});
  const std::string s = FormatVerifyIssues(issues);
  CHECK(s.find("edit unit 9 at byte 1009") != std::string::npos);
  CHECK(s.find("edit unit 10 ") == std::string::npos);
  CHECK(s.find("and 3 more") != std::string::npos);
}

}  // namespace

int main() {
  TestCleanBothLayouts();
  TestShiftedRunIsRepaired();
  TestEssenceDamageBetweenEntries();
  TestUnrepairable();
  TestNonIncreasingOffset();
  TestApplyIndexVerificationPolicy();
  TestFormatListsFirstTen();
  if (g_failures == 0) std::cout << "index_verify_test: all passed" << std::endl;
  return g_failures == 0 ? 0 : 1;
}
