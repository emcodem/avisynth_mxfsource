// AviSynth+ adapter: wraps core::SourceEngine (Phase 1-4: MXF header parse,
// forward essence walk, codec decode, ring-buffer decode thread) behind
// IClip. GetFrame(n) blocks on the engine's ring buffer -- see plan
// "avs/ adapter" section for the MT_SERIALIZED rationale.
#include <cstring>
#include <memory>
#include <string>

#include "avisynth.h"
#include "core/options.h"
#include "core/source_engine.h"

using mxfsrc::FetchStatus;
using mxfsrc::NormalizedFrame;
using mxfsrc::OpenOptions;
using mxfsrc::OpenResult;
using mxfsrc::OpenStatus;
using mxfsrc::PixelFormat;
using mxfsrc::SourceEngine;

namespace {

// AviSynth's only packed 4:2:2 format is YUY2 (Y0,U,Y1,V byte order); our
// decoders produce UYVY (U,Y0,V,Y1 -- the common real-world convention for
// MXF "uncompressed 4:2:2 8-bit" essence). Planar formats need no such
// reordering: AVS_linkage's PLANAR_Y/U/V constants abstract away AviSynth's
// internal VPlaneFirst plane storage order, so copying our logical Y/U/V
// planes straight across is correct.
bool MapPixelFormat(PixelFormat fmt, int& pixelType, bool& isPackedUyvy) {
  isPackedUyvy = false;
  switch (fmt) {
    case PixelFormat::kUyvy422:
      pixelType = VideoInfo::CS_YUY2;
      isPackedUyvy = true;
      return true;
    case PixelFormat::kYuv420p8:
      pixelType = VideoInfo::CS_YV12;
      return true;
    case PixelFormat::kYuv422p8:
      pixelType = VideoInfo::CS_YV16;
      return true;
    case PixelFormat::kYuv420p10:
      pixelType = VideoInfo::CS_YUV420P10;
      return true;
    case PixelFormat::kYuv422p10:
      pixelType = VideoInfo::CS_YUV422P10;
      return true;
    default:
      return false;
  }
}

void CopyPlane(const NormalizedFrame& src, int srcPlaneIdx, PVideoFrame& dst, int avsPlane) {
  BYTE* d = dst->GetWritePtr(avsPlane);
  const int dPitch = dst->GetPitch(avsPlane);
  const int rowBytes = dst->GetRowSize(avsPlane);
  const int height = dst->GetHeight(avsPlane);
  const uint8_t* s = src.planeData[srcPlaneIdx].data();
  const int sStride = src.stride[srcPlaneIdx];
  for (int y = 0; y < height; ++y) {
    std::memcpy(d + static_cast<ptrdiff_t>(y) * dPitch, s + static_cast<ptrdiff_t>(y) * sStride,
                static_cast<size_t>(rowBytes));
  }
}

// UYVY (U0 Y0 V0 Y1 per 2 pixels) -> YUY2 (Y0 U0 Y1 V0).
void CopyUyvyAsYuy2(const NormalizedFrame& src, PVideoFrame& dst) {
  BYTE* d = dst->GetWritePtr();
  const int dPitch = dst->GetPitch();
  const int height = dst->GetHeight();
  const int width = dst->GetRowSize() / 2;
  const uint8_t* s = src.planeData[0].data();
  const int sStride = src.stride[0];
  for (int y = 0; y < height; ++y) {
    const uint8_t* srow = s + static_cast<ptrdiff_t>(y) * sStride;
    BYTE* drow = d + static_cast<ptrdiff_t>(y) * dPitch;
    for (int x = 0; x + 1 < width; x += 2) {
      const uint8_t u = srow[x * 2 + 0];
      const uint8_t y0 = srow[x * 2 + 1];
      const uint8_t v = srow[x * 2 + 2];
      const uint8_t y1 = srow[x * 2 + 3];
      drow[x * 2 + 0] = y0;
      drow[x * 2 + 1] = u;
      drow[x * 2 + 2] = y1;
      drow[x * 2 + 3] = v;
    }
  }
}

class MxfSource final : public IClip {
 public:
  MxfSource(std::unique_ptr<SourceEngine> engine, IScriptEnvironment* env) : engine_(std::move(engine)) {
    const auto& info = engine_->StreamInfo();
    const auto& firstFrame = engine_->FirstFrame();

    int pixelType = 0;
    if (!MapPixelFormat(firstFrame.format, pixelType, isPackedUyvy_)) {
      env->ThrowError("MxfSource: decoded pixel format has no AviSynth+ mapping yet");
    }

    vi_ = VideoInfo();
    // Width/height come from the actually-decoded frame, not the MXF
    // descriptor's StoredWidth/StoredHeight: for field-coded/interlaced
    // essence, StoredHeight conventionally records a single field's height
    // (e.g. 544 for a 1080-line frame) while the codec still hands back a
    // full decoded frame. Sizing the output buffer from the descriptor
    // instead of the decoder's real output silently cropped such frames to
    // roughly their top half -- caught via real-file benchmarking, not the
    // synthetic-file unit tests (see plan Phase 7 notes).
    vi_.width = firstFrame.width;
    vi_.height = firstFrame.height;
    vi_.fps_numerator = info.editRate.num > 0 ? static_cast<unsigned>(info.editRate.num) : 25;
    vi_.fps_denominator = info.editRate.den > 0 ? static_cast<unsigned>(info.editRate.den) : 1;
    vi_.num_frames = static_cast<int>(info.numFrames);
    vi_.pixel_type = pixelType;
    vi_.audio_samples_per_second = 0;  // audio ring buffer not wired yet, see plan Phase 4 notes
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    std::shared_ptr<NormalizedFrame> frame;
    std::string err;
    if (engine_->GetVideoFrame(n, frame, &err) != FetchStatus::kOk) {
      env->ThrowError("MxfSource: %s", err.empty() ? "failed to decode frame" : err.c_str());
    }

    PVideoFrame out = env->NewVideoFrame(vi_);
    if (isPackedUyvy_) {
      CopyUyvyAsYuy2(*frame, out);
    } else {
      CopyPlane(*frame, 0, out, PLANAR_Y);
      CopyPlane(*frame, 1, out, PLANAR_U);
      CopyPlane(*frame, 2, out, PLANAR_V);
    }
    return out;
  }

  void __stdcall GetAudio(void* buf, int64_t /*start*/, int64_t count, IScriptEnvironment* /*env*/) override {
    std::memset(buf, 0, static_cast<size_t>(count) * static_cast<size_t>(vi_.BytesPerAudioSample()));
  }

  bool __stdcall GetParity(int /*n*/) override { return false; }

  int __stdcall SetCacheHints(int cachehints, int /*frame_range*/) override {
    // The engine's own mutex/condvar is the single source of thread-safety
    // truth (see core::SourceEngine); MT_SERIALIZED lets the AviSynth+ core
    // funnel concurrent Prefetch() callers through us one at a time instead
    // of relying on that engine-internal locking to also guard our own
    // public entry points.
    if (cachehints == CACHE_GET_MTMODE) return MT_SERIALIZED;
    // Without this, AviSynth+'s script interpreter auto-inserts a generic
    // Cache filter after us (its compatibility fallback for filters that
    // don't declare an opinion) -- pure redundancy on top of SourceEngine's
    // own bounded ring buffer (OpenOptions::buffer_frames), and the
    // VapourSynth adapter's real-file testing found that exact class of
    // extra host-side cache growing unbounded and degrading throughput over
    // a run (see vs_adapter.cpp's setCacheMode(cmForceDisable) fix and its
    // comment for the measured effect). Declaring CACHE_DONT_CACHE_ME here
    // is the AviSynth+ equivalent of that fix.
    if (cachehints == CACHE_DONT_CACHE_ME) return 1;
    return 0;
  }

  const VideoInfo& __stdcall GetVideoInfo() override { return vi_; }

 private:
  std::unique_ptr<SourceEngine> engine_;
  VideoInfo vi_;
  bool isPackedUyvy_ = false;
};

AVSValue __cdecl CreateMxfSource(AVSValue args, void* /*user_data*/, IScriptEnvironment* env) {
  const char* path = args[0].AsString(nullptr);
  if (!path || !*path) env->ThrowError("MxfSource: a file path is required");
  const int bufferFrames = args[1].AsInt(300);
  if (bufferFrames <= 0) env->ThrowError("MxfSource: buffer_frames must be positive");
  const int eagerdecode = args[2].AsInt(32);
  if (eagerdecode <= 0) env->ThrowError("MxfSource: eagerdecode must be positive");
  const char* logFile = args[3].AsString("");
  const char* httpHeaders = args[4].AsString("");
  const bool handleOrigin = args[5].AsBool(true);

  auto engine = std::make_unique<SourceEngine>();
  OpenOptions options;
  options.buffer_frames = bufferFrames;
  options.eagerdecode = eagerdecode;
  options.logfile = logFile;
  options.http_headers = httpHeaders;
  options.handle_origin = handleOrigin;
  const OpenResult result = engine->Open(path, options);
  if (result.status != OpenStatus::kOk) {
    env->ThrowError("MxfSource: %s", result.error.c_str());
  }

  return new MxfSource(std::move(engine), env);
}

}  // namespace

const AVS_Linkage* AVS_linkage = nullptr;

#if defined(_WIN32)
#define MXFSRC_AVS_EXPORT __declspec(dllexport)
#else
#define MXFSRC_AVS_EXPORT __attribute__((visibility("default")))
#endif

extern "C" MXFSRC_AVS_EXPORT const char* __stdcall AvisynthPluginInit3(
    IScriptEnvironment* env, const AVS_Linkage* const vectors) {
  AVS_linkage = vectors;
  env->AddFunction("MxfSource",
                    "[file]s[buffer_frames]i[eagerdecode]i[logfile]s[httpheaders]s[handle_origin]b",
                    CreateMxfSource, nullptr);
  return "MXF forward-only source plugin";
}
