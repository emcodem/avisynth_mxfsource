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
#include "mini_http_server.h"
#include "mxf/file_byte_source.h"
#include "mxf/range_byte_source.h"
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

// Coded-order picture type of edit unit i: a 12-frame GOP, I B B P B B P ...
char PictureTypeOf(int i) {
  if (i % 12 == 0) return 'I';
  return i % 3 == 0 ? 'P' : 'B';
}

// kEditUnits content packages, after a 64-byte KLV Fill so offset 0 is not an
// edit unit. Payload bytes never contain the UL designator. With
// `mpeg2Headers`, each picture starts with an MPEG-2 picture_header
// (00 00 01 00, temporal_reference, picture_coding_type I=1 P=2 B=3).
// `soundBefore` > 0: instead, three sound elements of that size between
// the System Item and the picture (as some writers order a package), and
// pictures of `pictureBytes`.
Stream BuildStream(bool mpeg2Headers = false, size_t soundBefore = 0, size_t pictureBytes = 0) {
  Stream s;
  AppendKlvHeader(s.bytes, {0x06, 0x0e, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x02, 0x03, 0x01, 0x02, 0x10, 0x01, 0, 0, 0},
                  64);
  s.bytes.resize(s.bytes.size() + 64, 0);
  for (int i = 0; i < kEditUnits; ++i) {
    s.systemItem.push_back(s.bytes.size());
    AppendKlvHeader(s.bytes, kSystemItemKey, 28);
    s.bytes.resize(s.bytes.size() + 28, 0x22);
    for (int t = 0; soundBefore > 0 && t < 3; ++t) {
      AppendEssenceElement(s.bytes, kAudioTrack + static_cast<uint32_t>(t), std::vector<uint8_t>(soundBefore, 0x33));
    }
    s.picture.push_back(s.bytes.size());
    std::vector<uint8_t> payload(pictureBytes > 0 ? pictureBytes : 1000 + 37 * (i % 5), 0x11);
    if (mpeg2Headers) {
      const char t = PictureTypeOf(i);
      const uint8_t codingType = t == 'I' ? 1 : t == 'P' ? 2 : 3;
      const uint8_t header[] = {0x00, 0x00, 0x01, 0x00, 0x00, static_cast<uint8_t>(codingType << 3), 0xff, 0xf8};
      std::copy(std::begin(header), std::end(header), payload.begin());
    }
    AppendEssenceElement(s.bytes, kVideoTrack, payload);
    if (soundBefore == 0) AppendEssenceElement(s.bytes, kAudioTrack, std::vector<uint8_t>(192, 0x33));
  }
  return s;
}

// Index flags as a real long-GOP Index Table carries them (I 0xc0, P 0x22,
// B 0x33) -- matching the GOP of BuildStream(true).
EssenceIndex IndexFrom(const std::vector<uint64_t>& offsets) {
  std::vector<FlatIndexEntry> entries;
  for (size_t i = 0; i < offsets.size(); ++i) {
    FlatIndexEntry e;
    e.editUnit = static_cast<int64_t>(i);
    e.byteOffset = offsets[i];
    const char t = PictureTypeOf(static_cast<int>(i));
    e.isKeyframe = t == 'I';
    e.flags = t == 'I' ? 0xc0 : t == 'P' ? 0x22 : 0x33;
    entries.push_back(e);
  }
  return EssenceIndex(std::move(entries));
}

MxfStreamInfo Mpeg2Info() {
  MxfStreamInfo info;
  info.hasVideo = true;
  info.videoTrackNumber = {0x15, 0x01, 0x05, 0x01};
  info.videoCodec = CodecFamily::kMpeg2Video;
  return info;
}

// Every test runs once per read mode (see main).
int g_overlapping = 0;
VerifyReads g_reads = VerifyReads::kAuto;

IndexVerifyResult Verify(const std::string& path, const EssenceIndex& index, const MxfStreamInfo& info) {
  return VerifyEssenceIndex(path, "", index, info, g_overlapping, g_reads);
}

// Entries first..last point one edit unit late (like the 3d mock).
std::vector<uint64_t> ShiftedByOneEditUnit(const std::vector<uint64_t>& original, int first, int last) {
  std::vector<uint64_t> wrong = original;
  for (int i = first; i <= last; ++i) wrong[i] = original[i + 1];
  return wrong;
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
    const IndexVerifyResult r = Verify(path.string(), IndexFrom(*offsets), Info());
    CHECK(!r.error);
    CHECK(r.checked == kEditUnits);
    CHECK(r.issues.empty());
    CHECK(r.essenceDamage.empty());
    CHECK(r.videoElementOffsets[5] == s.picture[5]);
    // At most one request per frame (fewer here: the frames are so small
    // that a fetch reaches into the following ones).
    if (g_reads == VerifyReads::kRanges) CHECK(r.readRequests > 0 && r.readRequests <= kEditUnits);
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
    const IndexVerifyResult r = Verify(path.string(), index, Info());
    CHECK(r.issues.size() == 5);
    CHECK(!r.issues.empty() && r.issues.front().editUnit == 12 && r.issues.back().editUnit == 16);
    CHECK(r.essenceDamage.empty());

    const IndexRepairResult repaired = RepairEssenceIndex(path.string(), "", index, Info(), r);
    CHECK(repaired.repaired == 5);
    CHECK(repaired.unrepaired.empty());
    CHECK(Offsets(repaired.index) == *original);
    CHECK(Verify(path.string(), repaired.index, Info()).issues.empty());
  }
  std::filesystem::remove(path);
}

// Damaged essence between two correct entries (like the 3b mock: the System
// Item key of edit unit 20 overwritten) is essence damage, not an index issue.
void TestEssenceDamageBetweenEntries() {
  Stream s = BuildStream();
  std::fill_n(s.bytes.begin() + static_cast<std::ptrdiff_t>(s.systemItem[20]), 16, 0xff);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_damage.mxf");
  const IndexVerifyResult r = Verify(path.string(), IndexFrom(s.picture), Info());
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
  const IndexVerifyResult r = Verify(path.string(), index, Info());
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
  const IndexVerifyResult r = Verify(path.string(), IndexFrom(wrong), Info());
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

// MPEG-2: the announced picture types match the stream -- no false alarms --
// and an index shifted by one edit unit is caught by the type check and
// repaired completely, including entries that pass by coincidence (B for B).
void TestPictureTypeCheckAndShiftRepair() {
  const Stream s = BuildStream(/*mpeg2Headers=*/true);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_types.mxf");

  const IndexVerifyResult clean = Verify(path.string(), IndexFrom(s.picture), Mpeg2Info());
  CHECK(clean.issues.empty());
  CHECK(clean.essenceDamage.empty());
  // The picture type check reads within the same request.
  if (g_reads == VerifyReads::kRanges) CHECK(clean.readRequests > 0 && clean.readRequests <= kEditUnits);

  const std::vector<uint64_t> wrong = ShiftedByOneEditUnit(s.picture, 12, 26);
  const EssenceIndex index = IndexFrom(wrong);
  const IndexVerifyResult r = Verify(path.string(), index, Mpeg2Info());
  CHECK(!r.issues.empty());
  bool typeMessage = false;
  for (const IndexVerifyIssue& issue : r.issues) typeMessage |= issue.what.find("the index announces") == 0;
  CHECK(typeMessage);
  CHECK(r.issues.size() < 16);  // some shifted entries pass by coincidence

  const IndexRepairResult repaired = RepairEssenceIndex(path.string(), "", index, Mpeg2Info(), r);
  CHECK(repaired.unrepaired.empty());
  CHECK(repaired.repairedUndetected > 0);
  CHECK(Offsets(repaired.index) == s.picture);
  std::filesystem::remove(path);
}

// Without a picture type parser (intra-only codec, unknown codec), a shift by
// whole edit units is still found at its start (the entry skips an edit unit)
// and end (offset does not increase), and repaired in between.
void TestShiftWithoutTypeParser() {
  const Stream s = BuildStream();
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_shift_units.mxf");
  const std::vector<uint64_t> wrong = ShiftedByOneEditUnit(s.picture, 12, 26);
  const EssenceIndex index = IndexFrom(wrong);
  const IndexVerifyResult r = Verify(path.string(), index, Info());
  bool skipAt12 = false;
  for (const IndexVerifyIssue& issue : r.issues) {
    skipAt12 |= issue.editUnit == 12 && issue.what.find("skips an edit unit") != std::string::npos;
  }
  CHECK(skipAt12);
  CHECK(r.essenceDamage.empty());

  const IndexRepairResult repaired = RepairEssenceIndex(path.string(), "", index, Info(), r);
  CHECK(repaired.unrepaired.empty());
  CHECK(Offsets(repaired.index) == s.picture);
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

// StreamingFileByteSource with blocks much smaller than the file: forward
// reads with gaps, reads across a block boundary, a jump back before the
// current block and a jump far ahead all return the file's bytes, and
// reading on through small gaps doesn't start extra blocks.
void TestStreamingFileByteSource() {
  std::vector<uint8_t> bytes(100000);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i * 131) ^ (i >> 8));
  const auto path = WriteTempFile(bytes, "mxfsrc_test_streaming_source.mxf");
  {
    StreamingFileByteSource source(path.string(), 4096);
    CHECK(source.IsOpen());
    CHECK(source.Size() == bytes.size());
    const auto readAt = [&](uint64_t pos, size_t n) {
      std::vector<uint8_t> got(n);
      return source.Seek(pos) && source.ReadBytes(got.data(), n) &&
             std::equal(got.begin(), got.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos));
    };
    for (uint64_t pos = 10; pos + 20 < 40000; pos += 1000) CHECK(readAt(pos, 20));  // gaps < 1 block
    CHECK(source.BlocksRead() == 10);  // bytes 10..40969, block after block
    CHECK(readAt(40950, 300));         // across a block boundary
    CHECK(readAt(5, 100));             // back before the current block
    CHECK(readAt(90000, 10000));       // far ahead, up to the very end
    std::vector<uint8_t> past(2);
    CHECK(!(source.Seek(bytes.size() - 1) && source.ReadBytes(past.data(), 2)));  // past the end
    MxfReader reader(std::make_unique<StreamingFileByteSource>(path.string(), 4096));
    CHECK(reader.IsOpen());
  }
  std::filesystem::remove(path);
}

// RangeByteSource: a prefetched range serves every read inside it with no
// further request; a read outside fetches kMissBytes from there; reads
// return the file's bytes either way.
void TestRangeByteSource() {
  std::vector<uint8_t> bytes(300000);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i * 7) ^ (i >> 9));
  const auto path = WriteTempFile(bytes, "mxfsrc_test_range_source.mxf");
  {
    RangeByteSource source(OpenRangeByteSource(path.string(), ""));
    CHECK(source.IsOpen());
    const auto readAt = [&](uint64_t pos, size_t n) {
      std::vector<uint8_t> got(n);
      return source.Seek(pos) && source.ReadBytes(got.data(), n) &&
             std::equal(got.begin(), got.end(), bytes.begin() + static_cast<std::ptrdiff_t>(pos));
    };
    source.Prefetch(1000, 21000);
    CHECK(source.Requests() == 1);
    CHECK(readAt(1000, 16) && readAt(5000, 1024) && readAt(20990, 10) && readAt(1500, 100));
    source.Prefetch(2000, 3000);  // already buffered
    CHECK(source.Requests() == 1);
    CHECK(readAt(100000, 20));  // miss: one request of kMissBytes
    CHECK(readAt(100000 + RangeByteSource::kMissBytes - 10, 10));
    CHECK(source.Requests() == 2);
    CHECK(readAt(299990, 10));  // up to the very end
    std::vector<uint8_t> past(2);
    CHECK(!(source.Seek(bytes.size() - 1) && source.ReadBytes(past.data(), 2)));
  }
  std::filesystem::remove(path);
}

bool SameFindings(const IndexVerifyResult& a, const IndexVerifyResult& b) {
  const auto same = [](const std::vector<IndexVerifyIssue>& x, const std::vector<IndexVerifyIssue>& y) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) {
      if (x[i].editUnit != y[i].editUnit || x[i].byteOffset != y[i].byteOffset || x[i].what != y[i].what) return false;
    }
    return true;
  };
  return !a.error && !b.error && same(a.issues, b.issues) && same(a.essenceDamage, b.essenceDamage) &&
         a.videoElementOffsets == b.videoElementOffsets;
}

// Over http(s): the same findings as for the local file, with one range
// request per frame at most (plus the size probe of each reader), also
// with several requests in flight; and still right from a server that
// ignores Range (every read then streams from the start).
void TestOverHttp() {
  Stream s = BuildStream(/*mpeg2Headers=*/true);
  std::fill_n(s.bytes.begin() + static_cast<std::ptrdiff_t>(s.systemItem[20]), 16, 0xff);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_http.mxf");
  MiniHttpServer server(s.bytes);
  const std::string url = server.BaseUrl() + "/file.mxf";
  for (const std::vector<uint64_t>& offsets :
       {s.picture, ShiftedByOneEditUnit(s.picture, 4, 12), s.systemItem}) {
    const EssenceIndex index = IndexFrom(offsets);
    const IndexVerifyResult local = VerifyEssenceIndex(path.string(), "", index, Mpeg2Info(), 1, VerifyReads::kRanges);
    CHECK(local.essenceDamage.size() == 1 || !local.issues.empty());
    for (const int overlapping : {1, 4, 0}) {
      const int before = server.RequestCount();
      const IndexVerifyResult remote = VerifyEssenceIndex(url, "", index, Mpeg2Info(), overlapping);
      CHECK(SameFindings(local, remote));
      // One reader here (40 entries are one run): its size probe plus at
      // most one request per frame.
      CHECK(server.RequestCount() - before <= kEditUnits + 1);
    }
  }
  server.SetRangeSupport(false);
  const EssenceIndex index = IndexFrom(s.picture);
  CHECK(SameFindings(VerifyEssenceIndex(path.string(), "", index, Mpeg2Info(), 1, VerifyReads::kRanges),
                     VerifyEssenceIndex(url, "", index, Mpeg2Info(), 1)));
  std::filesystem::remove(path);
}

// Sound elements before the picture, together larger than the minimum entry
// head, and frames larger than a miss read: still one request per frame
// after the first (the head is learned from it), with the same findings.
void TestSoundBeforePicture() {
  const Stream s = BuildStream(/*mpeg2Headers=*/true, /*soundBefore=*/3000, /*pictureBytes=*/100000);
  const auto path = WriteTempFile(s.bytes, "mxfsrc_test_verify_sound_first.mxf");
  for (const auto* offsets : {&s.systemItem, &s.picture}) {
    const IndexVerifyResult r =
        VerifyEssenceIndex(path.string(), "", IndexFrom(*offsets), Mpeg2Info(), 1, VerifyReads::kRanges);
    CHECK(!r.error && r.issues.empty() && r.essenceDamage.empty());
    CHECK(r.readRequests <= kEditUnits + 1);
  }
  const EssenceIndex shifted = IndexFrom(ShiftedByOneEditUnit(s.systemItem, 10, 20));
  const IndexVerifyResult streamed =
      VerifyEssenceIndex(path.string(), "", shifted, Mpeg2Info(), 1, VerifyReads::kStreaming);
  CHECK(!streamed.issues.empty());
  CHECK(SameFindings(streamed, VerifyEssenceIndex(path.string(), "", shifted, Mpeg2Info(), 4, VerifyReads::kRanges)));
  std::filesystem::remove(path);
}

void RunVerifyTests() {
  TestCleanBothLayouts();
  TestShiftedRunIsRepaired();
  TestEssenceDamageBetweenEntries();
  TestUnrepairable();
  TestNonIncreasingOffset();
  TestPictureTypeCheckAndShiftRepair();
  TestShiftWithoutTypeParser();
}

}  // namespace

int main() {
  const std::pair<int, VerifyReads> modes[] = {{1, VerifyReads::kRanges},
                                               {4, VerifyReads::kRanges},
                                               {1, VerifyReads::kStreaming},
                                               {0, VerifyReads::kAuto}};
  for (const auto& [overlapping, reads] : modes) {
    g_overlapping = overlapping;
    g_reads = reads;
    RunVerifyTests();
  }
  TestRangeByteSource();
  TestOverHttp();
  TestSoundBeforePicture();
  TestApplyIndexVerificationPolicy();
  TestFormatListsFirstTen();
  TestStreamingFileByteSource();
  if (g_failures == 0) std::cout << "index_verify_test: all passed" << std::endl;
  return g_failures == 0 ? 0 : 1;
}
