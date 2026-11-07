// Unit tests for core/frame_buffer.h's ring-buffer eviction-return contract
// (FrameBuffer::Put hands back the evicted slot instead of just destroying
// it) -- see SourceEngine::DecodeThreadMain, which recycles that value's
// allocation for the next produced frame rather than paying a fresh
// heap-allocation/OS-page-fault cost on every single decode (found via
// real-file benchmarking: this was dwarfing actual codec decode time on
// high-resolution/high-bit-depth content).
#include <iostream>
#include <memory>

#include "core/frame_buffer.h"

using namespace mxfsrc;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                             \
  do {                                                                          \
    if (!(cond)) {                                                              \
      std::cerr << "CHECK FAILED: " #cond " at " << __FILE__ << ":" << __LINE__ \
                << std::endl;                                                   \
      ++g_failures;                                                             \
    }                                                                           \
  } while (0)

void TestPutReturnsNulloptBeforeCapacity() {
  FrameBuffer<int> buf(/*capacity=*/3, /*margin=*/3);
  CHECK(!buf.Put(0, 10).has_value());
  CHECK(!buf.Put(1, 11).has_value());
  CHECK(!buf.Put(2, 12).has_value());
}

void TestPutReturnsEvictedValueOnceAtCapacity() {
  FrameBuffer<int> buf(/*capacity=*/3, /*margin=*/3);
  buf.Put(0, 10);
  buf.Put(1, 11);
  buf.Put(2, 12);

  // Index 3 must evict index 0's value (10) since capacity is 3.
  auto evicted = buf.Put(3, 13);
  CHECK(evicted.has_value());
  CHECK(evicted.value() == 10);

  int out = 0;
  CHECK(buf.Get(3, out) == FetchStatus::kOk);
  CHECK(out == 13);
  CHECK(buf.Get(0, out) == FetchStatus::kEvicted);
}

// Mirrors SourceEngine::DecodeThreadMain's actual recycling pattern: the
// evicted shared_ptr should be the exact same object identity that was
// originally stored (not a copy), and its use_count reflects whether
// anything else still holds a reference -- the real code only reuses it in
// place when use_count() == 1.
void TestEvictedValueIsSameObjectForSharedPtr() {
  FrameBuffer<std::shared_ptr<int>> buf(/*capacity=*/2, /*margin=*/2);
  auto first = std::make_shared<int>(42);
  int* rawPtr = first.get();
  buf.Put(0, first);  // buf now also holds a copy; first.use_count() == 2
  buf.Put(1, std::make_shared<int>(43));

  auto evicted = buf.Put(2, std::make_shared<int>(44));
  CHECK(evicted.has_value());
  CHECK(evicted->get() == rawPtr);
  // `first` (still in scope) and `*evicted` both reference it: use_count == 2,
  // matching the real code's "don't recycle if something else still holds it".
  CHECK(evicted->use_count() == 2);
}

// Reset (SourceEngine::RepositionProducer): the window restarts at the new
// index, old frames read as evicted, and producer calls still tagged with
// the old generation are dropped -- including one for exactly the new start
// index, which would otherwise put an old-position frame there.
void TestResetRestartsWindowAndDropsStaleGeneration() {
  FrameBuffer<int> buf(/*capacity=*/4, /*margin=*/4);
  const uint64_t oldGen = buf.Generation();
  buf.Put(0, 10, oldGen);
  buf.Put(1, 11, oldGen);

  buf.Reset(100);
  const uint64_t newGen = buf.Generation();
  CHECK(newGen != oldGen);
  CHECK(buf.NextIndex() == 100);
  CHECK(buf.LowestValid() == 100);
  int out = 0;
  CHECK(buf.Get(1, out) == FetchStatus::kEvicted);

  buf.Put(100, 999, oldGen);  // stale producer write
  CHECK(buf.NextIndex() == 100);
  buf.SignalEndOfStream(oldGen);  // stale: must not end the new run
  buf.SignalError("stale", oldGen);

  buf.Put(100, 1000, newGen);
  CHECK(buf.Get(100, out) == FetchStatus::kOk);
  CHECK(out == 1000);
  buf.SignalEndOfStream(newGen);
  CHECK(buf.Get(101, out) == FetchStatus::kEndOfStream);
}

// Grow (buffer_frames=auto): every held frame survives the re-slotting,
// eviction then follows the new capacity, and a smaller value is ignored.
void TestGrowKeepsFramesAndRaisesCapacity() {
  FrameBuffer<int> buf(/*capacity=*/3, /*margin=*/3);
  for (int i = 0; i < 5; ++i) buf.Put(i, 10 + i);  // holds 2, 3, 4
  buf.Grow(8);
  CHECK(buf.Capacity() == 8);
  int out = 0;
  for (int i = 2; i < 5; ++i) {
    CHECK(buf.Get(i, out) == FetchStatus::kOk);
    CHECK(out == 10 + i);
  }
  CHECK(buf.Get(1, out) == FetchStatus::kEvicted);

  for (int i = 5; i < 10; ++i) CHECK(!buf.Put(i, 10 + i).has_value());  // 2..9: 8 frames, no eviction
  auto evicted = buf.Put(10, 20);
  CHECK(evicted.has_value() && *evicted == 12);
  CHECK(buf.Get(3, out) == FetchStatus::kOk && out == 13);

  buf.Grow(4);
  CHECK(buf.Capacity() == 8);
}

}  // namespace

int main() {
  TestPutReturnsNulloptBeforeCapacity();
  TestPutReturnsEvictedValueOnceAtCapacity();
  TestEvictedValueIsSameObjectForSharedPtr();
  TestResetRestartsWindowAndDropsStaleGeneration();
  TestGrowKeepsFramesAndRaisesCapacity();

  if (g_failures == 0) {
    std::cout << "all frame_buffer tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " frame_buffer test(s) failed" << std::endl;
  return 1;
}
