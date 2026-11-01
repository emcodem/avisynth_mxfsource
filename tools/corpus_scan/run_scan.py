"""Decode the first second of every MXF in list.tsv through mxfsource.dll, 10 in parallel.

Usage: python -I run_scan.py <scan_dir>
Writes <scan_dir>/results.jsonl (one line per file, appended as each finishes).

Optional environment: SCAN_STRICT=true|false passes strict= to MxfSource (unset: plugin default),
SCAN_WORKERS (default 10), SCAN_DLL (default below). open_s / decode_s record each step's wall time.
"""
import json
import os
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

DLL = os.environ.get("SCAN_DLL", r"D:\dev\avs_mxf_plugin\build\src\avs\Release\mxfsource.dll")
SCAN = sys.argv[1]
WORKERS = int(os.environ.get("SCAN_WORKERS", "10"))
STRICT = os.environ.get("SCAN_STRICT")  # "true" / "false" / None
PROBE_TIMEOUT = 60
DECODE_TIMEOUT = 10

lock = threading.Lock()


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def probe(path):
    rc, out, err = run(["ffprobe", "-v", "error", "-show_entries",
                        "stream=codec_type,codec_name,pix_fmt,width,height,channels,sample_rate",
                        "-of", "json", path], PROBE_TIMEOUT)
    if rc != 0:
        return None, (err.strip().splitlines() or [str(rc)])[-1]
    try:
        return json.loads(out).get("streams", []), None
    except ValueError:
        return None, "bad ffprobe json"


def summarize(streams):
    v = [s for s in streams if s.get("codec_type") == "video"]
    a = [s for s in streams if s.get("codec_type") == "audio"]
    vdesc = ", ".join(f"{s.get('codec_name')} {s.get('width')}x{s.get('height')} {s.get('pix_fmt')}" for s in v)
    acodecs = sorted({s.get("codec_name") or "unknown" for s in a})  # ffprobe may omit codec_name
    return vdesc, len(a), acodecs


def test(idx, size, path):
    res = {"idx": idx, "path": path, "size": size}
    streams, perr = probe(path)
    if streams is not None:
        res["src_video"], res["src_audio_tracks"], res["src_audio_codecs"] = summarize(streams)
    else:
        res["probe_error"] = perr

    avs = os.path.join(SCAN, "avs", f"{idx:05d}.avs")
    log = os.path.join(SCAN, "avs", f"{idx:05d}.log")
    strict_arg = f", strict={STRICT}" if STRICT in ("true", "false") else ""
    script = f'LoadPlugin("{DLL}")\nmxfsource("""{path}""", logfile="""{log}"""{strict_arg})\n'
    try:
        data = script.encode("mbcs")  # AviSynth+ and the plugin's fopen use the ANSI codepage
    except UnicodeEncodeError:
        res["status"] = "path_not_ansi"
        return res
    with open(avs, "wb") as f:
        f.write(data)
    if os.path.exists(log):
        os.remove(log)

    def read_log():
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                res["plugin_log"] = f.read().strip()[-20000:]
        except OSError:
            pass

    # Stream layout as exposed by the plugin.
    t0 = time.time()
    rc, out, err = run(["ffprobe", "-v", "error", "-show_entries",
                        "stream=codec_type,codec_name,pix_fmt,width,height,channels,sample_rate,sample_fmt,nb_frames",
                        "-of", "json", avs], DECODE_TIMEOUT)
    res["open_s"] = round(time.time() - t0, 2)
    if rc != 0:
        res["status"] = "open_failed" if rc != "timeout" else "open_timeout"
        res["error"] = "\n".join(l for l in err.strip().splitlines() if l.strip())[-600:]
        read_log()
        return res
    pstreams = json.loads(out).get("streams", [])
    pa = [s for s in pstreams if s.get("codec_type") == "audio"]
    pv = [s for s in pstreams if s.get("codec_type") == "video"]
    res["plugin_video"] = ", ".join(f"{s.get('width')}x{s.get('height')} {s.get('pix_fmt')}" for s in pv)
    res["plugin_audio"] = ", ".join(f"{s.get('channels')}ch {s.get('sample_rate')}Hz {s.get('sample_fmt')}" for s in pa)
    if pv and str(pv[0].get("nb_frames", "")).isdigit():
        res["plugin_frames"] = int(pv[0]["nb_frames"])

    # Decode first second (video + audio).
    t0 = time.time()
    rc, out, err = run(["ffmpeg", "-hide_banner", "-nostats", "-i", avs, "-map", "0", "-t", "1",
                        "-f", "null", "-"], DECODE_TIMEOUT)
    res["decode_s"] = round(time.time() - t0, 2)
    # ffmpeg exits 0 even when the AviSynth demuxer fails mid-way ("Error during demuxing", the plugin's
    # error as "[in#0/avisynth @ ...] MxfSource: ..."), so check stderr too -- the plugin's own info lines
    # look like "[MxfSource] <path>: ..." and don't match.
    if rc == 0 and ("Error during demuxing" in err or re.search(r"\] MxfSource: ", err)):
        rc = "demux_error"
    if rc != 0:
        res["status"] = "decode_failed" if rc != "timeout" else "decode_timeout"
        res["error"] = "\n".join(l for l in err.strip().splitlines() if l.strip())[-600:]
        read_log()
        return res
    res["decode_tail"] = [l for l in err.splitlines() if l.startswith("[out#") or "video:" in l][-1:]
    read_log()
    res["status"] = "ok"
    return res


def main():
    # The console may be cp1252; a path with e.g. a combining diaeresis (U+0308) made print() raise, which
    # aborted the whole worker pool. Escape what the console can't show instead.
    sys.stdout.reconfigure(errors="backslashreplace")
    os.makedirs(os.path.join(SCAN, "avs"), exist_ok=True)
    entries = []
    with open(os.path.join(SCAN, "list.tsv"), encoding="utf-8", errors="surrogateescape") as f:
        for i, line in enumerate(f):
            size, path = line.rstrip("\n").split("\t", 1)
            entries.append((i, int(size), path.replace("/", "\\")))
    done_path = os.path.join(SCAN, "results.jsonl")
    done = set()
    if os.path.exists(done_path):
        with open(done_path, encoding="utf-8") as f:
            done = {json.loads(l)["idx"] for l in f if l.strip()}
    todo = [e for e in entries if e[0] not in done]
    print(f"{len(entries)} files, {len(todo)} to do, {WORKERS} in parallel, strict={STRICT}", flush=True)

    def work(e):
        try:
            r = test(*e)
        except Exception as ex:  # keep the scan going
            r = {"idx": e[0], "path": e[2], "size": e[1], "status": "harness_error", "error": repr(ex)}
        with lock:
            with open(done_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
            print(f"[{r['idx']}] {r['status']} {r['path']}", flush=True)

    with ThreadPoolExecutor(WORKERS) as ex:
        list(ex.map(work, todo))


if __name__ == "__main__":
    main()
