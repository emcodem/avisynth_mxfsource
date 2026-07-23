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

}  // namespace

int main() {
  TestPutReturnsNulloptBeforeCapacity();
  TestPutReturnsEvictedValueOnceAtCapacity();
  TestEvictedValueIsSameObjectForSharedPtr();

  if (g_failures == 0) {
    std::cout << "all frame_buffer tests passed" << std::endl;
    return 0;
  }
  std::cerr << g_failures << " frame_buffer test(s) failed" << std::endl;
  return 1;
}
