# PC-side atlas extractor v2: BNRY inst from gta3.img (validated layout:
# header 76 bytes, inst count u32@4, inst records at 76, 40 bytes each).
import json, os, struct
ROOT = r"C:\GTA.San.Andreas"
IMG = os.path.join(ROOT, "models", "gta3.img")
OUT = r"C:\Win7Assistant-up\atlas\bnry_inst.json"
WORDS = ["hosp", "polic", "cop", "ammu", "ammo", "gunshop", "spray",
         "transfender", "modshop", "locolow", "wheelarch", "casino",
         "airport", "airpt", "terminal", "stadium", "area69", "militar",
         "naval", "carrier", "prison", "gym", "burger", "pizza", "chicken",
         "donut", "barber", "tattoo", "binco", "zip", "victim", "subur",
         "prolap", "didier", "clothes", "safehouse", "savehouse", "school",
         "motel", "hotel", "bank", "diner", "restau", "cafe", "sexshop",
         "pawn", "airport", "hangar", "tower", "runway"]
# IDE names rebuilt the same way as v1
ide_names = {}
DATA = os.path.join(ROOT, "data")
def sections(text):
    cur, lines = None, []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if cur is None:
            if line == "end": continue
            cur, lines = line.lower(), []
        elif line == "end":
            yield cur, lines
            cur, lines = None, []
        else:
            lines.append(line)
    if cur is not None:
        yield cur, lines
for dp, _dn, fn in os.walk(DATA):
    for f in fn:
        if f.lower().endswith(".ide"):
            text = open(os.path.join(dp, f), "r", errors="replace").read()
            for sec, lines in sections(text):
                if sec != "objs": continue
                for ln in lines:
                    p = [s.strip() for s in ln.split(",")]
                    if len(p) >= 2:
                        try: ide_names[int(p[0])] = p[1]
                        except Exception: pass
def poi(mid):
    nm = ide_names.get(mid, "")
    low = nm.lower()
    for w in WORDS:
        if w in low: return nm
    return None
out = {"poi_insts": [], "files": 0, "inst_total": 0, "sec48_total": 0}
with open(IMG, "rb") as fh:
    head = fh.read(12)
    count = struct.unpack_from("<I", head, 4)[0]
    u32_8 = struct.unpack_from("<I", head, 8)[0]
    off = 12 if u32_8 == count * 32 else 8
    fh.seek(off)
    entries = []
    for _ in range(count):
        raw = fh.read(32)
        o, s = struct.unpack_from("<II", raw, 0)
        nm = raw[8:32].split(b"\0")[0].decode("ascii", "replace")
        entries.append((nm, o * 2048, s * 2048))
    for name, o, size in entries:
        if not name.lower().endswith(".ipl"): continue
        fh.seek(o)
        blob = fh.read(size)
        if blob[:4] != b"bnry" or len(blob) < 76: continue
        n = struct.unpack_from("<I", blob, 4)[0]
        n48 = struct.unpack_from("<I", blob, 20)[0]
        out["files"] += 1
        out["inst_total"] += n
        out["sec48_total"] += n48
        for i in range(n):
            ro = 76 + i * 40
            if ro + 40 > len(blob): break
            x, y, z = struct.unpack_from("<3f", blob, ro)
            mid = struct.unpack_from("<i", blob, ro + 28)[0]
            nm = poi(mid)
            if nm:
                out["poi_insts"].append({"src": "gta3.img:" + name, "id": mid,
                                         "name": nm, "x": x, "y": y, "z": z})
with open(OUT, "w", encoding="utf-8") as fh:
    json.dump(out, fh)
print("files", out["files"], "inst", out["inst_total"], "sec48", out["sec48_total"], "poi", len(out["poi_insts"]))
