"""Regenerate both CSVs from all scan data:
  scan/mxf_support_list.csv      one row per file (latest result per file)
  scan/mxf_remaining_issues.csv  one row per open issue, with count, diagnosis, fix, smallest example
Inputs: scan/results.jsonl, rescan*/results.jsonl (newer runs override), scan/timeouts_layout.jsonl,
scan/zero_filled_idx.txt, scan/body_index_check.jsonl, pcm_check.jsonl.
Usage: python -I make_csvs.py <scratchpad>"""
import collections
import csv
import glob
import json
import os
import re
import sys

sp = sys.argv[1]
J = lambda p: [json.loads(l) for l in open(p, encoding="utf-8") if l.strip()]

latest = {}
first = {}
for x in J(os.path.join(sp, "scan", "results.jsonl")):
    latest[x["path"]] = x
    first[x["path"]] = x
for rp in sorted(glob.glob(os.path.join(sp, "rescan*", "results.jsonl"))):
    for x in J(rp):
        x["idx"] = first[x["path"]]["idx"]
        x["rescanned"] = True
        latest[x["path"]] = x
layout = {y["idx"]: y for y in J(os.path.join(sp, "scan", "timeouts_layout.jsonl"))}
zero_filled = {int(s) for s in open(os.path.join(sp, "scan", "zero_filled_idx.txt")).read().split()}
body_idx = {y["path"]: y for y in J(os.path.join(sp, "scan", "body_index_check.jsonl"))}
pcm = {y["path"]: y for y in J(os.path.join(sp, "pcm_check.jsonl"))}

# issue key -> (title, diagnosis, suggested fix)
ISSUES = {
    "audio_index_clean": (
        "audio dropped: audio sample index fails on a clean file",
        "Plugin logs 'failed to build audio sample index'. Header Closed Complete, not truncated, and ffmpeg decodes "
        "the audio without errors (first 10 s + last 5 s checked), so this is a plugin bug in "
        "BuildAudioSampleIndexFromIndexTable, not file damage.",
        "Debug BuildAudioSampleIndexFromIndexTable with the example file; likely a few index layouts."),
    "audio_index_damaged": (
        "audio dropped: audio sample index fails, file truncated or damaged",
        "Same plugin message, but the file is truncated (FooterPartition offset beyond EOF) or ffmpeg also reports "
        "audio/index errors (e.g. 'Invalid PCM packet, data has size 1' when the last audio element is cut off).",
        "Expose audio up to the last complete element instead of dropping it entirely."),
    "audio_index_open_header": (
        "audio dropped: audio sample index fails, header Open Incomplete",
        "Same plugin message on files whose Header Partition is still Open Incomplete (growing/aborted recording); "
        "ffmpeg reads the audio fine.",
        "Likely the same root cause as the clean-file group; verify after that fix."),
    "audio_aiff": (
        "audio dropped: AIFF big-endian PCM",
        "SoundEssenceCompression = AIFFUncompressedCoding (...04 02 02 01 7E), big-endian samples.",
        "Byte-swap in the PCM interleave path, then map the UL to kPcm."),
    "audio_other": ("audio dropped: no reason logged", "Video-only with no audio-degradation log line.",
                    "Investigate."),
    "timeout_fixup1": (
        "open > 10 s: essence inside Header Partition (Fixup 1 over-read)",
        "Header partition holds the essence (BodySID != 0) and the only Body Partition (RIP[1]) is near EOF. "
        "header_parser.cpp Fixup 1 treats everything up to RIP[1] as header metadata and walks tens of GB.",
        "Only extend headerMetadataEnd up to the first essence element / Index Table Segment, or skip Fixup 1 when "
        "the Header Partition's BodySID != 0."),
    "timeout_no_rip_indexed_bodies": (
        "open > 10 s: no RIP/footer, but Body Partitions carry index segments",
        "Recording without RIP and with missing footer (often zero-filled tail); each Body Partition holds the "
        "index of the previous partition. The plugin only finds partitions via the RIP, so it falls back to a "
        "full bitstream scan at open.",
        "Partition discovery (proposal): verify partitions by ThisPartition == own offset, locate the last one "
        "from the data end, follow PreviousPartition links, read each partition's index, walk only the last "
        "partition's unindexed tail."),
    "timeout_no_index": (
        "open > 10 s: no index anywhere",
        "No RIP, no footer and no Body Partition with index segments in the first 400 MB; only a full essence scan "
        "can find frames.",
        "Lazy/background indexing, or frame count estimate from element sizes."),
    "timeout_no_index_zero": (
        "open > 10 s: no index anywhere, zero-filled regions",
        "As above, plus long zero-filled regions (preallocated, never fully written). ResyncToUlDesignator then "
        "advances one byte at a time with 2 syscalls per byte.",
        "Block-read + memchr resync; give up on long zero runs."),
    "open:decoded pixel format has no AviSynth+ mapping yet": (
        "open fails: pixel format not mapped",
        "Decodes, but FromAvPixFmt only maps 8/10-bit 4:2:0 and 4:2:2.",
        "Map yuv422p12 / yuv444p10 / yuv444p12 (+ alpha) in both adapters."),
    "open:unsupported or unavailable video codec": (
        "open fails: video codec label unknown",
        "PictureEssenceCoding UL not in ul_tables.h (XAVC-L variant from Woody IN2IT, JPEG XS).",
        "Dump the UL (dump_header_info), look it up in smpte-search, add to kCodecUls."),
    "open:file does not start with a Header Partition Pack": (
        "open fails: no Header Partition Pack at offset 0",
        "Not MXF at the start (ffprobe can't read it either), or a run-in.",
        "Check for a run-in (SMPTE 377 allows up to 64 KB before the header); otherwise not MXF."),
    "open:no video track found in Header Metadata": (
        "open fails: no video track resolved",
        "Header Metadata has no picture track the parser can resolve.",
        "Inspect the package/track structure of the example."),
    "open:failed to decode first frame: MXF essence ended before reaching the declared frame count": (
        "open fails: essence ends at frame 0",
        "Essence walk finds no usable frame although the header declares frames.",
        "Inspect the example (likely truncated, or another KAG/fill quirk)."),
}


def plugin_msg(x):
    m = re.search(r"MxfSource: ([^\n]+)", x.get("error", ""))
    msg = m.group(1).strip() if m else ""
    return re.sub(r" \(\d+\)$", "", msg)


# Plugin logs of runs whose result has no plugin_log (decode step timed out before it was read).
logs_by_path = {}
for lp in glob.glob(os.path.join(sp, "rescan*", "avs", "*.log")) + glob.glob(os.path.join(sp, "scan", "avs", "*.log")):
    try:
        text = open(lp, encoding="utf-8", errors="replace").read()
    except OSError:
        continue
    m = re.search(r"\[MxfSource\] (.+?\.mxf): ", text, re.IGNORECASE)
    if m:
        logs_by_path.setdefault(m.group(1), text)


def classify(x):
    """-> (verdict, issue_key or None, reason text)"""
    st, log = x["status"], x.get("plugin_log", "")
    if st == "decode_timeout":
        # 1 s of UHD took >10 s only under 10x parallel load (re-timed alone: 2-3 s), so judge it like "ok".
        x = dict(x, status="ok", plugin_log=logs_by_path.get(x["path"], ""))
        verdict, key, reason = classify(x)
        return verdict, key, reason + " (decode slow only under parallel load)"
    if st == "ok":
        if x.get("plugin_audio"):
            return "SUPPORTED", None, "video + audio"
        if not x.get("src_audio_tracks"):
            return "SUPPORTED", None, "video (source has no audio)"
        if "failed to build audio sample index" in log:
            p = pcm.get(x["path"], {})
            if p.get("footer_beyond_eof") or p.get("head_errors") or p.get("tail_errors"):
                k = "audio_index_damaged"
            elif p.get("header_status") not in (None, 4):
                k = "audio_index_open_header"
            else:
                k = "audio_index_clean"
            return "VIDEO ONLY", k, ISSUES[k][0]
        if "non-PCM" in log:
            return "VIDEO ONLY", "audio_aiff", ISSUES["audio_aiff"][0]
        return "VIDEO ONLY", "audio_other", ISSUES["audio_other"][0]
    if st == "open_timeout":
        if layout.get(x["idx"], {}).get("fixup1_overscan_gb", 0) > 0.1:
            k = "timeout_fixup1"
        elif body_idx.get(x["path"], {}).get("first_indexed_body") is not None:
            k = "timeout_no_rip_indexed_bodies"
        elif x["idx"] in zero_filled:
            k = "timeout_no_index_zero"
        else:
            k = "timeout_no_index"
        return "FAILS", k, ISSUES[k][0]
    k = "open:" + plugin_msg(x)
    return "FAILS", k, ISSUES.get(k, (k[5:] or st,))[0]


def fixed_note(x):
    old = first[x["path"]]
    if not x.get("rescanned"):
        return ""
    notes = []
    if "non-PCM" in old.get("plugin_log", ""):
        notes.append("audio label fix (SMPTE382M PCM UL)")
    if old["status"] == "open_failed" and (old.get("src_video") or "").startswith("dnxhd"):
        notes.append("UL version byte ignored (DNxHR)")
    if old["status"] == "open_failed" and len(x["path"]) >= 260:
        notes.append("long path support")
    return "; ".join(notes)


per_file = []
for x in latest.values():
    verdict, key, reason = classify(x)
    per_file.append((verdict, key, reason, x))

# --- per-file list
out1 = os.path.join(sp, "scan", "mxf_support_list.csv")
with open(out1, "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f, delimiter=";")
    w.writerow(["verdict", "reason", "path", "size_gb", "source_video", "source_audio_tracks", "source_audio_codecs",
                "plugin_audio", "changed by uncommitted fixes", "ffmpeg audio check"])
    for verdict, key, reason, x in sorted(per_file, key=lambda t: t[3]["path"].lower()):
        p = pcm.get(x["path"])
        ff = ""
        if p:
            errs = p["head_errors"] + p["tail_errors"]
            ff = ("errors: " + errs[0].split("] ")[-1][:100]) if errs else "clean"
            if p.get("footer_beyond_eof"):
                ff += " (truncated: footer offset beyond EOF)"
        w.writerow([verdict, reason, x["path"], round(x["size"] / 1e9, 3), x.get("src_video", ""),
                    x.get("src_audio_tracks", ""), ",".join(x.get("src_audio_codecs") or []),
                    x.get("plugin_audio", ""), fixed_note(x), ff])

# --- remaining issues
groups = collections.defaultdict(list)
for verdict, key, reason, x in per_file:
    if key:
        groups[key].append(x)
out2 = os.path.join(sp, "scan", "mxf_remaining_issues.csv")
with open(out2, "w", newline="", encoding="utf-8-sig") as f:
    w = csv.writer(f, delimiter=";")
    w.writerow(["issue", "files", "source video codecs", "diagnosis", "suggested fix", "example (smallest file)",
                "example size GB"])
    for key, xs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        xs.sort(key=lambda x: x["size"])
        title, diag, fix = ISSUES.get(key, (key, "", ""))
        codecs = ", ".join(f"{c} x{n}" for c, n in collections.Counter(
            (x.get("src_video") or "?").split(" ")[0] for x in xs).most_common())
        w.writerow([title, len(xs), codecs, diag, fix, xs[0]["path"], round(xs[0]["size"] / 1e9, 3)])

print(out1)
print(out2)
print(collections.Counter((v, r) for v, k, r, x in per_file).most_common())
