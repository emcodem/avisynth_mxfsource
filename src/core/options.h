#pragma once

#include <string>

#include "core/use_audio_mode.h"

namespace mxfsrc {

// Open-time options shared by both host adapters. Wired into the real
// decode engine starting in Phase 4; the AVS/VS Phase 0 scaffolds below
// hardcode a subset of these as filter arguments in the meantime.
struct OpenOptions {
  // Sliding ring-buffer capacity (frames): how much decoded history the
  // engine is willing to retain before evicting the oldest frame. Must
  // exceed the host's prefetch lookahead so GetFrame() requests are almost
  // always already-decoded cache hits.
  int buffer_frames = 300;

  // How far ahead of the highest-ever-requested frame the decode thread is
  // allowed to eagerly race UNSOLICITED (i.e. before being asked), capped
  // at buffer_frames. Deliberately much smaller than buffer_frames by
  // default: the naive design let a single request for even a nearby frame
  // (e.g. frame 3) unlock a race all the way to buffer_frames-deep (300)
  // in the background, fast enough (sub-second at ~200-300fps decode) to
  // evict early frames before a slow-per-frame filter (observed: QTGMC
  // preset="slower") got back around to re-reading one of them -- caught
  // via real-host testing, not synthetic unit tests. buffer_frames itself
  // still governs the true maximum window (e.g. an explicit deliberate
  // jump far ahead still gets the full buffer_frames of slack, evicting
  // older frames as documented) -- this only throttles the *automatic*
  // eager-ahead portion. Tune upward if a filter's genuine (not merely
  // eager-decode-outrunning-it) lookahead need exceeds this margin. Exposed
  // to scripts as MxfSource(..., eagerdecode=N).
  int eagerdecode = 32;

  // Absolute path to a file the ServeStats summary (see SourceEngine) is
  // appended to when the engine is destroyed, in addition to the existing
  // stderr line -- stderr alone is invisible for GUI hosts (e.g. AvsPmod)
  // that don't surface a console. Empty (the default) skips file logging
  // entirely.
  std::string logfile;

  // Raw "Name: value\r\nName2: value2" header block sent with every request
  // when the path given to SourceEngine::Open() is an http:// or https://
  // URL (ignored for local file paths). S3 presigned URLs need none of this
  // -- their auth is embedded in the URL's query string -- but a private
  // HTTP server behind e.g. an Authorization: Bearer token needs it.
  // Exposed to scripts as MxfSource(..., httpheaders="...").
  std::string http_headers;

  // Whether to honor the MXF SourcePackage Track.Origin property (see
  // MxfStreamInfo::origin in mxf/header_parser.h): the number of pre-
  // roll/trim frames at the start of the essence container that sit before
  // the MaterialPackage's intended playable range. When true (the
  // default), the engine skips those `origin` frames and exposes exactly
  // (SP Duration - Origin) frames, matching what an NLE would show for the
  // edited sequence. When false, Origin is ignored entirely and the engine
  // exposes the full raw essence from its first frame, as if Origin were 0
  // -- useful for files with an incorrect/stale Origin value, or to inspect
  // the untrimmed source essence directly. Exposed to scripts as
  // MxfSource(..., handle_origin=true/false).
  bool handle_origin = true;

  // Policy for audio that's present but not in a state this plugin can
  // output (see UseAudioMode). Exposed to scripts as
  // MxfSource(..., use_audio="always"/"never"/"supported").
  UseAudioMode use_audio = UseAudioMode::kSupported;

  // URL (or local path) to fetch a previously published essence index JSON
  // document from -- see mxf/index_transport.h -- instead of building the
  // index locally via mxf/index_table.h's BuildEssenceIndex. Published by
  // the separate MxfExtractIndex host function; for distributed setups (one
  // instance scans the file, e.g. remote worker instances working off a
  // byte-identical copy of the same source) where repeating that scan on
  // every instance is pure redundant I/O. Any failure -- network error,
  // malformed JSON, version/bodySid mismatch, or the byte-offset-vs-file-
  // size plausibility check -- fails Open() outright; there is deliberately
  // no silent fallback to a local rebuild, since masking a real mismatch
  // risks seeking into the wrong bytes. Ignored (always builds its own
  // index locally, without writing anything to disk) when empty, the
  // default. Exposed to scripts as MxfSource(..., indexurl="...").
  std::string index_url;

  // Frame the background decode thread (SourceEngine::DecodeThreadMain)
  // should start producing from, instead of always starting at frame 0
  // (Origin pre-roll aside -- see originSkip_) and self-throttling near
  // there until some caller's first real request eventually drags it
  // forward. 0 (the default) means "no hint, start at frame 0" -- byte-for-
  // byte unchanged behavior for any existing caller that never sets this.
  //
  // Set by a caller that already knows every frame it will ever request
  // lies at or after some other point -- e.g. a per-chunk transcode
  // pipeline that opens a fresh SourceEngine per chunk and immediately
  // requests frames [chunkStart, chunkStart+N) far from frame 0. Without
  // this, every one of those requests independently falls outside
  // SourceEngine::GetVideoFrame's ring-buffer window (the producer thread
  // never learns the real access point moved) and pays its own full
  // SeekAndDecode() fallback cost; with it, the producer jump-starts once,
  // at Open() time, via the same Index Table-assisted keyframe lookup
  // SeekAndDecode() itself uses, and serves the whole requested range from
  // the ring buffer cheaply instead.
  //
  // A value at or past the file's own frame count is ignored (logged, not
  // a hard Open() failure) and falls back to the frame-0 default, same as
  // 0 -- this is purely a performance hint, never a correctness requirement
  // (a request for a frame before the hint still works, via the existing
  // SeekAndDecode fallback, just without this optimization). Exposed to
  // scripts as MxfSource(..., start_frame=N).
  int64_t start_frame_hint = 0;

  // Refuse damaged files instead of opening whatever can be salvaged --
  // for automated pipelines, where a silently short or slow-to-open file is
  // worse than an explicit error. Open() fails when:
  //   - the file ends with an unexpected run of zero bytes
  //     (see mxf/integrity.h's kMaxExpectedTrailingZeroBytes), whatever
  //     the partition statuses,
  //   - no Index Table Segment is reachable without scanning the essence
  //     (Header Partition, e.g. a CBR/CBG segment, or partitions listed in
  //     the RIP) -- strict never runs the full-file bitstream scan of
  //     mxf/bitstream_index.h,
  //   - a Closed header declares a Footer Partition that isn't there (e.g.
  //     beyond EOF) and there is no Random Index Pack,
  //   - the file is finished (a Closed Footer Partition, or else a Closed
  //     header) and the index covers fewer edit units than its Duration.
  //     An unfinished (growing/aborted) recording has no final Duration, so
  //     it opens with whatever its index covers.
  // On by default. Exposed to scripts as MxfSource(..., strict=true/false).
  // MxfExtractIndex takes the same parameter with the same meaning; both go
  // through core/index_policy.h, which is where every one of these
  // decisions lives.
  // Full user-facing description: README.md, "Strict mode" -- keep it in
  // sync with any change here or in mxf/integrity.h.
  bool strict = true;

  // Print informational messages (the serve-stats summary when the clip
  // closes, "audio sample index built ...", ...) to stderr as well. Off by
  // default, so a normal run only prints real warnings (damaged essence,
  // missing index, audio dropped, ...). `logfile` receives every message
  // regardless of this setting. Exposed to scripts as
  // MxfSource(..., debug=true).
  bool debug = false;

  // Check every entry of the video index (built locally or fetched via
  // index_url) against the essence before opening -- see core/
  // index_verify.h. With `strict`, a wrong index fails Open(); without, the
  // wrong entries are repaired from the essence where possible and logged
  // as warnings. Costs one or a few small reads per frame. Off by default.
  // Exposed to scripts as MxfSource(..., verify=true).
  bool verify = false;
};

} // namespace mxfsrc
