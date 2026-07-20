// Phase 3 unit tests for src/codec/. RawUyvyDecoder is tested directly
// against a synthetic buffer (no FFmpeg involved). AvcodecDecoder is tested
// via a genuine encode-then-decode round trip: since no real MXF/ES sample
// files are available in this environment, we use avcodec's own MPEG-2
// encoder (test-only, not part of the plugin) to produce a valid elementary
// stream frame, then feed it through our AvcodecDecoder to prove the
// send/receive wiring, pixel format mapping, and plane extraction actually
// work end to end against the vendored libavcodec.
#include <cmath>
#include <cstring>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include "codec/avcodec_decoder.h"
#include "codec/raw_unpack.h"

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

void TestRawUyvyDecoder() {
  const int width = 4, height = 2;
  std::vector<uint8_t> essence(static_cast<size_t>(width) * height * 2);
  for (size_t i = 0; i < essence.size(); ++i) essence[i] = static_cast<uint8_t>(i);

  RawUyvyDecoder decoder(width, height);
  CHECK(decoder.IsValid());

  NormalizedFrame out;
  CHECK(decoder.SendAndReceive(essence, out));
  CHECK(out.format == PixelFormat::kUyvy422);
  CHECK(out.width == width && out.height == height);
  CHECK(out.stride[0] == width * 2);
  CHECK(out.planeData[0] == essence);
}

void TestRawUyvyDecoderRejectsWrongSize() {
  RawUyvyDecoder decoder(4, 2);
  NormalizedFrame out;
  std::vector<uint8_t> tooShort(5);
  CHECK(!decoder.SendAndReceive(tooShort, out));
}

// Encodes one 16x16 solid-color YUV420P frame with avcodec's native MPEG-2
// encoder and returns the resulting elementary stream bytes for the first
// output packet (empty vector on failure).
std::vector<uint8_t> EncodeOneMpeg2Frame(int width, int height, uint8_t lumaValue) {
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
  if (!enc) return {};

  AVCodecContext* encCtx = avcodec_alloc_context3(enc);
  if (!encCtx) return {};
  encCtx->width = width;
  encCtx->height = height;
  encCtx->pix_fmt = AV_PIX_FMT_YUV420P;
  encCtx->time_base = {1, 25};
  encCtx->framerate = {25, 1};
  encCtx->gop_size = 1;
  encCtx->max_b_frames = 0;

  std::vector<uint8_t> result;
  if (avcodec_open2(encCtx, enc, nullptr) < 0) {
    avcodec_free_context(&encCtx);
    return result;
  }

  AVFrame* frame = av_frame_alloc();
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  if (av_frame_get_buffer(frame, 32) < 0) {
    av_frame_free(&frame);
    avcodec_free_context(&encCtx);
    return result;
  }
  std::memset(frame->data[0], lumaValue, static_cast<size_t>(frame->linesize[0]) * height);
  std::memset(frame->data[1], 128, static_cast<size_t>(frame->linesize[1]) * ((height + 1) / 2));
  std::memset(frame->data[2], 128, static_cast<size_t>(frame->linesize[2]) * ((height + 1) / 2));
  frame->pts = 0;

  AVPacket* pkt = av_packet_alloc();
  avcodec_send_frame(encCtx, frame);
  avcodec_send_frame(encCtx, nullptr);  // flush
  while (avcodec_receive_packet(encCtx, pkt) == 0) {
    if (result.empty()) result.assign(pkt->data, pkt->data + pkt->size);
    av_packet_unref(pkt);
  }

  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&encCtx);
  return result;
}

void TestAvcodecDecoderMpeg2RoundTrip() {
  const int width = 16, height = 16;
  const uint8_t lumaValue = 100;
  std::vector<uint8_t> encoded = EncodeOneMpeg2Frame(width, height, lumaValue);
  CHECK(!encoded.empty());
  if (encoded.empty()) return;

  AvcodecDecoder decoder(CodecFamily::kMpeg2Video, width, height);
  CHECK(decoder.IsValid());

  NormalizedFrame out;
  // MPEG-2 has an inherent one-frame decode delay (true even for a lone
  // I-frame), so the frame surfaces on the drain call, not the send call --
  // exactly the "keep feeding, then drain at end of stream" pattern
  // real playback will use once more than one element is fed.
  bool sent = decoder.SendAndReceive(encoded, out);
  bool drained = decoder.SendAndReceive({}, out);
  CHECK(sent || drained);
  CHECK(out.width == width);
  CHECK(out.height == height);
  CHECK(out.format == PixelFormat::kYuv420p8);
  CHECK(!out.planeData[0].empty());
  if (!out.planeData[0].empty()) {
    // Lossy MPEG-2 compression won't reproduce the exact input value; a
    // generous tolerance is enough to prove the decode pipeline (packet in,
    // correctly-shaped pixels out) actually works end to end.
    CHECK(std::abs(static_cast<int>(out.planeData[0][0]) - lumaValue) < 30);
  }
}

void TestAvcodecDecoderRejectsUnsupportedFamily() {
  AvcodecDecoder decoder(CodecFamily::kUnknown, 16, 16);
  CHECK(!decoder.IsValid());
}

}  // namespace

int main() {
  TestRawUyvyDecoder();
  TestRawUyvyDecoderRejectsWrongSize();
  TestAvcodecDecoderMpeg2RoundTrip();
  TestAvcodecDecoderRejectsUnsupportedFamily();

  if (g_failures == 0) {
    std::cout << "all codec decoder tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
