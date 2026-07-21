#include "core/source_engine.h"

#include "codec/decoder_factory.h"

namespace mxfsrc {

SourceEngine::SourceEngine() = default;

SourceEngine::~SourceEngine() {
  if (videoBuffer_) videoBuffer_->Shutdown();
  if (decodeThread_.joinable()) decodeThread_.join();
}

OpenResult SourceEngine::Open(const std::string& path, const OpenOptions& options) {
  reader_ = std::make_unique<MxfReader>(path);
  if (!reader_->IsOpen()) return {OpenStatus::kFileError, "failed to open file: " + path};

  std::string parseError;
  auto info = ParseMxfHeader(*reader_, &parseError);
  if (!info) return {OpenStatus::kParseError, parseError};
  streamInfo_ = *info;

  if (!streamInfo_.hasVideo) return {OpenStatus::kParseError, "no video track found in Header Metadata"};
  if (streamInfo_.numFrames < 0) {
    return {OpenStatus::kParseError,
            "unable to determine frame count from Header Metadata (RIP/Index Table and essence-count "
            "fallback are not yet implemented, see plan Phase 1 Duration fallback chain step 5)"};
  }

  videoDecoder_ = CreateVideoDecoder(streamInfo_.videoCodec, static_cast<int>(streamInfo_.width),
                                      static_cast<int>(streamInfo_.height));
  if (!videoDecoder_) return {OpenStatus::kUnsupportedCodec, "unsupported or unavailable video codec"};

  walker_ = std::make_unique<EssenceWalker>(*reader_, streamInfo_.headerMetadataEnd, streamInfo_.videoTrackNumber,
                                             streamInfo_.hasVideo, streamInfo_.audioTrackNumber,
                                             streamInfo_.hasAudio);

  const int capacity = options.buffer_frames > 0 ? options.buffer_frames : 300;
  videoBuffer_ = std::make_unique<FrameBuffer<NormalizedFrame>>(capacity);

  decodeThread_ = std::thread(&SourceEngine::DecodeThreadMain, this);

  std::string peekError;
  if (videoBuffer_->Peek(0, firstFrame_, &peekError) != FetchStatus::kOk) {
    return {OpenStatus::kDecodeError, "failed to decode first frame: " + peekError};
  }
  return {OpenStatus::kOk, ""};
}

FetchStatus SourceEngine::GetVideoFrame(int64_t n, NormalizedFrame& out, std::string* errorOut) {
  return videoBuffer_->Get(n, out, errorOut);
}

void SourceEngine::DecodeThreadMain() {
  bool essenceExhausted = false;

  for (;;) {
    const int64_t next = videoBuffer_->NextIndex();
    if (next >= streamInfo_.numFrames) {
      videoBuffer_->SignalEndOfStream();
      return;
    }
    if (!videoBuffer_->WaitForCapacity(next)) return;  // shutdown requested

    NormalizedFrame frame;
    bool got = false;
    while (!got) {
      if (essenceExhausted) {
        // Some codecs (e.g. MPEG-2) hold one or more frames internally
        // until explicitly drained -- keep draining one at a time until
        // either a frame comes out or the decoder truly has nothing left.
        if (!videoDecoder_->SendAndReceive({}, frame)) {
          videoBuffer_->SignalError("MXF essence ended before reaching the declared frame count (" +
                                     std::to_string(streamInfo_.numFrames) + ")");
          return;
        }
        got = true;
        break;
      }

      EssenceElement element;
      if (!walker_->Next(element)) {
        essenceExhausted = true;
        continue;  // retry immediately via the drain branch above
      }
      if (!element.isVideo) continue;  // audio elements: not wired into a ring buffer yet
      got = videoDecoder_->SendAndReceive(element.data, frame);
    }

    videoBuffer_->Put(next, std::move(frame));
  }
}

}  // namespace mxfsrc
