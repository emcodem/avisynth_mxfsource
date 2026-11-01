"""Count essence elements per item type over the whole file by walking every KLV (partition packs,
metadata and index segments included, just not counted as essence). Independent of any index.

Usage: python -I count_elements.py <file>
"""
import collections
import sys

ITEMS = {0x05: "CP picture", 0x15: "GC picture", 0x06: "CP sound", 0x16: "GC sound", 0x04: "CP system",
         0x14: "GC system", 0x07: "CP data", 0x17: "GC data", 0x18: "GC compound"}
f = open(sys.argv[1], "rb")
f.seek(0, 2)
size = f.tell()
pos = 0
counts = collections.Counter()
while pos + 17 <= size:
    f.seek(pos)
    h = f.read(25)
    if h[:4] != b"\x06\x0e\x2b\x34":
        nxt = -1
        f.seek(pos)
        chunk = f.read(65536)
        nxt = chunk.find(b"\x06\x0e\x2b\x34", 1)
        if nxt < 0:
            counts["(stopped: no KLV in 64 KiB)"] += 1
            break
        counts["(non-KLV gap)"] += 1
        pos += nxt
        continue
    c = h[16]
    if c < 0x80:
        L, n = c, 1
    else:
        k = c & 0x7F
        L, n = int.from_bytes(h[17:17 + k], "big"), 1 + k
    if h[8:12] == bytes.fromhex("0d010301"):
        counts[ITEMS.get(h[12], f"item {h[12]:02x}")] += 1
    pos += 16 + n + L
for k, v in sorted(counts.items()):
    print(f"{v:8d}  {k}")
