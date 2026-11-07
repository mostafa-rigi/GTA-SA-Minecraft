#!/usr/bin/env python3
"""Verify cop_car.bin against the convicted geometry (fix round 3).

The bin is data; this script pins what the eye test measures: the
body's metres, the wheelbase, the wheels tucked inside the body's
slab and reaching the ground line. Run after convert_driver2_cop.py;
exits non-zero on any miss so a stale or wrong-scale bake cannot
slip through silently the way the 0.6x one did (its preview auto-fit
hid the size - this check has no picture to hide behind).

Checks, all in metres at the convicted 153.6 units = 1 m:
  - body bbox: ~1.72 wide x ~4.91 long x ~1.15 high (a real sedan,
    the host 596's class) - at the old 256 factor these read
    1.03 x 2.95 x 0.69 and every bound below fails loudly
  - wheelbase from the measure file's wheel centres: 2.79 +- 0.1
  - wheel outer faces inside the body slab: |centre x| + the tyre's
    half-width (17 units) <= body half width
  - contact patch: centre z - ring radius (69 units) = -0.866 +- 0.03
  - wheel diameter printed (0.898 - the source's stylised size, named
    in DRIVER2-STEP3-fa.md, not silently "fixed")
  - the repo's runtime copy (driver2/cop_car.bin) is byte-identical
"""
import hashlib
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, "..", "..", "driver2-assets"))
RUNTIME = os.path.normpath(os.path.join(HERE, "..", "driver2", "cop_car.bin"))
UPM = 153.6

failures = []


def check(ok, label, detail):
    print(("PASS " if ok else "FAIL ") + label + ": " + detail)
    if not ok:
        failures.append(label)


def main():
    bin_path = os.path.join(ASSETS, "cop_car.bin")
    data = open(bin_path, "rb").read()
    if data[:4] != b"D2C1":
        print("FAIL: not a D2C1 bin")
        return 1
    p = 12
    meshes = []
    for _ in range(4):
        nv, nc, _nt = struct.unpack_from("<III", data, p)
        p += 12
        verts = [struct.unpack_from("<3f", data, p + i * 12) for i in range(nv)]
        p += nv * 12 + nc * 15
        meshes.append(verts)
    body = meshes[0]
    xs = [v[0] for v in body]
    ys = [v[1] for v in body]
    zs = [v[2] for v in body]
    w, l, h = max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)
    check(1.65 < w < 1.80, "body width", "%.3f m (real sedan ~1.7-1.8)" % w)
    check(4.80 < l < 5.00, "body length", "%.3f m (real sedan ~4.9)" % l)
    check(1.05 < h < 1.25, "body height", "%.3f m (his low roofline)" % h)

    meas = json.load(open(os.path.join(ASSETS, "cop_car_measure.json")))
    wd = meas["wheelDisp_core_m"]
    wheelbase = wd[0][1] - wd[1][1]
    check(2.70 < wheelbase < 2.90, "wheelbase", "%.3f m (real sedan ~2.8)" % wheelbase)
    half_w = w / 2.0
    outer = max(abs(c[0]) for c in wd) + 17.0 / UPM
    check(outer <= half_w + 0.01, "wheels tucked in the slab",
          "outer face %.3f m vs body half width %.3f m" % (outer, half_w))
    contact = wd[0][2] - 69.0 / UPM
    check(-0.89 < contact < -0.84, "contact patch",
          "%.3f m under his origin" % contact)
    print("INFO wheel diameter: %.3f m (source-stylised; a real sedan is ~0.65-0.72)"
          % (2 * 69.0 / UPM))
    print("INFO max dent: %.3f m" % meas["dent_target"]["max_dent_m"])

    if os.path.exists(RUNTIME):
        same = hashlib.sha256(data).hexdigest() == \
            hashlib.sha256(open(RUNTIME, "rb").read()).hexdigest()
        check(same, "runtime copy identical", RUNTIME)
    else:
        check(False, "runtime copy exists", RUNTIME)

    print("VERIFY " + ("FAILED" if failures else "PASSED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
