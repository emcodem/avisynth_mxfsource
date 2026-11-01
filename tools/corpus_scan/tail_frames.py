"""Count picture elements from a given offset (e.g. the last Index Table entry) to the end of the essence:
walks KLVs, stops at the first Partition Pack, the RIP, or EOF. Tells whether frames a file declares but
doesn't index really exist in the essence.

Usage: python -I tail_frames.py <file> <offset>
"""
import sys

f = open(sys.argv[1], "rb")
f.seek(0, 2)
size = f.tell()
pos = int(sys.argv[2])
pictures, other, stop = 0, 0, "EOF"
while pos + 17 <= size:
    f.seek(pos)
    h = f.read(25)
    if h[:4] != b"\x06\x0e\x2b\x34":
        stop = f"non-KLV bytes at {pos}"
        break
    if h[4:6] == b"\x02\x05" and h[8:13] == bytes.fromhex("0d01020101"):
        stop = f"{'RIP' if h[13] == 0x11 else 'Partition Pack'} at {pos}"
        break
    c = h[16]
    if c < 0x80:
        L, n = c, 1
    else:
        k = c & 0x7F
        L, n = int.from_bytes(h[17:17 + k], "big"), 1 + k
    is_picture = h[4:6] == b"\x01\x02" and h[8:12] == bytes.fromhex("0d010301") and h[12] in (0x05, 0x15)
    pictures += is_picture
    other += not is_picture
    end = pos + 16 + n + L
    if end > size:
        stop = f"element at {pos} cut off ({end - size} bytes past EOF)"
        pictures -= is_picture
        break
    pos = end
print(f"picture elements from offset: {pictures} (including the one at the offset), other KLVs: {other}, "
      f"stopped at: {stop}, file size {size}")
