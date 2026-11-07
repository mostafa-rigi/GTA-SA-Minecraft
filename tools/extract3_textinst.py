# v3: text-IPL inst re-parse with the correct field layout
# (id, modelname, interior, x, y, z, ...) + IDE name cross-check.
import json, os
ROOT = r"C:\GTA.San.Andreas"
DATA = os.path.join(ROOT, "data")
OUT = r"C:\Win7Assistant-up\atlas\text_inst.json"
WORDS = ["hosp", "polic", "cop", "ammu", "ammo", "gunshop", "spray",
         "transfender", "modshop", "locolow", "wheelarch", "casino",
         "airport", "airpt", "terminal", "stadium", "area69", "militar",
         "naval", "carrier", "prison", "gym", "burger", "pizza", "chicken",
         "donut", "barber", "tattoo", "binco", "zip", "victim", "subur",
         "prolap", "didier", "clothes", "safehouse", "savehouse", "school",
         "motel", "hotel", "bank", "diner", "restau", "cafe", "sexshop",
         "pawn", "hangar", "tower", "runway"]
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
out = []
for dp, _dn, fn in os.walk(DATA):
    for f in fn:
        if not f.lower().endswith(".ipl") or f.lower().startswith("paths"):
            continue
        p = os.path.join(dp, f)
        rel = os.path.relpath(p, ROOT)
        text = open(p, "r", errors="replace").read()
        for sec, lines in sections(text):
            if sec != "inst": continue
            for ln in lines:
                parts = [s.strip() for s in ln.split(",")]
                if len(parts) < 6: continue
                name = parts[1]
                low = name.lower()
                if not any(w in low for w in WORDS): continue
                if "copse" in low: continue
                try:
                    mid = int(parts[0]); x, y, z = float(parts[3]), float(parts[4]), float(parts[5])
                except Exception:
                    continue
                out.append({"src": rel, "id": mid, "name": name, "x": x, "y": y, "z": z})
with open(OUT, "w", encoding="utf-8") as fh:
    json.dump({"poi_insts": out}, fh)
print("text poi insts:", len(out))
