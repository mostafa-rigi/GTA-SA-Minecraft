#!/usr/bin/env python3
"""Decode a Driver 2 LEVELS/*.LCF cosmetics file (CAR_COSMETICS table).

Spec: REDRIVER 2, GAME/C/COSMETIC.C (ProcessCosmeticsLump) and
GAME/DR2TYPES.H (struct CAR_COSMETICS, 236 bytes). The file opens with
MAX_COSMETICS (13) int32 offsets, one per EXTERNAL MODEL id; resident
slot i wears the record at offsets[residentModels[i]]. The cop car is
external model 0: CarHasSiren (GAMESND.C) returns nonzero exactly when
residentModels[slot] == 0, MapCarIndexToBank maps model 0 to the cop's
engine bank, and MAIN.C tests residentModels[3] == 0 for "cops in
pursuit" - so offsets[0] is the cop's own row.

Usage: decode_lcf.py CHICAGO.LCF [out.json]
"""
import json
import struct
import sys

REC = 236
N = 13
SVEC = struct.Struct("<hhhh")  # SVECTOR: vx, vy, vz, pad


def svec(buf, ofs):
    x, y, z, _ = SVEC.unpack_from(buf, ofs)
    return [x, y, z]


def decode_record(buf, ofs):
    r = {}
    names = ["headLight", "frontInd", "backInd", "brakeLight", "revLight",
             "policeLight", "exhaust", "smoke", "fire"]
    for i, n in enumerate(names):
        r[n] = svec(buf, ofs + i * 8)
    r["wheelDisp"] = [svec(buf, ofs + 72 + i * 8) for i in range(4)]
    (r["extraInfo"], r["powerRatio"], r["cbYoffset"], r["susCoeff"],
     r["traction"], r["wheelSize"]) = struct.unpack_from("<hhhhhh", buf, ofs + 104)
    r["cPoints"] = [svec(buf, ofs + 116 + i * 8) for i in range(12)]
    r["colBox"] = svec(buf, ofs + 212)
    r["cog"] = svec(buf, ofs + 220)
    (r["twistRateX"], r["twistRateY"], r["twistRateZ"],
     r["mass"]) = struct.unpack_from("<hhhh", buf, ofs + 228)
    return r


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    assert len(data) == 52 + N * REC, f"unexpected LCF size {len(data)}"
    offsets = struct.unpack_from("<13i", data, 0)
    rows = []
    for model in range(N):
        ofs = offsets[model]
        assert 52 <= ofs < len(data), f"model {model}: bad offset {ofs}"
        rec = decode_record(data, ofs)
        rec["model"] = model
        rec["offset"] = ofs
        rows.append(rec)
    out = {"file": path, "size": len(data), "offsets": list(offsets), "rows": rows}
    if len(sys.argv) > 2:
        with open(sys.argv[2], "w") as f:
            json.dump(out, f, indent=1)
    cop = rows[0]
    print(f"LCF {path}: {len(data)} bytes, offsets {list(offsets)}")
    for r in rows:
        print(f"  model {r['model']:2d} @ {r['offset']:5d}: power={r['powerRatio']:6d} "
              f"traction={r['traction']:6d} sus={r['susCoeff']:6d} wheel={r['wheelSize']:4d} "
              f"mass={r['mass']:6d} extra={r['extraInfo']}")
    print("COP (model 0):", json.dumps(cop, indent=1))


if __name__ == "__main__":
    main()
