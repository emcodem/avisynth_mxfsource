"""For each open_timeout result: header BodySID, RIP partition offsets, and whether the
header_parser Fixup-1 pattern applies (essence in header partition + RIP[1] is a Body
Partition far past the declared header metadata).  Usage: python -I check_layout.py <results.jsonl>"""
import json
import sys

rows = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")]
for r in sorted(rows, key=lambda x: x["idx"]):
    if r["status"] not in ("open_timeout", "decode_timeout"):
        continue
    out = {"idx": r["idx"], "status": r["status"], "gb": round(r["size"] / 1e9, 1), "video": r.get("src_video")}
    try:
        with open(r["path"], "rb") as f:
            hdr = f.read(256)
            ber = hdr[16]
            off = 17 + (ber & 0x7F) if ber & 0x80 else 17
            p = hdr[off:]
            header_byte_count = int.from_bytes(p[32:40], "big")
            index_byte_count = int.from_bytes(p[40:48], "big")
            body_sid = int.from_bytes(p[56:60], "big")
            partition_end = off + int.from_bytes(hdr[17:17 + (ber & 0x7F)], "big") if ber & 0x80 else off + ber
            declared_end = partition_end + header_byte_count + index_byte_count
            f.seek(-4, 2)
            rip_len = int.from_bytes(f.read(4), "big")
            rip = []
            if 0 < rip_len < 1 << 20:
                f.seek(-rip_len, 2)
                b = f.read(rip_len)
                if b[:13] == bytes.fromhex("060e2b3402050101 0d01020101".replace(" ", "")) and b[13] == 0x11:
                    lb = b[16]
                    vo = 17 + (lb & 0x7F) if lb & 0x80 else 17
                    v = b[vo:-4]
                    rip = [(int.from_bytes(v[i:i + 4], "big"), int.from_bytes(v[i + 4:i + 12], "big"))
                           for i in range(0, len(v) - 11, 12)]
            out.update(bodySid=body_sid, declaredHeaderEnd=declared_end, ripPartitions=len(rip))
            if len(rip) >= 2:
                f.seek(rip[1][1])
                k = f.read(16)
                is_body = k[:13] == bytes.fromhex("060e2b34020501010d01020101") and k[13] == 0x03
                out["rip1_body"] = is_body
                out["rip1_gb"] = round(rip[1][1] / 1e9, 2)
                out["fixup1_overscan_gb"] = round((rip[1][1] - declared_end) / 1e9, 2) if is_body else 0
    except OSError as e:
        out["err"] = str(e)
    print(json.dumps(out))
