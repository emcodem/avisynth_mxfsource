// Phase 0 placeholder so the test target and CTest wiring exist before
// Phase 1 adds real mxf/ parser unit tests against sample files.
#include "core/options.h"

int main() {
  mxfsrc::OpenOptions opts;
  return opts.buffer_frames == 0 ? 0 : 1;  // 0 = auto
}
