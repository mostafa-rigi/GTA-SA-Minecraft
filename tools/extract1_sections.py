# PC-side atlas extractor (Python 3.8): READ-ONLY over the install.
# Parses loose IPL sections (enex/zone/pick/cars/grge + zone files), IDE
# object names, and the gta3.img binary IPLs (BNRY inst, self-validating),
# then writes ONE json with everything the local builder needs + a sha
# manifest of every source file touched. Nothing in the game folder is
# written; output goes to the staging folder only.
import hashlib
import json
import os
import struct

ROOT = r"C:\GTA.San.Andreas"
DATA = os.path.join(ROOT, "data")
IMG = os.path.join(ROOT, "models", "gta3.img")
OUTDIR = r"C:\Win7Assistant-up\atlas"
OUT = os.path.join(OUTDIR, "atlas_raw.json")

POI_WORDS = ["hosp", "polic", "cop", "ammu", "ammo", "gunshop", "spray",
             "transfender", "modshop", "locolow", "wheelarch", "casino",
             "airport", "airpt", "terminal", "stadium", "area69", "militar",
             "naval", "carrier", "prison", "gym", "burger", "pizza",
             "chicken", "donut", "barber", "tattoo", "binco", "zip",
             "victim", "subur", "prolap", "didier", "clothes", "safehouse",
             "savehouse", "school", "motel", "hotel", "bank", "courier",
             "diner", "restau", "cafe", "bar1", "sexshop", "pawn", "ammu"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sections(text):
    """Yield (section_name, [lines]) from a text IPL/IDE/ZON file."""
    cur, lines = None, []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if cur is None:
            if line == "end":
                continue
            cur, lines = line.lower(), []
        elif line == "end":
            yield cur, lines
            cur, lines = None, []
        else:
            lines.append(line)
    if cur is not None:
        yield cur, lines


def split(line):
    return [p.strip() for p in line.split(",")]


def fnum(s):
    try:
        return float(s)
    except Exception:
        return None


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    man = {}
    ide_names = {}
    out = {"enex": [], "zones": [], "picks": [], "cars": [], "grge": [],
           "poi_insts": [], "img_ipl": [], "counts": {}}

    # ---- IDE object names (all loose .ide under data/)
    ide_files = []
    for dp, _dn, fn in os.walk(DATA):
        for f in fn:
            if f.lower().endswith(".ide"):
                ide_files.append(os.path.join(dp, f))
    for p in sorted(ide_files):
        man[os.path.relpath(p, ROOT)] = sha256(p)
        text = open(p, "r", errors="replace").read()
        for sec, lines in sections(text):
            if sec != "objs":
                continue
            for ln in lines:
                parts = split(ln)
                if len(parts) >= 2:
                    try:
                        ide_names[int(parts[0])] = parts[1]
                    except Exception:
                        pass
    out["counts"]["ide_names"] = len(ide_names)

    def poi_name(model_id):
        nm = ide_names.get(model_id, "")
        low = nm.lower()
        for w in POI_WORDS:
            if w in low:
                return nm
        return None

    # ---- text IPL sections + zone files
    ipl_files = []
    for dp, _dn, fn in os.walk(DATA):
        for f in fn:
            low = f.lower()
            if low.endswith(".ipl") and not low.startswith("paths"):
                ipl_files.append(os.path.join(dp, f))
            elif low.endswith(".zon"):
                ipl_files.append(os.path.join(dp, f))
    for p in sorted(ipl_files):
        rel = os.path.relpath(p, ROOT)
        man[rel] = sha256(p)
        text = open(p, "r", errors="replace").read()
        for sec, lines in sections(text):
            if sec == "enex":
                for ln in lines:
                    out["enex"].append({"src": rel, "f": split(ln)})
            elif sec == "zone":
                for ln in lines:
                    out["zones"].append({"src": rel, "f": split(ln)})
            elif sec == "pick":
                for ln in lines:
                    out["picks"].append({"src": rel, "f": split(ln)})
            elif sec == "cars":
                for ln in lines:
                    out["cars"].append({"src": rel, "f": split(ln)})
            elif sec == "grge":
                for ln in lines:
                    out["grge"].append({"src": rel, "f": split(ln)})
            elif sec == "inst":
                for ln in lines:
                    parts = split(ln)
                    if len(parts) >= 5:
                        try:
                            mid = int(parts[0])
                            x, y, z = float(parts[2]), float(parts[3]), float(parts[4])
                        except Exception:
                            continue
                        nm = poi_name(mid)
                        if nm:
                            out["poi_insts"].append({"src": rel, "id": mid,
                                                     "name": nm, "x": x, "y": y, "z": z})
    for k in ("enex", "zones", "picks", "cars", "grge", "poi_insts"):
        out["counts"][k + "_text"] = len(out[k])

    # ---- gta3.img binary IPLs: directory + BNRY inst (validated)
    with open(IMG, "rb") as fh:
        head = fh.read(12)
        magic = head[:4]
        count = struct.unpack_from("<I", head, 4)[0]
        u32_8 = struct.unpack_from("<I", head, 8)[0]
        entries_off = 12 if u32_8 == count * 32 else 8
        fh.seek(entries_off)
        directory = []
        for _i in range(count):
            raw = fh.read(32)
            if len(raw) < 32:
                break
            off_sectors, size_sectors = struct.unpack_from("<II", raw, 0)
            name = raw[8:32].split(b"\0")[0].decode("ascii", "replace")
            directory.append((name, off_sectors * 2048, size_sectors * 2048))
        ipl_entries = [d for d in directory if d[0].lower().endswith(".ipl")]
        out["counts"]["img_entries"] = len(directory)
        out["counts"]["img_ipl_entries"] = len(ipl_entries)
        for name, off, size in ipl_entries:
            fh.seek(off)
            blob = fh.read(size)
            info = {"name": name, "size": size, "magic": blob[:4].decode("ascii", "replace"),
                    "head_u32": list(struct.unpack_from("<19I", blob, 0)) if len(blob) >= 76 else [],
                    "inst": 0, "hyp": None}
            # Hypothesis: 76-byte header of (count, offset) pairs for
            # sections [inst, ?, ?, ?, ?, ?] starting at u32 index 1.
            # Validate inst records: 40 bytes each = pos(3f) quat(4f)
            # id(i32) interior(i32) lod(i32); id must be a known IDE id
            # (or 0..19999), positions within the map, quat near unit.
            if blob[:4] == b"BNRY" and len(blob) >= 76:
                u = info["head_u32"]
                for hyp in range(6):
                    cnt, secoff = u[1 + hyp * 2], u[2 + hyp * 2]
                    if cnt == 0 or cnt > 20000 or secoff + cnt * 40 > len(blob) or secoff < 76:
                        continue
                    ok = 0
                    recs = []
                    for i in range(cnt):
                        o = secoff + i * 40
                        x, y, z = struct.unpack_from("<3f", blob, o)
                        qx, qy, qz, qw = struct.unpack_from("<4f", blob, o + 12)
                        mid, _interior, _lod = struct.unpack_from("<3i", blob, o + 28)
                        qn = qx * qx + qy * qy + qz * qz + qw * qw
                        if abs(x) <= 6000 and abs(y) <= 6000 and abs(z) <= 1000 and 0.9 < qn < 1.1:
                            ok += 1
                            nm = poi_name(mid)
                            if nm:
                                recs.append({"id": mid, "name": nm, "x": x, "y": y, "z": z})
                    if cnt and ok >= 0.95 * cnt:
                        info["inst"] = cnt
                        info["hyp"] = hyp
                        for r in recs:
                            r["src"] = "gta3.img:" + name
                            out["poi_insts"].append(r)
                        break
            out["img_ipl"].append(info)
    out["counts"]["poi_insts_total"] = len(out["poi_insts"])
    out["manifest"] = man
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(out, fh)
    print("counts:", json.dumps(out["counts"]))
    print("wrote", OUT, os.path.getsize(OUT))


if __name__ == "__main__":
    main()
