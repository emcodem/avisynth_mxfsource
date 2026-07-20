#pragma once

namespace mxfsrc {

// Open-time options shared by both host adapters. Wired into the real
// decode engine starting in Phase 4; the AVS/VS Phase 0 scaffolds below
// hardcode a subset of these as filter arguments in the meantime.
struct OpenOptions {
  // Sliding ring-buffer capacity (frames). Must exceed the host's prefetch
  // lookahead so GetFrame() requests are almost always already-decoded
  // cache hits; see plan "core/ ring buffer" section for the eviction policy.
  int buffer_frames = 300;
};

} // namespace mxfsrc
