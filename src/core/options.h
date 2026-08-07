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
  // instance scans the file, e.g. AWS slave instances working off a
  // byte-identical copy of the same source) where repeating that scan on
  // every instance is pure redundant I/O. Any failure -- network error,
  // malformed JSON, version/bodySid mismatch, or the byte-offset-vs-file-
  // size plausibility check -- fails Open() outright; there is deliberately
  // no silent fallback to a local rebuild, since masking a real mismatch
  // risks seeking into the wrong bytes. Ignored (always builds its own
  // index locally, without writing anything to disk) when empty, the
  // default. Exposed to scripts as MxfSource(..., indexurl="...").
  std::string index_url;
};

} // namespace mxfsrc
