"""Walk KLVs like EssenceWalker does; report the first places alignment is lost.
Usage: python -I klvwalk.py <file> <start_offset> [max_klvs]"""
import collections
import sys

path, pos = sys.argv[1], int(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 10**9
f = open(path, "rb")
f.seek(0, 2)
size = f.tell()
keys = collections.Counter()
n = lost = 0
first = []
while pos + 17 <= size and n < limit:
    f.seek(pos)
    hdr = f.read(25)
    key = hdr[:16]
    if key[:4] != b"\x06\x0e\x2b\x34":
        lost += 1
        f.seek(pos)
        blk = f.read(64 << 20)
        nxt = blk.find(b"\x06\x0e\x2b\x34", 1)
        if len(first) < 8:
            first.append(f"lost at {pos} (bytes {hdr[:8].hex()}), next designator +{nxt}")
        if nxt < 0:
            break
        pos += nxt
        continue
    b = hdr[16]
    if b & 0x80:
        nb = b & 0x7F
        length = int.from_bytes(hdr[17:17 + nb], "big")
        voff = pos + 17 + nb
    else:
        length, voff = b, pos + 17
    keys[key[:13].hex() + ".." + key[13:].hex()] += 1
    if len(first) < 8 and n < 12:
        print(f"{pos:>12} len={length:<10} key={key.hex()}")
    pos = voff + length
    n += 1
print(f"walked {n} KLVs, lost alignment {lost} times, ended at {pos}/{size}")
for s in first:
    print(" ", s)
for k, c in keys.most_common(12):
    print(f"  {c:>7}  {k}")
