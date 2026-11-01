"""Time plugin open + 1 s decode serially for given avs ids.  Usage: python -I serial_time.py <avs_dir> <id> [...]"""
import os
import subprocess
import sys
import time

d = sys.argv[1]
for i in sys.argv[2:]:
    avs = os.path.join(d, f"{int(i):05d}.avs")
    t0 = time.time()
    try:
        p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type", "-of", "csv", avs],
                           capture_output=True, timeout=120, stdin=subprocess.DEVNULL)
        t1 = time.time()
        p2 = subprocess.run(["ffmpeg", "-v", "error", "-i", avs, "-map", "0", "-t", "1", "-f", "null", "-"],
                            capture_output=True, timeout=300, stdin=subprocess.DEVNULL)
        t2 = time.time()
        print(f"{i:>6}  open {t1 - t0:6.1f}s  decode1s {t2 - t1:6.1f}s  rc={p.returncode}/{p2.returncode} "
              f"streams={p.stdout.decode().split()}", flush=True)
    except subprocess.TimeoutExpired as e:
        print(f"{i:>6}  TIMEOUT after {time.time() - t0:.0f}s in {e.cmd[0]}", flush=True)
