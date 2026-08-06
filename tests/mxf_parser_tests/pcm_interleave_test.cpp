// Unit tests for codec/pcm_interleave.h's InterleaveAudioEditUnit -- the
// byte-exact repack of one edit unit's independently-read-per-track PCM
// essence elements into a single interleaved AudioBlock (see
// SourceEngine::DecodeThreadMain/FreshReadAudioBlock, the two callers).
#include <iostream>

#include "codec/pcm_interleave.h"

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

AudioTrackInfo MakeTrack(uint32_t channels, uint32_t bits) {
  AudioTrackInfo t;
  t.channelCount = channels;
  t.bitsPerSample = bits;
  t.codec = CodecFamily::kPcm;
  return t;
}

// Single track, single channel: interleaving is a pure passthrough --
// output bytes must equal the input bytes exactly.
void TestSingleTrackIsByteIdenticalPassthrough() {
  std::vector<AudioTrackInfo> tracks = {MakeTrack(1, 16)};
  EssenceElement element;
  element.data = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};  // 3 samples, 2 bytes each

  AudioBlock out;
  std::string err;
  CHECK(InterleaveAudioEditUnit({element}, tracks, out, &err));
  CHECK(out.sampleCount == 3);
  CHECK(out.data == element.data);
}

// Two tracks (track0: 1ch 16-bit, track1: 2ch 16-bit) interleave as
// [t0ch0, t1ch0, t1ch1] per sample frame, in track order.
void TestTwoTracksInterleaveInTrackOrder() {
  std::vector<AudioTrackInfo> tracks = {MakeTrack(1, 16), MakeTrack(2, 16)};

  EssenceElement e0;
  e0.data = {0xA0, 0xA1, 0xA2, 0xA3};  // track0: 2 samples, 1ch, 2 bytes/sample
  EssenceElement e1;
  e1.data = {0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7};  // track1: 2 samples, 2ch, 2 bytes/sample

  AudioBlock out;
  std::string err;
  CHECK(InterleaveAudioEditUnit({e0, e1}, tracks, out, &err));
  CHECK(out.sampleCount == 2);
  const std::vector<uint8_t> expected = {
      0xA0, 0xA1, 0xB0, 0xB1, 0xB2, 0xB3,  // sample 0: t0ch0, t1ch0, t1ch1
      0xA2, 0xA3, 0xB4, 0xB5, 0xB6, 0xB7,  // sample 1: t0ch0, t1ch0, t1ch1
  };
  CHECK(out.data == expected);
}

// A track's element byte length that isn't an exact multiple of its own
// frame size (channelCount*bytesPerSample) is malformed input -- must fail,
// not silently truncate.
void TestElementLengthNotMultipleOfFrameSizeFails() {
  std::vector<AudioTrackInfo> tracks = {MakeTrack(2, 16)};  // 4 bytes/sample-frame
  EssenceElement element;
  element.data = {0x01, 0x02, 0x03};  // 3 bytes -- not a multiple of 4

  AudioBlock out;
  std::string err;
  CHECK(!InterleaveAudioEditUnit({element}, tracks, out, &err));
  CHECK(!err.empty());
}

// Two tracks whose elements imply different sample counts for the same edit
// unit -- a real synchronized recording should never produce this; must
// fail rather than silently produce a truncated/misaligned block.
void TestDisagreeingSampleCountsAcrossTracksFails() {
  std::vector<AudioTrackInfo> tracks = {MakeTrack(1, 16), MakeTrack(1, 16)};
  EssenceElement e0;
  e0.data = {0x01, 0x02, 0x03, 0x04};  // 2 samples
  EssenceElement e1;
  e1.data = {0x05, 0x06};  // 1 sample

  AudioBlock out;
  std::string err;
  CHECK(!InterleaveAudioEditUnit({e0, e1}, tracks, out, &err));
  CHECK(!err.empty());
}

// A mismatch between the number of elements handed in and the number of
// tracks configured is also malformed -- must fail cleanly.
void TestElementCountMismatchFails() {
  std::vector<AudioTrackInfo> tracks = {MakeTrack(1, 16), MakeTrack(1, 16)};
  EssenceElement e0;
  e0.data = {0x01, 0x02};

  AudioBlock out;
  std::string err;
  CHECK(!InterleaveAudioEditUnit({e0}, tracks, out, &err));
  CHECK(!err.empty());
}

}  // namespace

int main() {
  TestSingleTrackIsByteIdenticalPassthrough();
  TestTwoTracksInterleaveInTrackOrder();
  TestElementLengthNotMultipleOfFrameSizeFails();
  TestDisagreeingSampleCountsAcrossTracksFails();
  TestElementCountMismatchFails();

  if (g_failures == 0) {
    std::cout << "all pcm_interleave tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
