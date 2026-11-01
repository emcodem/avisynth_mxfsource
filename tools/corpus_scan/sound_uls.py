"""Tally SoundEssenceCompression (tag 3D06) ULs in the header of every 'audio dropped' file.
Usage: python -I sound_uls.py <results.jsonl>"""
import collections
import json
import sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
dropped = [x for x in rows if x["status"] == "ok" and not x.get("plugin_audio") and x.get("src_audio_tracks")]
working = [x for x in rows if x["status"] == "ok" and x.get("plugin_audio")]


def uls(path):
    with open(path, "rb") as f:
        d = f.read(1 << 20)
    found = set()
    i = 0
    while (i := d.find(b"\x3d\x06\x00\x10", i)) >= 0:
        found.add(d[i + 4:i + 20].hex())
        i += 1
    return tuple(sorted(found)) or ("(no 3D06 tag)",)


for label, group in (("DROPPED", dropped), ("WORKING", working)):
    tally = collections.Counter()
    example = {}
    for x in group:
        try:
            u = uls(x["path"])
        except OSError as e:
            u = ("read error " + str(e)[:40],)
        tally[u] += 1
        example.setdefault(u, x["path"])
    print(f"== {label} ({len(group)} files)")
    for u, c in tally.most_common(10):
        print(f"  {c:>5}  {' + '.join(u)}")
        print(f"         e.g. {example[u][-90:]}")
