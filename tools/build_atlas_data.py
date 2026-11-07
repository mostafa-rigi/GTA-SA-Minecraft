#!/usr/bin/env python3
"""Build the atlas datasets from the extracted game data.

Inputs (atlas-assets/sources/): atlas_raw.json (text IPL sections +
zones), bnry_inst.json + text_inst.json (POI anchor placements).
Outputs:
  atlas-assets/poi_data.json       - the POI database
  atlas-assets/zones_data.json     - all zones + documented gang owners
  atlas-assets/interiors_data.json - enex door families (street doors,
                                     heaven records, interior ids)
  src-repo/src/core/AtlasData.inc  - the same as static C++ tables
  atlas-assets/atlas_summary.json  - counts + spot checks

Every POI carries its provenance: FILE (from the game's own data) or
DOC (documented community knowledge, cross-checked against the zone
it lands in). Nothing is placed from memory alone without the DOC tag.
"""

import json
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "sources")
REPO = os.path.normpath(os.path.join(HERE, "..", "src-repo"))

KIND = {"GENERIC": 0, "HOSPITAL": 1, "POLICE": 2, "AMMU": 3,
        "PAYNSPRAY": 4, "MODSHOP": 5, "SAFEHOUSE": 6, "FOOD": 7,
        "GYM": 8, "CLOTHES": 9, "BAR": 10, "STRIP": 11, "MOTEL": 12,
        "SHOP247": 13, "CASINO": 14, "AIRPORT": 15, "STADIUM": 16,
        "MILITARY": 17, "SCHOOL": 18}
REFUGE = {"HOSPITAL", "AMMU", "PAYNSPRAY", "MODSHOP", "SAFEHOUSE",
          "FOOD", "GYM"}
GANG = {"NONE": 0, "GSF": 1, "BALLAS": 2, "VAGOS": 3, "AZTECAS": 4}


def enex_kind(name):
    n = name.upper()
    if n.startswith("AMMUN"):
        return "AMMU"
    if n.startswith("POLICE"):
        return "POLICE"
    if n.startswith("SV") or n in ("CARLS",):
        return "SAFEHOUSE"
    if n.startswith(("FDPIZA", "FDBURG", "FDCHICK", "FDREST", "DINER", "REST")):
        return "FOOD"
    if n.startswith("GYM"):
        return "GYM"
    if n.startswith(("BARBER", "TATTOO", "CLOTH", "CS", "LACS", "CHANGER")):
        return "CLOTHES"  # CHANGER = the wardrobe family (documented)
    if n.startswith("BAR"):
        return "BAR"
    if n.startswith(("LASTRIP", "STRIP")):
        return "STRIP"
    if n.startswith("MOTEL"):
        return "MOTEL"
    if n.startswith(("X7", "X711")):
        return "SHOP247"
    return "GENERIC"


# Garage kinds, matched against the game's own grge names (the canonical
# garage list is documented in GTAMods' Garage page; the records themselves
# are the ones extracted from the user's files). timy1 is a documented
# UNUSED pay'n'spray and stays out on purpose.
GRGE_SPRAY = {"sprlae", "splaw2", "sprsfse", "sprsfw", "cespray", "cnspray",
              "cn2spry", "blob1"}
GRGE_MOD = {"bodlawn", "mdssfse", "mds1sfs", "modlast", "vecmod", "modglas"}
GRGE_IMPOUND = {"imp_la", "imp_sf", "imp_lv"}


def grge_kind(name):
    n = name.lower()
    if n in GRGE_SPRAY or "spr" in n or "spray" in n:
        return "PAYNSPRAY"
    if n in GRGE_MOD or "mod" in n:
        return "MODSHOP"
    if n in GRGE_IMPOUND:
        return "POLICE"
    if n == "lasbomb":
        return "GENERIC"  # the bomb shop: a real place, no better kind
    if "safe" in n or "sav" in n or n.startswith("sv") or n.endswith("sv") \
            or n == "hbgdsfs":
        return "SAFEHOUSE"
    return None


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def main():
    raw = json.load(open(os.path.join(SRC, "atlas_raw.json")))
    bnry = json.load(open(os.path.join(SRC, "bnry_inst.json")))
    text = json.load(open(os.path.join(SRC, "text_inst.json")))

    # ---- ped node positions for the reachability sanity filter
    nav = open(os.path.join(REPO, "cs-soldier", "nav_graph.bin"), "rb").read()
    ncount = struct.unpack_from("<I", nav, 6)[0]
    ped = []
    off = 14
    for i in range(ncount):
        ped.append(struct.unpack_from("<3f", nav, off + i * 12))
    def ped_near(x, y, r):
        r2 = r * r
        for (px, py, _pz) in ped:
            if (px - x) ** 2 + (py - y) ** 2 < r2:
                return True
        return False

    pois = []  # {kind, name, x,y,z, door:[x,y,z]|None, src, tag}

    # ---- 1) enex street doors (entry z < 500): the authoritative doors
    def parse_enex(e):
        f = e["f"]
        return {
            "entry": (float(f[0]), float(f[1]), float(f[2])),
            "exit": (float(f[7]), float(f[8]), float(f[9])),
            "interior": int(f[11]), "name": f[13].strip('"'),
            "flags": int(f[6]), "src": e["src"],
        }
    enex = [parse_enex(e) for e in raw["enex"]]
    street = [e for e in enex if e["entry"][2] < 500]
    heaven = [e for e in enex if e["entry"][2] >= 500]
    for e in street:
        k = enex_kind(e["name"])
        pois.append({"kind": k, "name": "%s door (%s)" % (k.title(), e["name"]),
                     "x": e["entry"][0], "y": e["entry"][1], "z": e["entry"][2],
                     "door": list(e["entry"]), "src": "enex:" + e["name"],
                     "tag": "FILE"})

    # ---- 2) garages (grge): spray shops, safehouse garages, mod shops
    for g in raw["grge"]:
        f = g["f"]
        name = f[10]
        k = grge_kind(name)
        if not k:
            continue
        cx = (float(f[0]) + float(f[3])) / 2.0
        cy = (float(f[1]) + float(f[4])) / 2.0
        cz = float(f[2])
        pois.append({"kind": k, "name": "%s garage (%s)" % (k.title(), name),
                     "x": cx, "y": cy, "z": cz, "door": [cx, cy, cz],
                     "src": "grge:" + name, "tag": "FILE"})

    # ---- 3) inst anchors (buildings), deduped against doors
    insts = [i for i in bnry["poi_insts"] + text["poi_insts"]
             if abs(i["z"]) <= 200 and "copse" not in i["name"]]
    def add_anchor(kind, name, x, y, z, src):
        for p in pois:
            if p["kind"] == kind and dist((p["x"], p["y"], p["z"]), (x, y, z)) < 20:
                return False
        pois.append({"kind": kind, "name": name, "x": x, "y": y, "z": z,
                     "door": None, "src": src, "tag": "FILE"})
        return True
    clusters = {}
    for i in insts:
        nm = i["name"]
        low = nm.lower()
        x, y, z = i["x"], i["y"], i["z"]
        if "hospital" in low or "hospground" in low:
            add_anchor("HOSPITAL", "Hospital (%s)" % nm, x, y, z, "inst:" + nm)
        elif "policest" in low or "copshop" in low or "police" in low:
            add_anchor("POLICE", "Police station (%s)" % nm, x, y, z, "inst:" + nm)
        elif "ammun" in low:
            add_anchor("AMMU", "Ammu-Nation (%s)" % nm, x, y, z, "inst:" + nm)
        elif "spray" in low or "payspray" in low:
            add_anchor("PAYNSPRAY", "Pay'n'Spray (%s)" % nm, x, y, z, "inst:" + nm)
        elif "modshop" in low:
            add_anchor("MODSHOP", "Mod shop (%s)" % nm, x, y, z, "inst:" + nm)
        elif "gym" in low and ("blok" in low or "scumgym" in low or "gym1" in low):
            add_anchor("GYM", "Gym (%s)" % nm, x, y, z, "inst:" + nm)
        elif "drivingschool" in low:
            add_anchor("SCHOOL", "Driving school (%s)" % nm, x, y, z, "inst:" + nm)
        elif "casino" in low:
            clusters.setdefault("CASINO", []).append((x, y, z))
        elif "airport" in low:
            clusters.setdefault("AIRPORT", []).append((x, y, z))
        elif "stadium" in low:
            clusters.setdefault("STADIUM", []).append((x, y, z))
        elif low.startswith("a51") or "mil_hangar" in low:
            clusters.setdefault("MILITARY69", []).append((x, y, z))
        elif "carrier" in low:
            clusters.setdefault("CARRIER", []).append((x, y, z))
    def cluster_pois(key, kind, label, maxlink=180.0):
        pts = clusters.get(key, [])
        pts = [p for p in pts if ped_near(p[0], p[1], 120)]
        groups = []
        for p in pts:
            for g in groups:
                if any(dist(p, q) < maxlink for q in g):
                    g.append(p)
                    break
            else:
                groups.append([p])
        for g in groups:
            cx = sum(p[0] for p in g) / len(g)
            cy = sum(p[1] for p in g) / len(g)
            cz = max(p[2] for p in g)
            add_anchor(kind, label, cx, cy, cz, "inst-cluster:" + key)
    cluster_pois("CASINO", "CASINO", "Casino (The Strip)")
    cluster_pois("AIRPORT", "AIRPORT", "Airport")
    cluster_pois("STADIUM", "STADIUM", "Stadium")
    cluster_pois("MILITARY69", "MILITARY", "Area 69 (restricted)")
    cluster_pois("CARRIER", "MILITARY", "Aircraft carrier (restricted)")

    # ---- 4) documented supplements (flagged DOC, zone-cross-checked)
    zones = []
    for z in raw["zones"]:
        f = z["f"]
        zones.append({"name": f[0], "type": int(f[1]),
                      "min": [float(f[2]), float(f[3]), float(f[4])],
                      "max": [float(f[5]), float(f[6]), float(f[7])]})
    def zone_names_at(x, y):
        return [z["name"] for z in zones
                if z["min"][0] <= x <= z["max"][0] and z["min"][1] <= y <= z["max"][1]]
    DOC_POIS = [
        # SF hospital (Santa Flora): documented landmark; lands in CIVI.
        ("HOSPITAL", "Santa Flora Hospital (SF)", -2658.0, 631.0, 65.0, "CIVI"),
        # Rural respawn clinics (documented), town zones must confirm:
        ("HOSPITAL", "Angel Pine clinic", -2163.0, -2427.0, 36.0, "ANGPI"),
        ("HOSPITAL", "El Quebrados clinic", -1517.0, 2528.0, 46.0, "ELQUE"),
        # Crippen Memorial, Montgomery (documented respawn town, zone MONT).
        ("HOSPITAL", "Crippen Memorial clinic (Montgomery)", 1250.0, 300.0, 25.0, "MONT"),
        # LS airport from its own zone boxes (LAIR1/LAIR2a/LAIR2b union).
        ("AIRPORT", "Los Santos International (zone LAIR)", 1670.0, -2290.0, 15.0, "LAIR1"),
    ]
    doc_notes = []
    for kind, name, x, y, z, want_zone in DOC_POIS:
        zn = zone_names_at(x, y)
        ok = want_zone in zn and ped_near(x, y, 120)
        doc_notes.append({"name": name, "zones": zn, "accepted": ok})
        if ok:
            pois.append({"kind": kind, "name": name, "x": x, "y": y, "z": z,
                         "door": None, "src": "documented", "tag": "DOC"})

    pois.sort(key=lambda p: (KIND[p["kind"]], p["name"], p["x"]))
    for p in pois:
        p["refuge"] = p["kind"] in REFUGE

    # ---- zones + documented gang ownership (initial state)
    # The documented STARTING turf map (GTA Wiki, Gangs in GTA San Andreas):
    # GSF hold Ganton, Playa del Seville and Santa Maria Beach (Temple's SUN
    # zones are deliberately NOT mapped - the SUN->Temple link is only a
    # police-radio quirk in the docs). Ballas hold Jefferson, Idlewood,
    # Verona Beach, Glen Park (contested) and south East LS. Vagos hold
    # north/east East LS and Los Flores. Aztecas (not a war gang in vanilla)
    # hold El Corona, Little Mexico and Unity. In the game this map is
    # painted by main.scm opcodes into the ZonePop table - zone boxes are
    # file data, ownership is the documented initial state only.
    GSF = {"GAN1", "GAN2", "PLS", "SMB1", "SMB2"}
    BAL = {"JEF1a", "JEF1b", "JEF2", "JEF3a", "JEF3b", "JEF3c",
           "VERO1", "VERO2", "VERO3", "VERO4a", "VERO4b",
           "IWD1", "IWD2", "IWD3a", "IWD3b", "IWD4", "IWD5",
           "ELS1a", "ELS1b", "GLN1", "GLN1b", "GLN2a"}
    VAG = {"ELS2", "ELS3a", "ELS3b", "ELS3c", "ELS4", "LFL1a", "LFL1b"}
    AZT = {"ELCO1", "ELCO2", "LMEX1a", "LMEX1b", "UNITY"}
    for z in zones:
        z["gang"] = ("GSF" if z["name"] in GSF else
                     "BALLAS" if z["name"] in BAL else
                     "VAGOS" if z["name"] in VAG else
                     "AZTECAS" if z["name"] in AZT else "NONE")

    # ---- interiors: door families
    fams = {}
    for e in street:
        fam = fams.setdefault(e["name"], {"name": e["name"],
                                          "kind": enex_kind(e["name"]),
                                          "doors": [], "heavens": []})
        fam["doors"].append({"entry": list(e["entry"]), "exit": list(e["exit"]),
                             "src": e["src"]})
    for e in heaven:
        fam = fams.setdefault(e["name"], {"name": e["name"],
                                          "kind": enex_kind(e["name"]),
                                          "doors": [], "heavens": []})
        fam["heavens"].append({"entry": list(e["entry"]), "exit": list(e["exit"]),
                               "interior": e["interior"], "src": e["src"]})
    interiors = [fams[k] for k in sorted(fams)]

    with open(os.path.join(HERE, "poi_data.json"), "w") as fh:
        json.dump(pois, fh, indent=1)
    with open(os.path.join(HERE, "zones_data.json"), "w") as fh:
        json.dump(zones, fh, indent=1)
    with open(os.path.join(HERE, "interiors_data.json"), "w") as fh:
        json.dump(interiors, fh, indent=1)

    # ---- the C++ static tables
    def esc(s):
        return s.replace("\\", "\\\\").replace('"', '\\"')
    lines = []
    lines.append("// GENERATED by atlas-assets/build_atlas_data.py from the")
    lines.append("// game's own files (see GTA-SA-ATLAS-fa.md). Do not edit by hand.")
    lines.append("// Kind codes: 0 generic 1 hospital 2 police 3 ammu 4 paynspray")
    lines.append("// 5 modshop 6 safehouse 7 food 8 gym 9 clothes 10 bar 11 strip")
    lines.append("// 12 motel 13 shop247 14 casino 15 airport 16 stadium 17 military")
    lines.append("// 18 school. Gang codes: 0 none 1 GSF 2 Ballas 3 Vagos 4 Aztecas.")
    lines.append("struct AtlasPoiRow { const char* name; unsigned char kind; unsigned char refuge; Vec3 at; Vec3 door; };")
    lines.append("static const AtlasPoiRow kAtlasPois[] = {")
    for p in pois:
        d = p["door"] if p["door"] else [p["x"], p["y"], p["z"]]
        lines.append('    { "%s", %d, %d, Vec3(%g, %g, %g), Vec3(%g, %g, %g) },'
                     % (esc(p["name"]), KIND[p["kind"]], 1 if p["refuge"] else 0,
                        p["x"], p["y"], p["z"], d[0], d[1], d[2]))
    lines.append("};")
    lines.append("struct AtlasZoneRow { const char* name; unsigned char gang; Vec3 mn; Vec3 mx; };")
    lines.append("static const AtlasZoneRow kAtlasZones[] = {")
    for z in zones:
        lines.append('    { "%s", %d, Vec3(%g, %g, %g), Vec3(%g, %g, %g) },'
                     % (esc(z["name"]), GANG[z["gang"]],
                        z["min"][0], z["min"][1], z["min"][2],
                        z["max"][0], z["max"][1], z["max"][2]))
    lines.append("};")
    lines.append("// Street doors of enterable interiors, grouped by enex family")
    lines.append("// name; kAtlasDoorFam[i] = family index into kAtlasFamNames /")
    lines.append("// kAtlasFamInterior (the interior id its heaven record carries).")
    lines.append("struct AtlasDoorRow { const char* fam; Vec3 entry; Vec3 exit; };")
    lines.append("static const AtlasDoorRow kAtlasDoors[] = {")
    seen_doors = set()
    for fam in interiors:
        for dr in fam["doors"]:
            key = (fam["name"], round(dr["entry"][0]), round(dr["entry"][1]))
            if key in seen_doors:
                continue  # duplicate enex lines exist across IPL files
            seen_doors.add(key)
            lines.append('    { "%s", Vec3(%g, %g, %g), Vec3(%g, %g, %g) },'
                         % (esc(fam["name"]), dr["entry"][0], dr["entry"][1],
                            dr["entry"][2], dr["exit"][0], dr["exit"][1], dr["exit"][2]))
    lines.append("};")
    lines.append("struct AtlasHeavenRow { const char* fam; int interior; Vec3 entry; };")
    lines.append("static const AtlasHeavenRow kAtlasHeavens[] = {")
    for fam in interiors:
        for hv in fam["heavens"]:
            lines.append('    { "%s", %d, Vec3(%g, %g, %g) },'
                         % (esc(fam["name"]), hv["interior"],
                            hv["entry"][0], hv["entry"][1], hv["entry"][2]))
    lines.append("};")
    with open(os.path.join(REPO, "src", "core", "AtlasData.inc"), "w") as fh:
        fh.write("\n".join(lines) + "\n")

    from collections import Counter
    summary = {
        "pois_total": len(pois),
        "pois_by_kind": dict(Counter(p["kind"] for p in pois)),
        "pois_file": sum(1 for p in pois if p["tag"] == "FILE"),
        "pois_doc": sum(1 for p in pois if p["tag"] == "DOC"),
        "doc_checks": doc_notes,
        "zones_total": len(zones),
        "gang_zones": dict(Counter(z["gang"] for z in zones if z["gang"] != "NONE")),
        "door_families": len(interiors),
        "street_doors": sum(len(f["doors"]) for f in interiors),
        "heaven_records": sum(len(f["heavens"]) for f in interiors),
        "spot_checks": {},
    }
    # spot checks: five file-proven places vs the extracted records
    def nearest_poi(x, y, kind=None):
        best, bd = None, 1e18
        for p in pois:
            if kind and p["kind"] != kind:
                continue
            dd = (p["x"] - x) ** 2 + (p["y"] - y) ** 2
            if dd < bd:
                bd, best = dd, p
        return best, math.sqrt(bd)
    for label, (x, y, k) in {
        "County General Hospital": (2050, -1401, "HOSPITAL"),
        "All Saints Hospital": (1134, -1338, "HOSPITAL"),
        "LSPD (enex POLICE1)": (1554.95, -1674.99, "POLICE"),
        "Johnson safehouse door (enex CARLS)": (2495.33, -1690.75, "SAFEHOUSE"),
        "Market Ammu-Nation door (enex AMMUN1)": (1368.35, -1279.06, "AMMU"),
    }.items():
        p, dd = nearest_poi(x, y, k)
        summary["spot_checks"][label] = {"poi": p["name"] if p else None,
                                         "dist_m": round(dd, 2),
                                         "src": p["src"] if p else None}
    with open(os.path.join(HERE, "atlas_summary.json"), "w") as fh:
        json.dump(summary, fh, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
