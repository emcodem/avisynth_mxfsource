// Unit tests for mxf/audio_sample_index.h's BuildAudioSampleIndexFromIndexTable
// -- the zero-I/O builder that derives the audio sample index directly from
// an already-parsed video EssenceIndex plus editRate/audioSampleRate, per
// this plugin's supported audio envelope (PCM only, no mid-file
// frame-rate/channel/track-config change, audio+video always sharing one
// BodySID covered by that BodySID's own Index Table). No synthetic MXF file
// is needed here -- the function touches no reader/file at all, so tests
// construct an EssenceIndex directly from hand-crafted FlatIndexEntry rows
// (same pattern index_table_test.cpp already uses).
#include <iostream>

#include "mxf/audio_sample_index.h"

using namespace mxfsrc;

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

std::vector<FlatIndexEntry> MakeContiguousEntries(int64_t count, uint64_t firstByteOffset = 1000,
                                                   uint64_t stride = 1000) {
  std::vector<FlatIndexEntry> entries;
  entries.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    FlatIndexEntry e;
    e.editUnit = i;
    e.byteOffset = firstByteOffset + static_cast<uint64_t>(i) * stride;
    e.isKeyframe = true;
    entries.push_back(e);
  }
  return entries;
}

// 25fps/48kHz divides evenly (1920 samples/edit-unit) -- the common case,
// where the closed-form cadence degenerates to a constant per-frame count.
void TestIntegerCadenceBuildsCorrectly() {
  EssenceIndex videoIndex(MakeContiguousEntries(3));

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/3);
  CHECK(index.has_value());
  if (!index) return;

  CHECK(index->NumEditUnits() == 3);
  CHECK(index->TotalSamples() == 3 * 1920);
  CHECK(index->SamplesInEditUnit(0) == 1920);
  CHECK(index->SamplesInEditUnit(1) == 1920);
  CHECK(index->SamplesInEditUnit(2) == 1920);
  CHECK(index->CumulativeSamplesBefore(0) == 0);
  CHECK(index->CumulativeSamplesBefore(1) == 1920);
  CHECK(index->CumulativeSamplesBefore(2) == 3840);
  CHECK(index->CumulativeSamplesBefore(3) == 5760);
  CHECK(index->ByteOffsetOf(0) == 1000);
  CHECK(index->ByteOffsetOf(1) == 2000);
  CHECK(index->ByteOffsetOf(2) == 3000);

  // Sample 2000 falls in edit unit 1 (samples [1920,3840)), offset 80 within it.
  auto loc = index->Locate(2000);
  CHECK(loc.has_value());
  if (loc) {
    CHECK(loc->editUnit == 1);
    CHECK(loc->sampleOffsetInEditUnit == 80);
    CHECK(loc->byteOffset == 2000);
  }

  CHECK(!index->Locate(5760).has_value());  // out of range (== TotalSamples())
  CHECK(!index->Locate(-1).has_value());
}

// editRate={30000,1001} (NTSC) @ 48kHz averages 1601.6 samples/edit-unit --
// the closed-form floor(i*sampleRate*den/num) formula must reproduce the
// exact non-uniform cadence real broadcast equipment uses, not a rounded
// constant. Regression guard for the rounding formula itself.
void TestNtscCadenceReproducesExpectedPattern() {
  EssenceIndex videoIndex(MakeContiguousEntries(5));

  MxfStreamInfo info;
  info.editRate = Rational{30000, 1001};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/5);
  CHECK(index.has_value());
  if (!index) return;

  // Hand-computed via floor(i*48000*1001/30000): cumulative = 0, 1601, 3203,
  // 4804, 6406, 8008 -> per-edit-unit deltas below.
  CHECK(index->SamplesInEditUnit(0) == 1601);
  CHECK(index->SamplesInEditUnit(1) == 1602);
  CHECK(index->SamplesInEditUnit(2) == 1601);
  CHECK(index->SamplesInEditUnit(3) == 1602);
  CHECK(index->SamplesInEditUnit(4) == 1602);
  CHECK(index->TotalSamples() == 8008);
}

// entries.size() != rawFrameCount (one edit unit missing entirely, e.g. a
// truncated/partial Index Table) must not silently build a wrong index.
void TestTooFewEntriesReturnsNullopt() {
  EssenceIndex videoIndex(MakeContiguousEntries(9));  // only 9 of 10 edit units covered

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/10);
  CHECK(!index.has_value());
}

// The index covers more edit units than rawFrameCount: real files carry
// essence past their SourcePackage Duration (observed: 628 indexed frames,
// Duration 615 with Origin 12, audio was dropped). Builds over
// [0, rawFrameCount) only.
void TestMoreEntriesThanRawFrameCountBuilds() {
  EssenceIndex videoIndex(MakeContiguousEntries(13));

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/10);
  CHECK(index.has_value());
  if (index) {
    CHECK(index->NumEditUnits() == 10);
    CHECK(index->TotalSamples() == 10 * 1920);
  }
}

// More entries than rawFrameCount, but with a gap inside [0, rawFrameCount)
// (edit unit 5 missing): still nullopt.
void TestMoreEntriesWithGapInsideRangeReturnsNullopt() {
  std::vector<FlatIndexEntry> entries = MakeContiguousEntries(13);
  entries.erase(entries.begin() + 5);
  EssenceIndex videoIndex(entries);

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  CHECK(!BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/10).has_value());
}

// Same entry count as rawFrameCount, but the covered range doesn't start at
// editUnit 0 -- e.g. an Index Table Segment for a later BodySID range or a
// dropped leading segment. Must not be mistaken for full coverage just
// because the counts match.
void TestEntriesNotStartingAtZeroReturnsNullopt() {
  std::vector<FlatIndexEntry> entries = MakeContiguousEntries(5);
  for (auto& e : entries) e.editUnit += 5;  // now covers editUnit 5..9, not 0..4

  EssenceIndex videoIndex(entries);

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/5);
  CHECK(!index.has_value());
}

void TestEmptyIndexReturnsNullopt() {
  EssenceIndex videoIndex;  // default-constructed, Empty() == true

  MxfStreamInfo info;
  info.editRate = Rational{25, 1};
  info.audioSampleRate = 48000;

  auto index = BuildAudioSampleIndexFromIndexTable(videoIndex, info, /*rawFrameCount=*/3);
  CHECK(!index.has_value());
}

// Defensive guards against malformed/absent rate metadata -- should never
// happen in practice (audioConfigSupported already validates this upstream)
// but must not divide by zero or silently build a bogus index.
void TestInvalidRateMetadataReturnsNullopt() {
  EssenceIndex videoIndex(MakeContiguousEntries(3));

  MxfStreamInfo zeroSampleRate;
  zeroSampleRate.editRate = Rational{25, 1};
  zeroSampleRate.audioSampleRate = 0;
  CHECK(!BuildAudioSampleIndexFromIndexTable(videoIndex, zeroSampleRate, 3).has_value());

  MxfStreamInfo zeroEditRateNum;
  zeroEditRateNum.editRate = Rational{0, 1};
  zeroEditRateNum.audioSampleRate = 48000;
  CHECK(!BuildAudioSampleIndexFromIndexTable(videoIndex, zeroEditRateNum, 3).has_value());
}

}  // namespace

int main() {
  TestIntegerCadenceBuildsCorrectly();
  TestNtscCadenceReproducesExpectedPattern();
  TestTooFewEntriesReturnsNullopt();
  TestMoreEntriesThanRawFrameCountBuilds();
  TestMoreEntriesWithGapInsideRangeReturnsNullopt();
  TestEntriesNotStartingAtZeroReturnsNullopt();
  TestEmptyIndexReturnsNullopt();
  TestInvalidRateMetadataReturnsNullopt();

  if (g_failures == 0) {
    std::cout << "all audio_sample_index tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
