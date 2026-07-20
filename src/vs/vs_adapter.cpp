// Phase 0 scaffold: proves the VapourSynth plugin ABI loads and a source
// filter can hand back frames end to end. GetFrame() currently synthesizes
// a solid-color image instead of decoding MXF essence -- that wiring lands
// in Phase 6 once src/core's decode engine exists.
#include <cstdint>
#include <cstring>
#include <new>

#include "VapourSynth4.h"
#include "VSHelper4.h"

namespace {

struct MxfSourceData {
  VSVideoInfo vi{};
};

const VSFrame* VS_CC MxfSourceGetFrame(int n, int activationReason, void* instanceData, void** /*frameData*/,
                                        VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
  auto* d = static_cast<MxfSourceData*>(instanceData);

  if (activationReason == arInitial) {
    VSFrame* frame = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, core);
    for (int plane = 0; plane < d->vi.format.numPlanes; ++plane) {
      const uint8_t value = (plane == 0) ? static_cast<uint8_t>(n & 0xFF) : 128;
      uint8_t* p = vsapi->getWritePtr(frame, plane);
      const ptrdiff_t stride = vsapi->getStride(frame, plane);
      const int rowSize = vsapi->getFrameWidth(frame, plane) * d->vi.format.bytesPerSample;
      const int height = vsapi->getFrameHeight(frame, plane);
      for (int y = 0; y < height; ++y) {
        std::memset(p + y * stride, value, static_cast<size_t>(rowSize));
      }
    }
    return frame;
  }

  (void)frameCtx;
  return nullptr;
}

void VS_CC MxfSourceFree(void* instanceData, VSCore* /*core*/, const VSAPI* /*vsapi*/) {
  delete static_cast<MxfSourceData*>(instanceData);
}

void VS_CC CreateMxfSource(const VSMap* in, VSMap* out, void* /*userData*/, VSCore* core, const VSAPI* vsapi) {
  int err = 0;
  int width = static_cast<int>(vsapi->mapGetInt(in, "width", 0, &err));
  if (err) width = 1920;
  int height = static_cast<int>(vsapi->mapGetInt(in, "height", 0, &err));
  if (err) height = 1080;
  int num_frames = static_cast<int>(vsapi->mapGetInt(in, "num_frames", 0, &err));
  if (err) num_frames = 300;
  int64_t fps_num = vsapi->mapGetInt(in, "fps_num", 0, &err);
  if (err) fps_num = 25;
  int64_t fps_den = vsapi->mapGetInt(in, "fps_den", 0, &err);
  if (err) fps_den = 1;

  // "file" is required by the arg spec but not yet used -- Phase 1 will open
  // and parse it instead of trusting explicit width/height/num_frames args.
  vsapi->mapGetData(in, "file", 0, &err);

  if (width <= 0 || height <= 0 || num_frames <= 0 || fps_num <= 0 || fps_den <= 0) {
    vsapi->mapSetError(out, "MxfSource: width/height/num_frames/fps_num/fps_den must be positive");
    return;
  }

  auto* d = new MxfSourceData();
  if (!vsapi->getVideoFormatByID(&d->vi.format, pfYUV420P8, core)) {
    vsapi->mapSetError(out, "MxfSource: failed to resolve YUV420P8 video format");
    delete d;
    return;
  }
  d->vi.width = width;
  d->vi.height = height;
  d->vi.fpsNum = fps_num;
  d->vi.fpsDen = fps_den;
  d->vi.numFrames = num_frames;

  vsapi->createVideoFilter(out, "MxfSource", &d->vi, MxfSourceGetFrame, MxfSourceFree,
                            fmUnordered, nullptr, 0, d, core);
}

} // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("com.ffastrans.mxfsource", "mxfsource", "Forward-only MXF source (Phase 0 scaffold)",
                        VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction("MxfSource", "file:data;width:int:opt;height:int:opt;num_frames:int:opt;fps_num:int:opt;fps_den:int:opt;",
                            "clip:vnode;", CreateMxfSource, nullptr, plugin);
}
