# TODO

Plans for the remaining work after Phases 0-6 (MVP: OP1a, frame-wrapped
essence, one video track, ring-buffer forward decode, both host adapters --
see README's "Current status" for what's already done and verified).

## Phase 7: Expand codec/OP coverage

### Context

Phases 0-6 deliver a working MVP: OP1a, frame-wrapped essence, one video
track, uncompressed UYVY or anything `libavcodec` can decode from a
byte-identical elementary-stream KLV Value (MPEG-2/XDCAM long-GOP, DNxHD,
ProRes, generic H.264). That covers the common case, but several real-world
MXF variants either aren't correctly handled yet or haven't been verified:

- **IMX/D-10**: essence items are *padded* to a fixed size (30/40/50 Mbps);
  handing the padded bytes straight to `avcodec_send_packet` (as we do today
  in `AvcodecDecoder::SendAndReceive`, `src/codec/avcodec_decoder.cpp`)
  works by luck for MPEG-2 (decoders stop at the picture-end marker) but
  isn't correct per spec and hasn't been tested against a real D-10 file.
- **AVC-Intra**: NAL framing varies (Annex-B start codes vs AVCC
  length-prefixed, SPS/PPS repeated per frame or only in the descriptor).
  We currently feed raw bytes straight through; no normalization exists.
- **ProRes / JPEG2000**: UL mappings and avcodec IDs exist in
  `src/mxf/ul_tables.h` and `avcodec_decoder.cpp`'s `ToAvCodecId`, but
  neither has been exercised against a real elementary stream the way
  MPEG-2 was in Phase 3's encode/decode round-trip test.
- **OP-Atom**: Avid's one-essence-per-file convention. Not handled at all
  today -- `header_parser.cpp`'s package-walk assumes a MaterialPackage
  pointing at a SourcePackage the normal OP1a way; `PartitionPack.operationalPattern`
  (parsed in `partition.cpp`) is captured but never inspected.
- **Clip-wrapped essence**: a single giant KLV Value for the whole track
  instead of one KLV per edit unit. `EssenceWalker::Next()`
  (`src/mxf/essence_walker.cpp`) assumes frame-wrapping (one Essence Element
  KLV = one frame) and will silently misbehave (treat the whole clip as
  "frame 0") if it ever encounters a clip-wrapped file.

None of this blocks the MVP use case (camera/mezzanine OP1a files), but all
of it is realistic for a general-purpose ffmpeg-facing MXF reader.

### Work items

#### 1. IMX/D-10 padding stripping
- Detect D-10 essence via `CodecFamily::kMpeg2Video` + the specific D-10 UL
  entry in `kCodecUls` (`src/mxf/ul_tables.h:82-84`) -- currently
  indistinguishable from plain XDCAM MPEG-2 once classified; needs its own
  `CodecFamily` value (e.g. `kMpeg2VideoD10`) or a separate flag so the
  decode path knows to trim.
  Actually the real signal is expected to also come from Descriptor's
  container/frame size fields, not the coding UL alone.
- Trim padding to the actual encoded picture length before
  `avcodec_send_packet` -- either by parsing the MPEG-2 picture end marker
  ourselves, or (simpler, and sufficient) by relying on the descriptor's
  known fixed frame size and just constraining reads; confirm which
  approach real D-10 files need once a sample is available.
- New test: synthetic D-10-shaped essence element with trailing zero
  padding, verify decode produces the correct frame and doesn't choke.

#### 2. AVC-Intra NAL normalization
- Add a small bitstream helper (`src/codec/h264_bitstream.{h,cpp}`) that
  detects Annex-B (0x00000001 / 0x000001 start codes) vs AVCC
  (4-byte-length-prefixed) framing and normalizes to whichever
  `avcodec_find_decoder(AV_CODEC_ID_H264)` expects (Annex-B, per the
  decoder's default `extradata`-free operation).
- Wire into `AvcodecDecoder` (`src/codec/avcodec_decoder.cpp`) as a
  pre-processing step gated on `CodecFamily::kH264`.
- Test: hand-construct both NAL framing styles for a tiny synthetic
  AVC-Intra-shaped payload (reuse the Phase 3 approach of using avcodec's
  own H.264 encoder to produce real bitstream bytes, then test both
  Annex-B passthrough and an AVCC-repackaged copy of the same bytes decode
  identically).

#### 3. Verify ProRes and JPEG2000 decode paths
- Extend `decoder_test.cpp`'s encode-then-decode round-trip pattern
  (`EncodeOneMpeg2Frame`, Phase 3) with `EncodeOneProResFrame` and
  `EncodeOneJpeg2000Frame` equivalents, using avcodec's own encoders as the
  test's real-bitstream source (same rationale as Phase 3: no real MXF/ES
  sample files are available in this environment).
- Confirms `ToAvCodecId`/`FromAvPixFmt` mappings in `avcodec_decoder.cpp`
  are actually correct for these codecs, not just plausible.

#### 4. OP-Atom support
- `PartitionPack.operationalPattern` (`partition.h`) needs to be threaded
  through into `MxfStreamInfo` (`header_parser.h`) so callers/tests can
  distinguish OP1a from OP-Atom.
- OP-Atom files are single-essence (one track, no MaterialPackage/
  SourcePackage split the same way OP1a uses); `ParseMxfHeader`
  (`header_parser.cpp`) will need an OP-Atom-specific package-walk path.
  Study BBC bmx's `OPAtomReader` (cited in the original design plan) for
  the exact structural difference before implementing.
- Since OP-Atom conventionally always includes a complete Index Table
  (Avid tooling relies on it), this is also the natural place to start
  actually reading Index Table Segments (still not implemented anywhere)
  if a cheap win presents itself -- but the plan's Duration fallback chain
  (`header_parser.h`'s `durationReliable` comment) doesn't require it, so
  don't block OP-Atom support on it. (Full Index Table reading is now
  tracked as its own item in Phase 8, since it also unlocks real
  random-seek performance -- see Phase 8 item 2.)
- Test: hand-craft a synthetic OP-Atom-shaped file (single track, no
  MaterialPackage indirection) analogous to `synthetic_mxf.h`'s existing
  OP1a builder.

#### 5. Clip-wrapped essence
- Detect wrapping type from the `EssenceContainer` label UL's wrapping-type
  byte (SMPTE 379-2 §7) -- exposed via `PartitionPack.essenceContainers`
  (`partition.h`) but not yet classified anywhere.
- `EssenceWalker` (`essence_walker.h/.cpp`) needs a second code path for
  clip-wrapped tracks: read the single giant KLV Value's frame count and
  size from the descriptor (uncompressed, fixed-size frames -- easy) or
  scan codec-specific internal frame boundaries (JPEG2000 codestream
  markers -- harder, lower priority).
- Start with fixed-size clip-wrapped uncompressed essence (straightforward
  stride-based slicing of the one big Value) before attempting JPEG2000
  codestream-boundary scanning.
- Test: synthetic clip-wrapped file with fixed-size uncompressed frames
  concatenated into one KLV Value.

### Suggested order

1. Clip-wrapped essence (uncompressed only) -- most contained change,
   touches only `EssenceWalker`.
2. OP-Atom -- second most contained, touches `header_parser` only.
3. AVC-Intra NAL normalization -- new isolated helper + one integration
   point in `avcodec_decoder.cpp`.
4. IMX/D-10 padding -- depends on deciding the trimming strategy; look for
   a real sample file before guessing at the "count MPEG-2 picture end
   marker vs. trust descriptor frame size" question.
5. ProRes/JPEG2000 round-trip verification -- lowest risk, mostly test
   coverage rather than new logic; do last since it's the least likely to
   surface design questions that affect the others.

### Verification

Same pattern as Phases 0-6: unit tests against hand-crafted synthetic
files/streams (no real sample MXF files available in this environment),
plus re-running the real end-to-end AviSynth+/VapourSynth verification
(`gen_test_mxf` + `ffmpeg -f avisynth` / `vspipe | ffmpeg`) for each new
code path once it's wired up, the same way Phase 5 caught two real
ring-buffer bugs that pure unit tests missed. Don't skip the real-host
step just because it's slower -- it has been the single highest-signal
check so far.

## Phase 8: Hardening + docs + sample scripts

### Context

Phase 7 (once done) rounds out codec/OP/wrapping coverage. Phase 8 is about
production-readiness of what's already there: real audio output (currently
`GetAudio` always returns silence), faster random seeks for the GOP-based
codecs that still fall back to an O(n) full re-decode-from-0
(`SourceEngine::SeekAndDecode`, `src/core/source_engine.cpp`), a real
answer to the still-unresolved "MXF Essence ended before reaching declared
framecount (4500)" bug hit on the very first real vendor file tested, and
bringing docs/samples in line with the actual current parameter surface
(`buffer_frames`, `eagerdecode`, `logfile` -- all implemented, none of them
in README or the sample scripts yet).

### Work items

#### 1. Real audio output (`GetAudio`/VapourSynth audio node)
- `MxfSource::GetAudio` (`src/avs/avs_adapter.cpp`) unconditionally
  `memset`s silence, and `vi_.audio_samples_per_second = 0` disables audio
  entirely for AviSynth+; the VapourSynth adapter (`src/vs/vs_adapter.cpp`)
  doesn't expose an audio output node at all yet.
- `EssenceWalker` already yields interleaved audio `EssenceElement`s
  (`isVideo == false`) whenever `streamInfo_.hasAudio` -- currently just
  discarded (`DecodeNextFrame`'s `if (!element.isVideo) continue;` in
  `source_engine.cpp`).
- Needs a decode path per audio `CodecFamily`: PCM (`kPcm`) is likely a
  near-trivial byte passthrough (like `RawUyvyDecoder`, no avcodec
  involved); AC-3/MP2/AAC (`kAc3`/`kMp2`/`kAac`) need an avcodec audio
  decode wrapper parallel to `AvcodecDecoder`.
- Needs its own buffering strategy, not a reuse of `FrameBuffer<T>` as-is:
  audio sample count per video frame isn't 1:1 (depends on sample rate vs.
  edit rate), and AviSynth's `GetAudio(buf, start, count)` contract expects
  sample-accurate random access, unlike video's forward-only design goal.
  A separate sample-position-addressable buffer (or just decode-on-demand
  with its own small cache) is more appropriate than trying to force audio
  through the existing frame-indexed ring buffer.
- Wire into AVS's `GetAudio` and VS's `vsapi->createAudioFilter` (a
  separate node from the video one, per VapourSynth's audio API).
- Test: synthetic PCM essence, verify `GetAudio`/the audio node returns
  correct decoded samples for arbitrary `start`/`count`, including ranges
  that don't align with video frame boundaries.

#### 2. Index Table Segment / RIP reading for real random-access seeks
- `SourceEngine::SeekAndDecode`'s GOP-based-codec branch still decodes
  forward from frame 0 for *every* out-of-window request -- correct but
  O(target), since no anchor/keyframe location is known without an index.
- Read Index Table Segments (SMPTE 377-1 §10) from Body/Footer partitions:
  gives byte offset + temporal/key-frame offset per edit unit, enabling a
  real seek -- jump to the nearest preceding I-frame's byte offset and
  decode forward only from there, instead of from 0.
- This is the highest-value single item in Phase 8 for the "primarily
  forward-decode but support random seeks where reasonably easy" goal --
  without it, GOP-based seeks (the codec family of the actual real file
  used for benchmarking this session, XDCAM long-GOP MPEG-2) stay O(n)
  forever, and the existing intra-only fast path in `SeekAndDecode`
  benefits nothing from it (already O(1)-ish via skip-without-decode).
- Also unblocks the frame-count fallback chain already stubbed in
  `source_engine.cpp:84-88` (the `numFrames < 0` early-return) -- an Index
  Table/RIP can supply an authoritative duration when Header Metadata's
  Duration property is absent or unreliable. This is directly relevant to
  the still-open real-file bug below (item 4).
- Test: synthetic file with an Index Table Segment, verify `SeekAndDecode`
  uses it to skip straight to the nearest preceding key frame instead of
  decoding from frame 0, and that the frame produced is byte-identical to
  what full forward decode would have produced.

#### 3. Essence-count fallback for frame count
- Per `source_engine.cpp:84-88`, when Header Metadata's Duration is
  absent/unreliable *and* no Index Table exists either, the only remaining
  fallback is counting essence elements by walking the file once --
  expensive, but still better than refusing to open the file at all
  (current behavior: `OpenStatus::kParseError`).
- Lower priority than item 2, since Index Table reading also solves this
  for any file that has one; this item only matters for the (rarer) files
  with neither a reliable Duration nor an Index Table.

#### 4. Resolve the open "declared frame count vs. actual essence" bug
- The very first real vendor file tested in this project
  (`E:\20201112_001299.mxf`) hit `MXF Essence ended before reaching
  declared framecount (4500)` -- Header Metadata's declared Duration
  didn't match the essence actually present. The user said they'd inspect
  it themselves; this was never resolved in code.
- Before implementing a fix, revisit what the user found. Options to weigh
  once the actual cause is known: fail hard (current behavior -- safest,
  most honest about a real mismatch), clamp `numFrames` down to whatever
  was actually decodable (more forgiving, matches how most players/ffmpeg
  behave with a slightly-off duration), or make the policy configurable.
- Separately, do a general malformed-input hardening pass regardless of
  this specific bug's root cause: truncated Partition Pack, corrupt Primer
  Pack local tags, KLV BER lengths that lie about remaining file size --
  verify these all produce a graceful `kParseError`/`kDecodeError` rather
  than an out-of-bounds read or crash, particularly in
  `header_parser.cpp` and `essence_walker.cpp`'s KLV walking.

#### 5. Cross-platform parity re-verification
- Several fixes made this session (avcodec threading, the cropping fix,
  the `shared_ptr` ring-buffer refactor, the margin-based eviction fix,
  the random-seek fallback + intra-only fast path, `eagerdecode`/`logfile`)
  were verified thoroughly on Windows (real AviSynth+/ffmpeg, real
  VapourSynth/vspipe) but Linux hasn't been re-verified against a real host
  since Phase 6 -- and the static-CRT-linking crash fix
  (`CMAKE_MSVC_RUNTIME_LIBRARY`) is MSVC-specific by nature, so it says
  nothing about Linux's equivalent hazard class.
- Re-run the full real-host verification pass on Linux (WSL native
  filesystem checkout, per README's build note) for at minimum: the
  margin-based eviction fix, the seek fallback, and `eagerdecode`/`logfile`.
- Apply the CRT-fix plan's own noted optional Linux hardening
  (`-static-libstdc++ -static-libgcc`) if a similar "host process bundles
  an incompatible runtime" hazard seems plausible there too -- low
  priority, only if a concrete need surfaces (unlike Windows, no crash has
  actually been observed on Linux).

#### 6. Multi-track handling
- Current design assumes exactly one video track and one audio track
  (`streamInfo_.videoTrackNumber`/`audioTrackNumber` are singular values,
  not a list) -- real MXF files can have multiple audio tracks (stereo
  pairs, multiple language tracks) or, less commonly, multiple video
  tracks (multicam OP1a).
- At minimum, detect and clearly error on multi-video-track files rather
  than silently picking one arbitrarily. Multi-audio-track support ties
  into item 1's audio design anyway (needs a track-selection parameter,
  e.g. `audio_track=N`, once real audio decode exists at all).

#### 7. Documentation and sample scripts
- README's "Current status" list stops at Phase 6 -- add entries once
  Phase 7/8 items land, and add a parameter reference table for
  `MxfSource()` (`buffer_frames`, `eagerdecode`, `logfile` are currently
  only documented in code comments, not README).
- `samples/test.avs`/`test.vpy` only demonstrate `buffer_frames` -- update
  to also show `eagerdecode`/`logfile` usage (mirroring what was actually
  verified end-to-end this session).
- Document known limitations explicitly in README rather than leaving them
  only in code comments: GOP-based codecs' seek fallback is O(n) until
  item 2 lands; audio is silent until item 1 lands.

### Suggested order

1. Resolve the open real-file frame-count bug (item 4's first bullet) --
   lowest effort relative to impact, and it's a concrete bug the user
   already reported and hasn't heard back on.
2. Index Table Segment / RIP reading (item 2) -- highest-value single
   item; unblocks both real random-seek performance and the frame-count
   fallback (item 3 becomes optional once this lands).
3. Real audio output (item 1) -- large, mostly independent piece of work;
   a source plugin with permanently silent audio is a real gap for a
   general-purpose ffmpeg-facing reader.
4. Cross-platform re-verification (item 5) -- do after the above land, so
   it verifies the final Phase 7/8 feature set rather than running twice.
5. Multi-track handling (item 6) and the general malformed-input hardening
   pass (item 4's second bullet) -- incremental hardening, lower urgency.
6. Documentation/sample updates (item 7) -- last, once the Phase 7/8 API
   surface is actually final.

### Verification

Same established pattern: synthetic unit tests plus mandatory real-host
end-to-end runs (`ffmpeg -f avisynth`, `vspipe | ffmpeg`) for anything
touching the ring buffer, the seek path, or audio output -- this project
has repeatedly found real bugs (the cropping bug, both ring-buffer eviction
bugs, the CRT crash) that synthetic unit tests alone missed. Don't skip the
real-host step just because it's slower.
