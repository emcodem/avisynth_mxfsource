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
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

#include "codec/avcodec_decoder.h"
#include "codec/decoder_factory.h"
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

// Encodes one DNxHR 444 frame (yuv444p10, flat grey) with avcodec's native
// DNxHD/DNxHR encoder; empty on failure. FFmpeg has no 12-bit DNxHR or
// ProRes encoder, so yuv422p12/yuv444p12 can only be covered by real files.
std::vector<uint8_t> EncodeOneDnxhr444Frame(int width, int height, uint16_t lumaValue) {
  const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_DNXHD);
  if (!enc) return {};
  AVCodecContext* encCtx = avcodec_alloc_context3(enc);
  if (!encCtx) return {};
  encCtx->width = width;
  encCtx->height = height;
  encCtx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
  av_opt_set(encCtx->priv_data, "profile", "dnxhr_444", 0);  // the encoder's private option, not encCtx->profile
  encCtx->time_base = {1, 25};
  encCtx->framerate = {25, 1};

  std::vector<uint8_t> result;
  AVFrame* frame = av_frame_alloc();
  AVPacket* pkt = av_packet_alloc();
  if (avcodec_open2(encCtx, enc, nullptr) == 0) {
    frame->format = AV_PIX_FMT_YUV444P10LE;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) == 0) {
      for (int p = 0; p < 3; ++p) {
        const uint16_t value = p == 0 ? lumaValue : 512;
        for (int y = 0; y < height; ++y) {
          auto* row = reinterpret_cast<uint16_t*>(frame->data[p] + static_cast<ptrdiff_t>(y) * frame->linesize[p]);
          for (int x = 0; x < width; ++x) row[x] = value;
        }
      }
      frame->pts = 0;
      avcodec_send_frame(encCtx, frame);
      avcodec_send_frame(encCtx, nullptr);
      while (avcodec_receive_packet(encCtx, pkt) == 0) {
        if (result.empty()) result.assign(pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
      }
    }
  }
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&encCtx);
  return result;
}

// 4:4:4 10-bit decodes to kYuv444p10 with full-height, full-width chroma
// planes (real case: DNxHR 444 10-bit UHD files, which failed to open with
// "decoded pixel format has no AviSynth+ mapping yet").
void TestAvcodecDecoderDnxhr444Yields444p10() {
  const int width = 256, height = 128;
  std::vector<uint8_t> encoded = EncodeOneDnxhr444Frame(width, height, 400);
  CHECK(!encoded.empty());
  if (encoded.empty()) return;

  AvcodecDecoder decoder(CodecFamily::kDnxhd, width, height);
  CHECK(decoder.IsValid());
  NormalizedFrame out;
  const bool sent = decoder.SendAndReceive(encoded, out);
  const bool drained = sent || decoder.SendAndReceive({}, out);
  CHECK(drained);
  CHECK(out.format == PixelFormat::kYuv444p10);
  CHECK(out.width == width && out.height == height);
  for (int p = 0; p < 3; ++p) {
    CHECK(out.planeHeight[p] == height);
    CHECK(out.stride[p] >= width * 2);
  }
  if (!out.planeData[0].empty()) {
    const uint16_t y0 = static_cast<uint16_t>(out.planeData[0][0] | (out.planeData[0][1] << 8));
    CHECK(std::abs(static_cast<int>(y0) - 400) < 8);
  }
}

// Encodes one ProRes 4444 frame (yuv444p10 input, flat grey) with
// avcodec's prores_ks encoder; empty on failure.
std::vector<uint8_t> EncodeOneProres4444Frame(int width, int height) {
  const AVCodec* enc = avcodec_find_encoder_by_name("prores_ks");
  if (!enc) return {};
  AVCodecContext* encCtx = avcodec_alloc_context3(enc);
  if (!encCtx) return {};
  encCtx->width = width;
  encCtx->height = height;
  encCtx->pix_fmt = AV_PIX_FMT_YUV444P10LE;
  encCtx->time_base = {1, 25};
  av_opt_set(encCtx->priv_data, "profile", "4444", 0);

  std::vector<uint8_t> result;
  AVFrame* frame = av_frame_alloc();
  AVPacket* pkt = av_packet_alloc();
  if (avcodec_open2(encCtx, enc, nullptr) == 0) {
    frame->format = AV_PIX_FMT_YUV444P10LE;
    frame->width = width;
    frame->height = height;
    if (av_frame_get_buffer(frame, 32) == 0) {
      for (int p = 0; p < 3; ++p) {
        for (int y = 0; y < height; ++y) {
          auto* row = reinterpret_cast<uint16_t*>(frame->data[p] + static_cast<ptrdiff_t>(y) * frame->linesize[p]);
          for (int x = 0; x < width; ++x) row[x] = p == 0 ? 400 : 512;
        }
      }
      frame->pts = 0;
      avcodec_send_frame(encCtx, frame);
      avcodec_send_frame(encCtx, nullptr);
      while (avcodec_receive_packet(encCtx, pkt) == 0) {
        if (result.empty()) result.assign(pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
      }
    }
  }
  av_packet_free(&pkt);
  av_frame_free(&frame);
  avcodec_free_context(&encCtx);
  return result;
}

// ProRes 4444 decodes at 12 bit only when the decoder knows the profile via
// the codec tag ('ap4h'), which comes from the PictureEssenceCoding UL (real
// case: an Adobe Premiere ProRes 4444 XQ MXF came out as yuv444p10 instead
// of ffmpeg's yuv444p12). Without a tag FFmpeg falls back to 10 bit.
void TestProres4444DecodesAt12BitWithCodecTag() {
  const int width = 64, height = 32;
  std::vector<uint8_t> encoded = EncodeOneProres4444Frame(width, height);
  CHECK(!encoded.empty());
  if (encoded.empty()) return;

  const Ul prores4444Ul = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D,
                           0x04, 0x01, 0x02, 0x02, 0x03, 0x06, 0x05, 0x00};
  for (bool withTag : {true, false}) {
    AvcodecDecoder decoder(CodecFamily::kProres, width, height, withTag ? ProresCodecTag(prores4444Ul) : 0);
    CHECK(decoder.IsValid());
    NormalizedFrame out;
    const bool got = decoder.SendAndReceive(encoded, out) || decoder.SendAndReceive({}, out);
    CHECK(got);
    CHECK(out.format == (withTag ? PixelFormat::kYuv444p12 : PixelFormat::kYuv444p10));
    for (int p = 0; p < 3; ++p) CHECK(out.planeHeight[p] == height);
  }
}

// RDD 44 profile byte -> FFmpeg codec tag, as FFmpeg's MXF demuxer maps it.
void TestProresCodecTagMapping() {
  Ul ul = {0x06, 0x0E, 0x2B, 0x34, 0x04, 0x01, 0x01, 0x0D, 0x04, 0x01, 0x02, 0x02, 0x03, 0x06, 0x00, 0x00};
  const char* expected[] = {"apco", "apcs", "apcn", "apch", "ap4h", "ap4x"};
  for (uint8_t profile = 1; profile <= 6; ++profile) {
    ul[14] = profile;
    const uint32_t tag = ProresCodecTag(ul);
    const char got[5] = {static_cast<char>(tag), static_cast<char>(tag >> 8), static_cast<char>(tag >> 16),
                         static_cast<char>(tag >> 24), 0};
    CHECK(std::string(got) == expected[profile - 1]);
  }
  ul[14] = 0x07;
  CHECK(ProresCodecTag(ul) == 0);
  ul[13] = 0x09;  // FFV1 family, not ProRes
  ul[14] = 0x05;
  CHECK(ProresCodecTag(ul) == 0);
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
  TestAvcodecDecoderDnxhr444Yields444p10();
  TestProres4444DecodesAt12BitWithCodecTag();
  TestProresCodecTagMapping();
  TestAvcodecDecoderRejectsUnsupportedFamily();

  if (g_failures == 0) {
    std::cout << "all codec decoder tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " check(s) failed" << std::endl;
  return 1;
}
