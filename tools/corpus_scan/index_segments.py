"""For each partition offset given, parse the Index Table Segment(s) after the partition pack.
Usage: python -I index_segments.py <file> <partition_offset> [...]"""
import sys

f = open(sys.argv[1], "rb")


def ber(b, i):
    if b[i] & 0x80:
        n = b[i] & 0x7F
        return int.from_bytes(b[i + 1:i + 1 + n], "big"), i + 1 + n
    return b[i], i + 1


for off in map(int, sys.argv[2:]):
    f.seek(off)
    d = f.read(16 << 10)
    plen, p = ber(d, 16)
    pv = d[p:p + plen]
    body_offset = int.from_bytes(pv[52:60], "big")
    i = p + plen
    while i < len(d) - 20:
        key = d[i:i + 16]
        ln, vi = ber(d, i + 16)
        if key[:4] != b"\x06\x0e\x2b\x34":
            break
        if key[13] == 0x10 and key[4:6] == b"\x02\x53":  # Index Table Segment
            v = d[vi:vi + ln]
            j = 0
            t = {}
            while j + 4 <= len(v):
                tag = int.from_bytes(v[j:j + 2], "big")
                tl = int.from_bytes(v[j + 2:j + 4], "big")
                t[tag] = v[j + 4:j + 4 + tl]
                j += 4 + tl
            start = int.from_bytes(t.get(0x3F0C, b""), "big")
            dur = int.from_bytes(t.get(0x3F0D, b""), "big")
            eubc = int.from_bytes(t.get(0x3F05, b""), "big")
            arr = t.get(0x3F0A, b"")
            first = last = None
            if len(arr) >= 8:
                n = int.from_bytes(arr[:4], "big")
                esz = int.from_bytes(arr[4:8], "big")
                ent = lambda k: int.from_bytes(arr[8 + k * esz + 3:8 + k * esz + 11], "big")
                if n:
                    first, last = ent(0), ent(n - 1)
            print(f"partition@{off} BodyOffset={body_offset}: IndexStart={start} Duration={dur} EUBC={eubc} "
                  f"entries stream offset first={first} last={last}")
        i = vi + ln
