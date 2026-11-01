"""For every 'failed to build audio sample index' file: does ffmpeg report PCM errors (first 10 s,
last 5 s), and is the file truncated (header FooterPartition offset beyond EOF)?
Usage: python -I pcm_check.py <scratchpad>   -> <scratchpad>/pcm_check.jsonl"""
import glob
import json
import os
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor

sp = sys.argv[1]
by_path = {}
for rp in [os.path.join(sp, "scan", "results.jsonl")] + sorted(glob.glob(os.path.join(sp, "rescan*", "results.jsonl"))):
    for l in open(rp, encoding="utf-8"):
        x = json.loads(l)
        by_path[x["path"]] = x
files = [x for x in by_path.values() if "failed to build audio sample index" in x.get("plugin_log", "")]
out_path = os.path.join(sp, "pcm_check.jsonl")
lock = threading.Lock()


def ff(args):
    try:
        p = subprocess.run(["ffmpeg", "-hide_banner", "-v", "error"] + args + ["-map", "0:a", "-f", "null", "-"],
                           capture_output=True, timeout=120, stdin=subprocess.DEVNULL)
        return [l for l in p.stderr.decode("utf-8", "replace").splitlines() if l.strip()]
    except subprocess.TimeoutExpired:
        return ["TIMEOUT"]


def check(x):
    r = {"path": x["path"], "size": x["size"]}
    try:
        with open(x["path"], "rb") as f:
            h = f.read(128)
            b = h[16]
            nb = b & 0x7F if b & 0x80 else 0
            v = h[17 + nb:]
            r["header_status"] = h[14]
            footer = int.from_bytes(v[24:32], "big")
            r["footer_offset"] = footer
            r["footer_beyond_eof"] = footer > x["size"]
    except OSError as e:
        r["read_error"] = str(e)
    head = ff(["-i", x["path"], "-t", "10"])
    tail = ff(["-sseof", "-5", "-i", x["path"]])
    r["head_errors"] = head[:3]
    r["tail_errors"] = tail[:3]
    r["invalid_pcm_packet"] = any("Invalid PCM packet" in l for l in head + tail)
    with lock:
        with open(out_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


if os.path.exists(out_path):
    os.remove(out_path)
print(len(files), "files", flush=True)
with ThreadPoolExecutor(10) as ex:
    list(ex.map(check, files))
print("done", flush=True)
