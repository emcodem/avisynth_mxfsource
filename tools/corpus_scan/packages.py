"""List every Package in an MXF file's Header Metadata (or the Footer's copy) with its tracks, Sequence
Durations, SourceClip references and Descriptor, and which package the EssenceContainerData links to
(= the file package that really describes the essence).

Usage: python -I packages.py <file> [partition_offset]   (default 0 = Header Partition)
"""
import sys

KINDS = {0x3600: "MaterialPackage", 0x3700: "SourcePackage", 0x3B00: "Track", 0x3A00: "StaticTrack",
         0x0F00: "Sequence", 0x1100: "SourceClip", 0x1400: "TimecodeComponent", 0x1800: "ContentStorage",
         0x2300: "EssenceContainerData", 0x4400: "MultipleDescriptor", 0x2800: "CDCIDescriptor",
         0x2900: "RGBADescriptor", 0x4200: "GenericSoundDescriptor", 0x4700: "AES3PCMDescriptor",
         0x4800: "WaveAudioDescriptor", 0x5100: "MPEGVideoDescriptor", 0x3000: "FileDescriptor",
         0x2500: "FileDescriptor", 0x2700: "GenericPictureDescriptor", 0x6100: "ExternalDescriptor"}


def ber(b, i):
    c = b[i]
    if c < 0x80:
        return c, i + 1
    n = c & 0x7F
    return int.from_bytes(b[i + 1:i + 1 + n], "big"), i + 1 + n


def batch(v):
    n, size = int.from_bytes(v[0:4], "big"), int.from_bytes(v[4:8], "big")
    return [v[8 + k * size:8 + (k + 1) * size] for k in range(n)]


f = open(sys.argv[1], "rb")
off = int(sys.argv[2]) if len(sys.argv) > 2 else 0
f.seek(off)
head = f.read(256)
L, v = ber(head, 16)
hbc = int.from_bytes(head[v + 32:v + 40], "big")
f.seek(off + v + L)
meta = f.read(hbc)

sets = {}
pos = 0
while pos + 17 <= len(meta):
    key = meta[pos:pos + 16]
    L, v = ber(meta, pos + 16)
    val = meta[v:v + L]
    pos = v + L
    if key[4:6] != b"\x02\x53" or key[8:13] != bytes.fromhex("0d01010101"):
        continue
    kind = (key[13] << 8) | key[14]
    tags = {}
    i = 0
    while i + 4 <= len(val):
        tag = int.from_bytes(val[i:i + 2], "big")
        n = int.from_bytes(val[i + 2:i + 4], "big")
        tags[tag] = val[i + 4:i + 4 + n]
        i += 4 + n
    uid = tags.get(0x3C0A)
    if uid:
        sets[uid] = (kind, tags)


def name(kind):
    return KINDS.get(kind, f"set {kind:04x}")


ecd_links = [t[0x2701].hex() for k, t in sets.values() if k == 0x2300 and 0x2701 in t]
for uid, (kind, t) in sets.items():
    if kind not in (0x3600, 0x3700):
        continue
    pkg_uid = t.get(0x4401, b"").hex()
    print(f"{name(kind)}  PackageUID ...{pkg_uid[-24:]}"
          f"{'  <-- linked by EssenceContainerData (file package)' if pkg_uid in ecd_links else ''}")
    if 0x4701 in t:
        dk, dt = sets.get(t[0x4701], (None, {}))
        print(f"    Descriptor: {name(dk) if dk is not None else 'not found'}")
    for tr in batch(t.get(0x4403, b"\0" * 8)):
        tk, tt = sets.get(tr, (None, {}))
        if tk is None:
            continue
        seq = sets.get(tt.get(0x4803), (None, {}))[1]
        dur = int.from_bytes(seq.get(0x0202, b""), "big", signed=True) if 0x0202 in seq else None
        dd = seq.get(0x0201, b"").hex()
        kind_s = {"01030201": "picture", "01030202": "sound", "01030201": "picture", "01030102": "timecode"}.get(dd[24:32], dd[20:32])
        clips = []
        for comp in batch(seq.get(0x1001, b"\0" * 8)):
            ck, ct = sets.get(comp, (None, {}))
            if ck == 0x1100:
                clips.append(f"SourceClip dur={int.from_bytes(ct.get(0x0202, b''), 'big', signed=True)} "
                             f"-> package ...{ct.get(0x1101, b'').hex()[-24:]} track {int.from_bytes(ct.get(0x1102, b''), 'big')}")
            elif ck is not None:
                clips.append(name(ck))
        print(f"    {name(tk)} id={int.from_bytes(tt.get(0x4801, b''), 'big')} number={tt.get(0x4804, b'').hex()} "
              f"origin={int.from_bytes(tt.get(0x4B02, b''), 'big', signed=True) if 0x4B02 in tt else '-'} "
              f"{kind_s}: Sequence Duration={dur}")
        for c in clips:
            print(f"        {c}")
