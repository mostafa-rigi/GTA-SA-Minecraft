import json
import os
import struct

ROOT = r"C:\GTA.San.Andreas"
IMG = os.path.join(ROOT, "models", "gta3.img")
OUT = r"C:\Win7Assistant-up\atlas\cars_check.json"


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


with open(IMG, "rb") as fh:
    img = fh.read()
assert img[:4] == b"VER2"
count = u32(img, 4)
entries = []
off = 8
for _ in range(count):
    o, s, name = struct.unpack_from("<II24s", img, off)
    off += 32
    entries.append((name.split(b"\0")[0].decode("ascii", "replace"), o * 2048, s * 2048))

files = []
total_cars = 0
ids_in_range = 0
ids_total = 0
id_min, id_max = 10 ** 9, -1
for name, o, s in entries:
    if not name.endswith(".ipl") or s < 76:
        continue
    d = img[o:o + s]
    if d[:4] != b"bnry":
        continue
    inst = u32(d, 4)
    cars = u32(d, 20)
    if cars == 0:
        continue
    cars_off = u32(d, 60)
    ok_layout = cars_off == 76 + inst * 40
    samples = []
    for i in range(cars):
        r = cars_off + i * 48
        x, y, z, ang = struct.unpack_from("<4f", d, r)
        veh, c1, c2, force, alarm, lock, u1, u2 = struct.unpack_from("<8i", d, r + 16)
        ids_total += 1
        if 400 <= veh <= 611:
            ids_in_range += 1
        id_min = min(id_min, veh)
        id_max = max(id_max, veh)
        if i < 3:
            samples.append({"pos": [round(x, 1), round(y, 1), round(z, 1)],
                            "veh": veh, "cols": [c1, c2], "force": force})
    total_cars += cars
    if len(files) < 6:
        files.append({"file": name, "inst": inst, "cars": cars,
                      "cars_off": cars_off, "layout_ok": ok_layout,
                      "samples": samples})
json.dump({"total_cars": total_cars, "ids_total": ids_total,
           "ids_in_range_400_611": ids_in_range,
           "id_min": id_min, "id_max": id_max, "files": files},
          open(OUT, "w"), indent=1)
print("done", total_cars)
