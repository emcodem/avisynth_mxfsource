# Corpus scan

These scripts decode the first second of every MXF on a share through the built AviSynth+ plugin and classify the
results. They were used for the 2026-10-16 baseline over `SERVER/SHARE/...` (1479 files). The next step is
the regression check for `handoff_partition_recovery_2026-10-16.md`.

Requirements: Python 3, `ffmpeg`/`ffprobe` with AviSynth+ input on PATH, and a Release build of
`build/src/avs/Release/mxfsource.dll` (the path is set in `run_scan.py`).

## Running a scan

```sh
D=tools/corpus_scan/my_run; mkdir -p "$D"
# list.tsv: "<size>\t<path>" per line
find "//SERVER/SHARE" -path "*/@Recycle" -prune -o -type f -iname "*.mxf" -printf "%s\t%p\n" > "$D/list.tsv"
python -I tools/corpus_scan/run_scan.py "$D"          # resumable: skips files already in results.jsonl
```

- `run_scan.py` runs 10 files in parallel. Opening the file through the plugin and decoding the first second each
  get **10 s** (a plugin open must take only seconds, even on huge files over SMB). The ffprobe of the source
  gets 60 s. Results go to `results.jsonl`, one JSON object per file.
- Re-time anything that looks slow **serially** with `serial_time.py`. 10 parallel UHD decodes produce false
  decode timeouts.
- Rescans of a subset go into their own folder (`rescan1/`, ...), each with its own `list.tsv`. `make_csvs.py`
  lets newer rescans override older results.

## Building the reports

```sh
python -I tools/corpus_scan/make_csvs.py tools/corpus_scan/results_2026-10-16
```

This writes `scan/mxf_support_list.csv` (one row per file) and `scan/mxf_remaining_issues.csv` (one row per
issue, with count, diagnosis, suggested fix and the smallest example file). Both are `;`-separated with a UTF-8
BOM, so they open in Excel directly.

`make_csvs.py` also reads the outputs of the classifiers below, so run those first for a new scan:

| Script | Writes | Purpose |
|---|---|---|
| `check_layout.py` | `scan/timeouts_layout.jsonl` (redirect stdout) | open timeouts: does the RIP point at a Body Partition far past the header (Fixup 1 over-read)? |
| `body_index_check.py` | `scan/body_index_check.jsonl` | open timeouts without a RIP: is there a Body Partition carrying index segments? |
| `pcm_check.py` | `pcm_check.jsonl` | "failed to build audio sample index" files: ffmpeg audio errors / truncation |
| `scan/zero_filled_idx.txt` | (hand-made) | ids of the zero-filled files, sampled at 50 % / 90 % of the file size |

## Single-file helpers

- `partitions.py <file>`: every Partition Pack (this/prev/footer/HBC/IBC/SIDs), gaps, audio element sizes.
- `index_segments.py <file> <partition offset>...`: the Index Table Segments after each given partition.
- `klvwalk.py <file> <offset>`: walk the KLVs the way `EssenceWalker` does and report where alignment is lost.
- `sound_uls.py <results.jsonl>`: tally of SoundEssenceCompression ULs, for files with dropped vs working audio.

## `results_2026-10-16/`

The baseline from the Release build of `e7d770c` (first scan, `scan/`), plus two rescans with the then-uncommitted
fixes: `rescan1/` (UL version byte + SMPTE382M PCM label) and `rescan2/` (long paths).
