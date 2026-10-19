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
