#include "core/source_engine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>

#include "codec/decoder_factory.h"
#include "mxfsrc_version.h"
#include "core/index_policy.h"
#include "mxf/file_open.h"
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
// "3, 7-9, 12, ..." for the serve-stats summary: sorted, duplicates dropped,
// consecutive frames merged into a range, cut off after kMaxRanges ranges.
// With Prefetch the fallback frames arrive out of order, and listing every
// one of them made a single log line of thousands of numbers.
std::string FormatFrameRanges(std::vector<int64_t> frames) {
  constexpr size_t kMaxRanges = 50;
  std::sort(frames.begin(), frames.end());
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
  std::string out;
  size_t ranges = 0;
  for (size_t i = 0; i < frames.size();) {
    size_t j = i;
    while (j + 1 < frames.size() && frames[j + 1] == frames[j] + 1) ++j;
    if (ranges == kMaxRanges) {
      out += ", ... (" + std::to_string(frames.size() - i) + " more frame(s))";
      break;
    }
    if (ranges != 0) out += ", ";
    out += std::to_string(frames[i]);
    if (j > i) out += "-" + std::to_string(frames[j]);
    ++ranges;
    i = j + 1;
  }
  return out;
}

bool SkipNextVideoElement(EssenceWalker& walker) {
  EssenceElement element;
  while (walker.Next(element, /*wantData=*/false)) {
    if (element.isVideo) return true;
  }
  return false;
}

}  // namespace

SourceEngine::SourceEngine() = default;

FieldOrder SourceEngine::FieldOrderOf(const NormalizedFrame& frame) const {
  if (frame.fieldOrder != FieldOrder::kUnknown) return frame.fieldOrder;
  switch (streamInfo_.frameLayout) {
    case 0:  // FullFrame
    case 4:  // SegmentedFrame (PsF): progressive content
      return FieldOrder::kProgressive;
    case 1:  // SeparateFields
    case 3:  // MixedFields
      return streamInfo_.fieldDominance == 1 ? FieldOrder::kTff : FieldOrder::kUnknown;
    default:
      return FieldOrder::kUnknown;
  }
}

EssenceWalkerOptions SourceEngine::WalkerOptions() {
  EssenceWalkerOptions walkerOptions;
  walkerOptions.maxResyncBytes = strict_ ? 0 : kMaxResyncBytes;
  walkerOptions.onDamage = [this](const WalkDamage& damage) { LogWalkDamage(damage); };
  return walkerOptions;
}

void SourceEngine::LogWalkDamage(const WalkDamage& damage) {
  {
    std::lock_guard<std::mutex> lock(damageLogMutex_);
    if (std::find(loggedDamageOffsets_.begin(), loggedDamageOffsets_.end(), damage.offset) !=
        loggedDamageOffsets_.end()) {
      return;
    }
    loggedDamageOffsets_.push_back(damage.offset);
  }
  std::string line = "[MxfSource] " + path_ + ": no valid KLV at byte " + std::to_string(damage.offset) +
                     " (the file is damaged there); ";
  if (damage.resumed) {
    line += "skipped " + std::to_string(damage.resumedAt - damage.offset) + " bytes to the next key at byte " +
            std::to_string(damage.resumedAt);
  } else if (damage.badLength) {
    line += "unreadable KLV header (invalid length field), the essence after it is not reachable";
  } else if (strict_) {
    line += "strict: not resyncing, the essence after it is not reachable";
  } else {
    line += "no key found within " + std::to_string(kMaxResyncBytes / (1024 * 1024)) +
            " MiB, the essence after it is not reachable";
  }
  AppendLogLine(LogLevel::kWarning, line);
}

void SourceEngine::AppendLogLine(LogLevel level, const std::string& line) const {
  if (level == LogLevel::kWarning || debug_) {
    fprintf(stderr, "%s\n", line.c_str());
    fflush(stderr);
  }

  // Also append to a user-specified log file (MxfSource(..., logfile=...))
  // if one was given -- stderr alone is invisible for GUI hosts (e.g.
  // AvsPmod) that don't surface a console window. Appended rather than
  // overwritten so the file accumulates a history across repeated runs;
  // silently skipped (not fatal) if the path can't be opened.
  if (logFile_.empty()) return;
  if (FILE* f = OpenFile(logFile_, "a")) {
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
  {
    std::lock_guard<std::mutex> lock(repositionMutex_);
    stopProducer_ = true;
  }
  repositionCv_.notify_all();
  if (videoBuffer_) videoBuffer_->Shutdown();
  if (audioBuffer_) audioBuffer_->Shutdown();
  if (decodeThread_.joinable()) decodeThread_.join();

  if (videoBuffer_) {
    std::string summary;
    {
      std::lock_guard<std::mutex> lock(stats_.mutex);
      summary = ServeSummaryLocked(/*interim=*/false);
    }
    AppendLogLine(LogLevel::kInfo, summary);
  }
}

std::string SourceEngine::ServeSummaryLocked(bool interim) const {
  std::string summary = "[MxfSource] " + path_ + (interim ? ": so far served " : ": served ") +
                        std::to_string(stats_.bufferedCount) + " frame(s) from buffer, " +
                        std::to_string(stats_.nonBufferedFrames.size()) +
                        " frame(s) via fallback re-decode (not buffered)";
  if (stats_.growths > 0) {
    summary += ", buffer grown " + std::to_string(stats_.growths) + " time(s) to " +
               std::to_string(videoBuffer_->Capacity()) + " frames";
  }
  if (stats_.repositions > 0) {
    summary += ", decoder restarted " + std::to_string(stats_.repositions) + " time(s) after a jump";
  }
  if (byteCache_) {
    auto mb = [](uint64_t bytes) { return std::to_string((bytes + (1u << 19)) >> 20); };
    summary += ", read " + mb(byteCache_->FetchedBytes()) + " MB from the source for " +
               mb(byteCache_->ServedBytes()) + " MB parsed (read cache " + mb(byteCache_->CapacityBytes()) + " MB)";
  }
  const size_t from = interim ? stats_.loggedNonBuffered : 0;
  if (stats_.nonBufferedFrames.size() > from) {
    const std::vector<int64_t> frames(stats_.nonBufferedFrames.begin() + static_cast<std::ptrdiff_t>(from),
                                      stats_.nonBufferedFrames.end());
    summary += (interim && from > 0 ? "; since the last summary: " : ": ") + FormatFrameRanges(frames);
  }
  return summary;
}

void SourceEngine::MaybeLogProgress() {
  constexpr auto kProgressLogInterval = std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  std::string summary;
  {
    std::lock_guard<std::mutex> lock(stats_.mutex);
    if (now - stats_.lastProgressLog < kProgressLogInterval) return;
    const int64_t requests = stats_.bufferedCount + static_cast<int64_t>(stats_.nonBufferedFrames.size());
    stats_.lastProgressLog = now;
    if (requests == stats_.loggedRequests) return;
    summary = ServeSummaryLocked(/*interim=*/true);
    stats_.loggedRequests = requests;
    stats_.loggedNonBuffered = stats_.nonBufferedFrames.size();
  }
  AppendLogLine(LogLevel::kInfo, summary);
}

OpenResult SourceEngine::Open(const std::string& path, const OpenOptions& options) {
  path_ = path;
  logFile_ = options.logfile;
  debug_ = options.debug;
  trace_ = options.trace;
  httpHeaders_ = options.http_headers;
  AppendLogLine(LogLevel::kInfo, "[MxfSource] mxfsource " MXFSRC_VERSION ", opening " + path_);
  {
    auto source = OpenByteSource(path, httpHeaders_);
    const int64_t cacheMb = options.read_cache >= 0 ? options.read_cache
                                                     : (IsNetworkSource(path) ? kAutoReadCacheNetworkMb
                                                                              : kAutoReadCacheLocalMb);
    if (source->IsOpen() && cacheMb > 0) {
      byteCache_ = std::make_shared<ByteCache>(path, httpHeaders_, source->Size(), source->SupportsRandomAccess(),
                                               static_cast<uint64_t>(cacheMb) << 20);
      reader_ = std::make_unique<MxfReader>(std::make_unique<CachingByteSource>(byteCache_, std::move(source)));
    } else {
      reader_ = std::make_unique<MxfReader>(std::move(source));
    }
  }
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

  // OpenOptions::strict: read-only checks, run before the index build so a
  // damaged file fails in a few reads instead of first paying for a
  // full-file bitstream scan. Same functions, same order as MxfExtractIndex
  // (core/index_policy.h), so both accept and refuse the same files.
  strict_ = options.strict;
  const IntegrityCheckResult integrity = CheckIntegrityBeforeIndex(*reader_, options.strict);
  if (integrity.error) return {OpenStatus::kDamagedFile, *integrity.error};
  if (!streamInfo_.hasVideo) return {OpenStatus::kParseError, "no video track found in Header Metadata"};

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
    // Never written to disk (see MxfExtractIndex for the separate host
    // function that does that, through the same BuildVideoIndex). Without
    // strict, a file with no index at all leaves videoIndex_ unset, which
    // SeekAndDecode treats as "no index available, use the
    // from-essence-element-0 fallback".
    VideoIndexResult built = BuildVideoIndex(*reader_, path_, httpHeaders_, streamInfo_, options.strict);
    if (built.error) return {OpenStatus::kDamagedFile, *built.error};
    if (built.index && !built.index->Empty()) {
      if (built.fromBitstreamScan) {
        AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": no Index Table Segment found; built a fallback index from raw "
                      "bitstream headers (" + std::to_string(built.index->GetEntries().size()) + " edit units)");
      }
      for (uint64_t partitionOffset : built.index->EssenceStartResyncs()) {
        AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": the end of the metadata/index declared by the partition at byte " +
                      std::to_string(partitionOffset) + " is not on a KLV boundary; found the next KLV by a resync");
      }
      videoIndex_ = std::move(built.index);
    }
  }

  // OpenOptions::verify: same check/repair as MxfExtractIndex(verify=true)
  // (core/index_policy.h), for a locally built and a fetched index alike.
  if (options.verify) {
    if (videoIndex_ && !videoIndex_->Empty()) {
      const IndexVerification verification =
          ApplyIndexVerification(path_, httpHeaders_, streamInfo_, options.strict, *videoIndex_,
                                 options.verify_overlapping);
      if (verification.error) return {OpenStatus::kDamagedFile, *verification.error};
      for (const std::string& warning : verification.warnings) {
        AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": " + warning);
      }
      AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": " + verification.summary);
    } else {
      AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": verify: this file has no index to verify");
    }
  }

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
  if (videoIndex_ && !videoIndex_->Empty()) {
    if (std::optional<std::string> error = CheckIndexCoverage(*videoIndex_, streamInfo_, integrity.finished)) {
      return {OpenStatus::kDamagedFile, *error};
    }
    // A file that got this far (strict=false, or a growing file in strict
    // mode) may declare more frames than its essence holds. Expose only the
    // indexed frames when the essence demonstrably ends after the last of
    // them -- otherwise the clip ends in decode errors and the audio, which
    // needs index coverage of every exposed frame, is dropped entirely. A file
    // whose index merely stops early while the essence goes on keeps its
    // declared frame count, as before.
    // If the last content package lacks some of its sound elements (cut off
    // mid-package), that frame goes too, so video and audio end together.
    const int64_t indexedFrames = videoIndex_->HighestCoveredEditUnit() + 1 - originSkip_;
    if (indexedFrames > 0 && indexedFrames < streamInfo_.numFrames) {
      std::vector<std::array<uint8_t, 4>> audioTrackNumbers;
      for (const AudioTrackInfo& t : streamInfo_.audioTracks) audioTrackNumbers.push_back(t.trackNumber);
      const EssenceEnd end = EssenceEndsAfter(*reader_, videoIndex_->GetEntries().back().byteOffset,
                                              streamInfo_.videoTrackNumber, audioTrackNumbers);
      const int64_t exposed = end.lastPackageComplete ? indexedFrames : indexedFrames - 1;
      if (end.ends && exposed > 0) {
        std::string why = " frames, but the essence ends after " + std::to_string(indexedFrames);
        if (!end.lastPackageComplete) why += ", the last one with sound elements missing";
        AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": Header Metadata declares " + std::to_string(streamInfo_.numFrames) +
                      why + "; exposing " + std::to_string(exposed) + " frames");
        streamInfo_.numFrames = exposed;
        streamInfo_.durationReliable = false;
      }
    }
  }
  // Raw, untrimmed essence-edit-unit-space frame count -- the coordinate
  // space videoIndex_'s own entries live in (see
  // BuildAudioSampleIndexFromIndexTable's doc comment). streamInfo_.numFrames
  // is now fully finalized (Header Metadata Duration, SourcePackage-Origin
  // override, the Index Table fallback above, or the clamp to the essence
  // end), so this is safe to derive.
  const int64_t rawFrameCount = originSkip_ + streamInfo_.numFrames;

  // OpenOptions::start_frame_hint: see its own doc comment. Clamp/ignore
  // (never a hard Open() failure) anything non-positive or at/past the
  // file's own frame count -- both are equivalent to "no hint", the field's
  // own default.
  if (options.start_frame_hint > 0 && options.start_frame_hint < streamInfo_.numFrames) {
    firstHostFrame_ = options.start_frame_hint;
  } else if (options.start_frame_hint > 0) {
    AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": start_frame hint " + std::to_string(options.start_frame_hint) +
                  " is out of range for a " + std::to_string(streamInfo_.numFrames) +
                  "-frame file, ignoring (starting at frame 0 as usual)");
  }

  videoDecoder_ = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                      static_cast<int>(streamInfo_.height), streamInfo_.pictureEssenceCoding);
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
      AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": audio configuration not supported (" +
                    streamInfo_.audioUnsupportedReason + "), continuing video-only");
    } else {
      audioSampleIndex_ = videoIndex_ ? BuildAudioSampleIndexFromIndexTable(*videoIndex_, streamInfo_, rawFrameCount)
                                       : std::nullopt;
      if (!audioSampleIndex_) {
        if (options.use_audio == UseAudioMode::kAlways) {
          return {OpenStatus::kUnsupportedCodec,
                  "audio not fully covered by the video Index Table (or invalid edit-rate/sample-rate metadata)"};
        }
        AppendLogLine(LogLevel::kWarning, "[MxfSource] " + path_ + ": failed to build audio sample index, continuing video-only");
      } else {
        exposeAudio_ = true;
        audioSampleOriginSkip_ = audioSampleIndex_->CumulativeSamplesBefore(originSkip_);
        AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": audio sample index built from video Index Table (" +
                      std::to_string(rawFrameCount) + " edit units, no essence scan)");
      }
    }
  }

  std::vector<std::array<uint8_t, 4>> audioTrackNumbersForWalker;
  if (exposeAudio_) {
    for (const AudioTrackInfo& t : streamInfo_.audioTracks) audioTrackNumbersForWalker.push_back(t.trackNumber);
  }
  // See OpenOptions::start_frame_hint / firstHostFrame_'s doc comments: when
  // a hint is in effect, resolve the nearest usable keyframe at or before
  // its target now (via ResolveSeekStart -- the exact same Index
  // Table-assisted lookup SeekAndDecode's own one-shot fallback uses), and
  // start walker_ reading from there instead of from the very first essence
  // element. DecodeThreadMain replays hintStartEditUnit_/hintReorderDelay_ to
  // discard-and-warm-up from that point up through the hint's own target
  // before it ever starts actually filling the ring buffer -- see there.
  uint64_t walkerStartOffset = streamInfo_.headerMetadataEnd;
  if (firstHostFrame_ > 0) {
    const SeekStartPoint seek = ResolveSeekStart(originSkip_ + firstHostFrame_);
    walkerStartOffset = seek.byteOffset;
    hintStartEditUnit_ = seek.editUnit;
    hintReorderDelay_ = seek.reorderDelay;
  }
  walker_ = std::make_unique<EssenceWalker>(*reader_, walkerStartOffset, streamInfo_.videoTrackNumber,
                                             streamInfo_.hasVideo, audioTrackNumbersForWalker, WalkerOptions());
  walkerAudioTracks_ = audioTrackNumbersForWalker;

  // buffer_frames=auto starts at the minimum and grows to its memory budget
  // right after the first frame (below), once the frame size is known.
  autoBuffer_ = options.buffer_frames <= 0;
  const int capacity = autoBuffer_ ? kAutoBufferMinFrames : options.buffer_frames;
  const int margin = options.eagerdecode > 0 ? options.eagerdecode : kAutoEagerFrames;
  eagerFrames_ = margin;
  backSeekPreroll_ = std::max(0, options.back_seek_preroll);
  // firstHostFrame_ (0 unless a start_frame hint is in effect) re-anchors
  // both ring buffers so their very first produced index is the hint's
  // target, not 0 -- see FrameBuffer's startIndex constructor param and
  // DecodeThreadMain, which produces ring index firstHostFrame_ first in
  // exactly the same way it produces ring index 0 first today.
  videoBuffer_ = std::make_unique<FrameBuffer<std::shared_ptr<NormalizedFrame>>>(capacity, margin, firstHostFrame_);
  if (exposeAudio_) {
    audioBuffer_ = std::make_unique<FrameBuffer<std::shared_ptr<AudioBlock>>>(capacity, margin, firstHostFrame_);
  }

  decodeThread_ = std::thread(&SourceEngine::DecodeThreadMain, this);

  // Peek() at firstHostFrame_ (0 unless hinted) rather than always 0 -- the
  // ring buffer's very first produced index moved there too (see just
  // above), so that's the index Open() needs to inspect for pixel format,
  // exactly like Peek(0, ...) already did for the un-hinted case.
  std::string peekError;
  if (videoBuffer_->Peek(firstHostFrame_, firstFrame_, &peekError) != FetchStatus::kOk) {
    return {OpenStatus::kDecodeError, "failed to decode first frame: " + peekError};
  }
  if (autoBuffer_) {
    int64_t frameBytes = 0;
    for (const auto& plane : firstFrame_->planeData) frameBytes += static_cast<int64_t>(plane.size());
    frameBytes = std::max<int64_t>(frameBytes, 1);
    const int64_t start = std::clamp<int64_t>(kAutoBufferBytes / frameBytes, kAutoBufferMinFrames,
                                              kAutoBufferMaxFrames);
    autoBufferMaxFrames_ = static_cast<int>(
        std::clamp<int64_t>(kAutoBufferMaxBytes / frameBytes, start, kAutoBufferGrowMaxFrames));
    GrowBuffers(static_cast<int>(start));
    AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": buffer_frames=auto: " + std::to_string(start) +
                  " frames (" + std::to_string(frameBytes / 1024) + " KiB each), may grow to " +
                  std::to_string(autoBufferMaxFrames_));
  }
  return {OpenStatus::kOk, ""};
}

namespace {
// What served the current thread's last GetVideoFrameImpl call, for the
// trace log (OpenOptions::trace).
thread_local const char* tl_servedBy = "";
thread_local bool tl_restarted = false;

int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Nothing served from the buffer for this long: nobody is reading at the
// producer's position (an editor after a seek), so a request away from it
// may move the producer at once. Requests from Prefetch threads, temporal
// filters or two positions read in turn come milliseconds apart.
constexpr int64_t kIdleBeforeRestartNs = 500000000;
}  // namespace

void SourceEngine::NoteBufferedServe(uint64_t request) {
  stats_.RecordBufferedServe();
  lastBufferedRequest_ = request;
  lastBufferedServeNs_ = SteadyNowNs();
  tl_servedBy = "buffer";
}

FetchStatus SourceEngine::GetVideoFrame(int64_t n, std::shared_ptr<NormalizedFrame>& out, std::string* errorOut) {
  if (!trace_) return GetVideoFrameImpl(n, out, errorOut);
  tl_servedBy = "?";
  tl_restarted = false;
  const auto start = std::chrono::steady_clock::now();
  const FetchStatus status = GetVideoFrameImpl(n, out, errorOut);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  char msBuf[32];
  std::snprintf(msBuf, sizeof(msBuf), "%.1f", ms);
  std::string line = "[MxfSource] trace: frame " + std::to_string(n) + " ";
  if (status == FetchStatus::kOk) {
    line += std::string("from ") + tl_servedBy + (tl_restarted ? " after a decoder restart" : "");
  } else {
    line += "failed (" + std::string(errorOut ? *errorOut : "") + ")";
  }
  line += ", " + std::string(msBuf) + " ms; buffer " + std::to_string(videoBuffer_->LowestValid()) + "-" +
          std::to_string(videoBuffer_->HighestProduced());
  AppendLogLine(LogLevel::kInfo, line);
  return status;
}

FetchStatus SourceEngine::GetVideoFrameImpl(int64_t n, std::shared_ptr<NormalizedFrame>& out, std::string* errorOut) {
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
  //
  // Bypassing only the one request wasn't enough, though: the producer
  // stayed where it was, so every following frame was still "far ahead"
  // and went through the same keyframe re-decode (real report: a script
  // starting with Trim(1250, ...) on long-GOP H.264 served 15 frames from
  // the buffer and 6027 via fallback). RepositionProducer moves the
  // producer to `n` instead; this request then waits in the buffer for the
  // restarted producer like any other, and the frames after it follow from
  // the buffer. SeekAndDecode remains for when that isn't possible.
  const uint64_t request = ++videoRequestSeq_;
  MaybeLogProgress();

  // A forward jump shorter than the buffer used to wait for the producer to
  // decode every frame up to it (real report: AvsPmod over HTTP, seek to
  // 3000 then play from 3200 waited 2.1 s; 250 frames ahead took 3.1 s vs
  // 0.5 s for a restart). When nobody has read from the buffer for a while
  // and the target's keyframe lies beyond the producer (FFMS2's rule:
  // restarting there decodes fewer frames than continuing), restart there.
  // Not while frames are being served (Prefetch, temporal filters, a
  // single-threaded encode of two positions within the buffer): those keep
  // the buffer they are reading from.
  {
    const int64_t hp = videoBuffer_->HighestProduced();
    if (n > hp + eagerFrames_ && n <= hp + videoBuffer_->Capacity() &&
        SteadyNowNs() - lastBufferedServeNs_.load() >= kIdleBeforeRestartNs) {
      const int preroll = std::min(backSeekPreroll_, videoBuffer_->Capacity() / 2);
      const int64_t restartAt = std::max<int64_t>(0, n - preroll);
      const int64_t keyframe = ResolveSeekStart(originSkip_ + restartAt).editUnit - originSkip_;
      if (keyframe > hp + 1) RepositionProducer(n, Jump::kNearAhead);
    }
  }

  if (n > videoBuffer_->HighestProduced() + videoBuffer_->Capacity()) RepositionProducer(n);
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
    tl_servedBy = "separate decode (fallback)";
    return FetchStatus::kOk;
  }

  FetchStatus status = videoBuffer_->Get(n, out, errorOut);
  if (status == FetchStatus::kOk) {
    NoteBufferedServe(request);
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

  // buffer_frames=auto: a request just below the window means the caller
  // needs more history than the buffer holds (stepping backwards, or
  // Prefetch spreading requests wider than the window) -- double it so the
  // next ones are served from the buffer. A far backward jump doesn't count;
  // more history wouldn't have caught it. Only while the window is full:
  // right after growing, the frames below it are still gone, and stepping
  // further back over them must not double the buffer again for each step.
  if (status == FetchStatus::kEvicted && autoBuffer_) {
    const int capacity = videoBuffer_->Capacity();
    const int64_t lowest = videoBuffer_->LowestValid();
    const bool full = videoBuffer_->HighestProduced() - lowest + 1 >= capacity;
    if (full && capacity < autoBufferMaxFrames_ && n >= lowest - capacity) {
      const int grown = std::min(capacity * 2, autoBufferMaxFrames_);
      GrowBuffers(grown);
      stats_.RecordGrowth();
      AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": frame " + std::to_string(n) +
                    " requested just below the buffer; buffer_frames=auto grows to " + std::to_string(grown));
    }
  }

  // A jump back below the buffer (Trim segments spliced out of order, Loop,
  // playing from a little before where an editor last seeked to) is the
  // Trim problem in reverse: the producer would stay where it was and every
  // following frame would be evicted too and re-decoded from its keyframe
  // (real report: seek to 3265 in AvsPmod, play from 3212 -- frames
  // 3212-3256 at ~0.4 s each over HTTP). Move the producer back -- but only
  // once nothing has read from the buffer for a few requests, however near
  // or far below it the request is. Requests still served from the buffer
  // in between mean someone keeps reading at the producer's position: a
  // temporal filter or Prefetch thread reaching just below it (auto growth
  // above handles that), or two far-apart positions read in turn
  // (Interleave or StackHorizontal of a clip and a Trim of it), where
  // moving the producer would throw that position away each time; those
  // stay on the fallback. The first few frames after a real jump also take
  // the fallback, a one-off cost -- unless nothing has come from the buffer
  // for kIdleBeforeRestartNs: then nobody is reading there (an
  // editor seeked, paused, then played from a little before the seek), and
  // the producer moves at once (real report: AvsPmod over HTTP stalled
  // ~2 s on those 4 fallback frames, each ~0.4 s, every time play was
  // pressed). Positions read in turn come milliseconds apart.
  if (status == FetchStatus::kEvicted) {
    constexpr uint64_t kIdleRequestsBeforeBackwardRestart = 4;
    const bool idle = request - lastBufferedRequest_.load() > kIdleRequestsBeforeBackwardRestart ||
                      SteadyNowNs() - lastBufferedServeNs_.load() >= kIdleBeforeRestartNs;
    if (idle && RepositionProducer(n, Jump::kBelow)) {
      status = videoBuffer_->Get(n, out, errorOut);
      if (status == FetchStatus::kOk) {
        NoteBufferedServe(request);
        return status;
      }
      if (status != FetchStatus::kEvicted && status != FetchStatus::kError) return status;
    }
  }

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
  tl_servedBy = "separate decode (fallback)";
  return FetchStatus::kOk;
}

SourceEngine::SeekStartPoint SourceEngine::ResolveSeekStart(int64_t essenceTarget) {
  if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
    // No frame depends on any other for these codecs -- every essence
    // element already *is* its own anchor. Every Index Table entry for an
    // intra-only codec is therefore always a "keyframe" (see
    // FlattenSegmentInto's CBR branch and IsKeyframeFlags for VBR), so
    // FindPrecedingKeyframe(essenceTarget) -- when the index covers it --
    // resolves to the target's *own* byte offset directly, with zero
    // backward walk and zero reorder delay. Consulting it turns a caller's
    // seek into an O(1) direct jump instead of an O(target) skip loop,
    // which -- even though it never decodes the skipped frames -- still
    // pays for reading every one of their full compressed payloads off
    // disk/network on its way to the target (SkipNextVideoElement's own
    // wantData=false only removes that cost when this direct jump isn't
    // available). Real bug found on a real 93GB XAVC-Intra file over an SMB
    // share: seeking to frame 5000 with no index-assisted jump dragged
    // several GB of essence data across the network merely to skip past
    // it. Absent/inapplicable index (e.g. a gap, or no Index Table Segment
    // at all) -> unchanged from-essence-element-0 behavior.
    uint64_t startOffset = streamInfo_.headerMetadataEnd;
    int64_t startEditUnit = 0;
    if (videoIndex_) {
      if (auto seekPoint = videoIndex_->FindPrecedingKeyframe(essenceTarget)) {
        startOffset = seekPoint->byteOffset;
        startEditUnit = seekPoint->editUnit;
        stats_.RecordIndexAssistedSeek();
      }
    }
    return {startOffset, startEditUnit, 0};
  }

  // GOP-based codec (e.g. XDCAM long-GOP MPEG-2, XAVC-L): consult the Index
  // Table-derived lookup (mxf/index_table.h), if available, for a byte
  // offset closer than essence element 0 -- FindPrecedingKeyframe only ever
  // returns an actual I-frame's offset, so restarting a brand-new decoder
  // there is valid; startEditUnit is handed back alongside startOffset so
  // a caller's decode-forward loop knows exactly how many more frames it
  // still needs (rather than needing to infer it from the byte offset).
  // Absent/inapplicable index -> unchanged from-essence-element-0 behavior.
  uint64_t startOffset = streamInfo_.headerMetadataEnd;
  int64_t startEditUnit = 0;
  // Decoding straight through to `essenceTarget` from a mid-stream keyframe
  // restart would land some number of edit units too far forward -- a
  // decoder restarted fresh needs its B-picture reordering pipeline to fill
  // before its Nth output genuinely corresponds to input element N, unlike
  // decoding continuously from edit unit 0 (this engine's other decode
  // paths: DecodeThreadMain, and this function's own startEditUnit==0
  // fallback), which is exactly as warmed-up at any point as the main
  // pipeline itself and needs no compensation at all. SeekAndDecode's own
  // `iterations` (computed there, once startEditUnit/reorderDelay are
  // final) already subtracts this out; real bug found via content
  // comparison against an independent ffmpeg decode: counting outputs
  // unconditionally landed 2 edit units too far forward on a real long-GOP
  // MPEG-2 camera file -- AVCodecContext::delay wasn't populated for that
  // codec and per-packet pts weren't reliably preserved through avcodec's
  // own reordering (confirmed again directly: explicitly setting PTS per
  // packet and reading it back off the decoded frame is *not* reliably
  // preserved through a real H.264 decoder's own internal reordering
  // either, even though the same technique round-trips correctly through a
  // real MPEG-2 encode/decode -- codec-specific, and not something to
  // build a general fix on), so neither could recover this number from the
  // decoder itself. See EssenceIndex::EstimateReorderDelay for how it's
  // derived from the index's own flags instead.
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

    // Open GOP: this GOP's own leading picture(s) may reference the
    // *previous* GOP's anchor frame, not anything within this GOP -- a
    // decoder restarted fresh at startEditUnit alone would be missing that
    // reference (verified against a real file: every GOP's leading two B
    // pictures carry a KeyFrameOffset pointing at the previous GOP's own
    // keyframe, not this one). Extending the restart back exactly one GOP
    // is sufficient to satisfy *this* GOP's leading pictures, regardless of
    // whether the GOP backed up to has the identical issue itself (its own
    // keyframe is an intra frame, self-sufficient by definition) -- but its
    // own leading pictures' *correctness* was wrongly assumed to not matter
    // here (see CountUndecodableLeadingPictures below for why it actually
    // does).
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
  //
  // CountUndecodableLeadingPictures(startEditUnit) folded in here too: if
  // the NeedsPreviousGop backup above just fired, startEditUnit's own
  // leading picture(s) reference an even-earlier GOP this restart never
  // decodes -- and, verified directly (extracted the exact raw
  // elementary-stream bytes SendAndReceive feeds avcodec for a real file's
  // true final GOP and fed them to a bare, independent avcodec instance),
  // avcodec doesn't error on these, it silently produces *no output frame
  // at all* for them, rather than emitting wrong-but-present content. The
  // old assumption ("its own correctness doesn't matter, only that
  // decoding it populates the reference this GOP's leading pictures need",
  // see the backup step above) was wrong specifically because of this: a
  // silently-dropped output permanently shifts every later output backward
  // by one position relative to naive one-input-per-output counting, the
  // same net effect on SeekAndDecode's `iterations` math as genuine reorder
  // delay, hence folded into the same value here. For an interior target
  // this just makes SeekAndDecode consume a couple of extra, otherwise-
  // unaccounted-for essence elements past the target with no visible
  // effect (there's always more essence past an interior target to absorb
  // it); for a target sitting at the file's true final essence element
  // there is no such slack, and the uncompensated version of this ran
  // `iterations` two calls too many, hitting genuine end-of-essence instead
  // of ever reaching the target at all -- the real, confirmed cause of the
  // "requested frame fell outside the ring buffer window ... fallback
  // re-decode also failed to reach it" failure on a real file's true final
  // chunk.
  const int reorderDelay = videoIndex_ ? (videoIndex_->EstimateReorderDelay(startEditUnit) +
                                           videoIndex_->CountUndecodableLeadingPictures(startEditUnit))
                                        : 0;
  return {startOffset, startEditUnit, reorderDelay};
}

std::unique_ptr<MxfReader> SourceEngine::OpenReader() const {
  if (byteCache_) return std::make_unique<MxfReader>(std::make_unique<CachingByteSource>(byteCache_, nullptr));
  return std::make_unique<MxfReader>(path_, httpHeaders_);
}

std::shared_ptr<NormalizedFrame> SourceEngine::SeekAndDecode(int64_t target) {
  const auto freshReaderOwner = OpenReader();
  MxfReader& freshReader = *freshReaderOwner;
  if (!freshReader.IsOpen()) return nullptr;

  auto freshDecoder = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                          static_cast<int>(streamInfo_.height), streamInfo_.pictureEssenceCoding);
  if (!freshDecoder) return nullptr;

  auto frame = std::make_shared<NormalizedFrame>();

  // Host frame index `target` maps to essence video element index
  // `originSkip_ + target` -- see MxfStreamInfo::origin / originSkip_.
  const int64_t essenceTarget = originSkip_ + target;
  const SeekStartPoint seek = ResolveSeekStart(essenceTarget);

  if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
    // EssenceWalker is constructed here (not shared with the GOP branch
    // below) since it holds a reference member and can't be reassigned
    // once the branch's own start offset is known. Video-only fallback --
    // audio has its own independent fresh-reader path, FreshReadAudioBlock.
    EssenceWalker freshWalker(freshReader, seek.byteOffset, streamInfo_.videoTrackNumber, streamInfo_.hasVideo, {},
                            WalkerOptions());
    for (int64_t i = seek.editUnit; i < essenceTarget; ++i) {
      if (!SkipNextVideoElement(freshWalker)) return nullptr;  // ran out of essence before reaching target
    }
    // The loop above discarded every video element between seek.editUnit
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

  const int64_t iterations = std::max<int64_t>(1, essenceTarget - seek.editUnit - seek.reorderDelay + 1);
  const int64_t adjustedTarget = seek.editUnit + iterations - 1;

  EssenceWalker freshWalker(freshReader, seek.byteOffset, streamInfo_.videoTrackNumber, streamInfo_.hasVideo, {},
                            WalkerOptions());
  bool essenceExhausted = false;
  EssenceElement scratch;
  for (int64_t i = seek.editUnit; i <= adjustedTarget; ++i) {
    if (!DecodeNextFrame(freshWalker, *freshDecoder, essenceExhausted, scratch, *frame, nullptr)) return nullptr;
  }
  return frame;
}

void SourceEngine::DecodeThreadMain() {
  int64_t start = firstHostFrame_;
  bool fromOpen = true;
  uint64_t videoGen = videoBuffer_->Generation();
  uint64_t audioGen = audioBuffer_ ? audioBuffer_->Generation() : 0;
  for (;;) {
    ProduceFrom(start, fromOpen, videoGen, audioGen);
    if (trace_) {
      std::string how;
      if (restartRequested_) {
        how = "a restart elsewhere was requested";
      } else if (videoBuffer_->IsShutdown()) {
        how = "shutdown";
      } else if (auto err = videoBuffer_->ErrorMessage()) {
        how = "error: " + *err;
      } else if (videoBuffer_->EndOfStreamReached()) {
        how = "end of stream";
      } else {
        how = "stopped";
      }
      AppendLogLine(LogLevel::kInfo, "[MxfSource] trace: decoder run from frame " + std::to_string(start) +
                                         " ended (" + how + "), buffer " +
                                         std::to_string(videoBuffer_->LowestValid()) + "-" +
                                         std::to_string(videoBuffer_->HighestProduced()));
    }
    // Production ended (end of stream, error, shutdown) or a consumer asked
    // for a restart elsewhere (RepositionProducer). Either way, wait for the
    // next restart or the engine shutting down.
    std::unique_lock<std::mutex> lock(repositionMutex_);
    repositionCv_.wait(lock, [&] { return stopProducer_ || pendingRestart_ >= 0; });
    if (stopProducer_) return;
    start = pendingRestart_;
    pendingRestart_ = -1;
    restartRequested_ = false;
    // Read under repositionMutex_, which RepositionProducer holds while it
    // resets both buffers: these are exactly the generations of that reset.
    videoGen = videoBuffer_->Generation();
    audioGen = audioBuffer_ ? audioBuffer_->Generation() : 0;
    fromOpen = false;
  }
}

void SourceEngine::GrowBuffers(int newCapacity) {
  videoBuffer_->Grow(newCapacity);
  if (audioBuffer_) audioBuffer_->Grow(newCapacity);
}

bool SourceEngine::RepositionProducer(int64_t hostFrame, Jump jump) {
  if (hostFrame < 0 || hostFrame >= streamInfo_.numFrames) return false;
  int64_t start = hostFrame;
  {
    std::lock_guard<std::mutex> lock(repositionMutex_);
    if (stopProducer_) return false;
    // Another consumer thread may have moved the producer in the meantime
    // (AviSynth's Prefetch asks for several neighbouring frames at once).
    const int capacity = videoBuffer_->Capacity();
    bool alreadyMoved = false;
    switch (jump) {
      case Jump::kFarAhead:
        alreadyMoved = hostFrame <= videoBuffer_->HighestProduced() + capacity;
        break;
      case Jump::kBelow:
        alreadyMoved = hostFrame >= videoBuffer_->LowestValid();
        break;
      case Jump::kNearAhead:
        // Another thread's restart just now covers it (its window starts at
        // or below hostFrame), or the producer caught up meanwhile.
        alreadyMoved = hostFrame <= videoBuffer_->HighestProduced() + eagerFrames_ ||
                       (hostFrame >= videoBuffer_->LowestValid() &&
                        SteadyNowNs() - lastRestartNs_.load() < kIdleBeforeRestartNs);
        break;
    }
    if (alreadyMoved) return true;
    // Restart a few frames early (OpenOptions::back_seek_preroll): a
    // temporal filter after the jump (e.g. TemporalSoften radius 3 behind
    // Trim) also asks for the frames just before it, and each of those would
    // otherwise be a separate keyframe re-decode (measured: ~250 ms each on
    // UHD long-GOP H.264). Usually free, since decoding starts from the
    // GOP's keyframe anyway.
    start = std::max<int64_t>(0, hostFrame - std::min(backSeekPreroll_, capacity / 2));
    if (audioBuffer_) audioBuffer_->Reset(start);
    videoBuffer_->Reset(start);
    pendingRestart_ = start;
    restartRequested_ = true;
    lastRestartNs_ = SteadyNowNs();
  }
  repositionCv_.notify_all();
  stats_.RecordReposition();
  tl_restarted = true;
  AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": frame " + std::to_string(hostFrame) +
                (jump == Jump::kBelow       ? " requested below the buffer, behind"
                 : jump == Jump::kNearAhead ? " requested after a pause, its keyframe beyond"
                                            : " requested far ahead of") +
                " the decode position; restarting the decoder at frame " +
                std::to_string(start));
  return true;
}

// One run of the background producer, from host frame `startHostFrame` until
// the end of the stream, an error, shutdown, or a restart request. The
// first run (`fromOpen`) uses the walker/decoder Open() prepared (from
// essence element 0, or from the start_frame hint); a restart creates new
// ones at the keyframe ResolveSeekStart picks for the new position and warms
// up exactly like a start_frame hint. `videoGen`/`audioGen` are the buffer
// generations this run fills (see FrameBuffer::Reset): once a consumer has
// reset a buffer again, everything this run still puts there is dropped.
void SourceEngine::ProduceFrom(int64_t startHostFrame, bool fromOpen, uint64_t videoGen, uint64_t audioGen) {
  bool essenceExhausted = false;
  EssenceElement scratch;

  // See OpenOptions::start_frame_hint / firstHostFrame_'s doc comments.
  // false (the default, no hint given) keeps every block below byte-for-
  // byte identical to before this was added; true replaces the ordinary
  // Origin-skip discard just below with the hinted discard/warm-up block
  // further down instead (walker_ already starts at hintStartEditUnit_, a
  // keyframe near the hint's own target, resolved once in Open()).
  const bool hinted = !fromOpen || startHostFrame > 0;
  int64_t hintStartEditUnit = hintStartEditUnit_;
  int hintReorderDelay = hintReorderDelay_;
  if (!fromOpen) {
    const SeekStartPoint seek = ResolveSeekStart(originSkip_ + startHostFrame);
    hintStartEditUnit = seek.editUnit;
    hintReorderDelay = seek.reorderDelay;
    if (trace_) {
      AppendLogLine(LogLevel::kInfo, "[MxfSource] trace: decoder run starts at frame " +
                                         std::to_string(startHostFrame) + ", decoding from keyframe " +
                                         std::to_string(seek.editUnit - originSkip_));
    }
    walker_.reset();
    walker_ = std::make_unique<EssenceWalker>(*reader_, seek.byteOffset, streamInfo_.videoTrackNumber,
                                               streamInfo_.hasVideo, walkerAudioTracks_, WalkerOptions());
    videoDecoder_ = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                        static_cast<int>(streamInfo_.height), streamInfo_.pictureEssenceCoding);
    if (!videoDecoder_) {
      const std::string err = "failed to create a video decoder for the restart at frame " +
                              std::to_string(startHostFrame);
      videoBuffer_->SignalError(err, videoGen);
      if (audioBuffer_) audioBuffer_->SignalError(err, audioGen);
      return;
    }
  }

  // Discard the leading `originSkip_` video essence elements (Origin
  // pre-roll/trim, see MxfStreamInfo::origin) before host frame 0 is ever
  // produced, so ring-buffer index 0 lines up with essence element
  // `originSkip_`. Mirrors SeekAndDecode's intra-only-vs-GOP branching:
  // intra-only codecs can skip the raw bytes without decoding them at all;
  // GOP-based codecs must actually decode (and discard) each one, since a
  // later frame may depend on it as a reference. Skipped entirely when
  // hinted -- see above.
  if (!hinted && originSkip_ > 0) {
    const std::string originError = "MXF essence ended before reaching the Origin offset (" +
                                     std::to_string(originSkip_) + ")";
    if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
      for (int64_t i = 0; i < originSkip_; ++i) {
        if (!SkipNextVideoElement(*walker_)) {
          videoBuffer_->SignalError(originError, videoGen);
          return;
        }
      }
    } else {
      NormalizedFrame discard;
      for (int64_t i = 0; i < originSkip_; ++i) {
        if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, scratch, discard, nullptr)) {
          videoBuffer_->SignalError(originError, videoGen);
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
  //
  // When hinted, the reorder delay is instead based at hintStartEditUnit_
  // (the keyframe walker_ actually starts from) rather than edit unit 0 --
  // see the hinted discard/warm-up block below, which pre-fills `pending`
  // with real lead-in frames from that keyframe before production ever
  // starts, so the hinted case doesn't need (and doesn't get) the
  // best-effort-repeat gap described above at all.
  const int reorderDelay = hinted
      ? hintReorderDelay
      : ((videoIndex_ && !IsIntraOnlyCodec(streamInfo_.videoCodec)) ? videoIndex_->EstimateReorderDelay(0) : 0);
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

  // Hinted jump-start: walker_ already begins at hintStartEditUnit_ (a
  // keyframe at or before essenceTarget = originSkip_ + firstHostFrame_,
  // resolved once in Open() via ResolveSeekStart() -- the exact same Index
  // Table-assisted lookup SeekAndDecode's own one-shot fallback uses).
  // Decode-and-discard from there through essenceTarget - 1 using the SAME
  // pending-deque reorder-delay mechanics the main loop below uses for its
  // own from-file-start warm-up, so production genuinely starts already
  // warmed up: unlike essence element 0 (no earlier keyframe exists to draw
  // real reorder-delay runway from, hence the main loop's own best-effort
  // placeholder repeat for its first `reorderDelay` ring indices), a hinted
  // start always has real lead-in frames available back to
  // hintStartEditUnit_ -- ResolveSeekStart() only ever settles on a keyframe
  // with enough runway before essenceTarget (see its own backward walk), so
  // ring index firstHostFrame_ itself gets fully correct content with no
  // placeholder needed, not just the frames after it.
  //
  // Audio is simply discarded here (nullptr audioElementsOut), matching the
  // ordinary Origin-skip discard above -- neither path attempts
  // reorder-delay compensation for audio during its own discard phase;
  // both leave that (already independently accepted, see pendingAudio's own
  // doc comment above) warm-up gap to pendingAudio's first few Put() calls
  // in the main loop below.
  if (hinted) {
    const int64_t essenceTarget = originSkip_ + startHostFrame;
    const std::string hintError =
        "MXF essence ended before reaching the start_frame hint's target position (" +
        std::to_string(startHostFrame) + ")";
    if (IsIntraOnlyCodec(streamInfo_.videoCodec)) {
      for (int64_t i = hintStartEditUnit; i < essenceTarget; ++i) {
        if (restartRequested_ || videoBuffer_->IsShutdown()) return;
        if (!SkipNextVideoElement(*walker_)) {
          videoBuffer_->SignalError(hintError, videoGen);
          return;
        }
      }
    } else {
      for (int64_t i = hintStartEditUnit; i < essenceTarget; ++i) {
        if (restartRequested_ || videoBuffer_->IsShutdown()) return;
        auto discardFrame = std::make_shared<NormalizedFrame>();
        std::vector<EssenceElement> audioScratch;
        if (!DecodeNextFrame(*walker_, *videoDecoder_, essenceExhausted, scratch, *discardFrame,
                             (audioBuffer_ && !audioFailed) ? &audioScratch : nullptr)) {
          videoBuffer_->SignalError(hintError, videoGen);
          return;
        }
        pending.push_back(std::move(discardFrame));
        if (static_cast<int>(pending.size()) > reorderDelay) pending.pop_front();  // discard -- before the target

        // Mirror `pending`'s own warm-up above: this loop's video-only
        // discard isn't the only thing with real lead-in content available
        // back to hintStartEditUnit_ -- the audio walked past here is just
        // as real, and feeding it into pendingAudio now is what lets ring
        // index firstHostFrame_'s audio come out already delay-compensated,
        // instead of falling into the "still warming up" repeat branch
        // below (which is only meant for the genuinely-unreachable gap at
        // essence element 0, see pendingAudio's own doc comment).
        if (audioBuffer_ && !audioFailed) {
          auto block = std::make_shared<AudioBlock>();
          std::string audioErr;
          if (!InterleaveAudioEditUnit(audioScratch, streamInfo_.audioTracks, *block, &audioErr)) {
            // Only ever expected on this loop's very *first* iteration
            // (i == hintStartEditUnit_), and only best-effort-recoverable
            // there: walker_ starts reading at exactly the resolved
            // seek-start video keyframe's own byte offset (ResolveSeekStart),
            // not at that edit unit's true interleave-group boundary -- if
            // this file's operational pattern places audio elements *before*
            // video within an edit unit, that leading edit unit's own audio
            // was positioned earlier in the file than this starting offset
            // and is structurally unreachable, exactly the same kind of gap
            // `pending`'s own warm-up already accepts for VIDEO at essence
            // element 0 (see its doc comment). Previously treated as fatal
            // for the source's entire remaining lifetime (audioFailed
            // latched permanently), which silently disabled real audio
            // production for every edit unit *after* this one too, not just
            // this genuinely-unreachable leading one -- confirmed via real
            // testing (HANDOFF_qtgmc_frame0_fallback_decode_2026-09-09.md's
            // "UPDATE 2026-09-09 (later still)" section) to fall back to
            // FreshReadAudioBlock's one-shot fresh-HTTP-connection-per-call
            // path for literally every subsequent audio block of a hinted
            // chunk, ~40s/chunk wasted over a real network for a single
            // boundary edit unit's worth of genuinely missing audio.
            //
            // No silence is synthesized here -- deliberately just a no-op
            // (nothing pushed to pendingAudio at all; an earlier version of
            // this fix's own log message wrongly said "treating as
            // silence", which was never accurate -- fixed). This leaves
            // pendingAudio one entry short of `reorderDelay` at this single
            // point; that's harmless, not a gap in served audio, because the
            // window still reaches its normal full depth well before this
            // (much longer) warm-up loop ends regardless -- the skipped
            // entry would have been pushed out and discarded by later real
            // entries before any consumer ever saw it, fix or no fix.
            // Verified directly, not just argued: decoded the same real
            // chunk with the pre-fix (slow-but-correct-via-fallback) and
            // fixed builds and diffed the two output audio tracks --
            // byte-for-byte identical. Logged once (not via SignalError,
            // which would mark the whole buffer permanently errored) so
            // this remains visible rather than fully silent.
            AppendLogLine(LogLevel::kInfo, "[MxfSource] " + path_ + ": leading audio edit unit near the start_frame hint's own "
                          "seek-start point is unreachable (" + audioErr + ") -- skipping it, no effect on served "
                          "audio (absorbed by the reorder-delay warm-up window), production continues normally");
          } else {
            pendingAudio.push_back(std::move(block));
            if (static_cast<int>(pendingAudio.size()) > reorderDelay) pendingAudio.pop_front();
          }
        }
      }
    }
  }

  for (;;) {
    if (restartRequested_) return;
    const int64_t next = videoBuffer_->NextIndex();
    if (next >= streamInfo_.numFrames) {
      videoBuffer_->SignalEndOfStream(videoGen);
      if (audioBuffer_) audioBuffer_->SignalEndOfStream(audioGen);
      return;
    }
    // audioBuffer_ deliberately has no unconditional WaitForCapacity gate of
    // its own: it's produced by this same loop in lockstep with video, at
    // exactly video's own pace -- gating on audioBuffer_'s independent
    // watermark as well would stall video production (and thus the whole
    // thread, since both come from one shared walker) the moment audio's
    // watermark fell behind video's, e.g. a host that reads video frames
    // without ever calling GetAudioSamples (or reads audio in far
    // larger/rarer chunks) -- audio's watermark_ starts at 0 and margin_ can
    // be as small as 1, so without this, DecodeThreadMain would deadlock
    // waiting for an audio consumer that may never come. Put() below still
    // evicts audioBuffer_'s oldest entry once full on its own (eviction
    // doesn't depend on WaitForCapacity, see FrameBuffer::Put) -- a
    // genuinely slow/absent audio consumer just falls back to
    // FreshReadAudioBlock for anything that fell outside the window, exactly
    // like a slow video consumer already falls back to SeekAndDecode.
    //
    // But the reverse case needs its own gate too: a caller that only ever
    // reads AUDIO and never calls GetVideoFrame (e.g. extracting just the
    // audio track) leaves videoBuffer_'s watermark_ parked at its initial
    // value forever, so an unconditional videoBuffer_->WaitForCapacity(next)
    // would block permanently once video's own eager-readahead margin filled
    // -- capping audio at whatever was produced during that initial window
    // and hanging every later audio request. Real bug, found via a hung
    // audio-only ffmpeg extraction of this same file. Poll video's gate with
    // a short timeout and also accept audioBuffer_ still wanting more as an
    // alternative reason to keep producing -- either consumer's real
    // progress is enough to justify continuing.
    for (;;) {
      if (videoBuffer_->WaitForCapacityOrTimeout(next, std::chrono::milliseconds(20))) break;
      if (videoBuffer_->IsShutdown() || restartRequested_) return;
      if (audioBuffer_ && audioBuffer_->WantsMore(next)) break;
    }

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
      videoBuffer_->SignalError(err, videoGen);
      if (audioBuffer_) audioBuffer_->SignalError(err, audioGen);
      return;
    }
    pending.push_back(std::move(frame));

    if (audioBuffer_ && !audioFailed) {
      auto block = std::make_shared<AudioBlock>();
      std::string audioErr;
      if (!InterleaveAudioEditUnit(audioScratch, streamInfo_.audioTracks, *block, &audioErr)) {
        audioBuffer_->SignalError("audio decode error near ring index " + std::to_string(next) + ": " + audioErr, audioGen);
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
    recycled = videoBuffer_->Put(next, std::move(outputFrame), videoGen).value_or(nullptr);

    if (audioBuffer_ && !audioFailed) {
      std::shared_ptr<AudioBlock> outputAudio;
      if (static_cast<int>(pendingAudio.size()) > reorderDelay) {
        outputAudio = std::move(pendingAudio.front());
        pendingAudio.pop_front();
      } else if (!pendingAudio.empty()) {
        // Still warming up. Unlike video (pending.front() above), which
        // genuinely has no correct content for these leading ring indices
        // (the true decode-order source would be a negative essence
        // position -- nothing exists before element 0, so a repeat really
        // is the best video can do), PCM audio has zero reordering of its
        // own: this iteration's own just-decoded block (pendingAudio.back()
        // -- pushed immediately above, nothing else pushes between there and
        // here) is genuinely this ring index's real audio content. Serving
        // it directly instead of repeating an older buffered block (which
        // .front() would do here) fixes real, repeated-audio output for the
        // leading reorderDelay ring indices of any non-hinted decode from
        // essence element 0 -- confirmed via a real file, byte-compared
        // against ffmpeg's own independent MXF demux (which needs no such
        // compensation for audio at all). Once past this warm-up window
        // (the `if` branch above), popping the oldest buffered block is
        // still exactly the delay compensation audio/video sync needs (see
        // this function's own comment on why pendingAudio mirrors `pending`
        // at all) -- only this leading window's fallback changes.
        outputAudio = pendingAudio.back();
      }
      if (outputAudio) audioBuffer_->Put(next, std::move(outputAudio), audioGen);
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
    if (available <= 0) {
      // audioSampleIndex_'s per-edit-unit sample count is a closed-form
      // estimate (BuildAudioSampleIndexFromIndexTable's floor(i*sampleRate*
      // den/num) cadence, e.g. NTSC's 1601/1602 pattern) -- a real file's
      // actual decoded block for this edit unit can disagree by a sample or
      // two if its own rounding phase differs from the formula's. When that
      // happens, `loc->sampleOffsetInEditUnit` lands at or past this edit
      // unit's real `sampleCount`, and every field above (loc, block,
      // available) would be identical on the next iteration too --
      // `remaining`/`hostSamplePos` must still advance, or this loop never
      // terminates (confirmed via a live hang: Locate() called forever with
      // the exact same samplePos). Treat the unreachable sample(s) as
      // silence and move on one sample at a time -- self-correcting within
      // an edit unit or two, same as the analogous hinted-audio warm-up gap.
      const int64_t skip = std::min<int64_t>(remaining, 1);
      std::memset(dst, 0, static_cast<size_t>(skip) * bytesPerFrame);
      dst += skip * bytesPerFrame;
      hostSamplePos += skip;
      remaining -= skip;
      continue;
    }
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
  // go straight to the fresh-reader fallback instead -- unless the video
  // producer is far behind too (an audio-only consumer, or audio requested
  // ahead of video): then move the producer there, like GetVideoFrame does.
  // RepositionProducer checks the *video* position, so audio that only lags
  // because its production failed doesn't keep restarting video.
  if (hostEditUnit > audioBuffer_->HighestProduced() + audioBuffer_->Capacity()) RepositionProducer(hostEditUnit);
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
  const auto freshReaderOwner = OpenReader();
  MxfReader& freshReader = *freshReaderOwner;
  if (!freshReader.IsOpen()) return nullptr;

  std::vector<std::array<uint8_t, 4>> audioTrackNumbers;
  audioTrackNumbers.reserve(streamInfo_.audioTracks.size());
  for (const AudioTrackInfo& t : streamInfo_.audioTracks) audioTrackNumbers.push_back(t.trackNumber);

  // hasVideo=false: video (and any other track) is skipped over cheaply via
  // EssenceWalker's generic unmatched-KLV path, no different from any other
  // untracked track -- this fallback only ever needs audio.
  EssenceWalker freshWalker(freshReader, byteOffset, /*videoTrackNumber=*/{}, /*hasVideo=*/false, audioTrackNumbers,
                            WalkerOptions());

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
