#include "core/source_engine.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>

#include "codec/decoder_factory.h"
#include "mxf/index_transport.h"
#include "mxf/ul_tables.h"

namespace mxfsrc {

namespace {

// Decodes exactly one more essence-walker-yielded video frame via `decoder`,
// draining (feeding an empty essenceData, per Decoder::SendAndReceive's
// contract) once `walker` runs out rather than stopping immediately -- some
// codecs (e.g. MPEG-2) hold one or more frames internally until explicitly
// asked to flush. Shared between the background DecodeThreadMain loop and
// SeekAndDecode's one-shot forward re-decode so both stay in sync with any
// future codec-specific quirks. Returns false only once the decoder has
// truly nothing left to give after essence is exhausted (real end of
// stream/error) -- the caller should treat that as fatal for its purpose.
// `scratch` is caller-owned and reused across every call (rather than a
// fresh EssenceElement declared per call) so its `data` vector's heap buffer
// survives from one frame to the next -- resize() then only grows the
// buffer on the rare frame that's bigger than any seen so far, instead of
// paying a fresh multi-frame-sized allocation on literally every single
// frame. Same "recycle instead of reallocate" rationale as
// SourceEngine::DecodeThreadMain's NormalizedFrame recycling, just applied
// to the input (compressed essence) side instead of the output side.
// `audioElementsOut`, when non-null, collects every audio element seen
// since the last successful video decode -- regardless of whether a given
// edit unit's audio KLVs come before or after its video KLV in file order,
// everything collected between two successful returns belongs to the edit
// unit that just finished (see SourceEngine::DecodeThreadMain, which clears
// this vector each iteration before calling in).
bool DecodeNextFrame(EssenceWalker& walker, Decoder& decoder, bool& essenceExhausted, EssenceElement& scratch,
                     NormalizedFrame& outFrame, std::vector<EssenceElement>* audioElementsOut) {
  for (;;) {
    if (essenceExhausted) {
      return decoder.SendAndReceive({}, outFrame);
    }
    if (!walker.Next(scratch)) {
      essenceExhausted = true;
      continue;
    }
    if (scratch.isAudio) {
      if (audioElementsOut) audioElementsOut->push_back(scratch);
      continue;
    }
    if (!scratch.isVideo) continue;
    if (decoder.SendAndReceive(scratch.data, outFrame)) return true;
  }
}

// Advances `walker` to its next VIDEO essence element (skipping audio/other
// tracks) and discards it *without decoding it* -- pays only for the KLV
// walk (each element's own small header), not for reading its actual
// payload bytes off disk/network (wantData=false, see EssenceWalker::Next)
// or for the actual (often far more expensive) codec decode work. Only
// safe for intra-only codecs (see IsIntraOnlyCodec), where no frame depends
// on any other, so there's nothing decoding it would have contributed
// anyway.
bool SkipNextVideoElement(EssenceWalker& walker) {
  EssenceElement element;
  while (walker.Next(element, /*wantData=*/false)) {
    if (element.isVideo) return true;
  }
  return false;
}

}  // namespace

SourceEngine::SourceEngine() = default;

void SourceEngine::AppendLogLine(const std::string& line) const {
  fprintf(stderr, "%s\n", line.c_str());
  fflush(stderr);

  // Also append to a user-specified log file (MxfSource(..., logfile=...))
  // if one was given -- stderr alone is invisible for GUI hosts (e.g.
  // AvsPmod) that don't surface a console window. Appended rather than
  // overwritten so the file accumulates a history across repeated runs;
  // silently skipped (not fatal) if the path can't be opened.
  if (logFile_.empty()) return;
  if (FILE* f = fopen(logFile_.c_str(), "a")) {
    const time_t now = time(nullptr);
    struct tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    char timeBuf[32] = {};
    strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    fprintf(f, "[%s] %s\n", timeBuf, line.c_str());
    fclose(f);
  }
}

SourceEngine::~SourceEngine() {
  if (videoBuffer_) videoBuffer_->Shutdown();
  if (audioBuffer_) audioBuffer_->Shutdown();
  if (decodeThread_.joinable()) decodeThread_.join();

  if (videoBuffer_) {
    std::string summary;
    {
      std::lock_guard<std::mutex> lock(stats_.mutex);
      summary = "[MxfSource] " + path_ + ": served " + std::to_string(stats_.bufferedCount) +
                " frame(s) from buffer, " + std::to_string(stats_.nonBufferedFrames.size()) +
                " frame(s) via fallback re-decode (not buffered)";
      if (!stats_.nonBufferedFrames.empty()) {
        summary += ": ";
        for (size_t i = 0; i < stats_.nonBufferedFrames.size(); ++i) {
          if (i != 0) summary += ", ";
          summary += std::to_string(stats_.nonBufferedFrames[i]);
        }
      }
    }
    AppendLogLine(summary);
  }
}

OpenResult SourceEngine::Open(const std::string& path, const OpenOptions& options) {
  path_ = path;
  logFile_ = options.logfile;
  httpHeaders_ = options.http_headers;
  reader_ = std::make_unique<MxfReader>(path, httpHeaders_);
  if (!reader_->IsOpen()) {
    std::string error = "failed to open file: " + path;
    const std::string detail = reader_->LastError();
    if (!detail.empty()) error += " (" + detail + ")";
    return {OpenStatus::kFileError, error};
  }

  std::string parseError;
  auto info = ParseMxfHeader(*reader_, &parseError);
  if (!info) {
    const std::string detail = reader_->LastError();
    if (!detail.empty()) parseError += " (" + detail + ")";
    return {OpenStatus::kParseError, parseError};
  }
  streamInfo_ = *info;

  // Override whatever ResolvePackage's generic first-package-wins loop
  // produced for numFrames with the SourcePackage-authoritative formula
  // (MP Duration = SP Duration - SP Origin) whenever a SourcePackage video
  // track was actually found -- see MxfStreamInfo::sourcePackageDuration.
  // Falls back to the generic value (unchanged) for files with no
  // resolvable SourcePackage.
  if (streamInfo_.sourcePackageDuration >= 0) {
    const int64_t origin = options.handle_origin ? streamInfo_.origin : 0;
    streamInfo_.numFrames = streamInfo_.sourcePackageDuration - origin;
  }
  originSkip_ = options.handle_origin ? streamInfo_.origin : 0;
  if (originSkip_ < 0) originSkip_ = 0;

  if (!options.index_url.empty()) {
    // Fail loud: unlike the local-build path below, a slave asked to reuse a
    // shared index has no reason to expect one might be unavailable, so any
    // problem here (network error, version/bodySid mismatch, implausible
    // byte offsets -- see mxf/index_transport.h) is reported as a hard
    // Open() failure rather than silently degrading to a local rebuild.
    std::string indexError;
    videoIndex_ = FetchEssenceIndex(options.index_url, streamInfo_.bodySid, reader_->Size(), &indexError);
    if (!videoIndex_) {
      return {OpenStatus::kParseError, "failed to fetch shared index from '" + options.index_url + "': " + indexError};
    }
  } else {
    // Best-effort, and never written to disk (see MxfExtractIndex for the
    // separate host function that does that): absent/failed index building
    // degrades silently to videoIndex_ staying nullopt, which SeekAndDecode
    // already treats as "no index available, use the
    // from-essence-element-0 fallback" -- never a hard Open() failure.
    videoIndex_ = BuildEssenceIndex(*reader_, streamInfo_.bodySid);
  }

  if (!streamInfo_.hasVideo) return {OpenStatus::kParseError, "no video track found in Header Metadata"};
  if (streamInfo_.numFrames < 0) {
    if (videoIndex_ && !videoIndex_->Empty()) {
      // Natural fallback win: an Index Table Segment's own covered range
      // supplies a frame count when Header Metadata's Duration is absent.
      streamInfo_.numFrames = videoIndex_->HighestCoveredEditUnit() + 1;
    }
    if (streamInfo_.numFrames < 0) {
      return {OpenStatus::kParseError,
              "unable to determine frame count from Header Metadata (no Index Table Segment either; "
              "essence-count fallback not yet implemented, see plan Phase 1 Duration fallback chain step 5)"};
    }
  }
  // Raw, untrimmed essence-edit-unit-space frame count -- the coordinate
  // space videoIndex_'s own entries live in (see
  // BuildAudioSampleIndexFromIndexTable's doc comment). streamInfo_.numFrames
  // is now fully finalized (Header Metadata Duration, SourcePackage-Origin
  // override, or the Index Table fallback above), so this is safe to derive.
  const int64_t rawFrameCount = originSkip_ + streamInfo_.numFrames;

  videoDecoder_ = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                      static_cast<int>(streamInfo_.height));
  if (!videoDecoder_) return {OpenStatus::kUnsupportedCodec, "unsupported or unavailable video codec"};

  // Decide whether this file's audio is actually exposed. Never an error
  // for a video-only source (no audio tracks at all); for a file that does
  // have audio, OpenOptions::use_audio governs what happens when the
  // configuration isn't one this plugin supports (mismatched track bit
  // depth/sample rate, non-PCM codec) or when the essence-level sample
  // index fails to build (malformed/truncated audio) -- see UseAudioMode.
  if (streamInfo_.hasAudio && options.use_audio != UseAudioMode::kNever) {
    if (!streamInfo_.audioConfigSupported) {
      if (options.use_audio == UseAudioMode::kAlways) {
        return {OpenStatus::kUnsupportedCodec, "audio configuration not supported: " + streamInfo_.audioUnsupportedReason};
      }
      AppendLogLine("[MxfSource] " + path_ + ": audio configuration not supported (" +
                    streamInfo_.audioUnsupportedReason + "), continuing video-only");
    } else {
      audioSampleIndex_ = videoIndex_ ? BuildAudioSampleIndexFromIndexTable(*videoIndex_, streamInfo_, rawFrameCount)
                                       : std::nullopt;
      if (!audioSampleIndex_) {
        if (options.use_audio == UseAudioMode::kAlways) {
          return {OpenStatus::kUnsupportedCodec,
                  "audio not fully covered by the video Index Table (or invalid edit-rate/sample-rate metadata)"};
        }
        AppendLogLine("[MxfSource] " + path_ + ": failed to build audio sample index, continuing video-only");
      } else {
        exposeAudio_ = true;
        audioSampleOriginSkip_ = audioSampleIndex_->CumulativeSamplesBefore(originSkip_);
        AppendLogLine("[MxfSource] " + path_ + ": audio sample index built from video Index Table (" +
                      std::to_string(rawFrameCount) + " edit units, no essence scan)");
      }
    }
  }

  std::vector<std::array<uint8_t, 4>> audioTrackNumbersForWalker;
  if (exposeAudio_) {
    for (const AudioTrackInfo& t : streamInfo_.audioTracks) audioTrackNumbersForWalker.push_back(t.trackNumber);
  }
  walker_ = std::make_unique<EssenceWalker>(*reader_, streamInfo_.headerMetadataEnd, streamInfo_.videoTrackNumber,
                                             streamInfo_.hasVideo, audioTrackNumbersForWalker);

  const int capacity = options.buffer_frames > 0 ? options.buffer_frames : 300;
  const int margin = options.eagerdecode > 0 ? options.eagerdecode : 32;
  videoBuffer_ = std::make_unique<FrameBuffer<std::shared_ptr<NormalizedFrame>>>(capacity, margin);
  if (exposeAudio_) {
    audioBuffer_ = std::make_unique<FrameBuffer<std::shared_ptr<AudioBlock>>>(capacity, margin);
  }

  decodeThread_ = std::thread(&SourceEngine::DecodeThreadMain, this);

  std::string peekError;
  if (videoBuffer_->Peek(0, firstFrame_, &peekError) != FetchStatus::kOk) {
    return {OpenStatus::kDecodeError, "failed to decode first frame: " + peekError};
  }
  return {OpenStatus::kOk, ""};
}

FetchStatus SourceEngine::GetVideoFrame(int64_t n, std::shared_ptr<NormalizedFrame>& out, std::string* errorOut) {
  // A forward request far enough beyond what's actually been produced so far
  // behaves exactly like a backward seek past the window: the ring buffer's
  // blocking Get() would just wait for the single background decode thread
  // to decode every intervening frame in order, one at a time -- as slow as
  // a full linear scan (the real-world bug this guards against: seeking to
  // frame 6413 of a long file blocked for ~30s while the background thread
  // sequentially decoded frames 0..6413). Bypass the wait and go straight to
  // the same Index Table-assisted SeekAndDecode() fallback used for genuine
  // backward seeks below, rather than only reaching for it once a frame has
  // already been evicted.
  if (n > videoBuffer_->HighestProduced() + videoBuffer_->Capacity()) {
    auto seeked = SeekAndDecode(n);
    if (!seeked) {
      if (errorOut) {
        *errorOut = "requested frame is far ahead of the current decode position, and the fallback "
                    "re-decode failed to reach it";
      }
      return FetchStatus::kError;
    }
    out = seeked;
    stats_.RecordNonBufferedServe(n);
    return FetchStatus::kOk;
  }

  FetchStatus status = videoBuffer_->Get(n, out, errorOut);
  if (status == FetchStatus::kOk) {
    stats_.RecordBufferedServe();
    return status;
  }
  // kEndOfStream: DecodeThreadMain legitimately reached streamInfo_.numFrames
  // -- nothing to retry. Anything else (kShutdown, if ever added) also
  // propagates as-is. kEvicted and kError both fall through to the same
  // fallback below: kError includes DecodeThreadMain's continuous decode
  // hitting a real decode failure or premature end-of-essence *before*
  // reaching the declared frame count -- real bug found on a real file
  // where the continuous background decode lost sync a few frames before
  // the true end (root cause still not fully understood), while a fresh,
  // independent SeekAndDecode restart for those exact same frames -- same
  // codec, same file, just a brand-new decoder instance -- decoded them
  // correctly every time. Whatever the continuous path's problem is, it
  // doesn't reproduce on a fresh restart, so falling back here recovers
  // frames that would otherwise be reported as unreachable even though
  // they're genuinely decodable.
  if (status != FetchStatus::kEvicted && status != FetchStatus::kError) return status;

  // Fell outside the ring buffer's window (or DecodeThreadMain failed) --
  // rather than failing outright, fall back to a one-shot re-decode from
  // the start of the file (or the nearest keyframe the index provides).
  // Slow without an index (O(n), no keyframe to jump via) but correct for
  // any codec; the primary decode thread and ring buffer are left
  // completely untouched.
  auto seeked = SeekAndDecode(n);
  if (!seeked) {
    if (errorOut) {
      *errorOut = "requested frame fell outside the ring buffer window (or the background decode failed to "
                  "reach it), and the fallback re-decode also failed to reach it";
    }
    return FetchStatus::kError;
  }
  out = seeked;
  stats_.RecordNonBufferedServe(n);
  return FetchStatus::kOk;
}

std::shared_ptr<NormalizedFrame> SourceEngine::SeekAndDecode(int64_t target) {
  MxfReader freshReader(path_, httpHeaders_);
  if (!freshReader.IsOpen()) return nullptr;

  auto freshDecoder = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                          static_cast<int>(streamInfo_.height));
  if (!freshDecoder) return nullptr;

  auto frame = std::make_shared<NormalizedFrame>();

  // Host frame index `target` maps to essence video element index
  // `originSkip_ + target` -- see MxfStreamInfo::origin / originSkip_.
  const int64_t essenceTarget = originSkip_ + target;

  if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
    // No frame depends on any other for these codecs -- every essence
    // element already *is* its own anchor. Every Index Table entry for an
    // intra-only codec is therefore always a "keyframe" (see
    // FlattenSegmentInto's CBR branch and IsKeyframeFlags for VBR), so
    // FindPrecedingKeyframe(essenceTarget) -- when the index covers it --
    // resolves to the target's *own* byte offset directly, with zero
    // backward walk. Consulting it turns this into an O(1) direct seek
    // instead of the O(target) skip loop below, which -- even though it
    // never decodes the skipped frames -- still pays for reading every one
    // of their full compressed payloads off disk/network on its way to the
    // target (SkipNextVideoElement's own wantData=false only removes that
    // cost when this direct jump isn't available). Real bug found on a
    // real 93GB XAVC-Intra file over an SMB share: seeking to frame 5000
    // with no index-assisted jump dragged several GB of essence data
    // across the network merely to skip past it. Absent/inapplicable index
    // (e.g. a gap, or no Index Table Segment at all) -> unchanged
    // from-essence-element-0 skip-walk behavior.
    uint64_t startOffset = streamInfo_.headerMetadataEnd;
    int64_t startEditUnit = 0;
    if (videoIndex_) {
      if (auto seekPoint = videoIndex_->FindPrecedingKeyframe(essenceTarget)) {
        startOffset = seekPoint->byteOffset;
        startEditUnit = seekPoint->editUnit;
        stats_.RecordIndexAssistedSeek();
      }
    }
    // EssenceWalker is constructed here (not shared with the GOP branch
    // below) since it holds a reference member and can't be reassigned
    // once the branch's own start offset is known. Video-only fallback --
    // audio has its own independent fresh-reader path, FreshReadAudioBlock.
    EssenceWalker freshWalker(freshReader, startOffset, streamInfo_.videoTrackNumber, streamInfo_.hasVideo, {});
    for (int64_t i = startEditUnit; i < essenceTarget; ++i) {
      if (!SkipNextVideoElement(freshWalker)) return nullptr;  // ran out of essence before reaching target
    }
    // The loop above discarded every video element between startEditUnit
    // and essenceTarget (zero iterations when the index jumped straight to
    // the target itself); the next one the walker yields is the target,
    // and this time we keep its bytes to actually decode.
    EssenceElement targetElement;
    for (;;) {
      if (!freshWalker.Next(targetElement)) return nullptr;
      if (targetElement.isVideo) break;
    }
    if (!freshDecoder->SendAndReceive(targetElement.data, *frame) &&
        !freshDecoder->SendAndReceive({}, *frame)) {  // defensive drain in case of any decoder-internal latency
      return nullptr;
    }
    return frame;
  }

  // GOP-based codec (e.g. XDCAM long-GOP MPEG-2, XAVC-L): consult the Index
  // Table-derived lookup (mxf/index_table.h), if available, for a byte
  // offset closer than essence element 0 -- FindPrecedingKeyframe only ever
  // returns an actual I-frame's offset, so restarting a brand-new decoder
  // there is valid; startEditUnit is handed back alongside startOffset so
  // the decode-forward loop below knows exactly how many more frames it
  // still needs (rather than needing to infer it from the byte offset).
  // Absent/inapplicable index -> unchanged from-essence-element-0 behavior.
  uint64_t startOffset = streamInfo_.headerMetadataEnd;
  int64_t startEditUnit = 0;
  // Decoding straight through to `essenceTarget` from a mid-stream keyframe
  // restart would land some number of edit units too far forward -- a
  // decoder restarted fresh needs its B-picture reordering pipeline to fill
  // before its Nth output genuinely corresponds to input element N, unlike
  // decoding continuously from edit unit 0 (this engine's other two decode
  // paths, DecodeThreadMain and the startEditUnit==0 fallback below), which
  // is exactly as warmed-up at any point as the main pipeline itself and
  // needs no compensation at all. `iterations` (computed below, once
  // startEditUnit/reorderDelay are final) already subtracts this out; real
  // bug found via content comparison against an independent ffmpeg decode:
  // counting outputs unconditionally landed 2 edit units too far forward on
  // a real long-GOP MPEG-2 camera file -- AVCodecContext::delay wasn't
  // populated for that codec and per-packet pts weren't reliably preserved
  // through avcodec's own reordering (confirmed again directly: explicitly
  // setting PTS per packet and reading it back off the decoded frame is
  // *not* reliably preserved through a real H.264 decoder's own internal
  // reordering either, even though the same technique round-trips correctly
  // through a real MPEG-2 encode/decode -- codec-specific, and not
  // something to build a general fix on), so neither could recover this
  // number from the decoder itself. See EssenceIndex::EstimateReorderDelay
  // for how it's derived from the index's own flags instead.
  //
  // A target within reorderDelay of its own nearest keyframe can't be
  // reached this way at all -- decoding even the minimum one iteration
  // already overshoots it (verified against ffmpeg: the two edit units
  // immediately at/after a keyframe come out as content from 1-2 units
  // further still). Rather than silently returning that wrong frame, walk
  // back to progressively earlier keyframes (more runway before the
  // target) until there's enough room, or fall back to element 0 (needs no
  // compensation, proven above) if the file's very first keyframe still
  // isn't far enough away. kMaxBackwardKeyframeHops bounds this walk-back
  // in case reorderDelay is ever wildly overestimated for a malformed
  // index, so a bad file can't spin forever.
  if (videoIndex_) {
    int64_t searchFrom = essenceTarget;
    constexpr int kMaxBackwardKeyframeHops = 8;
    for (int hop = 0; hop < kMaxBackwardKeyframeHops; ++hop) {
      auto seekPoint = videoIndex_->FindPrecedingKeyframe(searchFrom);
      if (!seekPoint) break;  // no keyframe at/before searchFrom -- use the element-0 fallback below
      const int delay = videoIndex_->EstimateReorderDelay(seekPoint->editUnit);
      if (seekPoint->editUnit == 0 || essenceTarget - seekPoint->editUnit - delay >= 1) {
        startOffset = seekPoint->byteOffset;
        startEditUnit = seekPoint->editUnit;
        stats_.RecordIndexAssistedSeek();
        break;
      }
      if (seekPoint->editUnit == 0) break;  // can't search earlier than this
      searchFrom = seekPoint->editUnit - 1;
    }

    // AVC open GOP: this GOP's own leading picture(s) may reference the
    // *previous* GOP's anchor frame, not anything within this GOP -- a
    // decoder restarted fresh at startEditUnit alone would be missing that
    // reference (verified against a real file: every GOP's leading two B
    // pictures carry a KeyFrameOffset pointing at the previous GOP's own
    // keyframe, not this one). Extending the restart back exactly one GOP
    // is sufficient regardless of whether *that* GOP's own leading pictures
    // have the same issue -- its own keyframe is an intra frame, self-
    // sufficient by definition, and everything decoded from it before
    // reaching startEditUnit is discarded by the reorderDelay/iterations
    // math below anyway, so its correctness doesn't matter, only that
    // decoding it populates the reference this GOP's leading pictures need.
    if (startEditUnit > 0 && videoIndex_->NeedsPreviousGop(startEditUnit)) {
      if (auto prevPoint = videoIndex_->FindPrecedingKeyframe(startEditUnit - 1)) {
        startOffset = prevPoint->byteOffset;
        startEditUnit = prevPoint->editUnit;
      }
    }
  }

  // Previously skipped whenever startEditUnit==0, on the assumption that
  // decoding continuously from the file's very first element needs no
  // compensation. Disproven against the real repro file: even its first GOP
  // has a real reorder delay (EstimateReorderDelay(0) == 2, confirmed via an
  // independent ffprobe decode), so every frame decoded via this path was
  // silently coming out shifted by that many positions. Frames within
  // reorderDelay of edit unit 0 itself remain a known gap (see the walk-back
  // loop above -- there's no earlier keyframe to borrow runway from when
  // startEditUnit is already 0), same as any other unreachable-near-keyframe
  // target.
  const int reorderDelay = videoIndex_ ? videoIndex_->EstimateReorderDelay(startEditUnit) : 0;
  const int64_t iterations = std::max<int64_t>(1, essenceTarget - startEditUnit - reorderDelay + 1);
  const int64_t adjustedTarget = startEditUnit + iterations - 1;

  EssenceWalker freshWalker(freshReader, startOffset, streamInfo_.videoTrackNumber, streamInfo_.hasVideo, {});
  bool essenceExhausted = false;
  EssenceElement scratch;
  for (int64_t i = startEditUnit; i <= adjustedTarget; ++i) {
    if (!DecodeNextFrame(freshWalker, *freshDecoder, essenceExhausted, scratch, *frame, nullptr)) return nullptr;
  }
  return frame;
}

void SourceEngine::DecodeThreadMain() {
  bool essenceExhausted = false;
  EssenceElement scratch;

  // Discard the leading `originSkip_` video essence elements (Origin
  // pre-roll/trim, see MxfStreamInfo::origin) before host frame 0 is ever
  // produced, so ring-buffer index 0 lines up with essence element
  // `originSkip_`. Mirrors SeekAndDecode's intra-only-vs-GOP branching:
  // intra-only codecs can skip the raw bytes without decoding them at all;
  // GOP-based codecs must actually decode (and discard) each one, since a
  // later frame may depend on it as a reference.
  if (originSkip_ > 0) {
    const std::string originError = "MXF essence ended before reaching the Origin offset (" +
                                     std::to_string(originSkip_) + ")";
    if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
      for (int64_t i = 0; i < originSkip_; ++i) {
        if (!SkipNextVideoElement(*walker_)) {
          videoBuffer_->SignalError(originError);
          return;
        }
      }
    } else {
      NormalizedFrame discard;
      for (int64_t i = 0; i < originSkip_; ++i) {
        if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, scratch, discard, nullptr)) {
          videoBuffer_->SignalError(originError);
          return;
        }
      }
    }
  }

  // Recycled from the ring buffer's most recent eviction (see
  // FrameBuffer::Put) whenever nothing else still holds a reference to it --
  // reusing its already-resident plane buffers avoids paying fresh
  // heap-allocation/OS-page-fault cost on every single decoded frame, which
  // real-file benchmarking found was dwarfing actual codec decode time (tens
  // of ms/frame reallocating multi-MB buffers vs ~1-2ms of real H.264
  // decode). Only reused when use_count() == 1: anything higher means a
  // consumer (e.g. the AVS/VS adapter) is still holding the shared_ptr from
  // an earlier Get(), so mutating it in place would corrupt data out from
  // under that caller.
  std::shared_ptr<NormalizedFrame> recycled;

  // Reorder-delay compensation: mirrors SeekAndDecode's reorderDelay math
  // (see there, EssenceIndex::EstimateReorderDelay) for this from-element-0
  // continuous path, which previously assumed no compensation was ever
  // needed here at all -- disproven against a real Canon XAVC file, whose
  // very first GOP has a genuine reorder delay of 2 (confirmed via an
  // independent ffprobe decode), so ring index 0 -- and every index after
  // it, permanently -- was silently getting served content shifted forward
  // by that many display positions, for the entire file.
  //
  // Implemented as a small sliding delay line rather than SeekAndDecode's
  // one-shot "decode `iterations` outputs, keep only the last" trick, since
  // this loop must keep producing forever: each newly decoded frame is
  // pushed onto `pending`; once it holds more than `reorderDelay` frames,
  // the oldest is popped and served, exactly `reorderDelay` positions
  // behind the raw decode stream (which is what makes ring index N line up
  // with display position N once popping starts). The first `reorderDelay`
  // ring indices have no earlier frame to draw that delay from -- nothing
  // before element 0 exists to reference -- the same structural gap
  // SeekAndDecode's own edit-unit-0 restart already accepts, so they're
  // served the oldest frame decoded so far without popping it: a defensible
  // best-effort repeat rather than blocking or fabricating content.
  const int reorderDelay =
      (videoIndex_ && !IsIntraOnlyCodec(streamInfo_.videoCodec)) ? videoIndex_->EstimateReorderDelay(0) : 0;
  std::deque<std::shared_ptr<NormalizedFrame>> pending;

  // Audio's counterpart of `pending`: PCM decode itself has zero internal
  // reordering (unlike avcodec's B-frame pipeline), but the audio elements
  // collected during loop iteration K are consumed in lockstep with that
  // same iteration's video decode -- and that iteration's VIDEO output only
  // gets assigned to ring index K after `reorderDelay` more iterations (via
  // `pending` above). Without an identical delay line, audio for essence
  // position K would be paired with ring index K's video, whose actual
  // content came from essence position K-reorderDelay -- reorderDelay edit
  // units of audio/video desync. Only matters for GOP-based codecs
  // (reorderDelay > 0); intra-only/PCM-friendly codecs pop every iteration
  // immediately, same as `pending` does.
  std::deque<std::shared_ptr<AudioBlock>> pendingAudio;
  // Once an audio decode genuinely fails, stop producing further audio for
  // the rest of this thread's life -- video keeps going unaffected. The one
  // SignalError() call already makes every future Get() beyond the last
  // successfully-Put index report the error; already-buffered audio stays
  // servable (see frame_buffer.h's error-check ordering).
  bool audioFailed = false;

  for (;;) {
    const int64_t next = videoBuffer_->NextIndex();
    if (next >= streamInfo_.numFrames) {
      videoBuffer_->SignalEndOfStream();
      if (audioBuffer_) audioBuffer_->SignalEndOfStream();
      return;
    }
    if (!videoBuffer_->WaitForCapacity(next)) return;  // shutdown requested
    // audioBuffer_ deliberately has no WaitForCapacity gate of its own: it's
    // produced by this same loop in lockstep with video, at exactly video's
    // own pace -- gating on audioBuffer_'s independent watermark as well
    // would stall video production (and thus the whole thread, since both
    // come from one shared walker) the moment audio's watermark fell behind
    // video's, e.g. a host that reads video frames without ever calling
    // GetAudioSamples (or reads audio in far larger/rarer chunks) -- audio's
    // watermark_ starts at 0 and margin_ can be as small as 1, so without
    // this, DecodeThreadMain would deadlock waiting for an audio consumer
    // that may never come. Put() below still evicts audioBuffer_'s oldest
    // entry once full on its own (eviction doesn't depend on
    // WaitForCapacity, see FrameBuffer::Put) -- a genuinely slow/absent
    // audio consumer just falls back to FreshReadAudioBlock for anything
    // that fell outside the window, exactly like a slow video consumer
    // already falls back to SeekAndDecode.

    std::shared_ptr<NormalizedFrame> frame;
    if (recycled && recycled.use_count() == 1) {
      frame = std::move(recycled);
    } else {
      frame = std::make_shared<NormalizedFrame>();
    }
    std::vector<EssenceElement> audioScratch;
    if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, scratch, *frame,
                         (audioBuffer_ && !audioFailed) ? &audioScratch : nullptr)) {
      const std::string err =
          "MXF essence ended before reaching the declared frame count (" + std::to_string(streamInfo_.numFrames) + ")";
      videoBuffer_->SignalError(err);
      if (audioBuffer_) audioBuffer_->SignalError(err);
      return;
    }
    pending.push_back(std::move(frame));

    if (audioBuffer_ && !audioFailed) {
      auto block = std::make_shared<AudioBlock>();
      std::string audioErr;
      if (!InterleaveAudioEditUnit(audioScratch, streamInfo_.audioTracks, *block, &audioErr)) {
        audioBuffer_->SignalError("audio decode error near ring index " + std::to_string(next) + ": " + audioErr);
        audioFailed = true;
      } else {
        pendingAudio.push_back(std::move(block));
      }
    }

    std::shared_ptr<NormalizedFrame> outputFrame;
    if (static_cast<int>(pending.size()) > reorderDelay) {
      outputFrame = std::move(pending.front());
      pending.pop_front();
    } else {
      outputFrame = pending.front();  // still warming up -- best-effort repeat, not yet consumed
    }
    recycled = videoBuffer_->Put(next, std::move(outputFrame)).value_or(nullptr);

    if (audioBuffer_ && !audioFailed) {
      std::shared_ptr<AudioBlock> outputAudio;
      if (static_cast<int>(pendingAudio.size()) > reorderDelay) {
        outputAudio = std::move(pendingAudio.front());
        pendingAudio.pop_front();
      } else if (!pendingAudio.empty()) {
        outputAudio = pendingAudio.front();  // still warming up -- same best-effort repeat as video
      }
      if (outputAudio) audioBuffer_->Put(next, std::move(outputAudio));
    }
  }
}

FetchStatus SourceEngine::GetAudioSamples(int64_t start, int64_t count, void* buf, std::string* errorOut) {
  if (!exposeAudio_ || !audioSampleIndex_) {
    if (errorOut) *errorOut = "audio not available";
    return FetchStatus::kError;
  }
  if (count <= 0) return FetchStatus::kOk;

  const uint32_t bytesPerFrame = streamInfo_.audioChannels * (streamInfo_.audioBitsPerSample / 8);
  uint8_t* dst = static_cast<uint8_t*>(buf);
  int64_t remaining = count;
  int64_t hostSamplePos = start;

  while (remaining > 0) {
    // audioSampleIndex_ operates in raw essence-edit-unit/sample space
    // (like videoIndex_'s EssenceIndex); audioSampleOriginSkip_ translates
    // the host-relative `hostSamplePos` GetAudio callers use into that raw
    // space -- see MxfStreamInfo::origin / SeekAndDecode's analogous
    // `originSkip_ + target` translation for video.
    const auto loc = audioSampleIndex_->Locate(hostSamplePos + audioSampleOriginSkip_);
    if (!loc) {
      // Past the last known sample -- a well-behaved host should never ask
      // for this, since both adapters set their total sample count exactly
      // from TotalAudioSamples(); zero-fill defensively rather than error.
      std::memset(dst, 0, static_cast<size_t>(remaining) * bytesPerFrame);
      return FetchStatus::kOk;
    }
    const int64_t hostEditUnit = loc->editUnit - originSkip_;

    std::shared_ptr<AudioBlock> block;
    const FetchStatus status = FetchAudioBlock(hostEditUnit, loc->byteOffset, block, errorOut);
    if (status != FetchStatus::kOk) return status;

    const int64_t available = block->sampleCount - loc->sampleOffsetInEditUnit;
    const int64_t toCopy = std::min<int64_t>(available, remaining);
    const uint8_t* src = block->data.data() + static_cast<size_t>(loc->sampleOffsetInEditUnit) * bytesPerFrame;
    std::memcpy(dst, src, static_cast<size_t>(toCopy) * bytesPerFrame);
    dst += toCopy * bytesPerFrame;
    hostSamplePos += toCopy;
    remaining -= toCopy;
  }
  return FetchStatus::kOk;
}

FetchStatus SourceEngine::FetchAudioBlock(int64_t hostEditUnit, uint64_t byteOffset, std::shared_ptr<AudioBlock>& out,
                                          std::string* errorOut) {
  // Mirrors GetVideoFrame's forward-seek bypass: a request far enough ahead
  // of what's actually been produced so far would otherwise block on the
  // background thread sequentially producing every intervening edit unit --
  // go straight to the fresh-reader fallback instead.
  if (hostEditUnit > audioBuffer_->HighestProduced() + audioBuffer_->Capacity()) {
    out = FreshReadAudioBlock(byteOffset);
    if (!out) {
      if (errorOut) {
        *errorOut = "requested audio is far ahead of the current decode position, and the fallback re-read failed "
                    "to reach it";
      }
      return FetchStatus::kError;
    }
    return FetchStatus::kOk;
  }

  const FetchStatus status = audioBuffer_->Get(hostEditUnit, out, errorOut);
  if (status == FetchStatus::kOk) return status;
  if (status != FetchStatus::kEvicted && status != FetchStatus::kError) return status;  // kEndOfStream propagates

  out = FreshReadAudioBlock(byteOffset);
  if (!out) {
    if (errorOut) {
      *errorOut = "requested audio fell outside the ring buffer window (or the background decode failed to reach "
                  "it), and the fallback re-read also failed to reach it";
    }
    return FetchStatus::kError;
  }
  return FetchStatus::kOk;
}

std::shared_ptr<AudioBlock> SourceEngine::FreshReadAudioBlock(uint64_t byteOffset) {
  MxfReader freshReader(path_, httpHeaders_);
  if (!freshReader.IsOpen()) return nullptr;

  std::vector<std::array<uint8_t, 4>> audioTrackNumbers;
  audioTrackNumbers.reserve(streamInfo_.audioTracks.size());
  for (const AudioTrackInfo& t : streamInfo_.audioTracks) audioTrackNumbers.push_back(t.trackNumber);

  // hasVideo=false: video (and any other track) is skipped over cheaply via
  // EssenceWalker's generic unmatched-KLV path, no different from any other
  // untracked track -- this fallback only ever needs audio.
  EssenceWalker freshWalker(freshReader, byteOffset, /*videoTrackNumber=*/{}, /*hasVideo=*/false, audioTrackNumbers);

  const size_t numTracks = audioTrackNumbers.size();
  std::vector<EssenceElement> group(numTracks);
  std::vector<bool> seen(numTracks, false);
  size_t seenCount = 0;

  EssenceElement element;
  while (seenCount < numTracks) {
    if (!freshWalker.Next(element)) return nullptr;
    if (!element.isAudio || element.audioTrackIndex < 0) continue;
    const size_t idx = static_cast<size_t>(element.audioTrackIndex);
    if (!seen[idx]) {
      group[idx] = element;
      seen[idx] = true;
      ++seenCount;
    }
  }

  auto block = std::make_shared<AudioBlock>();
  if (!InterleaveAudioEditUnit(group, streamInfo_.audioTracks, *block, nullptr)) return nullptr;
  return block;
}

}  // namespace mxfsrc
