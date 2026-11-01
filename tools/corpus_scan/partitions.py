"""List every partition pack in an MXF by walking KLVs, plus audio element sizes.
Usage: python -I partitions.py <file> [max_bytes]"""
import collections
import sys

path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 1 << 62
f = open(path, "rb")
f.seek(0, 2)
size = f.tell()
PP = bytes.fromhex("060e2b34020501010d01020101")
KINDS = {2: "Header", 3: "Body", 4: "Footer"}
STATUS = {1: "OpenIncomplete", 2: "ClosedIncomplete", 3: "OpenComplete", 4: "ClosedComplete"}
pos = 0
audio_sizes = collections.Counter()
n = 0
last_good = 0
while pos + 17 <= min(size, limit):
    f.seek(pos)
    h = f.read(64 << 10)
    if h[:4] != b"\x06\x0e\x2b\x34":
        nxt = h.find(b"\x06\x0e\x2b\x34")
        if nxt < 0:
            print(f"no KLV in 64 KB at {pos}: stopping (last good KLV end {last_good})")
            break
        print(f"  gap of {nxt} non-KLV bytes at {pos}")
        pos += nxt
        continue
    b = h[16]
    nb = b & 0x7F if b & 0x80 else 0
    length = int.from_bytes(h[17:17 + nb], "big") if nb else b
    voff = pos + 17 + nb
    key = h[:16]
    if key[:13] == PP and key[13] in KINDS:
        v = h[17 + nb:17 + nb + 88]
        g = lambda o, n: int.from_bytes(v[o:o + n], "big")
        print(f"{pos:>12} {KINDS[key[13]]:<6} {STATUS.get(key[14], key[14]):<16} this={g(8,8)} prev={g(16,8)} "
              f"footer={g(24,8)} HBC={g(32,8)} IBC={g(40,8)} IndexSID={g(48,4)} BodyOffset={g(52,8)} BodySID={g(60,4)}")
    elif key[:12] == bytes.fromhex("060e2b34010201010d010301") and key[12] == 0x16:
        audio_sizes[length] += 1
    pos = voff + length
    last_good = pos
    n += 1
print(f"walked {n} KLVs to {pos} of {size} bytes ({pos / size:.1%})")
print("audio element sizes (bytes: count):", dict(audio_sizes.most_common(8)))
