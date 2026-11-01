#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "codec/decoder.h"
#include "codec/pcm_interleave.h"
#include "core/frame_buffer.h"
#include "core/options.h"
#include "mxf/audio_sample_index.h"
#include "mxf/essence_walker.h"
#include "mxf/header_parser.h"
#include "mxf/index_table.h"
#include "mxf/klv.h"

namespace mxfsrc {

// Tracks how many GetVideoFrame() calls were satisfied from the ring buffer
// (the expected/fast path) versus how many fell outside its window and had
// to be served by SeekAndDecode()'s fallback full re-decode-from-start
// (rare/slow; only happens for genuine out-of-order/backward access, e.g. a
// real random seek). Logged as a summary when the engine is destroyed --
// see plan notes: the user wants visibility into how often the fallback
// path is hit and exactly which frames needed it, to gauge whether
// buffer_frames or eagerdecode need tuning for a given filter graph.
struct ServeStats {
  mutable std::mutex mutex;
  int64_t bufferedCount = 0;
  std::vector<int64_t> nonBufferedFrames;
  // How many SeekAndDecode() calls found a usable Index Table Segment entry
  // (mxf/index_table.h) and started decoding from a nearby keyframe instead
  // of essence element 0 -- see SourceEngine::videoIndex_.
  int64_t indexAssistedSeeks = 0;

  void RecordBufferedServe() {
    std::lock_guard<std::mutex> lock(mutex);
    ++bufferedCount;
  }
  void RecordNonBufferedServe(int64_t n) {
    std::lock_guard<std::mutex> lock(mutex);
    nonBufferedFrames.push_back(n);
  }
  void RecordIndexAssistedSeek() {
    std::lock_guard<std::mutex> lock(mutex);
    ++indexAssistedSeeks;
  }
};

enum class OpenStatus { kOk, kFileError, kParseError, kUnsupportedCodec, kDecodeError, kDamagedFile };

struct OpenResult {
  OpenStatus status = OpenStatus::kFileError;
  std::string error;
};

// Host-agnostic decode engine: owns the MXF reader, header info, essence
// walker, video decoder, and a background thread that decodes forward into
// a FrameBuffer sized by OpenOptions::buffer_frames. Both host adapters
// (Phase 5/6) wrap this with a thin per-frame GetFrame(n)-shaped API and
// their own MT-mode/filter-mode declaration; this class is the single
// source of thread-safety truth (see plan's "core/" section) so neither
// adapter needs its own locking around the engine.
//
// Audio (when ExposesAudio()) is decoded on the same background thread as
// video -- the essence is physically interleaved per edit unit, so a
// separate audio reader/thread would re-read (and on HTTP sources,
// re-transfer) every video KLV a second time just to skip it. Every audio
// track discovered is summed into one interleaved output stream (see
// mxf/header_parser.h's MxfStreamInfo::audioTracks); whether that's actually
// exposed depends on OpenOptions::use_audio and whether the file's audio
// configuration is one this plugin supports (see UseAudioMode, ul_tables.h
// CodecFamily::kPcm-only for now).
class SourceEngine {
 public:
  SourceEngine();
  ~SourceEngine();
  SourceEngine(const SourceEngine&) = delete;
  SourceEngine& operator=(const SourceEngine&) = delete;

  OpenResult Open(const std::string& path, const OpenOptions& options = {});

  const MxfStreamInfo& StreamInfo() const { return streamInfo_; }

  // Frame 0, decoded synchronously as part of Open() (both host adapters
  // need to know its actual pixel format -- a property of the decoded
  // output, not of Header Metadata alone -- before they can even construct
  // their VideoInfo/VSVideoInfo). Valid once Open() returns kOk. Fetched via
  // FrameBuffer::Peek() specifically so it does NOT advance the ring
  // buffer's consumer watermark -- see plan Phase 5 notes: doing so via the
  // normal Get() path let the producer race ahead and evict this very frame
  // before any real GetVideoFrame(0) call arrived.
  const NormalizedFrame& FirstFrame() const { return *firstFrame_; }

  // A decoded frame's field order: the decoder's own report if it has one,
  // otherwise the MXF picture descriptor's (FrameLayout/FieldDominance). Only
  // descriptor combinations verified against real files map to a value:
  // FullFrame/SegmentedFrame = progressive, SeparateFields/MixedFields with
  // field 1 dominant = TFF (XDCAM HD422, XAVC, D-10, K2 SD agree with the
  // bitstream); anything else stays kUnknown.
  FieldOrder FieldOrderOf(const NormalizedFrame& frame) const;
  // The clip's field order, from its first decoded frame -- for hosts that
  // take one value per clip (AviSynth's VideoInfo/GetParity).
  FieldOrder ClipFieldOrder() const { return FieldOrderOf(*firstFrame_); }

  // Blocks until frame `n` is decoded or the stream ends/errors. If `n` has
  // fallen behind the ring buffer's window (evicted, e.g. a genuine
  // backward seek -- forward waits are always satisfied normally and never
  // hit this), falls back to SeekAndDecode() rather than failing: this
  // plugin is designed for forward-only playback, but the user asked for
  // random-seek support wherever reasonably easy to add. See
  // FrameBuffer::Get for the ring-buffer status semantics this wraps.
  // `out` receives a shared_ptr rather than a copied NormalizedFrame: the
  // ring buffer keeps its own shared_ptr to the same frame (a repeat
  // request for an already-produced index -- which real hosts legitimately
  // make, see plan Phase 5 notes -- must keep working), so FrameBuffer::Get()
  // can only ever copy the *pointer*, not move the data out. Storing
  // NormalizedFrame by value in the ring buffer made that copy a full deep
  // copy of every decoded frame (multi-MB for HD video) on every single
  // Get() call -- caught via real-file performance benchmarking (measured
  // ~4x slower than ffmpeg's own native decode of the same file) rather
  // than the synthetic-file unit tests, which are too small for the cost
  // to show up.
  FetchStatus GetVideoFrame(int64_t n, std::shared_ptr<NormalizedFrame>& out, std::string* errorOut = nullptr);

  // Diagnostics: how many GetVideoFrame() calls were served from the ring
  // buffer (expected fast path) vs required the SeekAndDecode() fallback
  // (rare/slow path -- a genuine backward seek or an eagerdecode margin
  // shortfall). Also logged as a one-line summary (with the exact
  // non-buffered frame numbers) when the engine is destroyed.
  int64_t BufferedServeCount() const {
    std::lock_guard<std::mutex> lock(stats_.mutex);
    return stats_.bufferedCount;
  }
  std::vector<int64_t> NonBufferedServedFrames() const {
    std::lock_guard<std::mutex> lock(stats_.mutex);
    return stats_.nonBufferedFrames;
  }
  // How many SeekAndDecode() calls used an Index Table Segment-derived
  // keyframe offset instead of decoding forward from essence element 0 --
  // see videoIndex_.
  int64_t IndexAssistedSeekCount() const {
    std::lock_guard<std::mutex> lock(stats_.mutex);
    return stats_.indexAssistedSeeks;
  }

  // Diagnostics for manual tools (see tests/mxf_parser_tests/read_test_mxf.cpp):
  // whether a usable Index Table Segment-derived lookup was built at all,
  // and how far it reaches. videoIndex_ is populated once in Open() and
  // never mutated afterward, so no locking is needed to read it.
  bool HasEssenceIndex() const { return videoIndex_.has_value() && !videoIndex_->Empty(); }
  int64_t EssenceIndexHighestEditUnit() const { return videoIndex_ ? videoIndex_->HighestCoveredEditUnit() : -1; }

  // Whether this open file's audio is actually being decoded/exposed --
  // false for a video-only source, and also false whenever the audio
  // present is unsupported (mismatched track config, non-PCM codec) and
  // OpenOptions::use_audio degraded it rather than hard-failing Open(). Both
  // host adapters gate their VideoInfo/VSAudioInfo audio setup on this.
  bool ExposesAudio() const { return exposeAudio_; }

  // Total samples-per-channel exposed to the host -- i.e. excluding the
  // Origin pre-roll trim, the same way StreamInfo().numFrames already
  // excludes it for video (see audioSampleOriginSkip_). Known exactly at
  // Open() time (see mxf/audio_sample_index.h) -- both AviSynth's
  // VideoInfo::num_audio_samples and VapourSynth's VSAudioInfo::numSamples
  // need this up front. 0 if !ExposesAudio().
  int64_t TotalAudioSamples() const {
    return audioSampleIndex_ ? audioSampleIndex_->TotalSamples() - audioSampleOriginSkip_ : 0;
  }

  // Fills `buf` with `count` samples-per-channel (interleaved across every
  // summed audio channel, native bit depth -- see MxfStreamInfo::
  // audioChannels/audioBitsPerSample) starting at sample `start`. Mirrors
  // GetVideoFrame's ring-buffer-first, fresh-reader-fallback shape, but
  // keyed by sample position (via audioSampleIndex_) rather than frame
  // index directly, since a request doesn't generally align to an edit
  // unit's own sample-block boundary. A request extending past the last
  // known sample is zero-filled (silence) rather than treated as an error --
  // defensive only, since both adapters set their total sample count exactly
  // from TotalAudioSamples(). Only valid when ExposesAudio().
  FetchStatus GetAudioSamples(int64_t start, int64_t count, void* buf, std::string* errorOut = nullptr);

 private:
  void DecodeThreadMain();

  // Opens a *fresh*, independent MxfReader/EssenceWalker/Decoder on the
  // same file (separate file handle -- doesn't disturb the primary
  // background decode thread's own reader/walker/decoder or the ring
  // buffer's state at all, per the user's own call: "leave the buffer
  // as-is" for this case) and decodes forward from the very start of
  // essence up through `target`, discarding every frame before it. Works
  // for any codec (even long-GOP/inter-frame ones) since decode always
  // starts from a valid entry point with no index to jump via -- correct
  // but O(target) slow, since there is no keyframe index to seek to
  // directly. Returns nullptr if essence runs out before reaching `target`.
  std::shared_ptr<NormalizedFrame> SeekAndDecode(int64_t target);

  // Resolved restart point for essence element `essenceTarget`: the byte
  // offset an EssenceWalker should start at, the edit unit that offset
  // corresponds to, and the reorder delay (see EssenceIndex::
  // EstimateReorderDelay) a decoder restarted there needs before its Nth
  // output genuinely corresponds to input element N (always 0 for
  // intra-only codecs). {streamInfo_.headerMetadataEnd, 0, 0} when no
  // usable videoIndex_ exists -- callers get the same from-essence-element-0
  // behavior as always in that case.
  struct SeekStartPoint {
    uint64_t byteOffset;
    int64_t editUnit;
    int reorderDelay;
  };

  // Shared by SeekAndDecode's one-shot fallback and DecodeThreadMain's
  // start-frame-hint jump start (see firstHostFrame_) so both apply the
  // exact same Index Table-assisted keyframe/reorder-delay logic --
  // factored out of what was originally SeekAndDecode's own inline code,
  // byte-for-byte identical to it (see its call sites below), just callable
  // from more than one place. See SeekAndDecode's own (former, now moved
  // here) doc comments for the full real-file rationale behind each step:
  // the intra-only direct-jump case, the GOP-based backward keyframe walk
  // for reorder-delay runway, and the AVC open-GOP one-more-GOP-back
  // extension.
  SeekStartPoint ResolveSeekStart(int64_t essenceTarget);

  // Audio counterparts of GetVideoFrame/SeekAndDecode's ring-buffer-first,
  // fresh-reader-fallback split: FetchAudioBlock resolves ring index
  // `hostEditUnit` (host-frame-relative, same space as videoBuffer_/
  // GetVideoFrame's `n`) from audioBuffer_ (blocking, same as
  // videoBuffer_->Get) unless it's far enough ahead that waiting would be
  // as slow as a linear scan, or the buffer reports the index as evicted/
  // errored -- either case falls through to FreshReadAudioBlock, a one-shot
  // independent MxfReader/EssenceWalker read starting directly at
  // `byteOffset` (already resolved by the caller via audioSampleIndex_,
  // which operates in raw essence-edit-unit space -- see
  // audioSampleOriginSkip_ -- so FreshReadAudioBlock itself needs no
  // edit-unit/origin translation at all). Much simpler than SeekAndDecode's
  // video equivalent: PCM has no GOP/reorder-delay dependencies, so this is
  // always a direct index-assisted jump.
  FetchStatus FetchAudioBlock(int64_t hostEditUnit, uint64_t byteOffset, std::shared_ptr<AudioBlock>& out,
                              std::string* errorOut);
  std::shared_ptr<AudioBlock> FreshReadAudioBlock(uint64_t byteOffset);

  // Appends `line` to stderr, and to OpenOptions::logfile (if set) prefixed
  // with a timestamp -- factored out of what was originally ~SourceEngine's
  // own ServeStats-summary-only logging so an audio-degraded note (see
  // Open()'s UseAudioMode handling) can share the same "stderr alone is
  // invisible to GUI hosts" rationale without duplicating the timestamp
  // formatting.
  void AppendLogLine(const std::string& line) const;

  // Options for every EssenceWalker this engine creates: no resync past
  // damage in strict mode, a bounded one otherwise, each damaged spot
  // logged once (the fallback paths re-walk the same bytes).
  EssenceWalkerOptions WalkerOptions();
  void LogWalkDamage(const WalkDamage& damage);

  std::string path_;
  bool strict_ = true;
  std::mutex damageLogMutex_;
  std::vector<uint64_t> loggedDamageOffsets_;
  std::string logFile_;
  std::string httpHeaders_;
  // Number of leading video essence elements to discard before essence
  // element 0 becomes host frame 0 -- OpenOptions::handle_origin ?
  // streamInfo_.origin : 0. See DecodeThreadMain/SeekAndDecode.
  int64_t originSkip_ = 0;
  // OpenOptions::start_frame_hint, validated/clamped once in Open() -- 0
  // (the default) means no hint was given (or it was out of range), so
  // DecodeThreadMain's behavior is byte-for-byte unchanged from before this
  // was added. When > 0, it's both the ring buffer's very first index (see
  // FrameBuffer's startIndex constructor param, used for videoBuffer_/
  // audioBuffer_/Open()'s own Peek() call) and the host frame
  // DecodeThreadMain jump-starts production at, instead of frame 0.
  int64_t firstHostFrame_ = 0;
  // Set alongside firstHostFrame_ in Open() (via ResolveSeekStart(originSkip_
  // + firstHostFrame_)) when firstHostFrame_ > 0: the essence edit unit
  // walker_ was actually constructed to start reading from (a keyframe at or
  // before that target), and the reorder delay a decoder starting there
  // needs. Unused (left at their defaults) when firstHostFrame_ == 0, since
  // that path keeps walker_ starting at streamInfo_.headerMetadataEnd exactly
  // as before. See DecodeThreadMain's hinted discard/warm-up block.
  int64_t hintStartEditUnit_ = 0;
  int hintReorderDelay_ = 0;
  std::unique_ptr<MxfReader> reader_;
  MxfStreamInfo streamInfo_;
  // Best-effort Index Table lookup (mxf/index_table.h), built once in
  // Open() and never mutated afterward -- read-only, no locking needed,
  // same immutable-after-Open() lifecycle as streamInfo_ itself. nullopt if
  // no usable Index Table Segment was found for streamInfo_.bodySid --
  // SeekAndDecode() falls back to its original from-essence-element-0
  // behavior whenever this is unset, so absence is never a hard failure.
  std::optional<EssenceIndex> videoIndex_;
  std::unique_ptr<EssenceWalker> walker_;
  std::unique_ptr<Decoder> videoDecoder_;
  std::unique_ptr<FrameBuffer<std::shared_ptr<NormalizedFrame>>> videoBuffer_;
  std::shared_ptr<NormalizedFrame> firstFrame_;
  std::thread decodeThread_;
  ServeStats stats_;

  // See ExposesAudio(). Decided once in Open() from streamInfo_.hasAudio/
  // audioConfigSupported and OpenOptions::use_audio; never mutated
  // afterward (same immutable-after-Open() lifecycle as streamInfo_/
  // videoIndex_), except that DecodeThreadMain may stop *producing* new
  // audio blocks after a mid-file audio decode error without touching this
  // flag -- already-buffered/already-fresh-read audio remains servable, see
  // FetchAudioBlock/frame_buffer.h's FetchStatus semantics.
  bool exposeAudio_ = false;
  // Self-contained sample-position index (mxf/audio_sample_index.h), built
  // once in Open() when exposeAudio_ -- gives TotalAudioSamples() and
  // GetAudioSamples() a way to resolve any sample position to an edit unit/
  // byte offset without a lazy/incremental scan.
  std::optional<AudioSampleIndex> audioSampleIndex_;
  std::unique_ptr<FrameBuffer<std::shared_ptr<AudioBlock>>> audioBuffer_;
  // Audio-space equivalent of originSkip_: how many leading samples (across
  // audioSampleIndex_'s raw essence edit units [0, originSkip_)) are
  // trimmed off the front, so audioBuffer_'s host-relative ring index 0 (the
  // first edit unit DecodeThreadMain actually produces, essence edit unit
  // originSkip_) lines up with GetAudioSamples' sample position 0. Computed
  // once in Open() from audioSampleIndex_->CumulativeSamplesBefore(originSkip_).
  int64_t audioSampleOriginSkip_ = 0;
};

}  // namespace mxfsrc
