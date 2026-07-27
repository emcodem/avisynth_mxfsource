#include "codec/avcodec_decoder.h"

#include <cstddef>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace mxfsrc {

namespace {

AVCodecID ToAvCodecId(CodecFamily family) {
  switch (family) {
    case CodecFamily::kMpeg2Video: return AV_CODEC_ID_MPEG2VIDEO;
    case CodecFamily::kMpeg4: return AV_CODEC_ID_MPEG4;
    case CodecFamily::kDvVideo: return AV_CODEC_ID_DVVIDEO;
    case CodecFamily::kJpeg2000: return AV_CODEC_ID_JPEG2000;
    case CodecFamily::kDnxhd: return AV_CODEC_ID_DNXHD;
    case CodecFamily::kH264: return AV_CODEC_ID_H264;
    case CodecFamily::kH264LongGop: return AV_CODEC_ID_H264;
    case CodecFamily::kProres: return AV_CODEC_ID_PRORES;
    case CodecFamily::kFfv1: return AV_CODEC_ID_FFV1;
    default: return AV_CODEC_ID_NONE;
  }
}

PixelFormat FromAvPixFmt(int fmt) {
  switch (fmt) {
    case AV_PIX_FMT_YUV420P: return PixelFormat::kYuv420p8;
    case AV_PIX_FMT_YUV422P: return PixelFormat::kYuv422p8;
    case AV_PIX_FMT_YUV420P10LE: return PixelFormat::kYuv420p10;
    case AV_PIX_FMT_YUV422P10LE: return PixelFormat::kYuv422p10;
    default: return PixelFormat::kUnknown;
  }
}

int NumPlanes(PixelFormat f) { return f == PixelFormat::kUyvy422 || f == PixelFormat::kUnknown ? 1 : 3; }

// Chroma subsampling matches the plane's byte width/height to what avcodec
// actually allocated (frame->linesize[plane], not a hand-derived stride) --
// only the plane *height* needs deriving here since 4:2:0 halves it.
int ChromaHeight(PixelFormat f, int lumaHeight) {
  return (f == PixelFormat::kYuv420p8 || f == PixelFormat::kYuv420p10) ? (lumaHeight + 1) / 2 : lumaHeight;
}

}  // namespace

struct AvcodecDecoder::Impl {
  AVCodecContext* ctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* packet = nullptr;

  ~Impl() {
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
    if (ctx) avcodec_free_context(&ctx);
  }
};

AvcodecDecoder::AvcodecDecoder(CodecFamily family, int width, int height) : impl_(std::make_unique<Impl>()) {
  const AVCodecID id = ToAvCodecId(family);
  if (id == AV_CODEC_ID_NONE) return;
  const AVCodec* codec = avcodec_find_decoder(id);
  if (!codec) return;

  impl_->ctx = avcodec_alloc_context3(codec);
  if (!impl_->ctx) return;
  impl_->ctx->width = width;
  impl_->ctx->height = height;
  // Without this, AVCodecContext defaults to single-threaded decode --
  // ffmpeg's own CLI opens codecs with auto-detected frame+slice threading
  // (its default "-threads 0"), and skipping that here cost roughly an
  // order of magnitude of decode throughput on real GOP-capable codecs
  // (measured: ~94 fps vs ~254 fps on the same real-world MPEG-2 MXF file).
  // thread_count=0 asks libavcodec to auto-detect based on available cores
  // -- deliberately not hand-tuned to a specific number: manual trials with
  // fixed values (8/12/16/24 on a 24-logical-processor machine, content
  // with 8 slices/frame) landed in a noisy ~214-254fps band with no clean
  // trend, i.e. within measurement noise of each other, so there was no
  // reliable basis to override libavcodec's own well-tested auto-detection
  // (the same convention ffmpeg's CLI itself relies on). FF_THREAD_FRAME
  // adds up to thread_count frames of decode latency, which source_engine's
  // existing feed/drain loop already tolerates (it just keeps feeding
  // essence until a frame comes out, and drains explicitly at end of
  // stream) -- no design change needed here.
  impl_->ctx->thread_count = 0;
  impl_->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

  if (avcodec_open2(impl_->ctx, codec, nullptr) < 0) {
    avcodec_free_context(&impl_->ctx);
    return;
  }
  impl_->frame = av_frame_alloc();
  impl_->packet = av_packet_alloc();
}

AvcodecDecoder::~AvcodecDecoder() = default;

bool AvcodecDecoder::IsValid() const { return impl_->ctx != nullptr && impl_->frame != nullptr && impl_->packet != nullptr; }

bool AvcodecDecoder::SendAndReceive(const std::vector<uint8_t>& essenceData, NormalizedFrame& out) {
  if (!IsValid()) return false;

  if (essenceData.empty()) {
    // Drain mode: ask for a frame the codec is still holding (e.g. MPEG-2's
    // inherent one-frame decode delay), rather than feeding new input.
    const int drainRet = avcodec_send_packet(impl_->ctx, nullptr);
    if (drainRet < 0 && drainRet != AVERROR_EOF) return false;
  } else {
    const int size = static_cast<int>(essenceData.size());
    // Reuse the packet's existing buffer instead of av_new_packet's fresh
    // av_malloc on every single frame, whenever that's safe: it's safe
    // exactly when nothing else still holds a reference to it (frame-
    // threaded decode can keep earlier packets alive internally -- see
    // AV_INPUT_BUFFER_PADDING_SIZE-sized 0x00 tail below matching what
    // av_new_packet itself would zero) and it's already large enough.
    // avcodec_send_packet takes its own reference via av_packet_ref (a
    // cheap refcount bump, since our packet already carries a buf) rather
    // than requiring this function's memcpy to be the only copy in
    // existence, so handing back the same buffer here doesn't race with
    // that internal ref. Falls back to a fresh allocation otherwise (first
    // call, buffer still referenced elsewhere, or grown past capacity).
    AVBufferRef* buf = impl_->packet->buf;
    const bool canReuse =
        buf != nullptr && av_buffer_get_ref_count(buf) == 1 && buf->size >= size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (canReuse) {
      impl_->packet->data = buf->data;
      impl_->packet->size = size;
    } else {
      av_packet_unref(impl_->packet);
      if (av_new_packet(impl_->packet, size) < 0) return false;
    }
    std::memcpy(impl_->packet->data, essenceData.data(), essenceData.size());
    std::memset(impl_->packet->data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    const int sendRet = avcodec_send_packet(impl_->ctx, impl_->packet);
    if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) return false;
  }

  const int recvRet = avcodec_receive_frame(impl_->ctx, impl_->frame);
  if (recvRet < 0) return false;  // AVERROR(EAGAIN): needs more input before a frame is ready

  out.format = FromAvPixFmt(impl_->frame->format);
  out.width = impl_->frame->width;
  out.height = impl_->frame->height;
  const int numPlanes = NumPlanes(out.format);
  for (int p = 0; p < numPlanes; ++p) {
    const int planeHeight = p == 0 ? out.height : ChromaHeight(out.format, out.height);
    const int stride = impl_->frame->linesize[p];
    out.stride[p] = stride;
    out.planeHeight[p] = planeHeight;
    out.planeData[p].assign(impl_->frame->data[p], impl_->frame->data[p] + static_cast<size_t>(stride) * planeHeight);
  }
  for (int p = numPlanes; p < 3; ++p) out.planeData[p].clear();

  av_frame_unref(impl_->frame);
  return true;
}

}  // namespace mxfsrc
