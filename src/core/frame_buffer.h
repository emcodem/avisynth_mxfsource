#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mxfsrc {

enum class FetchStatus { kOk, kEvicted, kEndOfStream, kError };

// Thread-safe sliding-window ring buffer keyed by a monotonically
// increasing int64 index. Exactly one producer thread calls Put() in
// strictly increasing index order (0, 1, 2, ...); any number of consumer
// threads call Get().
//
// The producer self-paces via WaitForCapacity(): it may only produce index
// `i` once `i <= watermark + capacity`, where `watermark` is the highest
// index any consumer has ever asked for. Get() bumps the watermark as soon
// as it's called, so the producer keeps at most `capacity` frames buffered
// ahead of whatever the host has actually asked for (or, before anything's
// been asked for, eagerly fills the first `capacity` slots) -- see plan's
// "core/ ring buffer" section. Capacity should comfortably exceed the
// host's own prefetch depth (AviSynth+ Prefetch() lookahead, VapourSynth's
// core scheduler) so GetFrame() is almost always a cache hit.
template <typename T>
class FrameBuffer {
 public:
  explicit FrameBuffer(int capacity) : capacity_(capacity), slots_(static_cast<size_t>(capacity)) {}

  // --- producer side (single decode thread only) ---

  // Blocks until `index` is eligible to be produced, or the buffer is shut
  // down. Returns false on shutdown; the decode thread should stop.
  //
  // Strictly less-than (not <=) so the watermark index itself is never the
  // one evicted: real hosts legitimately re-request the same index (e.g.
  // ffmpeg's AviSynth demuxer reads frame 0 once to probe the stream format
  // and again for real playback) with no other request in between to move
  // the watermark forward. With <=, capacity frames of lookahead past the
  // watermark meant producing exactly that many frames past it evicted the
  // watermark's own frame before the second request could arrive -- caught
  // via real end-to-end testing, not the unit tests (see plan Phase 5 notes).
  bool WaitForCapacity(int64_t index) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return shutdown_ || index < watermark_ + capacity_; });
    return !shutdown_;
  }

  // The next index the producer should decode (highest produced so far, plus one).
  int64_t NextIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return highestProduced_ + 1;
  }

  // Stores `value` at `index`, which must equal NextIndex() at the time of
  // the call. Evicts the oldest buffered frame first if the window is
  // already at capacity.
  void Put(int64_t index, T value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (highestProduced_ >= 0 && index - lowestValid_ + 1 > capacity_) {
      slots_[SlotOf(lowestValid_)].reset();
      ++lowestValid_;
    }
    slots_[SlotOf(index)] = std::move(value);
    highestProduced_ = index;
    cv_.notify_all();
  }

  void SignalEndOfStream() {
    std::lock_guard<std::mutex> lock(mutex_);
    endOfStream_ = true;
    cv_.notify_all();
  }

  void SignalError(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::move(message);
    cv_.notify_all();
  }

  // Wakes any blocked producer/consumer so threads can exit; subsequent
  // Get() calls return kError.
  void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
    cv_.notify_all();
  }

  // --- consumer side (any number of threads) ---

  // Blocks until `index` is available, was evicted (fell behind the
  // window -- the caller should report this as "increase buffer_frames"),
  // end-of-stream was reached without ever producing it, or an error was
  // signaled.
  FetchStatus Get(int64_t index, T& out, std::string* errorOut = nullptr) {
    return GetImpl(index, out, /*advanceWatermark=*/true, errorOut);
  }

  // Like Get(), but does NOT advance the watermark. For inspecting a frame
  // that the eager initial fill already produced (e.g. SourceEngine::Open()
  // learning frame 0's pixel format) without that inspection itself
  // widening the producer's decode-ahead target -- doing so via a plain
  // Get() would let the producer race ahead and evict the very frame just
  // peeked before any real consumer ever asked for it (observed in
  // practice with a small buffer_frames; see plan Phase 5 notes).
  FetchStatus Peek(int64_t index, T& out, std::string* errorOut = nullptr) {
    return GetImpl(index, out, /*advanceWatermark=*/false, errorOut);
  }

 private:
  FetchStatus GetImpl(int64_t index, T& out, bool advanceWatermark, std::string* errorOut) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (advanceWatermark && index > watermark_) {
      watermark_ = index;
      cv_.notify_all();  // wake the producer: its capacity window just advanced
    }
    cv_.wait(lock, [&] {
      return (index <= highestProduced_) || (endOfStream_ && index > highestProduced_) || error_.has_value() ||
             shutdown_;
    });

    if (shutdown_) {
      if (errorOut) *errorOut = "engine shut down";
      return FetchStatus::kError;
    }
    if (error_.has_value()) {
      if (errorOut) *errorOut = *error_;
      return FetchStatus::kError;
    }
    if (index < lowestValid_) {
      if (errorOut) *errorOut = "requested frame fell outside the ring buffer window (increase buffer_frames)";
      return FetchStatus::kEvicted;
    }
    if (index > highestProduced_) {
      return FetchStatus::kEndOfStream;
    }
    out = *slots_[SlotOf(index)];
    return FetchStatus::kOk;
  }

 private:
  size_t SlotOf(int64_t index) const { return static_cast<size_t>(index % capacity_); }

  const int capacity_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::optional<T>> slots_;
  int64_t lowestValid_ = 0;
  int64_t highestProduced_ = -1;
  int64_t watermark_ = -1;
  bool endOfStream_ = false;
  bool shutdown_ = false;
  std::optional<std::string> error_;
};

}  // namespace mxfsrc
