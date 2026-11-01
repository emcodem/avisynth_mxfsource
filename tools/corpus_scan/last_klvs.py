"""Walk KLVs from an offset and print the last N before the walk stops (EOF, non-KLV bytes such as zero
fill, or an element cut off at EOF), with each key's item type -- shows whether the last content package
is complete.

Usage: python -I last_klvs.py <file> <start_offset> [N]
"""
import collections
import sys

ITEMS = {0x04: "CP system", 0x05: "CP picture", 0x06: "CP sound", 0x14: "GC system", 0x15: "GC picture",
         0x16: "GC sound", 0x17: "GC data"}
f = open(sys.argv[1], "rb")
f.seek(0, 2)
size = f.tell()
pos = int(sys.argv[2])
keep = collections.deque(maxlen=int(sys.argv[3]) if len(sys.argv) > 3 else 20)
stop = "EOF"
while pos + 17 <= size:
    f.seek(pos)
    h = f.read(25)
    if h[:4] != b"\x06\x0e\x2b\x34":
        stop = f"non-KLV bytes at {pos}: {h[:8].hex()}"
        break
    c = h[16]
    L, n = (c, 1) if c < 0x80 else (int.from_bytes(h[17:17 + (c & 0x7F)], "big"), 1 + (c & 0x7F))
    kind = ITEMS.get(h[12], "") if h[8:12] == bytes.fromhex("0d010301") else ""
    end = pos + 16 + n + L
    keep.append(f"{pos:>12} {h[:16].hex()} len={L:<8} {kind}{'  CUT OFF at EOF' if end > size else ''}")
    if end > size:
        stop = "element cut off at EOF"
        break
    pos = end
print("\n".join(keep))
print("stopped:", stop)
