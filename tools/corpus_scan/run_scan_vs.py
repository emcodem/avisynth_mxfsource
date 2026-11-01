"""Linux/VapourSynth variant of run_scan.py: open each MXF in list.tsv through mxfsource_vs.so and decode the
first second of video AND audio, WORKERS files in parallel.

Usage: python3 run_scan_vs.py <scan_dir> <plugin .so> [share_prefix_from share_prefix_to]...
  e.g. python3 run_scan_vs.py results_x /root/avs_mxf_plugin/build/src/vs/mxfsource_vs.so \
           //SERVER/SHARE /mnt/public97
Writes <scan_dir>/results.jsonl (one line per file, appended as each finishes; resumable).

Per file, each step with its own timeout (OPEN/DECODE_TIMEOUT, like run_scan.py's 10 s):
  open   vspipe --info on output 0; the script itself writes the clip/audio properties to a JSON file
  video  vspipe -o 0 for the first second of frames (raw output discarded)
  audio  vspipe -o 1 -c wav for the first second of audio (only if the plugin exposes audio)
y4m (the usual vspipe | ffmpeg path) carries no audio, hence the separate audio step.
The full plugin log is kept (run_scan.py kept only the last 600 characters).
Environment: SCAN_STRICT=true|false passes strict= (unset: plugin default), SCAN_WORKERS (default 20).
open_s / decode_s / audio_s record each step's wall time.
"""
import json
import math
import os
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

SCAN = sys.argv[1]
PLUGIN = sys.argv[2]
PREFIXES = list(zip(sys.argv[3::2], sys.argv[4::2]))  # (display prefix, local mount) pairs
STRICT = os.environ.get("SCAN_STRICT")
WORKERS = int(os.environ.get("SCAN_WORKERS", "20"))  # SCAN_WORKERS=1 to re-time timeouts serially
PROBE_TIMEOUT = 60
OPEN_TIMEOUT = 10
DECODE_TIMEOUT = 10

# VapourSynth format names -> ffmpeg pix_fmt names, so plugin_video compares with run_scan.py's results.
VS_TO_FFMPEG = {
    "YUV420P8": "yuv420p", "YUV422P8": "yuv422p", "YUV444P8": "yuv444p",
    "YUV420P10": "yuv420p10le", "YUV422P10": "yuv422p10le", "YUV444P10": "yuv444p10le",
    "YUV420P12": "yuv420p12le", "YUV422P12": "yuv422p12le", "YUV444P12": "yuv444p12le",
}

SCRIPT = """import json, vapoursynth as vs
core = vs.core
core.std.LoadPlugin({plugin!r})
r = core.mxfsource.MxfSource({path!r}, logfile={log!r}{strict})
clip = r if isinstance(r, vs.VideoNode) else r["clip"]
audio = None if isinstance(r, vs.VideoNode) else r["audio"]
info = {{"width": clip.width, "height": clip.height, "format": clip.format.name, "frames": clip.num_frames,
         "fps_num": clip.fps.numerator, "fps_den": clip.fps.denominator}}
if audio is not None:
    info["audio"] = {{"channels": audio.num_channels, "sample_rate": audio.sample_rate,
                      "bits": audio.bits_per_sample, "samples": audio.num_samples}}
with open({info!r}, "w") as f:
    json.dump(info, f)
clip.set_output(0)
if audio is not None:
    audio.set_output(1)
"""

lock = threading.Lock()


def run(cmd, timeout):
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return "timeout", "", ""


def last_error(err):
    lines = [l for l in err.strip().splitlines() if l.strip()]
    for l in lines:
        if "MxfSource:" in l:
            return l.strip()[-600:]
    return "\n".join(lines)[-600:]


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
    return vdesc, len(a), sorted({s.get("codec_name") or "unknown" for s in a})  # ffprobe may omit codec_name


def test(idx, size, path, display_path):
    res = {"idx": idx, "path": display_path, "size": size}
    streams, perr = probe(path)
    if streams is not None:
        res["src_video"], res["src_audio_tracks"], res["src_audio_codecs"] = summarize(streams)
    else:
        res["probe_error"] = perr

    base = os.path.join(SCAN, "vpy", f"{idx:05d}")
    vpy, log, info_path = base + ".vpy", base + ".log", base + ".json"
    for p in (log, info_path):
        if os.path.exists(p):
            os.remove(p)
    with open(vpy, "w", encoding="utf-8") as f:
        strict = {"true": ", strict=1", "false": ", strict=0"}.get(STRICT, "")
        f.write(SCRIPT.format(plugin=PLUGIN, path=path, log=log, info=info_path, strict=strict))

    def finish(status, error=None):
        res["status"] = status
        if error:
            res["error"] = error
        if os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                res["plugin_log"] = f.read()[-20000:]
        return res

    t0 = time.time()
    rc, out, err = run(["vspipe", "--info", "-o", "0", vpy, "-"], OPEN_TIMEOUT)
    res["open_s"] = round(time.time() - t0, 2)
    if rc != 0:
        return finish("open_failed" if rc != "timeout" else "open_timeout", last_error(err) if rc != "timeout" else None)
    with open(info_path, encoding="utf-8") as f:
        info = json.load(f)
    res["plugin_video"] = f"{info['width']}x{info['height']} {VS_TO_FFMPEG.get(info['format'], info['format'])}"
    res["plugin_frames"] = info["frames"]
    a = info.get("audio")
    res["plugin_audio"] = f"{a['channels']}ch {a['sample_rate']}Hz s{a['bits']}" if a else ""

    one_second = max(1, min(info["frames"], math.ceil(info["fps_num"] / max(1, info["fps_den"]))))
    t0 = time.time()
    rc, out, err = run(["vspipe", "-o", "0", "-e", str(one_second - 1), vpy, "/dev/null"], DECODE_TIMEOUT)
    res["decode_s"] = round(time.time() - t0, 2)
    if rc != 0:
        return finish("decode_failed" if rc != "timeout" else "decode_timeout",
                      "video: " + (last_error(err) if rc != "timeout" else "timeout"))
    if a:
        audio_frames = max(1, min(math.ceil(a["samples"] / 3072), math.ceil(a["sample_rate"] / 3072)))
        t0 = time.time()
        rc, out, err = run(["vspipe", "-o", "1", "-c", "wav", "-e", str(audio_frames - 1), vpy, "/dev/null"],
                           DECODE_TIMEOUT)
        res["audio_s"] = round(time.time() - t0, 2)
        if rc != 0:
            return finish("decode_failed" if rc != "timeout" else "decode_timeout",
                          "audio: " + (last_error(err) if rc != "timeout" else "timeout"))
    return finish("ok")


def main():
    os.makedirs(os.path.join(SCAN, "vpy"), exist_ok=True)
    entries = []
    with open(os.path.join(SCAN, "list.tsv"), encoding="utf-8", errors="surrogateescape") as f:
        for i, line in enumerate(f):
            size, display = line.rstrip("\n").split("\t", 1)
            local = display
            for frm, to in PREFIXES:
                if display.startswith(frm):
                    local = to + display[len(frm):]
                    break
            entries.append((i, int(size), local, display))
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
            r = {"idx": e[0], "path": e[3], "size": e[1], "status": "harness_error", "error": repr(ex)}
        with lock:
            with open(done_path, "a", encoding="utf-8") as f:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
            print(f"[{r['idx']}] {r['status']} {r['path']}", flush=True)

    with ThreadPoolExecutor(WORKERS) as ex:
        list(ex.map(work, todo))


if __name__ == "__main__":
    main()
