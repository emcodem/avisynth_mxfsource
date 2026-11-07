# avs_mxf_plugin

MXF (SMPTE 377M) source plugin for AviSynth+ and VapourSynth.

## Why this plugin exists

Source plugins like ffms2 and L-SMASH Works build a full frame index before
the first frame can be served. For a large MXF that means reading and
parsing the whole file first, which can take tens of minutes and has to be repeated in
every process that opens the file. 

This plugin exists to avoid that long-running index creation. An MXF file
already carries its own Index Table, so the plugin uses it and never scans
the video frames:

- **Opening is fast.** The file is opened from its header, footer and Index
  Table. Multi-GB and UHD files open in about a second.
- **`MxfExtractIndex` (optional) is a short action.** It reads only the MXF index and
  metadata, decodes nothing, and publishes the index once (to a path or a
  presigned URL). Every other machine opens the same file with `indexurl=`
  and starts without any scan. See
  [Chunked distributed processing](#chunked-distributed-processing).
- **Optimized for sequential processing, with random access.** A
  background thread decodes ahead into a configurable ring buffer (default
  300 frames), so a sequential run such as an ffmpeg encode is served from
  cache. Jumps outside the buffer use the Index Table to restart at the
  nearest keyframe, so scrubbing in an editor like AvsPmod works too.

## Features

- **Both hosts, one engine.** AviSynth+ and VapourSynth plugins share one
  core and take the same parameters.
- **Wide codec coverage** through libavcodec: MPEG-2 (XDCAM HD/HD422, D-10/
  IMX), H.264 (AVC-Intra, XAVC, XAVC-L), ProRes (incl. 4444, 12 bit),
  DNxHD/DNxHR and uncompressed video, all checked on real files. DV,
  JPEG 2000, MPEG-4 and FFV1 are also recognized but not yet verified against
  real MXF files. See [Supported MXF files](#supported-mxf-files).
- **Audio.** All audio tracks are mixed into one interleaved PCM stream,
  including D-10/IMX 8-channel AES3 and AES3-wrapped PCM; see [Audio](#audio).
- **Correct frame counts and field order**, with the MXF SourcePackage
  `Origin` trim honored by default (`handle_origin`).
- **[Strict mode](#strict-mode)** (default on): damaged or incomplete files
  fail fast with a precise message instead of silently producing wrong output.
- **[HTTP, HTTPS and S3 presigned URLs](#http-https-and-s3-presigned-urls)**
  as a source, with range requests, retries and no download to disk.
- **Published index for distributed processing.** `MxfExtractIndex` reads the
  index of a file once and publishes it (to a path or a presigned PUT URL);
  other machines open the same file with `indexurl=` and skip the scan.
- **Long and non-ASCII paths** on Windows.
- **Windows (MSVC) and Linux (gcc/clang)** builds; see [Build](#build).

## Quick start

AviSynth+:

```
LoadPlugin("mxfsource.dll")
MxfSource("D:\clips\interview.mxf")                       # local file
MxfSource("https://example.com/bucket/interview.mxf?X-Amz-...")   # presigned URL
```

VapourSynth:

```python
r = core.mxfsource.MxfSource("interview.mxf")
clip = r if isinstance(r, vs.VideoNode) else r["clip"]
```

All parameters are listed under [Parameters](#parameters).

## Development history

The notes below record how the plugin was built, phase by phase, and what
real-world files taught along the way. They are kept for reference.

Status:
- **Phase 0 (scaffolding) -- done.** The build system and both plugin ABIs
  are wired up and verified end to end via the actual production pipeline
  (`ffmpeg -f avisynth -i test.avs` and `vspipe -c y4m test.vpy - | ffmpeg`).
  `MxfSource()` still takes explicit width/height/num_frames/fps arguments
  and synthesizes solid-color frames instead of decoding real MXF essence.
- **Phase 1 (MXF Header Metadata parser) -- done.** `src/mxf/` parses the
  Header Partition Pack and walks Header Metadata (ContentStorage -> Package
  -> Track -> Sequence -> Descriptor) to resolve video/audio parameters and
  frame count from a real `.mxf` file, with zero essence decode and zero
  Index Table dependency. Not yet wired into `MxfSource()` -- that lands with
  Phase 2's essence walker. Unit-tested (`tests/mxf_parser_tests/`) against a
  hand-crafted synthetic header; real-world vendor-file testing is Phase 8.
- **Phase 2 (sequential essence KLV walker) -- done.** `src/mxf/essence_walker.*`
  walks forward from `headerMetadataEnd` through Body Partition(s), yielding
  one raw essence element (KLV Value bytes) at a time for the video/audio
  Track identified in Phase 1 (matched via TrackNumber, see FFmpeg
  mxfdec.c's `mxf_essence_element_key`), skipping partition packs, KLV Fill,
  index tables, system items, and other tracks' elements. Never seeks
  backward. MVP scope: frame-wrapped OP1a only (clip-wrapped essence and
  OP-Atom are Phase 7). Unit-tested against a synthetic Body Partition.
- **Phase 3 (codec decode integration) -- done.** `src/codec/` adds a small
  `Decoder` interface (`SendAndReceive`, with an empty-input "drain" call
  for codecs with internal reorder latency) plus two implementations:
  `RawUyvyDecoder` (uncompressed 4:2:2 8-bit, no FFmpeg involved -- the KLV
  Value already *is* the pixel data) and `AvcodecDecoder` (generic
  libavcodec wrapper covering every `CodecFamily` in `ul_tables.h` --
  MPEG-2, DNxHD, ProRes, H.264/AVC-Intra, FFV1, ...). FFmpeg is vendored via
  `cmake/FetchFFmpeg.cmake` (see Build section). Verified with a genuine
  encode-then-decode round trip (avcodec's own MPEG-2 encoder produces the
  test input, since no real MXF/ES sample files are available in this
  environment) on both Windows and Linux.
- **Phase 4 (ring-buffer engine + decode thread) -- done.** `src/core/frame_buffer.h`
  is a thread-safe sliding-window ring buffer: a single background decode
  thread produces frames in order, self-pacing to stay at most
  `buffer_frames` ahead of the highest index any consumer has requested (the
  "watermark"), eagerly filling from empty rather than waiting to be asked.
  `src/core/source_engine.*` wires this to the Phase 1-3 pieces (open file
  -> parse header -> pick a decoder -> walk essence -> decode -> ring
  buffer), including codec decode-latency handling (draining buffered
  frames once essence runs out, exactly the MPEG-2 behavior hit in the
  Phase 3 test). `GetVideoFrame(n)` blocks until `n` is ready, evicted (past
  the buffer window -- reported as a clear error, never a silent reseek),
  or end-of-stream. Unit-tested for sequential playback, cache hits,
  eviction, and unsupported-codec handling; stress-run 30-45x on both
  platforms with no observed race conditions. Audio is not wired into a
  ring buffer yet (`EssenceWalker` already yields audio elements; consuming
  them is a follow-up, see Phase 7/8).
- **Phase 5 (AviSynth+ adapter) -- done.** `MxfSource(file, buffer_frames=300)`
  now wraps a real `SourceEngine`: `GetVideoInfo()` reflects the actual
  parsed/decoded stream (dimensions, frame rate, frame count, pixel format
  -- UYVY packed essence is repacked to YUY2 since that's AviSynth's only
  packed 4:2:2 format, other formats map straight to CS_YV12/YV16/YUV420P10/
  YUV422P10), and `GetFrame(n)` blocks on the ring buffer. `MT_SERIALIZED`
  is declared so the AviSynth+ core funnels concurrent `Prefetch()` callers
  through us one at a time. Verified against the **real** AviSynth+ 3.7.3
  core DLL through ffmpeg's actual `-f avisynth` demuxer (not just our own
  unit tests) using synthetic `.mxf` files generated by
  `tests/mxf_parser_tests/gen_test_mxf.cpp` -- this real-host testing caught
  two genuine ring-buffer eviction bugs the in-process unit tests missed
  entirely (peeking frame 0 for its pixel format let the eager-fill producer
  evict frame 0 before the host's real first request; and real hosts
  legitimately re-request the same frame index twice in a row -- e.g. once
  to probe the stream format, once for real -- which an off-by-one in the
  producer's pacing formula didn't tolerate). Both are now covered by
  dedicated regression tests in `source_engine_test.cpp`. Also fixed along
  the way: Linux shared-library builds needed `POSITION_INDEPENDENT_CODE`
  once the adapters started actually linking `mxf_core`'s implementation
  rather than just its headers.
- **Phase 6 (VapourSynth adapter) -- done.** `MxfSource(file, buffer_frames=300)`
  wraps the same `SourceEngine`. `fmUnordered` is used (matches BestSource's
  own choice for this exact "single logical decode stream the core still
  prefetches" situation). Since VapourSynth has no packed pixel formats at
  all, UYVY essence is unpacked into 3-plane YUV422P8 (`queryVideoFormat`
  with `subSamplingW=1, subSamplingH=0`) rather than AviSynth's YUY2
  byte-shuffle; other formats map straight across. Verified against the
  real VapourSynth R76 core through `vspipe -c y4m ... | ffmpeg`, including
  under explicit high concurrency (`vspipe -r 16`, forcing 16 concurrent
  frame requests) -- all passed cleanly with no changes needed beyond
  Phase 5's fixes, confirming those ring-buffer fixes were correctly made
  host-agnostic at the `SourceEngine`/`FrameBuffer` layer rather than
  AviSynth-specific. Stress-run 18+ times across both small (buffer_frames=5,
  20 frames) and larger (default buffer_frames=300, 60 frames) synthetic
  files with no failures.
- **Phase 9 (real-world Atomos Shogun XAVC fix + XAVC-L support) -- done.**
  Real XAVC-Intra and XAVC-L (long-GOP) files from an Atomos Shogun (both
  1920x1088/1080p50) exposed two general parsing bugs, not anything
  XAVC-specific: (1) `header_parser.cpp`'s RIP-based `headerMetadataEnd`
  fixup assumed RIP's second entry is always a Body Partition immediately
  following the Header -- files with no separate Body Partition at all
  (essence written directly after Header Metadata, RIP's second entry is
  the Footer instead) had it jump `headerMetadataEnd` straight to the
  Footer, causing `EssenceWalker` to skip every essence element and fail
  with "MXF essence ended before reaching the declared frame count"; fixed
  by only trusting that RIP entry when it actually resolves to a Body
  Partition Pack. (2) `EssenceWalker` misparsed this recorder's raw
  zero-byte KAG-boundary padding (not a spec-compliant KLV Fill Item) as a
  run of bogus zero-length KLVs, desyncing before it reached the real
  essence; fixed with a resync helper that scans forward for the next
  legitimate KLV key (SMPTE UL designator `06.0E.2B.34`) instead of
  trusting a length field derived from noise. **Correction (2026-10-21):**
  (2) was a misdiagnosis. A KLV walk from byte 0 shows these files are
  intact KLV for KLV; their padding is regular KLV Fill. The walk started
  at a wrong offset, inside a KLV Fill's zero value, because
  HeaderByteCount was counted from the end of the Partition Pack instead
  of from after its KAG Fill (see `PartitionContentStart`). With the
  corrected base, no resync is needed on these files, and strict mode no
  longer resyncs at all (see [Strict mode](#strict-mode)). Both are regression-tested
  with hand-crafted synthetic files reproducing each layout
  (`essence_walker_test.cpp`). Separately, XAVC-L's PictureEssenceCoding UL
  (registered under a different family than AVC-Intra) is now classified as
  its own `CodecFamily::kH264LongGop` -- decoded via the same generic
  `AvcodecDecoder`/H.264 path as `kH264`, but correctly excluded from the
  intra-only seek fast path since it has real B-frame dependencies (XAVC-I
  itself already matched the existing AVC-Intra UL entry and needed no
  table change). Verified end-to-end against the real files with
  `read_test_mxf` (every frame of both files decodes correctly), not just
  synthetic unit tests.
- **Phase 10 (Index Table Segment reading for real random-access seeks) --
  done.** New `src/mxf/index_table.h/.cpp` parses MXF Index Table Segments
  (SMPTE 377-1 SS10, key/tags verified against FFmpeg's mxfdec.c/mxfenc.c)
  and builds a flattened, binary-searchable per-BodySID lookup
  (`EssenceIndex`) supporting both CBR (`EditUnitByteCount` formula) and
  VBR (`IndexEntryArray`, via a new batch reader -- `GetUlArray` wasn't
  reusable, it's hardcoded to 16-byte-UL batches) edit units, stitched
  across multiple segments/partitions where present.
  `SourceEngine::SeekAndDecode`'s GOP-based branch (previously always a
  full O(target) redecode from essence element 0 for every out-of-window
  request -- the real mechanism behind slow/janky scrubbing for long-GOP
  codecs like XDCAM MPEG-2 and XAVC-L) now consults this index first: when
  it resolves, decode starts from the nearest preceding keyframe's byte
  offset instead, exposed via a new `IndexAssistedSeekCount()` diagnostic.
  Absent or inapplicable index (nothing found, target outside every
  covered range) degrades silently to the original from-0 behavior --
  purely additive, verified via the existing intra-only fast path and a
  new synthetic-GOP regression suite remaining unaffected. Also unblocks
  the frame-count fallback: `Open()` now derives `numFrames` from the
  index's own covered range when Header Metadata's Duration is absent.
  Real-file verification (`read_test_mxf`, extended to report index
  coverage) against the Phase 9 XAVC-L file found and fixed a second
  instance of that phase's zero-byte-KAG-padding bug (since found to be the
  same wrong-offset error, see the correction under Phase 9) -- this time in the
  new Index Table scanner, not `EssenceWalker` -- fixed by promoting the
  resync helpers to shared utilities in `mxf/klv.h/.cpp`. Confirmed against
  the real file: index built, covers all 3115 frames, a genuine
  eviction-forced seek resolved via the index and decoded correctly. New
  tests: `tests/mxf_parser_tests/index_table_test.cpp` (13 cases) plus
  `source_engine_test.cpp` additions using a real encoded MPEG-2 I/P-frame
  sequence (avcodec's own encoder) to prove byte-identical output both with
  and without an index present.
- **Corpus hardening (2026-10-16 scan of 1479 real files, fixes 2026-10-19)
  -- done, corpus re-run pending.** UL matching ignores the version byte
  (DNxHR, KLV Fill); SMPTE 382 default PCM label; long and Unicode paths;
  [strict mode](#strict-mode) (default on) that fails damaged files in under
  a second instead of scanning them; essence carried in the Header Partition
  (Fixup 1 no longer treats it as metadata: 39-978 GB UHD H.264 files open in
  1-2 s); CBR index entries no longer run past the essence, and audio is
  built when the index covers more than the Duration (audio back on about
  250 files); D-10/IMX 8-Ch AES3 Element audio; yuv422p12/yuv444p10/
  yuv444p12 output and 12-bit ProRes 4444 (codec tag from the coding UL);
  block-read resync over raw padding and garbage. Each fix was checked
  bit-exactly against ffmpeg on real files from the scan.

## Supported MXF files

**MXF structure**

| | Status |
|---|---|
| OP1a, frame-wrapped essence (one KLV per edit unit) | supported |
| Index Table present (in the Header, Body or Footer Partition) | required by [strict mode](#strict-mode); without strict, a file with no index is decoded from the start |
| Header Open/Incomplete, truncated or growing files | refused in strict mode; without strict, opened as far as they can be read |
| OP-Atom, clip-wrapped essence (one KLV for the whole track; e.g. P2 DV and AVC-Intra) | **not supported**: strict mode refuses it, without strict it exposes only the first frame or times out |
| OP1b with several BodySIDs (e.g. P2 AVC-LongG) | **not supported**: the per-stream indexes are not separated |

One video track per file; every audio track is mixed into one stream.

**Video codecs** (decoded by libavcodec; output formats in [Video output formats](#video-output-formats))

| Codec | Typical source | Checked on real files |
|---|---|---|
| MPEG-2, 4:2:2 and 4:2:0 | XDCAM HD / HD422, D-10 / IMX | yes |
| H.264, 8 and 10 bit | AVC-Intra, XAVC, XAVC-L (also UHD), generic H.264 | yes |
| Apple ProRes 422, 4444, 4444 XQ | | yes |
| DNxHD / DNxHR (incl. HQX, 444, 10 and 12 bit) | | yes |
| Uncompressed 8-bit 4:2:2 (UYVY) | | yes |
| DV, JPEG 2000, MPEG-4, FFV1 | | recognized and mapped, **not verified** against real MXF files |

Anything libavcodec decodes to another pixel format (for example ProRes with
alpha) fails at open with a clear message.

**Audio** (details and `use_audio` policy in [Audio](#audio))

- Supported: uncompressed linear PCM, 16/24/32 bit, in SMPTE 382 Generic
  Container sound elements (WAVE or AES3 descriptors) and in the D-10/IMX
  8-channel AES3 element.
- Not supported: compressed audio (AC-3, MP2, AAC), big-endian AIFF coding, and
  tracks that disagree on bit depth or sample rate. Such files play as
  video-only by default, or fail to open with `use_audio="always"`.

## Parameters

`MxfSource(...)` takes the same parameters in both AviSynth+ and VapourSynth
(VapourSynth spells `handle_origin`/`use_audio` etc. the same way, just via
keyword args to `core.mxfsource.MxfSource(...)`):

| Parameter | Type | Default | Meaning |
|---|---|---|---|
| `file` | string | *(required)* | Path (or `http(s)://` URL) to the `.mxf` file. |
| `buffer_frames` | int | auto | Ring-buffer window size, in frames -- how much decoded history is retained before the oldest frame is evicted. Not given: sized by memory, 1 GiB worth of frames (16-300; about 240 for HD, 30 for UHD 10-bit 4:2:2), doubled up to 4 GiB (at most 1024 frames) whenever a frame just below the window is requested, e.g. when stepping backwards in an editor. A given value is a fixed size and never grows. |
| `eagerdecode` | int | `8` | How far ahead of the highest-ever-requested frame the background decode thread may eagerly race unsolicited, capped at `buffer_frames`. |
| `back_seek_preroll` | int | `8` | When a jump far ahead or far back restarts the decoder, it starts this many frames before the requested frame (at most half the buffer), so a temporal filter after the jump and stepping back from there are served from the buffer. Usually free on long-GOP, where decoding starts at the preceding keyframe anyway; raise it for wide temporal filters or `Reverse()`. |
| `logfile` | string | *(none)* | Path to append every plugin message to, warnings and informational ones (including the serve-stats summary when the clip closes), timestamped, regardless of `debug` -- stderr alone is invisible to GUI hosts. |
| `httpheaders` | string | *(none)* | Raw `"Name: value\r\n..."` header block sent with every request when `file` is an `http(s)://` URL. |
| `handle_origin` | bool | `true` | Whether to honor the MXF SourcePackage `Track.Origin` pre-roll trim (matches what an NLE shows for the edited sequence) or expose the full untrimmed essence. |
| `use_audio` | string | `"supported"` | Audio fallback policy -- see below. |
| `indexurl` | string | *(none)* | Fetch a previously published essence index (JSON, see `MxfExtractIndex` below) from this URL or local path instead of building it locally. Any problem (network error, BodySID/version mismatch, byte offsets implausible for this file) fails the open; there is no silent fallback to a local build. |
| `start_frame` | int | `0` | Performance hint: the first frame the caller will request. The decode thread starts at the nearest keyframe before it (via the Index Table) instead of at frame 0, so a pipeline that processes a chunk far into the file is served from the ring buffer. Earlier frames still work, just without this optimization. Values `<= 0` or past the end are ignored. |
| `strict` | bool (VapourSynth: int) | `true` | Refuse incomplete/damaged files with an explicit error instead of opening whatever can be salvaged, and never scan the whole file to build an index -- see [Strict mode](#strict-mode). |
| `verify` | bool (VapourSynth: int) | `false` | Check every index entry against the essence before opening, see [Verifying an index](#verifying-an-index). With `strict`, a wrong index or damaged essence fails the open; without, wrong entries are repaired from the essence. |
| `debug` | bool (VapourSynth: int) | `false` | Also print informational messages to stderr, such as the serve-stats summary when the clip closes (frames served from the ring buffer vs. via fallback re-decode). By default only warnings are printed (damaged essence, missing index, audio dropped, ...). |

In VapourSynth, `MxfSource` returns a single video clip for files without
exposed audio, and `clip` plus `audio` otherwise:

```python
r = core.mxfsource.MxfSource("file.mxf")
clip = r if isinstance(r, vs.VideoNode) else r["clip"]
```

`MxfExtractIndex(file, targeturl, httpheaders="", strict=true, verify=false)`
(both hosts; `strict` and `verify` are ints in VapourSynth) builds the essence
index of `file` and
publishes it as JSON to `targeturl` (local path or `http(s)://` URL, written
with PUT), for `MxfSource(..., indexurl=...)` on other machines that read a
byte-identical copy of the same file. It checks the file and builds the
index with exactly the same code as `MxfSource` (`src/core/index_policy.h`),
so with the same `strict` setting it refuses exactly the files `MxfSource`
refuses, with the same message, and never publishes an index for a file
`MxfSource` wouldn't open. The one inherent difference: without strict,
`MxfSource` can still open a file for which no index can be built at all
(it then decodes from the start), while `MxfExtractIndex` has nothing to
publish and fails.

### Seeking and access patterns

The plugin is built for forward decoding: one background thread decodes the
file in order into a ring buffer (`buffer_frames`), staying up to
`eagerdecode` frames ahead of the highest frame requested so far. Every other
access pattern works too and always returns the right frame, but how fast it
is depends on whether the frame comes from that buffer or has to be decoded
on its own.

A frame that isn't in the buffer is decoded separately: a fresh decoder
starts at the preceding keyframe (found through the Index Table) and decodes
up to the frame. For intra-only codecs (IMX/D-10, AVC-Intra, DNxHD/DNxHR,
ProRes, uncompressed, DV, JPEG 2000) that is one frame; for long-GOP
(XDCAM HD, XAVC-L and other long-GOP H.264) it is up to a whole GOP per
frame -- measured roughly 30 ms for HD and 250 ms for UHD. To keep this a
one-off, a jump of more than the buffer moves the background decoder
instead:

| Access pattern | Example | What happens |
|---|---|---|
| Sequential | encoding a whole file | Everything from the buffer. |
| Jump ahead | `Trim(1250, 7250)`, seeking forward in an editor | The decoder restarts at the target (from its keyframe), `back_seek_preroll` frames early; everything after it comes from the buffer. |
| Jump back | `Trim(5000,6000) ++ Trim(100,200)`, `Loop()`, scrubbing back to the start and playing | The first 4 frames after the jump are decoded separately, then the decoder restarts there and the rest comes from the buffer. The wait keeps two positions read in turn (next row) from moving the decoder back and forth. |
| Two positions far apart, read in turn | `Interleave(c, c.Trim(1000,0))`, `StackHorizontal` of a clip and a Trim of it | The decoder stays with one of them; the other is decoded frame by frame. |
| Temporal filters, `Prefetch()` | `TemporalSoften(3, ...)`, `Prefetch(16)` | From the buffer: requests spread around the current position, within the buffer. With `buffer_frames` auto, a request just below the buffer doubles it (up to 4 GiB, at most 1024 frames). |
| Stepping back | `Reverse()`, stepping backwards frame by frame in an editor | Frames still in the buffer come from it; earlier ones are decoded separately. After a jump only `back_seek_preroll` frames lie below the target, so stepping back past them is slow on long-GOP. |
| Audio ahead of video by more than the buffer | large negative `DelayAudio`, a host that reads audio far ahead | Correct, but slow: audio and video move the decoder in turn, and most video frames are decoded separately. |

What helps:

- `debug=true` or `logfile=...` prints a summary when the clip closes:
  `served N frame(s) from buffer, M frame(s) via fallback re-decode`, the
  frame ranges that were decoded separately, how often the buffer grew and
  how often the decoder restarted after a jump. Each restart is also logged.
- `back_seek_preroll`: raise it for wide temporal filters behind a `Trim`,
  or for `Reverse()` / stepping back after a jump. It is limited to half the
  buffer, so raise `buffer_frames` along with it when needed.
- `buffer_frames`: a fixed, larger buffer for patterns that reach further
  back than the auto size allows; costs memory (one UHD 10-bit 4:2:2 frame
  is about 33 MB).
- `start_frame`: when a process only reads one range of the file (chunked
  processing), the decoder starts there right away.

### File paths and URLs

`file`, `logfile` and local `indexurl`/`targeturl` paths accept:

- **Long paths**: on Windows, paths of 260 characters or more are opened as
  extended-length paths (`\\?\` for local paths, `\\?\UNC\` for network
  shares), with no `longPathAware` manifest or registry setting needed.
- **Non-ASCII paths**: on Windows the path is decoded as UTF-8 if it is
  valid UTF-8 (what VapourSynth passes), otherwise in the ANSI codepage
  (what AviSynth+ scripts normally carry), and opened through the
  wide-character API. On Linux paths are byte strings and opened as they
  are.
- **`http://` / `https://` URLs** for `file`, with `httpheaders` for
  authentication (S3 presigned URLs need none). See
  [HTTP, HTTPS and S3 presigned URLs](#http-https-and-s3-presigned-urls).

### HTTP, HTTPS and S3 presigned URLs

`file` can be an `http(s)://` URL. The MXF is streamed over HTTP with
libcurl and never downloaded to disk, so a presigned S3 object (or any
HTTP server that supports byte ranges) works like a local path:

```
MxfSource("https://my-bucket.s3.eu-central-1.amazonaws.com/clip.mxf?X-Amz-Algorithm=...&X-Amz-Signature=...")
```

**Authentication.**
- An S3 **presigned GET URL** carries its authorization in the query string,
  so `httpheaders` is not needed.
- For a server that wants a header, pass it in `httpheaders`, e.g.
  `httpheaders="Authorization: Bearer <token>\r\n"`. The headers are sent
  with every request for the source file.
- The URL's signature expires. A `403` is reported as
  `HTTP 403 Forbidden (if this is an S3 presigned URL, it may have expired)`.
  Make the URL valid for as long as the job runs: the file is read for the
  whole playback, not just at open.

**How the file is read.**
- Open sends one ranged `GET` for the first byte (never `HEAD`, because a
  presigned URL is signed for one method, usually `GET`) and takes the file
  size from `Content-Range`.
- Reading is sequential, with up to 4 MiB read ahead. A forward jump of up
  to 256 KiB is skipped on the open connection; a larger or backward jump
  starts a new request with a `Range` header.
- A server without range support still works. Seeking is then emulated by
  discarding data or restarting the download, which can be slow, and a
  warning is logged once.
- Transient failures (connect, DNS, timeout, reset, TLS connect, empty
  reply, HTTP 5xx) are retried twice with 0.5 s and 1 s backoff, resuming
  at the byte where the transfer stopped. Other errors (`403`, `404`,
  `416`, ...) fail at once.
- Connect timeout is 15 s; a transfer below 512 bytes/s for 30 s is
  aborted. TLS certificates are verified and up to 5 redirects are followed.

**Strict mode over HTTP.** The same checks apply as for local files, and
each one costs a ranged request (footer, RIP, index), not a download.

**Publishing and reusing an index.** `MxfExtractIndex(file, targeturl)`
builds the index of a file (local or HTTP) and writes it as JSON to
`targeturl`: a local path, or an `http(s)://` URL written with a single
`PUT`. With S3 that means a **presigned PUT URL**. Other machines then use
`MxfSource(file, indexurl=<presigned GET URL or path>)` against a
byte-identical copy of the file and skip the scan.
- A presigned URL is valid for one method only: use one URL to write the
  index and a different one to read it back.
- No custom headers are sent for the index transfer, so use presigned URLs
  or open paths for `targeturl` and `indexurl`.
- A fetched index is checked against the file (version, BodySID, and that
  its last entry lies within the file size). Any mismatch fails the open;
  it never falls back to a local scan.
- When indexing an HTTP source, the index sections are fetched with
  parallel ranged requests.

### Verifying an index

The Index Table is written by the recording application and is normally
trusted as is. If one of its byte offsets is wrong, or the essence is damaged,
this only shows up later, as a seek that fails ("the fallback re-decode
failed to reach it") or, without strict, as a wrong frame. `verify=true`
(on `MxfSource` and `MxfExtractIndex`) checks this up front:

- **Every index entry** must point at a KLV, and walking on from there the
  next picture element of the video track must come before the next entry.
- **Picture type** (MPEG-2 and long-GOP H.264): the I/P/B type the entry
  announces must match the picture header of the frame it leads to.
- **Between two entries** the KLV headers must chain without damage from
  one picture element exactly to the next entry, passing no other picture
  element (else the entry skips a frame).

The last two catch an index shifted by whole frames, whose offsets all land
on valid KLVs: without `verify`, such an index makes seeks return the wrong
frame without any error.

Only KLV headers and the first 1 KiB of each frame are read: a few small
reads per frame, spread over several threads. For a local file that is seconds for a feature
length file; over `http(s)://` every read is a range request, so it takes
much longer.

| | `strict=true` (default) | `strict=false` |
|---|---|---|
| All good | opens / publishes | opens / publishes |
| Wrong index entries | fails, listing the first 10 (edit unit, byte offset, what is there) | warns, **repairs** them from the essence, opens / publishes the repaired index |
| Damaged essence | fails, naming where | warns; the frames there stay undecodable |

The repair walks the essence from the last good entry before a problem
(headers only, never guessing past damage) and compares every following entry
with the walk until 32 in a row agree, so it also fixes entries inside a
shifted region that passed the checks by coincidence. It repairs wrong
offsets in an intact file exactly; entries behind damaged essence are left as
they are and listed. A fetched index (`indexurl=`) is verified against the file as well.

To diagnose a file that fails to seek, run once and send the message:

```
LoadPlugin("mxfsource.dll")
MxfExtractIndex("file.mxf", "file.index.json", verify=true)
BlankClip()
```

### Chunked distributed processing

The published index is meant for splitting one file across many workers
(serverless functions, batch jobs, render nodes). A typical layout:

1. A **dispatcher** runs `MxfExtractIndex(source, <presigned PUT url>)`
   once. It scans the file and stores the index at a presigned URL.
2. The dispatcher starts one **worker** per chunk (for example per 100 frames)
   and hands each one the source URL, the index URL and its frame range.
3. Each worker opens the source with `indexurl` and `start_frame`, filters and
   encodes only its frames, and writes the result to a presigned URL.

```
src = MxfSource("<presigned GET url of the mxf>", indexurl="<presigned GET url of the index>", start_frame=1200)
src.Trim(1200, 1299)    # worker for the chunk 1200..1299
```

- `MxfExtractIndex` is a short action, unlike building an index with ffms2: it
  never decodes video and never reads the essence frames. It reads the
  file's MXF Index Table and its metadata, sets up no decoder, and publishes
  the result. With strict mode on (the default) it never scans the whole
  file; a file without a usable index is refused instead. Without strict, a
  file with no Index Table can still make it read through the essence.
- Without `indexurl`, every worker would repeat the full index scan.
  With it, a worker reads the index (a few MB at most) and goes straight to
  the keyframe before `start_frame`.
- `start_frame` only places the decode thread. Earlier frames still work, just
  without that optimization.
- All workers must read a byte-identical copy of the file, or the index check
  fails the open.
- Presigned URLs must outlive the slowest worker, including queue time.

### Video output formats

| Decoded format | AviSynth+ | VapourSynth | Typical source |
|---|---|---|---|
| UYVY 4:2:2 8-bit (uncompressed) | `YUY2` (byte-reordered) | `YUV422P8` (unpacked) | uncompressed MXF |
| yuv420p | `YV12` | `YUV420P8` | MPEG-2 4:2:0, H.264 |
| yuv422p | `YV16` | `YUV422P8` | MPEG-2 422P (XDCAM HD422, D-10) |
| yuv420p10 | `YUV420P10` | `YUV420P10` | H.264 10-bit |
| yuv422p10 | `YUV422P10` | `YUV422P10` | AVC-Intra, XAVC, DNxHR HQX 10-bit, ProRes 422 |
| yuv422p12 | `YUV422P12` | `YUV422P12` | DNxHR HQX 12-bit |
| yuv444p10 | `YUV444P10` | `YUV444P10` | DNxHR 444 10-bit |
| yuv444p12 | `YUV444P12` | `YUV444P12` | DNxHR 444 12-bit, ProRes 4444 / 4444 XQ |

Anything else (e.g. ProRes 4444 with alpha, `yuva444p`) fails at open with
"decoded pixel format has no ... mapping yet". For ProRes, the decoder gets
FFmpeg's codec tag (`apco` ... `ap4x`) from the PictureEssenceCoding UL, as
FFmpeg's own MXF demuxer does; without it, 4444 profiles would decode at 10
instead of 12 bit.

### Field order

Output frames are always whole frames (frame-based, never fields). Their
field order comes from the decoder, which reads it from the bitstream for
MPEG-2, H.264, DV, ProRes and DNxHD. If the decoder doesn't report one
(e.g. uncompressed video), the MXF picture descriptor is used:
FrameLayout FullFrame or SegmentedFrame means progressive, and
SeparateFields or MixedFields with FieldDominance 1 means TFF. Any other
combination stays unknown; there's no verified real file for a BFF
descriptor.

| | AviSynth+ | VapourSynth |
|---|---|---|
| Per clip | `IsTFF()`/`IsBFF()` and `GetParity()`, from the first frame; progressive or unknown sets neither | -- |
| Per frame | `_FieldBased` frame property (AviSynth+ interface 8 or later) | `_FieldBased` frame property |

`_FieldBased` is 0 for progressive, 1 for BFF, 2 for TFF, and is not set
when the field order is unknown. On the sample share, interlaced XDCAM HD422,
XAVC, D-10 and K2 SD material is TFF, and ProRes, DNxHR, XAVC-L UHD and 720p
material is progressive. The plugin matches ffprobe frame for frame on these.
(Before 2026-10-21, `GetParity` always returned false, i.e. BFF, and no
`_FieldBased` was set.)

### Audio

Every audio track found in the file is summed into one interleaved output
stream at the file's native bit depth (`SAMPLE_INT16`/`SAMPLE_INT24`/
`SAMPLE_INT32` in AviSynth+, the equivalent integer format in VapourSynth) --
no format conversion, no resampling. With `handle_origin=true` the Origin
pre-roll is trimmed from the audio exactly like from the video.

Supported (all uncompressed linear PCM; the UL version byte is ignored):

| Sound essence | How it is recognized | Payload |
|---|---|---|
| Generic Container sound element (SMPTE 382), WAVE or AES3 descriptor | SoundEssenceCompression `06 0E 2B 34 04 01 01 xx 04 02 02 01 00`, `... 01 01` (SMPTE382MDefaultUncompressedSoundCoding, the most common label on real files), `... 01 7F`, or no SoundEssenceCompression at all (AES3 descriptors often omit it) | plain packed little-endian PCM, used as is |
| D-10/IMX **8-Ch AES3 Element** (SMPTE 386 / 331) | element key `06 0E 2B 34 01 02 01 01 0D 01 03 01 06 01 10 00` (TrackNumber item type 0x06, element type 0x10) | 4-byte element header, then per sample 8 slots of a 32-bit word with the PCM sample in bits 4-27; unpacked to plain PCM (`(word >> 4) & 0xFFFFFF` for 24 bit, `>> 12` for 16 bit) |

Not supported, treated as "unsupported audio": compressed codecs
(AC-3/MP2/AAC), AIFF coding (`... 04 02 02 01 7E`, big-endian), any other
SDTI-CP sound element type, and tracks that disagree on bit depth or sample
rate.

Audio is located through the video Index Table, without scanning the
essence: the index must cover every edit unit from 0 up to the
SourcePackage Duration (it may cover more; edit units past the Duration are
ignored). A file without such an index, e.g. a truncated one, gets no
audio. All of this is governed by `use_audio`:

| `use_audio` | Audio present & supported | Audio present & unsupported | No audio at all |
|---|---|---|---|
| `"supported"` (default) | exposed | degrades to video-only, no error, note in `logfile` | video-only, no error |
| `"always"` | exposed | `MxfSource()` throws/fails to open | video-only, no error |
| `"never"` | suppressed (video-only) | suppressed (video-only) | video-only, no error |

A file with no audio at all never fails under any `use_audio` setting --
only "audio present but not in a state this plugin can output" is ever a
hard failure, and only under `"always"`.

### Strict mode

**Strict mode is the default (`strict=true`).** Pass `strict=false` to open
incomplete or damaged files on a best-effort basis. Everything below applies
identically to `MxfSource` and `MxfExtractIndex`: both run the same checks
in the same order and fail with the same messages.

`strict` is meant for automated pipelines, where a file that
is silently shorter than declared, or that takes minutes to open, is worse
than an explicit error. With `strict=true`, `MxfSource()` fails at open time
for files that are incomplete or damaged, and it never builds an index by
walking the whole file. With `strict=false` the plugin opens such files on a
best-effort basis (see [Without strict](#without-strict)).

Strict mode only adds checks. It never changes how an intact file is
parsed, indexed or decoded: a file that passes opens exactly as it would
with `strict=false`.

#### Terms

- **Closed / Open partition:** byte 14 of a Partition Pack key (SMPTE
  377-1). `0x03` Closed Incomplete and `0x04` Closed Complete count as
  Closed; `0x01` Open Incomplete, `0x02` Open Complete and anything else
  (e.g. `0x00` in pre-2004 files) count as not Closed.
- **Valid partition:** a Partition Pack whose own `ThisPartition` field
  equals the byte offset it was found at. A pointer into essence, garbage or
  zero fill practically never passes this.
- **Valid RIP:** a Random Index Pack parses at the end of the file, its
  first entry is offset 0 and its last entry is a valid partition.
- **Closed Footer:** a valid Footer Partition Pack with Closed status,
  found through the Header Partition Pack's `FooterPartition` field or the
  last entry of the RIP.
- **Finished file:** a Closed Footer exists, *or else* the Header Partition
  is Closed. The footer wins: a recorder that leaves the header Open but
  writes a Closed Footer has finished the file, and the plugin already takes
  Duration and the other metadata from the footer's copy in that case.
- **Growing file:** not finished, i.e. Open header and no Closed Footer --
  a recording still being written, or one that was aborted. Its Duration is
  not final.
- **Reachable index:** Index Table Segments in the Header Partition (e.g. a
  CBR/CBG segment with `EditUnitByteCount`, which indexes every frame by
  arithmetic) or in any partition listed in the RIP. Body Partitions that
  are not in a RIP are never searched for.

#### Checks, in order

The first failing check ends the open with the message shown. Checks 1-3
run right after the Header Metadata is parsed, before any index work, so a
damaged file fails within a few small reads (well under a second over SMB,
also for 100+ GB files).

| # | Fails when | Applies to | Error starts with |
|---|---|---|---|
| 1 | The last 64 KiB of the file are all zero bytes. | every file | `strict: unexpected zero bytes at offset O (verified from there to the end of the file at S)` |
| 2 | The Header Partition is Closed, its `FooterPartition` field is nonzero, there is no valid Footer Partition Pack at that offset, no valid RIP and no Closed Footer. | Closed header | `strict: Header Partition is Closed and declares the Footer Partition at byte F, but the file is only S bytes; the file is truncated` (offset beyond EOF) or `... but there is no valid Footer Partition Pack there and no Random Index Pack` |
| 3 | No Index Table Segment is reachable. | every file | `strict: no Index Table Segment found in the Header Partition or any partition listed in a Random Index Pack` |
| 4 | The index covers fewer edit units than the file's Duration (SourcePackage Duration, including Origin pre-roll). | finished file | `strict: the file is finalized (Closed Partition) and declares N edit units, but the Index Table covers only M` |

Every message ends with `(disable strict to open it anyway)`.

Notes on the individual checks:

1. **Zero tail.** A valid MXF file ends with a RIP or a Footer Partition,
   never with zeros. Real recorders pad with KLV Fill items, so 64 KiB
   (`kMaxExpectedTrailingZeroBytes`) leaves a wide margin against a
   coincidentally zero-valued tail. This also fails growing files: a recorder that preallocated the
   file with zeros is still incomplete. The check is one 64 KiB read at the
   end of the file. The message reports only what was read: the offset where
   that block starts (file size - 64 KiB) and that it is zero up to the end.
   The zero run may well start earlier, but the plugin does not read further
   to find out.
2. **Missing footer.** A Closed header promises a footer. A `FooterPartition`
   of 0 (unknown) is not an error by itself, since the Footer Partition is
   optional; such a file is still covered by checks 3 and 4. A valid RIP or a
   Closed Footer found through the RIP overrides a wrong `FooterPartition`
   field (seen on Omneon recordings).
3. **No reachable index.** Without strict, the plugin then builds an index
   by walking every KLV of the file (`BuildEssenceIndexFromBitstream`),
   which takes minutes on large files over SMB. Strict never does that, for
   any file, finished or growing. A growing file therefore opens in strict
   mode only if it has a RIP or a CBR/CBG index in its Header Partition.
4. **Short index.** Only for finished files. A growing file has no final
   Duration; it opens, with the frame count rule of
   [Frame count when the essence ends early](#frame-count-when-the-essence-ends-early).
   Coverage is measured as the highest indexed edit unit + 1; gaps inside
   the index are not checked. Only the top-level file package's Duration
   counts: lower-level source packages (tape, import) may legitimately be
   much longer (an Avid import package declares 21600000 edit units).

#### Decision overview

| File | strict=true | strict=false |
|---|---|---|
| Finished, intact, index covers Duration | opens | opens |
| Any file ending in > 64 KiB of zeros | fails (1) | opens; without a reachable index via the full-file scan |
| Closed header, footer missing/beyond EOF, no RIP | fails (2) | opens via the full-file scan |
| Finished, no reachable index | fails (3) | opens via the full-file scan |
| Finished, index shorter than Duration | fails (4) | opens; frame count clamped if the essence ends early (below) |
| Growing, index reachable (RIP or CBR/CBG in Header Partition) | opens; frame count clamped if the essence ends early | opens; same |
| Growing, no reachable index | fails (3) | opens via the full-file scan |

#### What strict mode does not do

- It does not repair or partially open damaged files. There is no recovery
  of partitions that the RIP doesn't list, and no indexing of a last
  partition whose index would have been in the missing footer.
- It does not verify essence content (codec bitstream errors, frames cut
  off in the middle are only found when decoding).
- It does not verify the essence before playback. During playback the
  essence walk reads key and length of each KLV and skips every value by its
  length, never looking into it. In an intact file every byte belongs to a
  KLV, so the next key always starts where the previous value ends. If it
  doesn't, the file is damaged there. Strict mode never guesses past that
  spot: the walk ends there, the frames after it fail with an error, and the
  plugin log names the offset (`no valid KLV at byte X (the file is damaged
  there)`). Without strict, the walk searches for the next key, but only
  within 10 MiB (`kMaxResyncBytes`), and logs what it skipped. A damaged
  length field ends the walk in both modes.
- With `indexurl`, the fetched index replaces the local index build, so
  check 3 does not apply; checks 1, 2 and 4 do.

#### Without strict

`strict=false` is best effort: no integrity checks, the full-file bitstream
scan when no index is reachable (minutes on large files over SMB), and the
frame count rule below.

#### Frame count when the essence ends early

Applies to every file that opens with an index shorter than its Duration,
i.e. with `strict=false`, and to growing files in strict mode. Many real
files declare more frames than they contain: truncated copies, recordings
cut off in zero fill, and writers that record the planned length
(Schuhegestohlen: Duration 714, 706 frames of essence, then a proper Footer).

After the last index entry the plugin reads about one content package of
KLV headers:

- If the **end of the essence** follows (Footer Partition, RIP, end of file
  or zero fill), only the frames that exist are exposed, and the log says
  `Header Metadata declares N frames, but the essence ends after M; exposing
  M frames`. Their audio is exposed too. Without this, the clip claimed N
  frames, ended in decode errors at frame M, and lost all its audio (the
  audio index must cover every exposed frame).
- If that last content package lacks some of its sound elements (cut off in
  the middle), that frame is dropped too, so video and audio end together
  (`..., the last one with sound elements missing; exposing M-1 frames`).
- If **more essence** follows (another picture element or a Body
  Partition), the index merely stops early; the declared frame count stays
  and the unindexed frames are reached by the sequential walk.

Real files, `strict=false`: Schuhegestohlen 714 -> 706 frames, Wienheute
2260 -> 1535, Sobotka 1401 -> 1348 (last of 1349 frames incomplete); all
decode without errors, audio bit-identical to ffmpeg.

#### Real-world examples

From the 2026-10-16 scan of 1479 files on `SERVER/SHARE/...`, opened
with `strict=true`:

| File | Result |
|---|---|
| XDCAM HD422 / D10 recordings of 0.42, 2.06 and 126.6 GB with a zero-filled tail where the footer should be | fail with check 1 in < 0.5 s (without strict they open only via the full-file scan) |
| First 0.70 GB of a finished 1.75 GB file, cut off mid-frame (header says the footer is at byte 1748772352) | fails with check 2 in 0.2 s |
| `klvfill_reprod - Kopie.mxf`: footer declared 590 KB beyond EOF | fails with check 2 in 0.2 s |
| XDCAM HD422 files without any index, Footer or RIP (Header Closed Complete, `FooterPartition=0`; 20 files of 0.02-2.4 GB). Some are complete, many are cut off at a KLV boundary (e.g. 22.7 MB holding 75 of 670 declared frames); without an index only a full scan could tell which | fail with check 3 by design; open with `strict=false` |
| Intact 9.5 GB AVC-Intra file | opens, unchanged |
| Header Open Incomplete, index reachable | opens |

#### Implementation

- `src/mxf/integrity.{h,cpp}`: `ProbeFileIntegrity()` (read-only probe:
  partition statuses, Footer/RIP validity, zero tail) and
  `CheckStrictIntegrity()` (checks 1 and 2).
- `src/core/index_policy.{h,cpp}`: the shared sequence both `MxfSource`
  (`SourceEngine::Open()`) and `MxfExtractIndex` (`ExtractAndPublishIndex()`)
  run, in this order: `CheckIntegrityBeforeIndex()` (checks 1 and 2),
  `BuildVideoIndex()` (check 3, no bitstream scan under strict),
  `CheckIndexCoverage()` (check 4). Neither caller may implement any of
  these decisions on its own. `EssenceEndsAfter()` (same file) decides the
  frame count clamp; `SourceEngine::Open()` applies it.
- `src/core/options.h`: `OpenOptions::strict`; `ExtractAndPublishIndex`'s
  `strict` parameter.
- Tests: `TestStrict*` and `TestExtractIndexAgreesWithOpenForEveryStrictCase`
  (same verdict and message from both entry points for every strict
  scenario, strict on and off) in
  `tests/mxf_parser_tests/source_engine_test.cpp`.

Changes to strict mode must update this section.

## Build

Requires a C++17 compiler and CMake >= 3.24. FFmpeg's `libavcodec`/`libavutil`
(LGPL prebuilt shared libraries, from BtbN/FFmpeg-Builds) are fetched
automatically by CMake's `FetchContent` on first configure -- no system
FFmpeg install needed, and nothing is committed to the repo (cached under
`build/_deps/`, gitignored). Phase 0-2 alone have no external dependencies
beyond the vendored AviSynth+/VapourSynth SDK headers in `third_party/`.

**If building on WSL against a Windows-hosted checkout** (`/mnt/c`, `/mnt/d`,
...): the DrvFs mount doesn't reliably preserve the symlinks inside the
FFmpeg archive, and `FetchContent`'s extraction can fail with `file RENAME
failed ... Permission denied`. Check the repo out onto WSL's native
filesystem (e.g. `~/`) instead -- this isn't a project bug, just a DrvFs
limitation with symlink-containing archives.

### Windows (MSVC via Visual Studio 2022)

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs `build/src/avs/Release/mxfsource.dll` and
`build/src/vs/Release/mxfsource_vs.dll`.

### Linux (gcc/clang)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -- -j$(nproc)
```

Outputs `build/src/avs/mxfsource_avs.so` and `build/src/vs/mxfsource_vs.so`.

The FFmpeg libraries are copied next to each plugin and found through an
`$ORIGIN` run path, so a plugin directory with the `.so` and the three
`libav*`/`libswresample` files can be moved anywhere. The prebuilt binaries
need glibc 2.38 or newer and OpenSSL 3 (for example Ubuntu 24.04, Debian 13);
build from source on older distributions.

## Verify

`samples/test.avs`/`test.vpy` point at `path/to/file.mxf` as a placeholder --
point them at a real `.mxf` file, or generate a small synthetic one (no real
sample files are needed to sanity-check a build):

```
cmake --build build --target gen_test_mxf
build/tests/mxf_parser_tests/Release/gen_test_mxf test.mxf 60 64 32   # 60 frames, 64x32

# AviSynth+, matching the intended ffmpeg pipeline:
ffmpeg -f avisynth -i samples/test.avs -f null -

# VapourSynth, matching the intended ffmpeg pipeline:
vspipe -c y4m samples/test.vpy - | ffmpeg -i - -f null -
```

Standalone parser unit tests (no AVS/VS host needed):

```
ctest --test-dir build
```
