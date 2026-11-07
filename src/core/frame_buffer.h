#pragma once

#include <algorithm>
#include <chrono>
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
// `i` once `i <= watermark + min(capacity, margin)`, where `watermark` is
// the highest index any consumer has ever asked for and `margin` (usually
// much smaller than `capacity`) bounds how far the producer is allowed to
// eagerly race UNSOLICITED. `capacity` alone would let a single request for
// even a nearby index (e.g. frame 3) unlock a race all the way to
// `capacity` frames deep in the background -- fast enough at real decode
// speeds to evict early frames before a slow-per-frame downstream filter
// got back around to re-reading one of them, well before `capacity` frames
// of genuine buffering were ever needed (caught via real-host testing: a
// QTGMC preset="slower" chain re-reading frame 0 after the engine had
// already eagerly raced to frame 302 of a 300-frame buffer in under a
// second). `min(capacity, margin)` naturally reduces to today's original
// `watermark + capacity` formula whenever `capacity <= margin` (e.g. a
// deliberately small buffer_frames), preserving the "jump straight to a
// distant frame evicts everything below it" behavior for that case, while
// throttling the common case (large capacity, nearby requests) to a much
// smaller automatic eager-ahead window. `capacity` still governs the *true*
// maximum window (eviction itself, in Put(), is unchanged) -- an explicit
// request that itself exceeds the margin still legitimately extends the
// watermark and thus the ceiling, same as before.
template <typename T>
class FrameBuffer {
 public:
  // `startIndex` (default 0) seeds the ring's first-ever index: `watermark_`,
  // `lowestValid_`, and `highestProduced_+1` (i.e. the first index NextIndex()
  // returns) all start there instead of 0, so a producer given a start-frame
  // hint (see OpenOptions::start_frame_hint) can begin filling the buffer at
  // an arbitrary absolute frame number instead of always at 0 -- consumers
  // address frames by that same absolute number throughout this class
  // (SourceEngine::GetVideoFrame passes its own `n` straight through as
  // `index`), so the ring has to be told where production will actually
  // begin, not just how big its window is. Any index below startIndex
  // behaves exactly like an already-evicted one (see GetImpl's
  // `index < lowestValid_` branch) -- correct, since nothing before
  // startIndex will ever be produced by this buffer's own producer; a
  // caller that genuinely needs one of those (e.g. SourceEngine::
  // GetVideoFrame/FetchAudioBlock for a frame before the hint) already falls
  // back to a fresh one-shot re-decode for an evicted index, same as today.
  FrameBuffer(int capacity, int eagerReadaheadMargin, int64_t startIndex = 0)
      : capacity_(capacity),
        requestedMargin_(eagerReadaheadMargin),
        margin_(std::min(capacity, eagerReadaheadMargin)),
        slots_(static_cast<size_t>(capacity)),
        lowestValid_(startIndex),
        highestProduced_(startIndex - 1),
        watermark_(startIndex) {}

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
    cv_.wait(lock, [&] { return shutdown_ || index < watermark_ + margin_; });
    return !shutdown_;
  }

  // Like WaitForCapacity, but wakes after `timeout` even if the condition
  // hasn't been met -- for a producer that shares one decode loop between
  // this buffer and a second, independently-watermarked one (see
  // SourceEngine::DecodeThreadMain, which combines videoBuffer_'s own gate
  // with audioBuffer_->WantsMore() so a caller that only ever reads audio,
  // and never calls GetVideoFrame, doesn't stall production forever once
  // video's own eager-readahead margin is exhausted -- real bug found via a
  // hung audio-only ffmpeg extraction: video's watermark_ never advances
  // when nothing ever calls Get()/Peek() on videoBuffer_, so the unconditional
  // WaitForCapacity(next) that used to gate this loop blocked permanently
  // after only `margin` frames, capping audio at whatever was produced
  // during that initial window). Returns whether the condition is true at
  // return time (not just "not shut down") so a caller can tell a timeout
  // apart from real progress.
  bool WaitForCapacityOrTimeout(int64_t index, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return shutdown_ || index < watermark_ + margin_; });
    return !shutdown_ && index < watermark_ + margin_;
  }

  // Non-blocking counterpart of WaitForCapacity's predicate -- see
  // WaitForCapacityOrTimeout's doc comment for why a producer needs to poll
  // a second buffer's watermark without blocking on it.
  bool WantsMore(int64_t index) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index < watermark_ + margin_;
  }

  bool IsShutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
  }

  // The next index the producer should decode (highest produced so far, plus one).
  int64_t NextIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return highestProduced_ + 1;
  }

  // Stores `value` at `index`, which must equal NextIndex() at the time of
  // the call. Evicts the oldest buffered frame first if the window is
  // already at capacity, handing that evicted value back to the caller
  // (instead of just destroying it) so the producer can recycle its
  // allocation for a future Put() -- see SourceEngine::DecodeThreadMain,
  // where this is the difference between allocating a fresh multi-MB
  // NormalizedFrame per frame (paying OS page-fault cost on every single
  // decode) and reusing one whose buffers are already resident.
  //
  // `generation` (see Reset()): a producer that was told to start over
  // passes the generation it was started for; a Put() for an older
  // generation arrives after the reset and is dropped, so a frame decoded
  // for the previous position can never land in the new window.
  std::optional<T> Put(int64_t index, T value, std::optional<uint64_t> generation = std::nullopt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation && *generation != generation_) return std::nullopt;
    std::optional<T> evicted;
    if (highestProduced_ >= 0 && index - lowestValid_ + 1 > capacity_) {
      evicted = std::move(slots_[SlotOf(lowestValid_)]);
      slots_[SlotOf(lowestValid_)].reset();
      ++lowestValid_;
    }
    slots_[SlotOf(index)] = std::move(value);
    highestProduced_ = index;
    cv_.notify_all();
    return evicted;
  }

  // `generation`: as for Put().
  void SignalEndOfStream(std::optional<uint64_t> generation = std::nullopt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation && *generation != generation_) return;
    endOfStream_ = true;
    cv_.notify_all();
  }

  void SignalError(std::string message, std::optional<uint64_t> generation = std::nullopt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation && *generation != generation_) return;
    error_ = std::move(message);
    cv_.notify_all();
  }

  // Empties the window and makes `startIndex` the next index to produce,
  // as if the buffer had just been constructed with it (end-of-stream and
  // error are cleared too). Called by a consumer that jumped far ahead of
  // the producer (see SourceEngine::RepositionProducer): the producer then
  // restarts there instead of the consumer re-decoding every following
  // frame separately. Bumps the generation, so producer calls still in
  // flight for the old position are dropped. A consumer waiting for an
  // index below `startIndex` wakes up with kEvicted, one waiting for an
  // index at or above it keeps waiting for the restarted producer.
  void Reset(int64_t startIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) slot.reset();
    lowestValid_ = startIndex;
    highestProduced_ = startIndex - 1;
    watermark_ = startIndex;
    endOfStream_ = false;
    error_.reset();
    ++generation_;
    cv_.notify_all();
  }

  // For SourceEngine's trace log: how the current window's production ended.
  bool EndOfStreamReached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return endOfStream_;
  }
  std::optional<std::string> ErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

  uint64_t Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
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

  // Non-blocking peek at the highest index produced so far (or -1 if
  // nothing has been produced yet) and at the buffer's total window size.
  // Lets a caller decide, before ever calling the blocking Get(), whether a
  // requested index is close enough to let the producer catch up naturally
  // or far enough ahead that waiting for sequential production would be as
  // slow as a full linear scan -- see SourceEngine::GetVideoFrame's
  // forward-seek bypass.
  int64_t HighestProduced() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return highestProduced_;
  }
  int Capacity() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return capacity_;
  }

  // Lowest index still held (everything below it was evicted or never
  // produced). See SourceEngine::GetVideoFrame's auto-growth.
  int64_t LowestValid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lowestValid_;
  }

  // Enlarges the window to `newCapacity` frames, keeping every frame it
  // holds; a smaller or equal value does nothing. The eager-readahead
  // margin becomes min(newCapacity, the margin asked for at construction).
  // Used for buffer_frames=auto: the engine starts small and grows once it
  // knows the frame size, and again when requests land just below the
  // window (see SourceEngine::GrowBuffers).
  void Grow(int newCapacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (newCapacity <= capacity_) return;
    std::vector<std::optional<T>> slots(static_cast<size_t>(newCapacity));
    for (int64_t i = lowestValid_; i <= highestProduced_; ++i) {
      slots[static_cast<size_t>(i % newCapacity)] = std::move(slots_[SlotOf(i)]);
    }
    slots_ = std::move(slots);
    capacity_ = newCapacity;
    margin_ = std::min<int64_t>(newCapacity, requestedMargin_);
    cv_.notify_all();  // a wider margin may let the producer continue
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

    // An already-produced, still-resident frame is servable regardless of
    // anything that happened afterward -- checked ahead of shutdown_/
    // error_ so a *later* permanent producer failure can't retroactively
    // invalidate frames that were already correctly decoded and are still
    // sitting in the ring buffer. Previously checked after shutdown_/
    // error_: a background decode failure at, say, ring index 634 set
    // error_ permanently, which made every already-buffered, genuinely
    // correct frame from 604 (window start) through 633 spuriously bounce
    // to the caller's fallback re-decode too, instead of being served
    // directly -- wasteful, and (combined with the producer racing ahead
    // of the consumer by up to `margin`) made the exact index where a real
    // file's failure appeared to start vary run to run with OS thread
    // scheduling, not anything about the file itself.
    if (index >= lowestValid_ && index <= highestProduced_) {
      out = *slots_[SlotOf(index)];
      return FetchStatus::kOk;
    }
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
    // index > highestProduced_, endOfStream_ per the wait condition above.
    return FetchStatus::kEndOfStream;
  }

 private:
  size_t SlotOf(int64_t index) const { return static_cast<size_t>(index % capacity_); }

  int capacity_;
  const int64_t requestedMargin_;
  int64_t margin_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::optional<T>> slots_;
  int64_t lowestValid_ = 0;
  int64_t highestProduced_ = -1;
  // Starts at 0, not -1: WaitForCapacity's eager-fill window is `index <
  // watermark_ + margin_`, and margin_ can be as small as 1 (buffer_frames=1
  // or eagerdecode=1, both legal). With watermark_ == -1, that window becomes
  // `index < 0`, which no frame index ever satisfies -- the producer could
  // never produce even frame 0, and Open()'s initial Peek(0) (which
  // deliberately never advances the watermark, see Peek's own comment) would
  // block forever with no consumer ever arriving to unblock it. Starting at 0
  // guarantees the window always covers frame 0 regardless of how small
  // margin_ is. Found via a real file opened with buffer_frames=1: Open()
  // hung indefinitely in AvsPmod.
  int64_t watermark_ = 0;
  bool endOfStream_ = false;
  bool shutdown_ = false;
  uint64_t generation_ = 0;
  std::optional<std::string> error_;
};

}  // namespace mxfsrc
