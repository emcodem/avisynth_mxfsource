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

// D-10/IMX 8-Ch AES3 Element (SMPTE 331): 4-byte element header, then per
// sample frame 8 little-endian 32-bit slots with the sample in bits 4-27,
// channel number in bits 0-3 and V/U/C/P in 28-31. Words taken verbatim
// from the first sound element of the real SDPLAYERAPISUB.mxf (8ch 24-bit),
// with V/U/C/P set on two of them to show they are dropped.
void TestEightChAes3Element24BitUnpacked() {
  AudioTrackInfo track = MakeTrack(8, 24);
  track.eightChAes3Element = true;
  const uint32_t words[2][8] = {
      {0x00a93000, 0x00764001, 0x005ab002, 0x002a4003, 0x0021c804, 0x0045f205, 0x0005e606, 0x00014807},
      {0xf0a03000, 0x006e6001, 0x00529002, 0x00221003, 0x0025d804, 0x0048d905, 0x00044306, 0x1fff5a07},
  };
  EssenceElement element;
  element.data = {0x00, 0x02, 0x00, 0xff};  // flags, sample count 2 (LE), channel-valid mask
  for (const auto& frame : words) {
    for (uint32_t w : frame) {
      for (int b = 0; b < 4; ++b) element.data.push_back(static_cast<uint8_t>(w >> (8 * b)));
    }
  }

  AudioBlock out;
  std::string err;
  CHECK(InterleaveAudioEditUnit({element}, {track}, out, &err));
  CHECK(out.sampleCount == 2);
  CHECK(out.data.size() == 2 * 8 * 3);
  if (out.data.size() == 2 * 8 * 3) {
    for (int s = 0; s < 2; ++s) {
      for (int ch = 0; ch < 8; ++ch) {
        const uint32_t expected = (words[s][ch] >> 4) & 0xFFFFFF;
        const uint8_t* got = &out.data[static_cast<size_t>((s * 8 + ch) * 3)];
        CHECK((got[0] | (got[1] << 8) | (got[2] << 16)) == static_cast<int>(expected));
      }
    }
  }
}

// 16-bit 8-Ch AES3 Element: the sample is bits 12-27. Fewer channels than the 8 slots:
// only the first channelCount slots are kept.
void TestEightChAes3Element16BitKeepsFirstChannels() {
  AudioTrackInfo track = MakeTrack(2, 16);
  track.eightChAes3Element = true;
  EssenceElement element;
  element.data = {0x00, 0x01, 0x00, 0x03};
  const uint32_t frame[8] = {0x01234000, 0x0abcd001, 0x0fffff02, 0, 0, 0, 0, 0};
  for (uint32_t w : frame) {
    for (int b = 0; b < 4; ++b) element.data.push_back(static_cast<uint8_t>(w >> (8 * b)));
  }

  AudioBlock out;
  std::string err;
  CHECK(InterleaveAudioEditUnit({element}, {track}, out, &err));
  CHECK(out.sampleCount == 1);
  const std::vector<uint8_t> expected = {0x34, 0x12, 0xcd, 0xab};
  CHECK(out.data == expected);
}

// An 8-Ch AES3 Element that isn't 4 + n*32 bytes is malformed.
void TestEightChAes3ElementWithPartialFrameFails() {
  AudioTrackInfo track = MakeTrack(8, 24);
  track.eightChAes3Element = true;
  EssenceElement element;
  element.data.assign(4 + 32 + 5, 0);

  AudioBlock out;
  std::string err;
  CHECK(!InterleaveAudioEditUnit({element}, {track}, out, &err));
  CHECK(err.find("8-Ch AES3 Element") != std::string::npos);
}

int main() {
  TestEightChAes3Element24BitUnpacked();
  TestEightChAes3Element16BitKeepsFirstChannels();
  TestEightChAes3ElementWithPartialFrameFails();
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
