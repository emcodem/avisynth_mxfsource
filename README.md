# avs_mxf_plugin

Forward-only MXF (SMPTE 377M) source plugin for AviSynth+ and VapourSynth.

Unlike ffms2 / L-SMASH Works, this plugin never builds a full random-access
seek index. It's designed for the ffmpeg-processing use case: frames are
always decoded forward, frame 0, 1, 2, ... to EOF. A background thread walks
the MXF file and decodes ahead into a configurable ring buffer (default 300
frames) so the host's own prefetch/read-ahead requests are served from cache.
See `docs/plan.md`-equivalent context in the design plan for the full
rationale and architecture (partition/KLV parser, avcodec decode layer,
shared core engine, thin AviSynth+/VapourSynth adapters).

Current status:
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
- Phases 4+ (see task list) add the ring-buffer engine, then wire `src/mxf`
  + `src/codec` into both adapters in place of the Phase 0 solid-color stub.

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

## Verify

```
# AviSynth+, matching the intended ffmpeg pipeline:
ffmpeg -f avisynth -i samples/test.avs -f null -

# VapourSynth, matching the intended ffmpeg pipeline:
vspipe -c y4m samples/test.vpy - | ffmpeg -i - -f null -
```

Standalone parser unit tests (no AVS/VS host needed):

```
ctest --test-dir build
```
