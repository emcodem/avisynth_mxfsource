"""Compare a new scan (run_scan.py or run_scan_vs.py results.jsonl) against a baseline results folder.

Usage: python3 compare_scans.py <baseline_dir> <new results.jsonl[,override.jsonl...]> [report.md]
  baseline_dir: e.g. results_2026-10-16 (scan/ + rescan*/ merged, newer rescans override, as make_csvs.py)

Report sections:
  1. SUPPORTED before, not now: expected strict refusals ("strict:" message) vs. everything else (regressions)
  2. not SUPPORTED before, SUPPORTED now, by old status
  3. SUPPORTED in both: video format / audio differences (UYVY: AviSynth YUY2 = VapourSynth yuv422p)
  4. timeouts in the new scan, to re-time serially
"""
import collections
import glob
import json
import os
import re
import sys


def key(path):
    return path.replace("\\", "/").lower()


def load_baseline(d):
    rows = {}
    for p in [os.path.join(d, "scan", "results.jsonl")] + sorted(glob.glob(os.path.join(d, "rescan*", "results.jsonl"))):
        for line in open(p, encoding="utf-8"):
            r = json.loads(line)
            rows[key(r["path"])] = r
    return rows


def audio_desc(s):
    m = re.match(r"(\d+)ch (\d+)Hz", s or "")
    return f"{m.group(1)}ch {m.group(2)}Hz" if m else ""


def video_desc(s):
    return (s or "").replace("yuyv422", "yuv422p").replace("uyvy422", "yuv422p")


def short_error(r):
    e = (r.get("error") or "").strip().splitlines()
    e = e[-1] if e else r.get("status", "")
    m = re.search(r"(MxfSource: )?(strict: .*)", e)
    if m:
        e = m.group(2)
    return re.sub(r"\d{3,}", "N", e)[:140]


def main():
    base = load_baseline(sys.argv[1])
    new = {}
    for p in sys.argv[2].split(","):  # later files (e.g. serial re-times) override earlier ones
        for r in map(json.loads, open(p, encoding="utf-8")):
            new[key(r["path"])] = r
    out = []
    p = out.append

    both = [k for k in new if k in base]
    p(f"# Scan comparison\n\nbaseline: {len(base)} files, new: {len(new)} files, in both: {len(both)}\n")
    p("| status | baseline | new |\n|---|---|---|")
    bs = collections.Counter(base[k]["status"] for k in both)
    ns = collections.Counter(new[k]["status"] for k in both)
    for s in sorted(set(bs) | set(ns)):
        p(f"| {s} | {bs[s]} | {ns[s]} |")

    lost = [k for k in both if base[k]["status"] == "ok" and new[k]["status"] != "ok"]
    strict = [k for k in lost if "strict:" in (new[k].get("error") or "")]
    other = [k for k in lost if k not in strict]
    p(f"\n## 1. SUPPORTED before, not now: {len(lost)}\n\n### Expected strict refusals: {len(strict)}\n")
    for msg, n in collections.Counter(short_error(new[k]) for k in strict).most_common():
        p(f"- {n} x `{msg}`")
    p(f"\n### Everything else (regressions to investigate): {len(other)}\n")
    for k in sorted(other):
        r = new[k]
        p(f"- `{r['status']}` {short_error(r)} -- {r['path']}")

    gained = [k for k in both if base[k]["status"] != "ok" and new[k]["status"] == "ok"]
    p(f"\n## 2. not SUPPORTED before, SUPPORTED now: {len(gained)}\n")
    for s, n in collections.Counter(base[k]["status"] for k in gained).most_common():
        p(f"- {n} x was `{s}`")
    still = collections.Counter((base[k]["status"], new[k]["status"], short_error(new[k]))
                                for k in both if base[k]["status"] != "ok" and new[k]["status"] != "ok")
    p(f"\nStill not SUPPORTED: {sum(still.values())}\n")
    for (b, n_, e), n in still.most_common():
        p(f"- {n} x `{b}` -> `{n_}`: {e}")

    ok_both = [k for k in both if base[k]["status"] == "ok" and new[k]["status"] == "ok"]
    vdiff = [k for k in ok_both if video_desc(base[k].get("plugin_video")) != video_desc(new[k].get("plugin_video"))]
    a_gain = [k for k in ok_both if not audio_desc(base[k].get("plugin_audio")) and audio_desc(new[k].get("plugin_audio"))]
    a_lost = [k for k in ok_both if audio_desc(base[k].get("plugin_audio")) and not audio_desc(new[k].get("plugin_audio"))]
    a_diff = [k for k in ok_both if audio_desc(base[k].get("plugin_audio")) and audio_desc(new[k].get("plugin_audio"))
              and audio_desc(base[k].get("plugin_audio")) != audio_desc(new[k].get("plugin_audio"))]
    p(f"\n## 3. SUPPORTED in both: {len(ok_both)}\n")
    p(f"- video format differs: {len(vdiff)}")
    for k in vdiff[:50]:
        p(f"  - `{base[k].get('plugin_video')}` -> `{new[k].get('plugin_video')}` -- {new[k]['path']}")
    p(f"- audio gained: {len(a_gain)}")
    for a, n in collections.Counter(audio_desc(new[k].get("plugin_audio")) for k in a_gain).most_common():
        p(f"  - {n} x {a}")
    p(f"- audio lost: {len(a_lost)}")
    for k in a_lost:
        why = [l for l in (new[k].get("plugin_log") or "").splitlines() if "audio" in l][:1]
        p(f"  - {new[k]['path']} -- {why[0].split(': ', 1)[-1] if why else ''}")
    p(f"- audio layout differs: {len(a_diff)}")
    for k in a_diff[:50]:
        p(f"  - `{base[k].get('plugin_audio')}` -> `{new[k].get('plugin_audio')}` -- {new[k]['path']}")

    timeouts = [k for k in new if new[k]["status"].endswith("timeout")]
    p(f"\n## 4. Timeouts in the new scan (re-time serially): {len(timeouts)}\n")
    for k in sorted(timeouts):
        p(f"- `{new[k]['status']}` {new[k].get('error') or ''} -- {new[k]['path']}")

    text = "\n".join(out) + "\n"
    if len(sys.argv) > 3:
        open(sys.argv[3], "w", encoding="utf-8").write(text)
    print(text)


if __name__ == "__main__":
    main()
