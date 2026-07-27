// VapourSynth adapter: wraps core::SourceEngine (Phase 1-4: MXF header
// parse, forward essence walk, codec decode, ring-buffer decode thread)
// behind a video filter node. GetFrame blocks on the engine's ring buffer
// under fmUnordered -- see plan "vs/ adapter" section: fmUnordered still
// guarantees exclusive execution of our getFrame callback (matches
// BestSource's own choice for a single logical decode stream that the core
// still prefetches), so blocking on our internal condvar here is safe.
#include <cstring>
#include <memory>
#include <string>

#include "VapourSynth4.h"
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

struct FormatInfo {
  VSVideoFormat format{};
  int bytesPerSample = 1;
  bool isPackedUyvy = false;
};

// VapourSynth has no packed pixel formats at all (planar only), unlike
// AviSynth+'s YUY2 -- UYVY essence must be unpacked into 3-plane YUV422P8
// rather than just byte-shuffled. Formats that are already planar in our
// NormalizedFrame representation map straight across: VS stores planes in
// logical Y/U/V order with no AviSynth-style VPlaneFirst quirk to account for.
bool MapPixelFormat(PixelFormat fmt, VSCore* core, const VSAPI* vsapi, FormatInfo& out) {
  int bitsPerSample = 8, subW = 0, subH = 0;
  switch (fmt) {
    case PixelFormat::kUyvy422:
      out.isPackedUyvy = true;
      subW = 1;
      subH = 0;
      break;
    case PixelFormat::kYuv420p8:
      subW = 1;
      subH = 1;
      break;
    case PixelFormat::kYuv422p8:
      subW = 1;
      subH = 0;
      break;
    case PixelFormat::kYuv420p10:
      bitsPerSample = 10;
      subW = 1;
      subH = 1;
      out.bytesPerSample = 2;
      break;
    case PixelFormat::kYuv422p10:
      bitsPerSample = 10;
      subW = 1;
      subH = 0;
      out.bytesPerSample = 2;
      break;
    default:
      return false;
  }
  return vsapi->queryVideoFormat(&out.format, cfYUV, stInteger, bitsPerSample, subW, subH, core) != 0;
}

void CopyPlanarFrame(const NormalizedFrame& src, VSFrame* dst, const VSAPI* vsapi, int bytesPerSample) {
  for (int p = 0; p < 3; ++p) {
    uint8_t* d = vsapi->getWritePtr(dst, p);
    const ptrdiff_t dStride = vsapi->getStride(dst, p);
    const int rowBytes = vsapi->getFrameWidth(dst, p) * bytesPerSample;
    const int height = vsapi->getFrameHeight(dst, p);
    const uint8_t* s = src.planeData[p].data();
    const int sStride = src.stride[p];
    // See avs_adapter.cpp's CopyPlane for why the contiguous case is worth
    // special-casing: one bulk memcpy instead of `height` row-by-row calls.
    if (dStride == sStride && dStride == rowBytes) {
      std::memcpy(d, s, static_cast<size_t>(rowBytes) * static_cast<size_t>(height));
      continue;
    }
    for (int y = 0; y < height; ++y) {
      std::memcpy(d + static_cast<ptrdiff_t>(y) * dStride, s + static_cast<ptrdiff_t>(y) * sStride,
                  static_cast<size_t>(rowBytes));
    }
  }
}

// UYVY (U0 Y0 V0 Y1 per 2 pixels, single plane) -> YUV422P8 (3 planes, Y full
// width, U/V half width).
void UnpackUyvyToYuv422Planar(const NormalizedFrame& src, VSFrame* dst, const VSAPI* vsapi) {
  uint8_t* yPlane = vsapi->getWritePtr(dst, 0);
  uint8_t* uPlane = vsapi->getWritePtr(dst, 1);
  uint8_t* vPlane = vsapi->getWritePtr(dst, 2);
  const ptrdiff_t yStride = vsapi->getStride(dst, 0);
  const ptrdiff_t uStride = vsapi->getStride(dst, 1);
  const ptrdiff_t vStride = vsapi->getStride(dst, 2);
  const int height = vsapi->getFrameHeight(dst, 0);
  const int chromaWidth = vsapi->getFrameWidth(dst, 1);
  const uint8_t* s = src.planeData[0].data();
  const int sStride = src.stride[0];
  for (int y = 0; y < height; ++y) {
    const uint8_t* srow = s + static_cast<ptrdiff_t>(y) * sStride;
    uint8_t* yrow = yPlane + static_cast<ptrdiff_t>(y) * yStride;
    uint8_t* urow = uPlane + static_cast<ptrdiff_t>(y) * uStride;
    uint8_t* vrow = vPlane + static_cast<ptrdiff_t>(y) * vStride;
    for (int cx = 0; cx < chromaWidth; ++cx) {
      const uint8_t u = srow[cx * 4 + 0];
      const uint8_t y0 = srow[cx * 4 + 1];
      const uint8_t v = srow[cx * 4 + 2];
      const uint8_t y1 = srow[cx * 4 + 3];
      yrow[cx * 2 + 0] = y0;
      yrow[cx * 2 + 1] = y1;
      urow[cx] = u;
      vrow[cx] = v;
    }
  }
}

struct MxfSourceData {
  std::unique_ptr<SourceEngine> engine;
  VSVideoInfo vi{};
  bool isPackedUyvy = false;
  int bytesPerSample = 1;
};

const VSFrame* VS_CC MxfSourceGetFrame(int n, int activationReason, void* instanceData, void** /*frameData*/,
                                        VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
  auto* d = static_cast<MxfSourceData*>(instanceData);
  if (activationReason != arInitial) return nullptr;

  std::shared_ptr<NormalizedFrame> frame;
  std::string err;
  if (d->engine->GetVideoFrame(n, frame, &err) != FetchStatus::kOk) {
    vsapi->setFilterError(err.empty() ? "MxfSource: failed to decode frame" : ("MxfSource: " + err).c_str(),
                           frameCtx);
    return nullptr;
  }

  VSFrame* out = vsapi->newVideoFrame(&d->vi.format, d->vi.width, d->vi.height, nullptr, core);
  if (d->isPackedUyvy) {
    UnpackUyvyToYuv422Planar(*frame, out, vsapi);
  } else {
    CopyPlanarFrame(*frame, out, vsapi, d->bytesPerSample);
  }
  return out;
}

void VS_CC MxfSourceFree(void* instanceData, VSCore* /*core*/, const VSAPI* /*vsapi*/) {
  delete static_cast<MxfSourceData*>(instanceData);
}

void VS_CC CreateMxfSource(const VSMap* in, VSMap* out, void* /*userData*/, VSCore* core, const VSAPI* vsapi) {
  int err = 0;
  const char* path = vsapi->mapGetData(in, "file", 0, &err);
  if (err || !path || !*path) {
    vsapi->mapSetError(out, "MxfSource: a file path is required");
    return;
  }
  const int64_t bufferFramesArg = vsapi->mapGetInt(in, "buffer_frames", 0, &err);
  const int bufferFrames = err ? 300 : static_cast<int>(bufferFramesArg);
  if (bufferFrames <= 0) {
    vsapi->mapSetError(out, "MxfSource: buffer_frames must be positive");
    return;
  }
  const int64_t marginArg = vsapi->mapGetInt(in, "eagerdecode", 0, &err);
  const int eagerdecode = err ? 32 : static_cast<int>(marginArg);
  if (eagerdecode <= 0) {
    vsapi->mapSetError(out, "MxfSource: eagerdecode must be positive");
    return;
  }
  int logFileErr = 0;
  const char* logFile = vsapi->mapGetData(in, "logfile", 0, &logFileErr);
  int httpHeadersErr = 0;
  const char* httpHeaders = vsapi->mapGetData(in, "httpheaders", 0, &httpHeadersErr);
  int handleOriginErr = 0;
  const int64_t handleOriginArg = vsapi->mapGetInt(in, "handle_origin", 0, &handleOriginErr);
  const bool handleOrigin = handleOriginErr ? true : (handleOriginArg != 0);

  auto engine = std::make_unique<SourceEngine>();
  OpenOptions options;
  options.buffer_frames = bufferFrames;
  options.eagerdecode = eagerdecode;
  options.logfile = logFileErr ? "" : logFile;
  options.http_headers = httpHeadersErr ? "" : httpHeaders;
  options.handle_origin = handleOrigin;
  const OpenResult result = engine->Open(path, options);
  if (result.status != OpenStatus::kOk) {
    vsapi->mapSetError(out, ("MxfSource: " + result.error).c_str());
    return;
  }

  const auto& firstFrame = engine->FirstFrame();
  FormatInfo fmtInfo;
  if (!MapPixelFormat(firstFrame.format, core, vsapi, fmtInfo)) {
    vsapi->mapSetError(out, "MxfSource: decoded pixel format has no VapourSynth mapping yet");
    return;
  }

  auto* d = new MxfSourceData();
  const auto& info = engine->StreamInfo();
  d->vi.format = fmtInfo.format;
  // Width/height come from the actually-decoded frame, not the MXF
  // descriptor's StoredWidth/StoredHeight -- see the matching comment in
  // avs_adapter.cpp for why (field-coded/interlaced essence conventionally
  // stores a single field's height there while the codec hands back a full
  // decoded frame; sizing from the descriptor silently cropped such frames).
  d->vi.width = firstFrame.width;
  d->vi.height = firstFrame.height;
  d->vi.fpsNum = info.editRate.num > 0 ? info.editRate.num : 25;
  d->vi.fpsDen = info.editRate.den > 0 ? info.editRate.den : 1;
  d->vi.numFrames = static_cast<int>(info.numFrames);
  d->isPackedUyvy = fmtInfo.isPackedUyvy;
  d->bytesPerSample = fmtInfo.bytesPerSample;
  d->engine = std::move(engine);

  // Explicitly disabled (cmForceDisable) rather than left at VS core's
  // default cmAuto: SourceEngine already provides its own bounded
  // ring-buffer window (OpenOptions::buffer_frames), so an additional
  // generic VS-core cache on top is pure redundancy -- and, worse, real
  // testing (vspipe piping this filter's entire output, which issues many
  // concurrent frame requests) showed VS core's automatic cache retaining
  // frames for the *whole file* with no eviction, growing without bound
  // (538 4K 4:2:2 10-bit frames is ~18GB) and making per-frame throughput
  // degrade continuously over the run (25fps -> 10fps) instead of holding
  // steady. Mirrors avs_adapter.cpp's MT_SERIALIZED choice: this class owns
  // the one source of caching/threading truth, so the host's own generic
  // mechanism is turned off rather than layered on top.
  VSNode* node = vsapi->createVideoFilter2("MxfSource", &d->vi, MxfSourceGetFrame, MxfSourceFree, fmUnordered,
                                            nullptr, 0, d, core);
  if (!node) {
    vsapi->mapSetError(out, "MxfSource: failed to create filter node");
    return;
  }
  vsapi->setCacheMode(node, cmForceDisable);
  vsapi->mapSetNode(out, "clip", node, maAppend);
  vsapi->freeNode(node);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("com.ffastrans.mxfsource", "mxfsource", "Forward-only MXF source",
                        VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction(
      "MxfSource",
      "file:data;buffer_frames:int:opt;eagerdecode:int:opt;logfile:data:opt;httpheaders:data:opt;"
      "handle_origin:int:opt;",
      "clip:vnode;", CreateMxfSource, nullptr, plugin);
}
