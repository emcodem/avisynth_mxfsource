// VapourSynth adapter: wraps core::SourceEngine (Phase 1-4: MXF header
// parse, forward essence walk, codec decode, ring-buffer decode thread)
// behind a video filter node. GetFrame blocks on the engine's ring buffer
// under fmUnordered -- see plan "vs/ adapter" section: fmUnordered still
// guarantees exclusive execution of our getFrame callback (matches
// BestSource's own choice for a single logical decode stream that the core
// still prefetches), so blocking on our internal condvar here is safe.
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "VapourSynth4.h"
#include "core/index_extract.h"
#include "core/options.h"
#include "core/source_engine.h"
#include "core/use_audio_mode.h"

using mxfsrc::ExtractAndPublishIndex;
using mxfsrc::ExtractIndexResult;
using mxfsrc::FetchStatus;
using mxfsrc::NormalizedFrame;
using mxfsrc::OpenOptions;
using mxfsrc::OpenResult;
using mxfsrc::OpenStatus;
using mxfsrc::ParseUseAudioMode;
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
  // shared_ptr, not unique_ptr: when the file's audio is exposed, a second,
  // independent VSNode (audio) is created from the same CreateMxfSource
  // call and needs to share this same SourceEngine -- VapourSynth gives
  // each VSNode wholly independent instance data with no other mechanism to
  // share state between them.
  std::shared_ptr<SourceEngine> engine;
  VSVideoInfo vi{};
  bool isPackedUyvy = false;
  int bytesPerSample = 1;
};

struct MxfAudioData {
  std::shared_ptr<SourceEngine> engine;
  VSAudioInfo ai{};
  // VSAudioFormat's own container size (always a power of 2, e.g. 4 for a
  // 24-bit format -- see VapourSynth4.h's bytesPerSample doc comment) --
  // used only for the destination stride when writing into a VSFrame.
  int vsBytesPerSample = 2;
  // MxfStreamInfo::audioBitsPerSample/8 -- SourceEngine::GetAudioSamples
  // fills its buffer at this native, possibly-narrower size (e.g. 3 for
  // 24-bit PCM), NOT vsBytesPerSample; conflating the two here previously
  // caused every sample past the first to read from the wrong offset
  // (caught via real vspipe verification, see plan Phase 8/audio
  // real-host-testing task -- an in-process-only test would have missed
  // this, the two sizes only ever differ for a 24-bit source).
  int nativeBytesPerSample = 2;
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

// VapourSynth's audio model is frame-indexed like video, just with a fixed
// length per frame (VS_AUDIO_FRAME_SAMPLES) -- `n` here is an audio frame
// index, not a sample position, so it's translated to a
// SourceEngine::GetAudioSamples sample range first.
const VSFrame* VS_CC MxfSourceGetAudioFrame(int n, int activationReason, void* instanceData, void** /*frameData*/,
                                             VSFrameContext* frameCtx, VSCore* core, const VSAPI* vsapi) {
  auto* d = static_cast<MxfAudioData*>(instanceData);
  if (activationReason != arInitial) return nullptr;

  const int64_t start = static_cast<int64_t>(n) * VS_AUDIO_FRAME_SAMPLES;
  const int64_t count = std::min<int64_t>(VS_AUDIO_FRAME_SAMPLES, d->ai.numSamples - start);
  if (count <= 0) return nullptr;

  const int numChannels = d->ai.format.numChannels;
  const int nativeBytesPerSample = d->nativeBytesPerSample;
  const int vsBytesPerSample = d->vsBytesPerSample;

  // SourceEngine::GetAudioSamples fills one interleaved buffer at the
  // file's *native* bit depth (matching AviSynth's own GetAudio contract),
  // which can be narrower than VS's own container size (e.g. 3 bytes for a
  // 24-bit source vs. VS's 4-byte container) -- sized/strided by
  // nativeBytesPerSample here, NOT vsBytesPerSample. VapourSynth audio
  // frames are also planar (one plane per channel, see getWritePtr/
  // getStride's doc comments), so this de-interleaves on the way in.
  std::vector<uint8_t> interleaved(static_cast<size_t>(count) * static_cast<size_t>(numChannels) *
                                    static_cast<size_t>(nativeBytesPerSample));
  std::string err;
  if (d->engine->GetAudioSamples(start, count, interleaved.data(), &err) != FetchStatus::kOk) {
    vsapi->setFilterError(err.empty() ? "MxfSource: failed to decode audio" : ("MxfSource: " + err).c_str(),
                           frameCtx);
    return nullptr;
  }

  VSFrame* out = vsapi->newAudioFrame(&d->ai.format, static_cast<int>(count), nullptr, core);
  for (int c = 0; c < numChannels; ++c) {
    uint8_t* dst = vsapi->getWritePtr(out, c);
    for (int64_t s = 0; s < count; ++s) {
      const uint8_t* src = interleaved.data() +
                            (static_cast<size_t>(s) * numChannels + c) * static_cast<size_t>(nativeBytesPerSample);
      uint8_t* dstSample = dst + static_cast<size_t>(s) * vsBytesPerSample;
      // Low-justified: the native-width value occupies the low
      // nativeBytesPerSample bytes of VS's wider container, zero-padded
      // above -- the natural reading for an unsigned/positive-biased PCM
      // value and the layout every other VS audio-producing filter assumes
      // for a sub-container bit depth.
      if (vsBytesPerSample > nativeBytesPerSample) {
        std::memset(dstSample, 0, static_cast<size_t>(vsBytesPerSample));
      }
      std::memcpy(dstSample, src, static_cast<size_t>(nativeBytesPerSample));
    }
  }
  return out;
}

void VS_CC MxfAudioFree(void* instanceData, VSCore* /*core*/, const VSAPI* /*vsapi*/) {
  delete static_cast<MxfAudioData*>(instanceData);
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
  int useAudioErr = 0;
  const char* useAudioStr = vsapi->mapGetData(in, "use_audio", 0, &useAudioErr);
  const auto useAudio = ParseUseAudioMode(useAudioErr ? "supported" : useAudioStr);
  if (!useAudio) {
    vsapi->mapSetError(out, "MxfSource: use_audio must be 'always', 'never', or 'supported'");
    return;
  }
  int indexUrlErr = 0;
  const char* indexUrl = vsapi->mapGetData(in, "indexurl", 0, &indexUrlErr);

  auto engine = std::make_shared<SourceEngine>();
  OpenOptions options;
  options.buffer_frames = bufferFrames;
  options.eagerdecode = eagerdecode;
  options.logfile = logFileErr ? "" : logFile;
  options.http_headers = httpHeadersErr ? "" : httpHeaders;
  options.handle_origin = handleOrigin;
  options.use_audio = *useAudio;
  options.index_url = indexUrlErr ? "" : indexUrl;
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
  d->engine = engine;

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

  if (!engine->ExposesAudio()) return;

  // Every discovered audio channel is summed into one raw N-channel layout
  // -- there's no per-channel semantic assignment (e.g. "5.1") to derive
  // from an arbitrary set of MXF audio tracks, so this just claims the
  // lowest N channelLayout bits (VSAudioChannels enum order), which is all
  // queryAudioFormat needs to accept an N-channel format.
  const uint32_t numChannels = info.audioChannels;
  const uint64_t channelLayout = numChannels >= 64 ? ~0ULL : ((1ULL << numChannels) - 1);

  VSAudioFormat audioFormat{};
  if (!vsapi->queryAudioFormat(&audioFormat, stInteger, static_cast<int>(info.audioBitsPerSample), channelLayout,
                                core)) {
    vsapi->mapSetError(out, "MxfSource: unsupported audio format for VapourSynth");
    return;
  }

  auto* ad = new MxfAudioData();
  ad->ai.format = audioFormat;
  ad->ai.sampleRate = static_cast<int>(info.audioSampleRate);
  ad->ai.numSamples = engine->TotalAudioSamples();
  ad->ai.numFrames =
      static_cast<int>((ad->ai.numSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
  ad->vsBytesPerSample = audioFormat.bytesPerSample;
  ad->nativeBytesPerSample = static_cast<int>(info.audioBitsPerSample / 8);
  ad->engine = engine;

  VSNode* audioNode = vsapi->createAudioFilter2("MxfSource", &ad->ai, MxfSourceGetAudioFrame, MxfAudioFree,
                                                 fmUnordered, nullptr, 0, ad, core);
  if (!audioNode) {
    vsapi->mapSetError(out, "MxfSource: failed to create audio filter node");
    return;
  }
  vsapi->setCacheMode(audioNode, cmForceDisable);
  vsapi->mapSetNode(out, "audio", audioNode, maAppend);
  vsapi->freeNode(audioNode);
}

void VS_CC CreateMxfExtractIndex(const VSMap* in, VSMap* out, void* /*userData*/, VSCore* /*core*/,
                                  const VSAPI* vsapi) {
  int err = 0;
  const char* path = vsapi->mapGetData(in, "file", 0, &err);
  if (err || !path || !*path) {
    vsapi->mapSetError(out, "MxfExtractIndex: a file path is required");
    return;
  }
  int targetErr = 0;
  const char* targetUrl = vsapi->mapGetData(in, "targeturl", 0, &targetErr);
  if (targetErr || !targetUrl || !*targetUrl) {
    vsapi->mapSetError(out, "MxfExtractIndex: a target URL/path is required");
    return;
  }
  int httpHeadersErr = 0;
  const char* httpHeaders = vsapi->mapGetData(in, "httpheaders", 0, &httpHeadersErr);

  const ExtractIndexResult result = ExtractAndPublishIndex(path, targetUrl, httpHeadersErr ? "" : httpHeaders);
  if (!result.ok) {
    vsapi->mapSetError(out, ("MxfExtractIndex: " + result.error).c_str());
    return;
  }
  vsapi->mapSetInt(out, "result", 1, maAppend);
}

}  // namespace

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin* plugin, const VSPLUGINAPI* vspapi) {
  vspapi->configPlugin("com.ffastrans.mxfsource", "mxfsource", "Forward-only MXF source",
                        VS_MAKE_VERSION(0, 1), VAPOURSYNTH_API_VERSION, 0, plugin);
  vspapi->registerFunction(
      "MxfSource",
      "file:data;buffer_frames:int:opt;eagerdecode:int:opt;logfile:data:opt;httpheaders:data:opt;"
      "handle_origin:int:opt;use_audio:data:opt;indexurl:data:opt;",
      // "audio" is only actually present in the output VSMap for files whose
      // audio is exposed (see CreateMxfSource's early return when
      // !engine->ExposesAudio()) -- declared here regardless since the
      // return type spec is fixed for the plugin's whole lifetime, not
      // per-invocation.
      "clip:vnode;audio:anode;", CreateMxfSource, nullptr, plugin);
  vspapi->registerFunction("MxfExtractIndex", "file:data;targeturl:data;httpheaders:data:opt;", "result:int;",
                            CreateMxfExtractIndex, nullptr, plugin);
}
