#include "core/source_engine.h"

#include <cstdio>
#include <ctime>

#include "codec/decoder_factory.h"
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
bool DecodeNextFrame(EssenceWalker& walker, Decoder& decoder, bool& essenceExhausted, NormalizedFrame& outFrame) {
  for (;;) {
    if (essenceExhausted) {
      return decoder.SendAndReceive({}, outFrame);
    }
    EssenceElement element;
    if (!walker.Next(element)) {
      essenceExhausted = true;
      continue;
    }
    if (!element.isVideo) continue;  // audio elements: not wired into a ring buffer yet
    if (decoder.SendAndReceive(element.data, outFrame)) return true;
  }
}

// Advances `walker` to its next VIDEO essence element (skipping audio/other
// tracks) and discards it *without decoding it* -- still pays for the KLV
// walk and reading its raw bytes off disk, but skips the actual (often far
// more expensive) codec decode work entirely. Only safe for intra-only
// codecs (see IsIntraOnlyCodec), where no frame depends on any other, so
// there's nothing decoding it would have contributed anyway.
bool SkipNextVideoElement(EssenceWalker& walker) {
  EssenceElement element;
  while (walker.Next(element)) {
    if (element.isVideo) return true;
  }
  return false;
}

}  // namespace

SourceEngine::SourceEngine() = default;

SourceEngine::~SourceEngine() {
  if (videoBuffer_) videoBuffer_->Shutdown();
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

    fprintf(stderr, "%s\n", summary.c_str());
    fflush(stderr);

    // Also append to a user-specified log file (MxfSource(..., logfile=...))
    // if one was given -- stderr alone is invisible for GUI hosts (e.g.
    // AvsPmod) that don't surface a console window. Appended rather than
    // overwritten so the file accumulates a history across repeated runs;
    // silently skipped (not fatal) if the path can't be opened.
    if (!logFile_.empty()) {
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
        fprintf(f, "[%s] %s\n", timeBuf, summary.c_str());
        fclose(f);
      }
    }
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

  // Best-effort: absent/failed index building degrades silently to
  // videoIndex_ staying nullopt, which SeekAndDecode already treats as "no
  // index available, use the from-essence-element-0 fallback" -- never a
  // hard Open() failure.
  videoIndex_ = BuildEssenceIndex(*reader_, streamInfo_.bodySid);

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

  videoDecoder_ = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                      static_cast<int>(streamInfo_.height));
  if (!videoDecoder_) return {OpenStatus::kUnsupportedCodec, "unsupported or unavailable video codec"};

  walker_ = std::make_unique<EssenceWalker>(*reader_, streamInfo_.headerMetadataEnd, streamInfo_.videoTrackNumber,
                                             streamInfo_.hasVideo, streamInfo_.audioTrackNumber,
                                             streamInfo_.hasAudio);

  const int capacity = options.buffer_frames > 0 ? options.buffer_frames : 300;
  const int margin = options.eagerdecode > 0 ? options.eagerdecode : 32;
  videoBuffer_ = std::make_unique<FrameBuffer<std::shared_ptr<NormalizedFrame>>>(capacity, margin);

  decodeThread_ = std::thread(&SourceEngine::DecodeThreadMain, this);

  std::string peekError;
  if (videoBuffer_->Peek(0, firstFrame_, &peekError) != FetchStatus::kOk) {
    return {OpenStatus::kDecodeError, "failed to decode first frame: " + peekError};
  }
  return {OpenStatus::kOk, ""};
}

FetchStatus SourceEngine::GetVideoFrame(int64_t n, std::shared_ptr<NormalizedFrame>& out, std::string* errorOut) {
  FetchStatus status = videoBuffer_->Get(n, out, errorOut);
  if (status == FetchStatus::kOk) {
    stats_.RecordBufferedServe();
    return status;
  }
  if (status != FetchStatus::kEvicted) return status;  // real error/shutdown/end-of-stream: propagate as-is

  // Fell outside the ring buffer's window -- rather than failing outright,
  // fall back to a one-shot re-decode from the start of the file. Slow
  // (O(n), no keyframe index to jump via) but correct for any codec; the
  // primary decode thread and ring buffer are left completely untouched.
  auto seeked = SeekAndDecode(n);
  if (!seeked) {
    if (errorOut) {
      *errorOut = "requested frame fell outside the ring buffer window, and the fallback re-decode "
                  "from file start also failed to reach it";
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
    // element already *is* its own anchor, so skip every video frame
    // before the target without decoding any of them (much cheaper than a
    // full decode), then decode only the target frame itself. EssenceWalker
    // is constructed here (not shared with the GOP branch below) since it
    // holds a reference member and can't be reassigned once the branch's
    // own start offset is known.
    EssenceWalker freshWalker(freshReader, streamInfo_.headerMetadataEnd, streamInfo_.videoTrackNumber,
                               streamInfo_.hasVideo, streamInfo_.audioTrackNumber, streamInfo_.hasAudio);
    for (int64_t i = 0; i < essenceTarget; ++i) {
      if (!SkipNextVideoElement(freshWalker)) return nullptr;  // ran out of essence before reaching target
    }
    // The loop above discarded exactly `essenceTarget` video elements
    // (indices [0, essenceTarget)); the next one the walker yields is the
    // target itself, and this time we keep its bytes to actually decode.
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
  if (videoIndex_) {
    if (auto seekPoint = videoIndex_->FindPrecedingKeyframe(essenceTarget)) {
      startOffset = seekPoint->byteOffset;
      startEditUnit = seekPoint->editUnit;
      stats_.RecordIndexAssistedSeek();
    }
  }

  EssenceWalker freshWalker(freshReader, startOffset, streamInfo_.videoTrackNumber, streamInfo_.hasVideo,
                             streamInfo_.audioTrackNumber, streamInfo_.hasAudio);
  bool essenceExhausted = false;
  for (int64_t i = startEditUnit; i <= essenceTarget; ++i) {
    if (!DecodeNextFrame(freshWalker, *freshDecoder, essenceExhausted, *frame)) return nullptr;
  }
  return frame;
}

void SourceEngine::DecodeThreadMain() {
  bool essenceExhausted = false;

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
        if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, discard)) {
          videoBuffer_->SignalError(originError);
          return;
        }
      }
    }
  }

  for (;;) {
    const int64_t next = videoBuffer_->NextIndex();
    if (next >= streamInfo_.numFrames) {
      videoBuffer_->SignalEndOfStream();
      return;
    }
    if (!videoBuffer_->WaitForCapacity(next)) return;  // shutdown requested

    auto frame = std::make_shared<NormalizedFrame>();
    if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, *frame)) {
      videoBuffer_->SignalError("MXF essence ended before reaching the declared frame count (" +
                                 std::to_string(streamInfo_.numFrames) + ")");
      return;
    }

    videoBuffer_->Put(next, std::move(frame));
  }
}

}  // namespace mxfsrc
