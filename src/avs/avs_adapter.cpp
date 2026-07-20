// Phase 0 scaffold: proves the AviSynth+ plugin ABI loads and a source
// filter can hand back frames end to end. GetFrame() currently synthesizes
// a solid-color image instead of decoding MXF essence -- that wiring lands
// in Phase 5 once src/core's decode engine exists.
#include <cstring>

#include "avisynth.h"

namespace {

class MxfSource final : public IClip {
public:
  MxfSource(int width, int height, int num_frames, unsigned fps_num, unsigned fps_den) {
    vi_ = VideoInfo();
    vi_.width = width;
    vi_.height = height;
    vi_.fps_numerator = fps_num;
    vi_.fps_denominator = fps_den;
    vi_.num_frames = num_frames;
    vi_.pixel_type = VideoInfo::CS_YV12;
    vi_.audio_samples_per_second = 0; // no audio track yet
  }

  PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment* env) override {
    PVideoFrame frame = env->NewVideoFrame(vi_);
    FillPlane(frame, PLANAR_Y, static_cast<BYTE>(n & 0xFF));
    FillPlane(frame, PLANAR_U, 128);
    FillPlane(frame, PLANAR_V, 128);
    return frame;
  }

  void __stdcall GetAudio(void* buf, int64_t start, int64_t count, IScriptEnvironment* /*env*/) override {
    (void)start;
    std::memset(buf, 0, static_cast<size_t>(count) * static_cast<size_t>(vi_.BytesPerAudioSample()));
  }

  bool __stdcall GetParity(int /*n*/) override { return false; }

  int __stdcall SetCacheHints(int cachehints, int /*frame_range*/) override {
    // Forward-only decode is driven by a single background thread in the
    // shared core engine (Phase 4); MT_SERIALIZED lets the AviSynth+ core
    // funnel concurrent Prefetch() callers through us one at a time instead
    // of us having to guard every public entry point ourselves.
    if (cachehints == CACHE_GET_MTMODE) return MT_SERIALIZED;
    return 0;
  }

  const VideoInfo& __stdcall GetVideoInfo() override { return vi_; }

private:
  static void FillPlane(PVideoFrame& frame, int plane, BYTE value) {
    BYTE* p = frame->GetWritePtr(plane);
    const int pitch = frame->GetPitch(plane);
    const int row_size = frame->GetRowSize(plane);
    const int height = frame->GetHeight(plane);
    for (int y = 0; y < height; ++y) {
      std::memset(p + static_cast<ptrdiff_t>(y) * pitch, value, static_cast<size_t>(row_size));
    }
  }

  VideoInfo vi_;
};

AVSValue __cdecl CreateMxfSource(AVSValue args, void* /*user_data*/, IScriptEnvironment* env) {
  const char* path = args[0].AsString("");
  const int width = args[1].AsInt(1920);
  const int height = args[2].AsInt(1080);
  const int num_frames = args[3].AsInt(300);
  const int fps_num = args[4].AsInt(25);
  const int fps_den = args[5].AsInt(1);
  (void)path; // Phase 1 will open and parse this file instead of trusting explicit args
  if (width <= 0 || height <= 0 || num_frames <= 0 || fps_num <= 0 || fps_den <= 0) {
    env->ThrowError("MxfSource: width/height/num_frames/fps_num/fps_den must be positive");
  }
  return new MxfSource(width, height, num_frames, static_cast<unsigned>(fps_num), static_cast<unsigned>(fps_den));
}

} // namespace

const AVS_Linkage* AVS_linkage = nullptr;

#if defined(_WIN32)
#define MXFSRC_AVS_EXPORT __declspec(dllexport)
#else
#define MXFSRC_AVS_EXPORT __attribute__((visibility("default")))
#endif

extern "C" MXFSRC_AVS_EXPORT const char* __stdcall AvisynthPluginInit3(
    IScriptEnvironment* env, const AVS_Linkage* const vectors) {
  AVS_linkage = vectors;
  env->AddFunction(
      "MxfSource", "[file]s[width]i[height]i[num_frames]i[fps_num]i[fps_den]i", CreateMxfSource, nullptr);
  return "MXF forward-only source plugin (Phase 0 scaffold)";
}
