#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codec/decoder.h"
#include "core/frame_buffer.h"
#include "core/options.h"
#include "mxf/essence_walker.h"
#include "mxf/header_parser.h"
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

  void RecordBufferedServe() {
    std::lock_guard<std::mutex> lock(mutex);
    ++bufferedCount;
  }
  void RecordNonBufferedServe(int64_t n) {
    std::lock_guard<std::mutex> lock(mutex);
    nonBufferedFrames.push_back(n);
  }
};

enum class OpenStatus { kOk, kFileError, kParseError, kUnsupportedCodec, kDecodeError };

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
// Audio is not wired into a ring buffer yet -- EssenceWalker already yields
// interleaved audio elements (see StreamInfo().hasAudio), but GetAudioSamples
// is a follow-up (see plan Phase 4 notes / README).
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

  std::string path_;
  std::string logFile_;
  std::string httpHeaders_;
  std::unique_ptr<MxfReader> reader_;
  MxfStreamInfo streamInfo_;
  std::unique_ptr<EssenceWalker> walker_;
  std::unique_ptr<Decoder> videoDecoder_;
  std::unique_ptr<FrameBuffer<std::shared_ptr<NormalizedFrame>>> videoBuffer_;
  std::shared_ptr<NormalizedFrame> firstFrame_;
  std::thread decodeThread_;
  ServeStats stats_;
};

}  // namespace mxfsrc
