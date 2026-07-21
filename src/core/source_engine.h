#pragma once

#include <memory>
#include <string>
#include <thread>

#include "codec/decoder.h"
#include "core/frame_buffer.h"
#include "core/options.h"
#include "mxf/essence_walker.h"
#include "mxf/header_parser.h"
#include "mxf/klv.h"

namespace mxfsrc {

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
  const NormalizedFrame& FirstFrame() const { return firstFrame_; }

  // Blocks until frame `n` is decoded, evicted (n fell behind the
  // configured buffer_frames window), or the stream ends/errors -- see
  // FrameBuffer::Get for exact status semantics.
  FetchStatus GetVideoFrame(int64_t n, NormalizedFrame& out, std::string* errorOut = nullptr);

 private:
  void DecodeThreadMain();

  std::unique_ptr<MxfReader> reader_;
  MxfStreamInfo streamInfo_;
  std::unique_ptr<EssenceWalker> walker_;
  std::unique_ptr<Decoder> videoDecoder_;
  std::unique_ptr<FrameBuffer<NormalizedFrame>> videoBuffer_;
  NormalizedFrame firstFrame_;
  std::thread decodeThread_;
};

}  // namespace mxfsrc
