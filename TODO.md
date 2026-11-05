# TODO

Plans for the remaining work after Phases 0-6 (MVP: OP1a, frame-wrapped
essence, one video track, ring-buffer forward decode, both host adapters --
see README's "Current status" for what's already done and verified).

## Phase 7: Expand codec/OP coverage -- mostly done

### Status update (real-file verification session)

Items 1-3 below are no longer purely theoretical concerns -- they've each had
real-file verification this session (see README's Phase 9), which changed
the risk picture without necessarily completing the originally-scoped
engineering task:

- **XAVC-Intra**: already matched the existing AVC-Intra UL entry and
  decodes correctly end-to-end (1.9GB/3116-frame real file) with zero
  bitstream normalization -- Annex-B passthrough just works.
- **XAVC-L (long-GOP)**: was previously unclassified (`kUnknown` ->
  "unsupported codec"); added as a new `CodecFamily::kH264LongGop`, verified
  against a real file, all frames decode correctly.
- **ProRes**: verified against a real 6434-frame Adobe Premiere export --
  every frame decodes correctly. Found (but did not fix) a real gap: we
  never set `codec_tag`/extradata, so avcodec logs `Unknown prores profile
  0` on every decoder init. Decode output looks correct regardless (ProRes
  self-describes enough per-frame) -- **fixed 2026-10-19**: the codec tag
  now comes from the PictureEssenceCoding UL (`ProresCodecTag`), which also
  made 4444 profiles decode at 12 instead of 10 bit -- but item 3's "verify" task remains open
  for anything beyond this one real sample, and there's still no committed
  regression test for ProRes decode (see item 3 below).
- **D-10/IMX**: verified against a real 234MB Bremer-Test D-10 IMX50 file --
  decodes all 751 frames correctly. Hit the same "declared header size
  undercounts real metadata" class of bug fixed for XAVC, but via
  `EssenceWalker`'s zero-padding resync (added for a different reason)
  rather than a proper fix -- see item 1's caveat below, still open.
- **DNxHD**: 7 real "DNxHD"-named sample files actually all carry a genuine
  MPEG-2 422P@HL coding UL (not the DNxHD-specific UL family in our table)
  and decode correctly as MPEG-2. The DNxHD-specific UL entries in
  `ul_tables.h` remain unverified against any real sample.
- **JPEG2000**: no real samples exist on the available sample share (a
  broadcast-QC archive; J2K-in-MXF is mostly a digital-cinema/IMF thing).
  Still purely synthetic-verified, unchanged.
- **Two general parsing bugs found and properly fixed** (not codec-specific,
  affects any file with these layouts): `header_parser.cpp`'s RIP-based
  `headerMetadataEnd` fixup jumping to the Footer Partition on files with no
  separate Body Partition, and `EssenceWalker` desyncing on non-compliant
  raw zero-byte KAG padding. Both are regression-tested
  (`essence_walker_test.cpp`).

Items 4 (OP-Atom) and 5 (clip-wrapped essence) below are untouched --  no
real sample of either was available this session.

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
- **Update:** a real D-10 file (Bremer-Test IMX50) decodes correctly today,
  but only because `EssenceWalker`'s zero-byte-padding resync (added for the
  XAVC fix, see status update above) happened to recover from a
  metadata-undercount gap in this specific file. That resync mechanism can
  in principle false-positive-lock onto a UL-designator-looking byte
  sequence embedded in unrelated real metadata (not just literal zero
  padding) and land on a non-KLV boundary -- it worked here, but isn't a
  guaranteed fix for this class of bug the way the RIP-based
  `headerMetadataEnd` fix is for the no-Body-Partition case. Worth hardening
  properly (e.g. extending `ReadMetadataSets` to keep reading past a
  too-small `HeaderByteCount` as long as it keeps finding legitimately-shaped
  local-set KLVs) rather than relying on this side effect.
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
- **Update:** verified against two real Atomos Shogun files (XAVC-Intra and
  XAVC-L, both H.264, see README's Phase 9) that raw passthrough decodes
  every frame correctly with no NAL reframing at all -- both use Annex-B
  start codes already, which is what avcodec's H.264 decoder expects by
  default. Still unverified against AVCC-framed (length-prefixed) AVC-Intra
  from other vendors, so this item isn't fully closed, but it's lower risk
  than previously assumed.
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
- **Update:** ProRes verified against a real file, decodes correctly, but
  surfaced a real gap worth fixing: `AvcodecDecoder` never sets
  `AVCodecContext::codec_tag`, so avcodec can't identify which ProRes
  sub-profile (Proxy/LT/422/HQ/4444/4444XQ) it's decoding and logs `Unknown
  prores profile 0`. Fixing this needs an authoritative UL-suffix-byte ->
  FourCC mapping (SMPTE RDD 44's picture coding register) verified against
  more than one real sample -- don't guess it, a wrong FourCC could silently
  produce worse output (e.g. mislabeling 4:4:4 content) than today's
  harmless warning. JPEG2000 still has zero real samples available.
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

#### 2. Index Table Segment / RIP reading for real random-access seeks -- DONE
- New `src/mxf/index_table.h/.cpp`: parses Index Table Segments (SMPTE
  377-1 SS10, key/tags verified against FFmpeg's mxfdec.c/mxfenc.c) --
  `EnumeratePartitionsForIndex`/`ScanPartitionForIndexSegments` locate them
  via the existing RIP + each partition's `indexByteCount`/`indexSid`
  (mirroring `ReadMetadataSets`'s scan pattern); `BuildEssenceIndex`
  flattens one-or-more segments per BodySID (both CBR via the
  `EditUnitByteCount` formula and VBR via a new `IndexEntryArray` batch
  reader -- `GetUlArray` wasn't reusable, it's hardcoded to 16-byte-UL
  batches) into a single sorted, binary-searchable `EssenceIndex`, with
  full multi-segment stitching (stable-sorted, deterministic
  first-file-order-wins on any overlap).
- `SourceEngine` builds this once in `Open()` (`videoIndex_`, immutable
  after) and `SeekAndDecode`'s GOP-based branch now consults
  `FindPrecedingKeyframe(essenceTarget)` first: if it resolves, a fresh
  `EssenceWalker`/decoder starts at that keyframe's byte offset instead of
  essence element 0, decoding only from there forward -- a new
  `ServeStats::indexAssistedSeeks` counter (`SourceEngine::IndexAssistedSeekCount()`)
  confirms when this actually engaged. Absent/inapplicable index (no
  segment found, target outside every covered range, etc.) degrades
  silently to the original from-0 behavior in every case -- purely
  additive, never a regression risk.
- Also unblocks the frame-count fallback: `Open()` now derives `numFrames`
  from the index's own covered range when Header Metadata's Duration is
  absent, before giving up with `kParseError`.
- **Real-file verification found and fixed a second instance of this
  session's zero-byte-KAG-padding bug** (see README Phase 9): the same
  Atomos Shogun XAVC-L file that has no separate Body Partition also pads
  its Index Table region with raw zero bytes, which the new scanner
  initially mis-parsed the same way `EssenceWalker` originally did. Fixed
  by promoting `EssenceWalker`'s `LooksLikeKlvKey`/`ResyncToUlDesignator`
  helpers to shared utilities in `mxf/klv.h/.cpp` and reusing them in
  `ScanPartitionForIndexSegments` too. Confirmed end-to-end against the
  real XAVC-L file: `essenceIndex: built=1 highestCoveredEditUnit=3114`
  (covers the full 3115-frame file), a deliberate eviction-forcing seek
  probe resolved via the index (`indexAssistedSeeks=1`) and decoded
  correctly.
- Tests: `tests/mxf_parser_tests/index_table_test.cpp` (13 cases covering
  VBR/CBR parsing, `IndexEntryArray` trailing-byte skipping, keyframe
  lookup edge cases, multi-segment stitching, malformed/out-of-bounds
  entry handling) plus `source_engine_test.cpp` additions using a real
  encoded MPEG-2 I/P-frame sequence (avcodec's own encoder, `gop_size>1`)
  to prove both the unchanged no-index fallback and the index-assisted
  path produce byte-identical output to sequential playback. Note: the
  original plan to additionally corrupt an earlier GOP to "prove" the
  index bypasses it turned out to be untestable through the public API --
  `GetVideoFrame` only ever invokes `SeekAndDecode` for a frame the primary
  background thread already decoded successfully and then evicted, and
  that thread always decodes sequentially from element 0 first, so any
  earlier corruption poisons the whole engine before eviction can even
  occur. The `IndexAssistedSeekCount()` counter is the direct proof of use
  instead.

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
- **Update:** two real *causes* that produce this exact error message have
  since been found and fixed (real Atomos Shogun XAVC-Intra/XAVC-L files,
  see README's Phase 9): (a) `header_parser.cpp`'s RIP-based
  `headerMetadataEnd` fixup jumping straight to the Footer Partition on
  files with no separate Body Partition, and (b) `EssenceWalker` desyncing
  on non-compliant raw zero-byte KAG padding. Both are now handled and
  regression-tested. Whether either explains the original
  `E:\20201112_001299.mxf` report is unconfirmed (that file isn't available
  in this environment) -- if the error resurfaces on a real file, capture
  its RIP entries/partition layout (see the fix's commit for the diagnostic
  approach: dump the Random Index Pack, the partition kind at each entry,
  and a hex scan from `headerMetadataEnd` for any non-KLV-shaped bytes)
  before assuming it's a new root cause.
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
  item 2 lands (**update: item 2 is done**, see above -- O(n) only remains
  for files with no usable Index Table Segment); audio is silent until
  item 1 lands.

### Suggested order

1. ~~Resolve the open real-file frame-count bug (item 4's first bullet)~~ --
   substantially addressed this session, see item 4's update.
2. ~~Index Table Segment / RIP reading (item 2)~~ -- **done**; unblocked
   both real random-seek performance and the frame-count fallback (item 3
   is now optional, only matters for files with neither a reliable
   Duration nor an Index Table).
3. Real audio output (item 1) -- large, mostly independent piece of work;
   a source plugin with permanently silent audio is a real gap for a
   general-purpose ffmpeg-facing reader. Next up.
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

## Open items from the 2026-10-16 corpus scan

### Jump to the end of a long-GOP file re-decodes every frame separately

Observed 2026-10-19 on `SERVER/SHARE/...`
(39.2 GB, UHD H.264 long-GOP, 30020 frames), AviSynth+ via ffmpeg:

```
c = mxfsource("...FussballBL_HD_auf_UHD_AJA_FSHDR.mxf")
c.Trim(c.FrameCount - 25, 0)
```

The 25 frames are correct (bit-identical to ffmpeg), but the serve-stats line
reports `served 0 frame(s) from buffer, 26 frame(s) via fallback re-decode`:
every frame, including the strictly sequential ones after the first, goes
through `SeekAndDecode` instead of the ring buffer. Frame 29995 is requested
twice. Each fallback decode starts again from the preceding keyframe, so for
long-GOP UHD this is far slower than decoding the range once.

Expected: after the first out-of-window request, the producer thread should
continue from there, so the following frames come from the buffer (compare
`TestForwardSeekBeyondWindowUsesFastPathInsteadOfSequentialWait`, which
covers a forward jump but apparently not this access pattern).
`start_frame` avoids it when the caller knows the range up front.

Not investigated yet: whether the cause is the jump distance, the
end-of-stream position, or the access pattern of AviSynth's `Trim`.

### Corpus re-run against the 2026-10-16 baseline -- done 2026-10-19

Ran as part of the 2026-10-19 scan: no regressions (all 64 baseline-OK files
that now fail do so with a `strict:` message), see the triage report below.
Original plan: compare against
`tools/corpus_scan/results_2026-10-16/`: every file that was SUPPORTED must
stay SUPPORTED, except the expected strict-mode failures below; re-time any
decode timeouts serially (10 parallel UHD decodes caused false timeouts).
Expected changes:

- SUPPORTED -> FAILED by strict (default on), all damaged or without any
  index: of the 57 baseline-OK files without a RIP, 50 now fail (19 footer
  beyond EOF, 10 zero tail, 21 no reachable index), plus files with a zero
  tail or a short index that do have a RIP. Each must fail with a `strict:`
  message and open with `strict=false`.
- Open timeouts -> SUPPORTED: the 67 Fixup 1 files (essence in the Header
  Partition).
- Open timeouts -> FAILED (strict) in < 1 s: zero-tail / missing-footer
  recordings.
- Audio dropped -> exposed: about 250 files (CBR index past the essence,
  index longer than Duration, D-10 8-Ch AES3 Element).
- Pixel format not mapped -> SUPPORTED: 15 DNxHR / ProRes 4444 files.

The scan logs keep only the last 600 characters of the plugin log; keep the
full log (or at least the first lines) in the re-run, they were needed to
classify the audio failures.

### Open-header file whose declared index isn't found -- resolved, not a bug

`SERVER/SHARE/...`
Checked 2026-10-20: the declared index region (bytes 112284-112796) is all
zeros, there is no segment key in the first 1 MB, and the file has no
footer or RIP (exactly 272 MiB). It is an aborted recording whose index was
never written, so the strict refusal is correct.

## Open items from the 2026-10-19 scan

Scan: 12,040 files, build `650208c`. Full triage, with all categories and
example paths: `D:\mxfscan_2026-10-19\triage\triage_report.md`. Ranked by
priority (user decision 2026-10-20).

Order of the remaining work (user decision 2026-10-21): 5 (full
clip-wrapped support), 9, then 8. 6 and 7 only need to end with the
correct error message. 2 is not going to be supported.

### 1. Header Metadata truncated at the declared HeaderByteCount end (plugin bug) -- implemented, rescan pending

`ParseMxfHeader` reads metadata only up to `partitionEnd + HeaderByteCount`.
Fixup 1 widens that only when RIP[1] is a Body Partition, and
`ReadMetadataSets` silently stops at the first set that crosses the end.
On the measured files the writers aren't wrong: they count HeaderByteCount
from the first KLV after the Partition Pack's KAG Fill (normally the Primer
Pack), not from the end of the pack. The plugin's end is therefore too
early by the size of that Fill (D-10 IBM/MXFTK/K2 2011, ffmpeg's own mxf
muxer, SARNOFF ffmpeg experiments), so the last sets are dropped:

- 265 files with video fail with "no video track found" (the picture
  descriptor is cut off), e.g.
  `SERVER/SHARE/...`.
- 52 strict-OK files silently have no audio (the sound track's
  Sequence/SourceClip or the AES3 descriptor is cut off), e.g.
  `SERVER/SHARE/...`
  (declared end 5244, metadata really ends at 5403) and
  `SERVER/SHARE/...` (5788 vs 5945).
- 1 file with `HeaderByteCount = 0xFFFFFFFFFFFFFFFF` (the end wraps to 155):
  `SERVER/SHARE/...`.

Fix (2026-10-20, uncommitted):
- HeaderByteCount now counts from `PartitionContentStart` (partition.h).
- `ReadMetadataSets` also reads on past the declared end through further
  metadata. It stops at the first non-metadata KLV, at the next known
  partition, and at hard bounds (1 MiB past the declared end, 16 MiB in
  total), so it never walks the file.
- A HeaderByteCount that doesn't fit in the file counts as unknown.
- Verified bit-exact against ffmpeg (video framemd5 + audio md5) on the
  D-10, ffmpeg_play, IBM and R103 examples.
- Still to do: full rescan and the Linux tests.

### 2. OP1b with several BodySIDs merges every index into the video index -- won't fix (OP1b not supported)

Deferred by user decision 2026-10-20. P2 AVC-LongG OP1b (video BodySID 1,
audio BodySIDs 2-6): the video index gets 475,552 entries for 255 frames,
which leads to audio fetch failures (3 decode_failed, 6 decode_timeout) and
3-7 s opens. Strict passes these files because the inflated index looks
complete. Example:
`SERVER/SHARE/...`.

### 3. Partition index scan starts at the wrong offset -- implemented, rescan pending

`ReadOnePartitionForIndex` starts at `partitionEnd + HeaderByteCount`.
Writers that put a KLV Fill right after the Partition Pack count
HeaderByteCount from after it (same cause as item 1), so the scan starts
inside the Fill.
`ScanPartitionForIndexSegments` then `break`s on the BER failure
(`index_table.cpp:237`) instead of resyncing. As a result, 8 intact K2
Summit `*_stream.mxf` files lose the Footer's last index segment and are
wrongly refused by strict check 4 (with strict=false their audio is
dropped), e.g.
`SERVER/SHARE/...`
(segment really at 917669888, scan starts at 917669548).

Fix (2026-10-20, uncommitted):
- `IndexRangeStart` (index_table.cpp) counts from `PartitionContentStart`.
  The old base is kept where the segment is only found there.
- A HeaderByteCount that doesn't fit in the file is replaced by the
  bounded `ScanHeaderMetadataEnd`.
- A failed KLV header read now resyncs (bounded) instead of ending the scan.
- A CBR segment whose EditUnitByteCount doesn't fit in the file even once
  (a placeholder, 0xFFFFFFFF) is no index. Without this, the WildeKaribik
  file, an unfinished recording, opened in strict mode as 1 frame.
- Verified:
  - both K2 example files open in strict mode with 3000 and 750 frames;
    video (first 50 and last 10 frames) and 8 ch audio are bit-identical
    with ffmpeg;
  - GAUDI (336 partitions) is unchanged and identical.
- Still to do: full rescan and the Linux tests.

### 4. Essence walk past damage -- implemented, rescan pending

Decisions (2026-10-21):
- Strict mode never resyncs. If no valid KLV starts right after a value,
  the file is damaged there: the walk ends, later frames fail with an
  error, and the log names the offset.
- strict=false resyncs, but only within 10 MiB (`kMaxResyncBytes`).
- A damaged length field ends the walk in both modes; no new recovery.
- The open-time resync in `ResyncToActualEssenceStart` is bounded to
  10 MiB too.
- Every damaged spot and every open-time resync is logged.
- Not fixed, by decision: when the walk ends early, the frame flushed from
  the decoder is served under the wrong number. On the Cambria file below,
  ffmpeg's frame 17 comes out as frame 15 without an error, and frame 16
  then fails. Strict mode does report an error (at frame 16), which is
  enough.

Findings:
- The resync existed only for the Atomos "raw zero padding", which was a
  misdiagnosis: the walk started at a wrong offset inside a KLV Fill's
  zero value (same counting base as items 1/3). A KLV walk from byte 0
  finds no non-KLV byte in BeepFlash XAVC-L/-Intra. With the item 1/3 fix
  and strict (no resync), the XAVC-L file decodes all 3115 frames
  bit-identical to ffmpeg; XAVC-Intra matches on its first 50 and last 10
  frames.
- The 2 `TimecodeFiles\TC_*_Origin4.mxf` failures were the same base
  error. Their KLV structure is intact from byte 0, and with items 1/3,
  646 frames match ffmpeg's frames 4-649 (Origin 4).
- Cambria (example below) is really damaged: 3 non-KLV runs in the essence
  (5780 bytes at 5933056 where a sound element's key and length should be;
  364 bytes at 13379732 and at 24531604). The log now says `no valid KLV at
  byte 5933056 (the file is damaged there)`, in both modes.

Remaining: the full rescan must show which intact files (if any) still log
a damaged spot or an open-time resync. Any such file must be explained.

Example:
`SERVER/SHARE/...`.

### 5. Clip-wrapped essence (Phase 7 item 5) -- now with real files

312 files, all OP-Atom: 227 P2 DV
(`SERVER/SHARE/...`),
84 P2 AVC-Intra (`P2 12 PK Le Livre\CONTENTS\VIDEO`), and `avid-xavc.mxf`.

- Strict refuses them with a misleading "no Index Table Segment". The
  index segments are in the Header Partition, but the file has no RIP, so
  the Body Partition isn't enumerated and `LiesInPartitionEssence` drops
  every entry.
- strict=false exposes 1 frame instead of ~46 (263 files) or times out
  (48 files).
- No file that strict opens is clip-wrapped.

Survey of all 312 (2026-10-21, `D:\mxfscan_2026-10-19\triage\item5\`):

| Files | Codec | Partitions / RIP | Index | Clip value vs index |
|---|---|---|---|---|
| 227 | DV (P2) | Header, Body, Footer / no RIP | 1 CBR segment, EUBC 576000, IndexDuration 0 | exact multiple of EUBC |
| 84 | AVC-Intra (P2) | Header, Body, Footer / no RIP | 2 CBR segments: EU 0 568832, EU 1.. 568320 | running sum == clip length |
| 1 | AVC-Intra (`avid-xavc.mxf`) | Header, Body, Footer / RIP | 1 CBR segment 568320 | last EU 5120 bytes longer |

- The partitions are found without a RIP through
  `Header.FooterPartition -> Footer.PreviousPartition -> Body`.
- Offsets count from the start of the clip KLV's value. In P2 AVC-Intra
  the access unit delimiters sit exactly at the running sums.
- P2 AVC-Intra carries SPS/PPS only in EU 0; `avid-xavc.mxf` carries none
  at all (ffmpeg generates AVC-Intra SPS/PPS internally).
- No corpus file has clip-wrapped audio next to video. The 2220
  clip-wrapped audio files are audio-only OP-Atom.

Plan for full support (video; clip-wrapped audio out of scope):
1. Partition discovery without a RIP: follow `FooterPartition` and then
   `PreviousPartition` back to the header, accepting only valid partitions
   (ThisPartition == offset, strictly decreasing, bounded count). Used by
   the local and the HTTP index build alike (MxfExtractIndex parity). This
   changes strict mode's definition of a "reachable index" (README).
2. Detect clip wrapping from the picture essence container label (header
   pack / descriptor), and locate the video clip KLV (offset and length of
   its value) through the partition of the video BodySID.
3. Clip index: entry offsets are the clip value start plus the running sum
   of EditUnitByteCount over the segments (also correct for frame-wrapped
   multi-segment CBR, where `editUnit*EUBC` is wrong as well). A segment
   with IndexDuration 0 runs to the clip end. The last EU extends to the
   clip end. Every entry must lie inside the clip, and each entry's size is
   the next offset minus its own. VBR clip indexes use their StreamOffsets
   the same way.
4. A clip element source next to `EssenceWalker`, with the same `Next()`
   contract: it yields one video element per index entry by reading
   [offset, size). It is used by the decode thread, the seek fallback and
   the intra-only fast path when the video is clip-wrapped. Random access
   is direct.
5. AVC-Intra: read SPS/PPS from EU 0 at open and give them to every
   decoder (extradata), so decoding can start at any frame. With no SPS/PPS
   in the file (`avid-xavc.mxf`): an explicit error, not supported. Skipped
   for now (user decision 2026-10-21); supporting it would need ffmpeg's
   AVC-Intra SPS/PPS tables in the plugin.
6. Strict: the index must cover Duration (check 4, unchanged), and the
   index must account for the whole clip value: the last EU may only absorb
   the remainder (not more than one EU's worth). Otherwise "damaged".
7. MxfExtractIndex / `indexurl`: the serialized index carries the clip
   value end (index blob version 2). Old version-1 blobs stay readable for
   frame-wrapped files.
8. Tests: synthetic OP-Atom clip-wrapped files without a RIP (single CBR
   with IndexDuration 0, two CBR segments, VBR), random access, strict
   checks, MxfSource/MxfExtractIndex agreement. Real files: all 311 P2
   files bit-exact against ffmpeg (framemd5) on Windows and Linux, then the
   full rescan.

### 6. Duration 0 gives 0 frames and an empty error -- only the error message to fix

Aborted or open recordings write Duration 0. `header_parser` sets
`numFrames = 0`, and the index fallback runs only for `numFrames < 0`
(`source_engine.cpp:203`). `Peek(0)` then returns kEndOfStream with no
message, so the error reads "failed to decode first frame:".

- Affected: 12 files (strict refuses all of them for other reasons), e.g.
  `SERVER/SHARE/...`
  (ffmpeg decodes 751 frames).
- Fix: treat 0 as unknown, at least when the file is not finished, and give
  the end-of-stream case at open a message.

### 7. MPEG-2 coding UL gaps -- only the error message to fix

- 13 files use `06 0e 2b 34 04 01 01 03 04 01 02 02 01 03 03 00`
  (MP@HL long GOP), on 1080, 1440 and 720x608 4:2:0, e.g.
  `SERVER/SHARE/...`.
- 1 file uses `...01 05 03` (MP@H-14).
- ffmpeg matches the 13-byte prefix `04 01 02 02 01` as MPEG-2.
- 4 Pinnacle Liquid D-10 files have no PictureEssenceCoding at all; ffmpeg
  falls back to the container UL (`...0d 01 03 01 02 04 60 01`), e.g.
  `SERVER/SHARE/...`.

### 8. Cold open with many partitions over SMB

`SERVER/SHARE/...`
(23.8 GB, RIP with 336 partitions, 334 of them with an index) takes 13.5 s
to open serially on a cold cache and 0.3 s warm. `BuildEssenceIndex` reads
the partitions one after another. The 10 s rule is violated.

### 9. Linux-only open timeouts on very large files (unverified)

6 files of 42-86 GB open in < 1 s on Windows but take > 10 s under
VapourSynth/Linux, also when re-timed serially, e.g.
`SERVER/SHARE/...`.
There is no plugin log line, and ffprobe also timed out on that file in the
Linux run, which points at the CIFS mount. Re-time one file serially on
Linux with `logfile` before changing anything.

### Later (from the same triage, lower priority)

Clearer error messages (empty file, audio-only file, "no valid RIP" when an
invalid one exists, OP1b external essence); the clamp is skipped when it
would leave 0 frames (`PQ_short.mxf`); run-in before the Header Partition
Pack plus the MPEG-4 Visual UL (265 Sony `*S01.MXF` proxies); big-endian/AIFF
PCM (4 files); an optional strict check for a CBR EditUnitByteCount that
doesn't match the essence (K2 SD exports, `SGI_BSO_Cristal_Gala`); 2
`K2Sum92_D10_*_stream.mxf` files lose audio for an unknown reason.

### Best-effort recovery of damaged files (postponed)

`handoff_partition_recovery_2026-10-16.md` sections C/D (partition discovery
from the data end via `PreviousPartition`, tail walk of the last partition)
are postponed by decision of 2026-10-19: damaged files are only detected and
fail in strict mode. If picked up again, it goes into a separate module that
only runs with `strict=false`.

### Smaller gaps

- ProRes 4444 with alpha (`yuva444p10/12`) has no output mapping; no file in
  the corpus uses it.
- `EnumeratePartitionsForIndex` finds partitions only via offset 0 and the
  RIP, not via the header's `FooterPartition` pointer. A file without a RIP
  whose index is only in the Footer would get no index. No such file in the
  corpus (checked 2026-10-19: none of the 105 RIP-less files has a valid
  Footer carrying an index).

## Open items from the 2026-11-05 mock tests

Found with mock files made from a 30 min XDCAM HD422 RDD9 file (ffmpeg MPEG-2
422P@HL long GOP, rewrapped with bmxtranswrap `-t rdd9`): 3b = the System
Item key of edit unit 662 overwritten with 0xFF, essence otherwise intact.

### 1. A wrong frame is served before the error when the essence ends mid-GOP

On 3b, a sequential decode (strict) serves frames 0..661 and then fails at
662 -- but frame 661 carries the picture of frame 663. The background walk
ends at the damaged KLV, `DecodeNextFrame` drains the decoder, and the
held-back anchor picture (stored before the B pictures, displayed as 663)
comes out next and is served as 661. No error for that frame. Probably the
same wherever the essence ends before the declared frame count inside a GOP
(truncated file with strict=false). Fix direction: once the essence ended
early, don't trust drained outputs past the last decodable display position
(or check this case explicitly against the index's reorder information).

### 2. The seek error doesn't say why the re-decode failed

"requested frame is far ahead of the current decode position, and the
fallback re-decode failed to reach it" (and the "fell outside the ring
buffer window" variant) is all the user sees; `SeekAndDecode` only returns
nullptr. The causes are distinct and each needs a different answer: second
reader can't open the file; essence ends before the target (truncated);
the walk stops at damaged essence (log line "no valid KLV at byte X");
the index offset doesn't point at a KLV (wrong index, `verify=true` finds
it). Return the reason from `SeekAndDecode` and include it (with the byte
offset) in the error message.
