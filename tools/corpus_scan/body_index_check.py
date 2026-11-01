"""For the 'no index/RIP' open timeouts: is there a Body Partition carrying Index Table Segments
(IndexByteCount > 0)?  Reads forward in 8 MB blocks (max 400 MB), accepting a partition pack only
if its ThisPartition field equals its own file offset.
Usage: python -I body_index_check.py <scratchpad>  -> <scratchpad>/scan/body_index_check.jsonl"""
import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor

sp = sys.argv[1]
layout = {json.loads(l)["idx"]: json.loads(l) for l in open(os.path.join(sp, "scan", "timeouts_layout.jsonl"))}
rows = {json.loads(l)["idx"]: json.loads(l) for l in open(os.path.join(sp, "scan", "results.jsonl"), encoding="utf-8")}
for l in open(os.path.join(sp, "rescan1", "results.jsonl"), encoding="utf-8"):
    x = json.loads(l)
    if x["status"] == "open_timeout":
        idx = next(k for k, v in rows.items() if v["path"] == x["path"])
        rows[idx] = dict(x, idx=idx)
targets = [x for x in rows.values() if x["status"] == "open_timeout"
           and not layout.get(x["idx"], {}).get("fixup1_overscan_gb", 0) > 0.1]

KEY = bytes.fromhex("060e2b34020501010d01020101")
BLOCK = 8 << 20
LIMIT = 400 << 20


def check(x):
    r = {"idx": x["idx"], "path": x["path"], "size": x["size"], "body_partitions": 0, "first_indexed_body": None}
    try:
        with open(x["path"], "rb") as f:
            pos = 0
            while pos < min(LIMIT, x["size"]):
                f.seek(pos)
                d = f.read(BLOCK + 128)
                i = 0
                while (i := d.find(KEY, i)) >= 0 and i < BLOCK:
                    if d[i + 13] == 0x03 and i + 120 <= len(d):
                        b = d[i + 16]
                        nb = b & 0x7F if b & 0x80 else 0
                        v = d[i + 17 + nb:]
                        if int.from_bytes(v[8:16], "big") == pos + i:
                            r["body_partitions"] += 1
                            ibc = int.from_bytes(v[40:48], "big")
                            if ibc > 0 and r["first_indexed_body"] is None:
                                r["first_indexed_body"] = pos + i
                                r["ibc"] = ibc
                    i += 1
                if r["first_indexed_body"] is not None:
                    break
                pos += BLOCK
            r["scanned_mb"] = round(min(pos + BLOCK, x["size"]) / 1e6)
    except OSError as e:
        r["err"] = str(e)
    return r


with ThreadPoolExecutor(10) as ex:
    res = list(ex.map(check, targets))
with open(os.path.join(sp, "scan", "body_index_check.jsonl"), "w", encoding="utf-8") as f:
    for r in res:
        f.write(json.dumps(r, ensure_ascii=False) + "\n")
print(len(res), "files;", sum(r["first_indexed_body"] is not None for r in res), "have an indexed Body Partition")
for r in sorted(res, key=lambda r: r["size"]):
    print(f"{r['size'] / 1e9:7.2f} GB  bodies={r['body_partitions']:<3} indexedBody@{r['first_indexed_body']}  "
          f"scanned {r.get('scanned_mb')} MB  {ascii(r['path'][-60:])}")
