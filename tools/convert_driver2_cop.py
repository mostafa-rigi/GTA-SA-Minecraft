#!/usr/bin/env python3
"""Offline converter: Driver 2's own Chicago cop car -> the core's runtime
mesh file cop_car.bin (format "D2C1") plus its texture PNGs.

Sources (the user's own disc image, extracted PC-side; the slices are
byte-exact regions of CHICAGO.LEV / VOICES2.BLK - see slice_on_pc.py and
lev_info.json for the map):
  slices/lump28_carmodels_r0.bin  LUMP_CAR_MODELS (28) of DATA1
  slices/lump34_textureinfo_r0.bin LUMP_TEXTUREINFO (34) of DATA1
  slices/lump01_models_r2.bin     LUMP_MODELS (1, the MDS pack) of DATA2
  slices/lump12_modelnames_r0.bin LUMP_MODELNAMES (12) of DATA1
  slices/tpage.bin                the TPAGE region (after DATA1)
  chicago-lcf.json                the decoded CHICAGO.LCF (wheel data)

Format spec = the REDRIVER 2 rebuild's own loaders, followed exactly:
  MODELS.C ProcessCarModelLump / GetCarModel (car model lump layout),
  CARS.C buildNewCarFromModel (poly record interpretation, pass order),
  DRAW.C PolySizes (record sizes), TEXTURE.C ProcessTextureInfo /
  LoadPermanentTPages / LoadTPageAndCluts / unpackTexture (pages, CLUTs,
  the backward RLE), MAIN.C InitModelNames + MAP.C FindModelPtrWithName
  (wheel models by name), CARS.C DrawCarWheels (the wheel vertex rewrite
  and placement law), COSMETIC.C/HANDLING.C (the LCF row, FixCarCos).

Axis mapping PS1 -> core (a proper rotation, determinant +1, so texture
lettering is never mirrored): the PS1 car frame is x = side, y = DOWN
(the light-bar vertex cluster sits at y = -88, the frame's top), z =
toward the rear (headlights at z = -354 in the LCF). The core frame is
x = right, y = forward, z = up, so:
  core = (-x, -z, -y) / UNITS_PER_METRE

UNITS_PER_METRE = 153.6 - convicted in fix round 3 (2026-11-07), NOT
the 256 the brain's speed rescale once suggested. The original draws
model vertices 1:1 in world units (CARS.C buildNewCarFromModel keeps
model->vertices verbatim; DrawCarObject/RenderModel apply rotation and
translation only - no scale anywhere in the car path), so one factor
must serve the whole world, and the geometry anchors convict ~154:
  - his own length: 754 units (bbox) / 764 (LCF colBox) -> 4.9 m at 153.9
  - his own wheelbase: 429 units (post-FixCarCos wheelDisp) -> 2.8 m at 153.2
  - CONE1_TASTIC in the same MDS pack: 123 units tall -> 0.80 m at 153.6
    (a standard 31-inch road cone)
  - ROAD1 tile in the same pack: 1034 units wide -> 6.7 m at 153.6
    (two 3.35 m lanes; at 256 it would be a 4.0 m one-lane "road")
At 256 the car came out 2.95 m long next to San Andreas cars - the eye
test's "much smaller than the other cars". His proportions are stylised
(the 69-unit wheel ring is a 0.90 m tyre at this factor, big for a
sedan; the roofline is low) - no single factor makes every part real,
so the body's own two hard dimensions rule. The preview renders in
this frame at ABSOLUTE scale against a 4.9 m reference outline and is
the gate: lettering readable, light bar on top, nose forward, SIZE
true.

Outputs (driver2-assets/):
  cop_car.bin            the runtime mesh (body clean + dam, wheel
                         clean + fast - the original's own variants)
  cop_tex<i>.png         one PNG per (page, CLUT) pair the meshes use
  cop_car_measure.json   measurements + provenance for every number
  cop_preview_*.png      offline renders (the preview-before-install gate)
"""
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(os.path.dirname(HERE), "..", "driver2-assets")
ASSETS = os.path.normpath(ASSETS)
SLICES = os.path.join(ASSETS, "slices")

POLY_SIZES = [8, 12, 16, 24, 20, 20, 28, 32, 8, 12, 16, 16, 0, 0, 0, 0,
              12, 12, 12, 16, 20, 20, 24, 24]

# Driver 2 world units per metre for GEOMETRY - convicted in fix round
# 3 from the car's own length (754 u -> 4.9 m) and wheelbase
# (429 u -> 2.8 m), cross-checked on the MDS pack's cone (123 u ->
# 0.80 m) and road tile (1034 u -> 6.7 m). See the module docstring.
UNITS_PER_METRE = 153.6


def load(name):
    with open(os.path.join(SLICES, name), "rb") as f:
        return f.read()


# --------------------------------------------------------------------------
# textures: TEXTUREINFO + TPAGE region (TEXTURE.C, followed exactly)
# --------------------------------------------------------------------------
def parse_texture_info(ti):
    tpage_amount = struct.unpack_from("<i", ti, 0)[0]
    ptr = 8 + (tpage_amount + 1) * 8
    pages_texinf = []
    for _ in range(tpage_amount):
        cnt = struct.unpack_from("<i", ti, ptr)[0]
        ptr += 4
        pages_texinf.append(cnt)
        ptr += cnt * 8
    nperms = struct.unpack_from("<i", ti, ptr)[0]
    ptr += 4
    perms = [struct.unpack_from("<ii", ti, ptr + i * 8) for i in range(nperms)]
    return perms


def unpack_texture(src):
    """TEXTURE.C unpackTexture, byte for byte: fills 32768 bytes BACKWARD
    from dest+0x7fff - a signed byte with the top bit set repeats the next
    byte (2 - b) times, a byte b >= 0 copies b+1 literal bytes."""
    dest = bytearray(32768)
    ptr = 32767
    i = 0
    while ptr >= 0:
        b = src[i]
        i += 1
        sb = b - 256 if b >= 128 else b
        if b & 0x80:
            p = src[i]
            i += 1
            for _ in range(2 - sb):
                dest[ptr] = p
                ptr -= 1
        else:
            for _ in range(sb + 1):
                dest[ptr] = src[i]
                i += 1
                ptr -= 1
    return dest


class PageSet:
    """The permanent pages, in permlist order in the TPAGE region, each
    block advancing by align2048(size) (LoadPermanentTPages)."""

    def __init__(self, tpage_region, perms):
        self.pages = {}  # page id -> (cluts [16 x u16], pixel indices 256x256)
        off = 0
        for pid, size in perms:
            block = tpage_region[off:off + size]
            npal = struct.unpack_from("<i", block, 0)[0]
            cluts = [struct.unpack_from("<16H", block, 4 + k * 32) for k in range(npal)]
            pixels = unpack_texture(block[4 + npal * 32:])
            self.pages[pid] = (cluts, pixels)
            off += (size + 2047) & ~2047

    def render(self, pid, clut_idx):
        """A page through one CLUT as RGBA. CLUT value 0x0000 is the PSX
        transparent black; index 0 of the palette is alpha 0 only when its
        colour word is exactly 0 (the standard PSX rule)."""
        cluts, pixels = self.pages[pid]
        clut = cluts[clut_idx]
        rgba = np.zeros((256, 256, 4), dtype=np.uint8)
        idx = np.zeros((256, 256), dtype=np.uint8)
        raw = np.frombuffer(bytes(pixels), dtype=np.uint8).reshape(256, 128)
        idx[:, 0::2] = raw & 0xF
        idx[:, 1::2] = raw >> 4
        for c in range(16):
            w = clut[c]
            r = ((w & 31) << 3) | ((w & 31) >> 2)
            g = (((w >> 5) & 31) << 3) | (((w >> 5) & 31) >> 2)
            b = (((w >> 10) & 31) << 3) | (((w >> 10) & 31) >> 2)
            a = 0 if (c == 0 and w == 0) else 255
            rgba[idx == c] = (r, g, b, a)
        return rgba


# --------------------------------------------------------------------------
# models: MODEL headers + poly streams (MODELS.C / CARS.C / DRAW.C)
# --------------------------------------------------------------------------
def parse_model(buf, base, with_polys=True):
    (shape, flags2, inst, tri_verts, zbias, bsphere, n_pn, n_v, n_p,
     v_ofs, p_ofs, n_ofs, pn_ofs, c_ofs) = struct.unpack_from("<HHhBBhHHHiiiii", buf, base)
    verts = [struct.unpack_from("<hhhh", buf, base + v_ofs + i * 8)[:3] for i in range(n_v)]
    polys = []
    if with_polys:
        p = base + p_ofs
        for i in range(n_p):
            t = buf[p] & 0x1F
            if t >= len(POLY_SIZES) or POLY_SIZES[t] == 0:
                raise ValueError(f"poly {i}: type {t} has no size - stream desync")
            rec = buf[p:p + POLY_SIZES[t]]
            polys.append((t, rec))
            p += POLY_SIZES[t]
    return {"shape_flags": shape, "instance": inst, "bounding_sphere": bsphere,
            "verts": verts, "polys": polys, "num_point_normals": n_pn,
            "num_polys_field": n_p}


def poly_tris(t, rec):
    """One poly record -> (vertex indices, uv per corner or None, rgb,
    texture (set, id) or None), split exactly as buildNewCarFromModel.
    UVs are page-absolute pixel coordinates."""
    if t in (0, 18):  # F3
        v = (rec[1], rec[2], rec[3])
        return [(v, None, (rec[4], rec[5], rec[6]), None)]
    if t in (1, 19):  # F4: indices at bytes 4..7, colour at 8..10
        v = (rec[4], rec[5], rec[6], rec[7])
        rgb = (rec[8], rec[9], rec[10])
        return [((v[0], v[1], v[2]), None, rgb, None),
                ((v[0], v[2], v[3]), None, rgb, None)]
    if t == 20:  # FT3
        v = (rec[4], rec[5], rec[6])
        uv = ((rec[8], rec[9]), (rec[10], rec[11]), (rec[12], rec[13]))
        return [(v, uv, (rec[16], rec[17], rec[18]), (rec[1], rec[2]))]
    if t == 21:  # FT4
        v = (rec[4], rec[5], rec[6], rec[7])
        uv = ((rec[8], rec[9]), (rec[10], rec[11]), (rec[12], rec[13]), (rec[14], rec[15]))
        rgb = (rec[16], rec[17], rec[18])
        tex = (rec[1], rec[2])
        return [((v[0], v[1], v[2]), (uv[0], uv[1], uv[2]), rgb, tex),
                ((v[0], v[2], v[3]), (uv[0], uv[2], uv[3]), rgb, tex)]
    if t == 22:  # GT3: + normal indices at 8..10, uv at 12.., rgb at 20..
        v = (rec[4], rec[5], rec[6])
        uv = ((rec[12], rec[13]), (rec[14], rec[15]), (rec[16], rec[17]))
        return [(v, uv, (rec[20], rec[21], rec[22]), (rec[1], rec[2]))]
    if t == 23:  # GT4
        v = (rec[4], rec[5], rec[6], rec[7])
        uv = ((rec[12], rec[13]), (rec[14], rec[15]), (rec[16], rec[17]), (rec[18], rec[19]))
        rgb = (rec[20], rec[21], rec[22])
        tex = (rec[1], rec[2])
        return [((v[0], v[1], v[2]), (uv[0], uv[1], uv[2]), rgb, tex),
                ((v[0], v[2], v[3]), (uv[0], uv[2], uv[3]), rgb, tex)]
    raise ValueError(f"unhandled poly type {t}")


def ps1_to_core(v):
    x, y, z = v
    return (-x / UNITS_PER_METRE, -z / UNITS_PER_METRE, -y / UNITS_PER_METRE)


class MeshBuilder:
    def __init__(self):
        self.tex_index = {}   # (page, clut) -> runtime texture number
        self.tex_list = []

    def tex(self, pair):
        if pair not in self.tex_index:
            self.tex_index[pair] = len(self.tex_list)
            self.tex_list.append(pair)
        return self.tex_index[pair]

    def build(self, model, spin_verts=frozenset()):
        verts = [ps1_to_core(v) for v in model["verts"]]
        corners = []  # (vert, u, v, tex, r, g, b, spin)
        # pass order of buildNewCarFromModel: GT (0), FT (1), flat (2)
        order = sorted(range(len(model["polys"])),
                       key=lambda i: 0 if model["polys"][i][0] in (22, 23) else
                       (1 if model["polys"][i][0] in (20, 21) else 2))
        for i in order:
            t, rec = model["polys"][i]
            for (vidx, uv, rgb, tex) in poly_tris(t, rec):
                for k in range(3):
                    vi = vidx[k]
                    if uv is not None:
                        u, v = uv[k][0] / 256.0, uv[k][1] / 256.0
                        ti = self.tex(tex)
                    else:
                        # flat-colour poly (F3/F4): his records carry no
                        # texture, only RGB. They ride a solid white card
                        # (one extra runtime texture) so the sink's
                        # texture x colour law yields exactly his RGB.
                        u, v, ti = 0.5, 0.5, self.tex((-1, -1))
                    corners.append((vi, u, v, ti, rgb[0], rgb[1], rgb[2],
                                    1 if vi in spin_verts else 0))
        return verts, corners


# --------------------------------------------------------------------------
# the wheel vertex rewrite (CARS.C DrawCarWheels, at spin angle 0)
# --------------------------------------------------------------------------
def bake_wheel_spin0(verts, wheel_size):
    """Verts 8..23 of a wheel model are placeholders the game rewrites
    every frame from wheelSize and the spin angle; this bakes angle 0
    (rcossin table: sin 0 = 0, cos 0 = 4096 in 12-bit fixed point, so
    FW1z = 0 and FW2z = sizeScale). The runtime spins verts 8..23 around
    the wheel's x axis from these baked positions."""
    v = [list(x) for x in verts]
    size_scale = (wheel_size * 14142) // 10000
    f1, f2 = 0, size_scale
    v[8][2], v[15][2], v[11][1], v[12][1] = f1, f1, f1, f1
    v[9][1], v[14][1], v[10][2], v[13][2] = -f1, -f1, -f1, -f1
    v[8][1], v[15][1], v[9][2], v[14][2] = f2, f2, f2, f2
    v[10][1], v[13][1], v[11][2], v[12][2] = -f2, -f2, -f2, -f2
    for i in (23, 22):
        v[i][2] = 0
    for i in (21, 20, 19, 18):
        v[i][1] = 0
    for i in (17, 16):
        v[i][2] = 0
    v[23][1] = v[22][1] = wheel_size
    v[21][2] = v[20][2] = wheel_size
    v[19][2] = v[18][2] = -wheel_size
    v[17][1] = v[16][1] = -wheel_size
    return [tuple(x) for x in v]


def render_preview(meshes, pages, path, view):
    """A tiny software rasteriser: textured triangles, orthographic.
    view: (right axis, up axis, depth axis) as core-frame unit vectors
    with axis indices + signs, plus a label. This is the gate the
    soldier passed: the body is SEEN before anything is installed.

    ABSOLUTE scale (fix round 3): the frame is a fixed 6.6 m window for
    every view - the old version auto-fitted the geometry to the frame,
    which made a 2.95 m car and a 4.9 m car render identically and hid
    the scale defect through two installs. A red reference outline of
    a real sedan (4.9 m long, 1.72 m wide, roof 1.44 m over the contact
    patch) is drawn behind the car, and a white ground line marks the
    contact patch (core z = -0.866 m at the convicted factor), so size
    and ride height are judged, never fitted."""
    W = H = 512
    img = np.zeros((H, W, 3), dtype=np.uint8)
    img[:] = (24, 24, 28)
    zbuf = np.full((H, W), -1e18)
    (ai, asg), (bi, bsg), (ci, csg) = view
    allp = []
    for (verts, corners, tex_rgba) in meshes:
        for i in range(0, len(corners), 3):
            pts = []
            for k in range(3):
                vi, u, v, ti, r, g, b, _ = corners[i + k]
                p = verts[vi]
                pts.append((p[ai] * asg, p[bi] * bsg, p[ci] * csg, u, v, ti, (r, g, b)))
            allp.append(pts)
    # the reference sedan in core metres: x half-width 0.86, y
    # half-length 2.45, z from the contact patch to the roof
    ref = ((-0.86, 0.86), (-2.45, 2.45), (-0.866, 0.574))
    rh = ref[ai][0] * asg, ref[ai][1] * asg
    rv = ref[bi][0] * bsg, ref[bi][1] * bsg
    cx, cy = (min(rh) + max(rh)) / 2, (min(rv) + max(rv)) / 2
    sc = W / 6.6  # pixels per metre, fixed - the absolute scale

    def to_px(x, y):
        return W / 2 + (x - cx) * sc, H / 2 - (y - cy) * sc

    # ground line: the view's horizontal axis at contact-patch height,
    # only for views whose vertical axis is the core z
    if bi == 2:
        gy = int(to_px(0.0, -0.866 * bsg)[1])
        if 0 <= gy < H:
            img[gy, :] = (220, 220, 220)
    # reference outline (drawn first; triangles overdraw it)
    x0, y0 = to_px(min(rh), max(rv))
    x1, y1 = to_px(max(rh), min(rv))
    x0, x1 = max(0, int(x0)), min(W - 1, int(x1))
    y0, y1 = max(0, int(y0)), min(H - 1, int(y1))
    if x1 > x0 and y1 > y0:
        img[y0, x0:x1] = (200, 60, 60)
        img[y1, x0:x1] = (200, 60, 60)
        img[y0:y1, x0] = (200, 60, 60)
        img[y0:y1, x1] = (200, 60, 60)
    texcache = {}
    for pts in allp:
        (x0, y0, d0, u0, v0, t0, c0), (x1, y1, d1, u1, v1, t1, c1), (x2, y2, d2, u2, v2, t2, c2) = pts
        sx = [W / 2 + (x - cx) * sc for x in (x0, x1, x2)]
        sy = [H / 2 - (y - cy) * sc for y in (y0, y1, y2)]
        dep = (d0 + d1 + d2) / 3
        minx, maxx = max(0, int(min(sx))), min(W - 1, int(max(sx)) + 1)
        miny, maxy = max(0, int(min(sy))), min(H - 1, int(max(sy)) + 1)
        den = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
        if abs(den) < 1e-9:
            continue
        tex = None
        if t0 >= 0:
            if t0 not in texcache:
                texcache[t0] = tex_rgba[t0]
            tex = texcache[t0]
        for yy in range(miny, maxy + 1):
            for xx in range(minx, maxx + 1):
                w0 = ((sy[1] - sy[2]) * (xx - sx[2]) + (sx[2] - sx[1]) * (yy - sy[2])) / den
                w1 = ((sy[2] - sy[0]) * (xx - sx[2]) + (sx[0] - sx[2]) * (yy - sy[2])) / den
                w2 = 1 - w0 - w1
                if w0 < 0 or w1 < 0 or w2 < 0:
                    continue
                if dep <= zbuf[yy, xx]:
                    continue
                zbuf[yy, xx] = dep
                if tex is not None:
                    tu = int((u0 * w0 + u1 * w1 + u2 * w2) * 256) % 256
                    tv = int((v0 * w0 + v1 * w1 + v2 * w2) * 256) % 256
                    px = tex[tv, tu]
                    if px[3] == 0:
                        continue
                    col = px[:3].astype(np.int32)
                else:
                    col = np.array(c0, dtype=np.int32)
                img[yy, xx] = (col * np.array([c0[0], c0[1], c0[2]]) // 128) if tex is not None else col
    Image.fromarray(img, "RGB").save(path)


def main():
    car_lump = load("lump28_carmodels_r0.bin")
    tex_info = load("lump34_textureinfo_r0.bin")
    mds = load("lump01_models_r2.bin")
    names_buf = load("lump12_modelnames_r0.bin")
    tpage_region = load("tpage.bin")
    with open(os.path.join(ASSETS, "chicago-lcf.json")) as f:
        lcf = json.load(f)
    cop_cos = lcf["rows"][0]
    wheel_size = cop_cos["wheelSize"]

    perms = parse_texture_info(tex_info)
    pages = PageSet(tpage_region, perms)

    # the denting tables (CHICAGO.DEN, DENTING.C ProcessDentLump): the
    # same 13-offset table shape as the LCF; record = 6 zones x 50 vertex
    # ids (0xFF-terminated), 6 x 70 poly ids, 255 damage levels. Model
    # 0's record is the cop's. Carried as data (see the report for what
    # the runtime does and does not do with it).
    den = load("../CHICAGO.DEN") if os.path.exists(os.path.join(SLICES, "..", "CHICAGO.DEN")) else None
    dent_zones = None
    if den is not None:
        den_ofs = struct.unpack_from("<i", den, 0)[0]
        rec = den[den_ofs:den_ofs + 975]
        dent_zones = {
            "record_offset": den_ofs,
            "zone_verts": [[b for b in rec[z * 50:(z + 1) * 50] if b != 0xFF] for z in range(6)],
            "zone_polys": [[b for b in rec[300 + z * 70:300 + (z + 1) * 70] if b != 0xFF] for z in range(6)],
            "levels_bytes": len(rec[720:975]),
        }

    # the cop: external model 0, offsets relative to lump + 164
    base = 164
    clean_ofs, dam_ofs, low_ofs = struct.unpack_from("<iii", car_lump, 4)
    body_clean = parse_model(car_lump, base + clean_ofs)
    # the "dam" blob is NOT a second body: it is the DENT TARGET - same
    # vertex count, no poly stream of its own (its poly_block field is a
    # stale copy of the clean model's point-normal offset, never read).
    # DENTING.C blends every vertex from clean toward dam by the car's
    # per-zone damage. Only its vertices are read here.
    body_dam = parse_model(car_lump, base + dam_ofs, with_polys=False)
    assert len(body_dam["verts"]) == len(body_clean["verts"]), "dent target must share the vertex list"

    # wheels by name, the game's own lookup (counting every string)
    names = []
    i = 0
    while i < len(names_buf):
        j = names_buf.index(b"\x00", i)
        names.append(names_buf[i:j].decode("ascii", "replace"))
        i = j + 1
    n_models = struct.unpack_from("<i", mds, 0)[0]
    assert len(names) == n_models, (len(names), n_models)
    entries = []
    ptr = 4
    for _ in range(n_models):
        sz = struct.unpack_from("<i", mds, ptr)[0]
        ptr += 4
        entries.append((ptr, sz))
        ptr += sz

    def named(name):
        off, _ = entries[names.index(name)]
        return parse_model(mds, off)

    wheel_clean = named("CLEANWHEEL")
    wheel_fast = named("FASTWHEEL")
    # MangleWheelModels (CARS.C) patches the wheel models at load: poly
    # headers 0 and 1 are overwritten with polys 2 and 3's, and
    # DrawWheelObject then draws the WHOLE wheel with one page+CLUT -
    # the first poly's, i.e. poly 2's original (texture_set, texture_id)
    # = page 50, texture 28 in this file. The 0xff set bytes stored in
    # six of the eight records are placeholders for that patch. Net
    # effect applied here: every wheel poly wears poly 2's texture.
    for w in (wheel_clean, wheel_fast):
        hdr2 = bytes(w["polys"][2][1][1:3])
        w["polys"] = [(t, rec[:1] + hdr2 + rec[3:]) for (t, rec) in w["polys"]]
    spin = frozenset(range(8, 24))
    for w in (wheel_clean, wheel_fast):
        w["verts"] = bake_wheel_spin0(w["verts"], wheel_size)

    mb = MeshBuilder()
    meshes = {}
    for key, model, sp in (("clean", body_clean, frozenset()),
                           ("dam", body_dam, frozenset()),
                           ("wheel_clean", wheel_clean, spin),
                           ("wheel_fast", wheel_fast, spin)):
        meshes[key] = mb.build(model, sp)
    # DrawWheelObject never reads the wheel polys' stored colour: it
    # writes its own bright/dim greyscale (combointensity) per side.
    # The stored bytes (0, 191, 96 in this file) are not a tint - left
    # in, they would paint the wheels green. Neutralise them; the
    # runtime light does the rest, exactly as on the body.
    for key in ("wheel_clean", "wheel_fast"):
        verts, corners = meshes[key]
        meshes[key] = (verts, [(vi, u, v, ti, 128, 128, 128, s)
                               for (vi, u, v, ti, r, g, b, s) in corners])

    # textures out
    tex_rgba = []
    for n, (pid, ci) in enumerate(mb.tex_list):
        if (pid, ci) == (-1, -1):
            rgba = np.full((256, 256, 4), 255, dtype=np.uint8)  # the flat card
        else:
            rgba = pages.render(pid, ci)
        tex_rgba.append(rgba)
        Image.fromarray(rgba, "RGBA").save(os.path.join(ASSETS, f"cop_tex{n}.png"))

    # runtime bin: D2C1 v1
    blob = bytearray()
    blob += b"D2C1" + struct.pack("<I", 1)
    blob += struct.pack("<I", len(mb.tex_list))
    for key in ("clean", "dam", "wheel_clean", "wheel_fast"):
        verts, corners = meshes[key]
        blob += struct.pack("<III", len(verts), len(corners), len(corners) // 3)
        for v in verts:
            blob += struct.pack("<3f", *v)
        for (vi, u, v, ti, r, g, b, s) in corners:
            blob += struct.pack("<HffBB", vi, u, v, ti if ti >= 0 else 255, s)
            blob += struct.pack("<3B", r, g, b)
    with open(os.path.join(ASSETS, "cop_car.bin"), "wb") as f:
        f.write(blob)

    # measurements
    def bbox(model):
        vs = [ps1_to_core(v) for v in model["verts"]]
        return {ax: [min(v[i] for v in vs), max(v[i] for v in vs)]
                for i, ax in enumerate("xyz")}

    meas = {
        "source": "CHICAGO.LEV LUMP_CAR_MODELS (DATA1), external model 0 (cop)",
        "clean": {"verts": len(body_clean["verts"]), "polys": len(body_clean["polys"]),
                  "tris": len(meshes["clean"][1]) // 3, "bbox_core_m": bbox(body_clean),
                  "bounding_sphere_units": body_clean["bounding_sphere"]},
        "dent_target": {"verts": len(body_dam["verts"]),
                        "note": "vertex overlay for the clean body (DENTING.C blend), "
                                "no polys of its own; bin slot 1 carries 0 corners",
                        "max_dent_m": max(math.dist(ps1_to_core(a), ps1_to_core(b))
                                          for a, b in zip(body_clean["verts"], body_dam["verts"]))},
        "dent_zones": dent_zones,
        "wheels": {"CLEANWHEEL_index": names.index("CLEANWHEEL"),
                   "FASTWHEEL_index": names.index("FASTWHEEL"),
                   "DAMWHEEL_index": names.index("DAMWHEEL"),
                   "verts": len(wheel_clean["verts"]), "polys": len(wheel_clean["polys"]),
                   "wheelSize": wheel_size,
                   "spin_verts": "8..23 (CARS.C rewrite group)"},
        # Wheel centres, re-derived in fix round 3 from the game's own
        # placement law (CARS.C DrawCarWheels), in the body model's own
        # frame (the runtime draws body and wheels in one frame, and
        # the original draws the body at physics + R*cog while the
        # wheels ride the physics origin - so the centres below are
        # relative to the MODEL origin, cog subtracted):
        #   drawn x (file frame) = (wheelnum & 2) == 0 ? 17 - vx
        #                        : -17 - vx
        #     - the +-17 is the wheel's own half-width: the LCF vx is
        #     the placement of the wheel's INNER edge reference, not
        #     its centre. Piece 3 placed the centres at -vx outright,
        #     which put the tyre's outer face ~0.07 m OUTSIDE the
        #     body's side slab (centre 122-126 + half-width 17 = up to
        #     143 vs the slab at 132) - the eye test's "glued on".
        #     Corrected centres: |x| = 105..109, outer face at 122.5,
        #     just inside the slab, exactly as the original sits.
        #   drawn z (file frame) = -vz_post, vz already shifted by
        #     FixCarCos's -13 (HANDLING.C); cog z is 0 in the data and
        #     FixCarCos never touches it, so core y = vz_post.
        #     Wheels 0,2 (+222) are the FRONT axle; even wheelnums
        #     steer (DrawCarWheels' front model + steer matrix).
        #   height: the sWheelPos.vy formula gives the centre at
        #     (-wheelSize - vy) - sus + 14 = -5 - sus relative to the
        #     physics origin; the model origin rides cog y = -95 over
        #     it, so in the model frame the centre is 90 - sus. The
        #     measured rest compression (sus = 26) lands it at 64
        #     units under the model origin - the same 64 the piece-3
        #     arch measurement fixed independently (arch lip at core
        #     z -0.017, floor pan at -0.573 at this factor), so the
        #     formula and the measurement now agree and 64 rules.
        "wheelDisp_core_m": [[-((17 - cop_cos["wheelDisp"][k][0]) if k < 2
                                else (-17 - cop_cos["wheelDisp"][k][0])) / UNITS_PER_METRE,
                              (cop_cos["wheelDisp"][k][2] - 13) / UNITS_PER_METRE,
                              -64.0 / UNITS_PER_METRE] for k in range(4)],
        "wheel_height_note": "centre z = -64 units = -0.417 m at the convicted "
                             "factor (DrawCarWheels vy law + cog, rest "
                             "compression 26; matches the arch measurement)",
        "wheel_diameter_m": 2 * 69 / UNITS_PER_METRE,
        "units_per_metre": UNITS_PER_METRE,
        "textures": [{"file": f"cop_tex{n}.png",
                      "page": p if p >= 0 else "flat-white-card", "clut": c}
                     for n, (p, c) in enumerate(mb.tex_list)],
        "bin_bytes": len(blob),
    }
    with open(os.path.join(ASSETS, "cop_car_measure.json"), "w") as f:
        json.dump(meas, f, indent=1)

    # previews: body clean + wheels placed by the LCF (drawn at rest)
    disp = meas["wheelDisp_core_m"]
    placed = []
    bv, bc = meshes["clean"]
    placed.append((bv, bc, tex_rgba))
    wv, wc = meshes["wheel_clean"]
    for k in range(4):
        cx, cy, cz = disp[k]
        verts2 = [(cx + v[0], cy + v[1], cz + v[2]) for v in wv]
        placed.append((verts2, wc, tex_rgba))
    render_preview(placed, pages, os.path.join(ASSETS, "cop_preview_side.png"),
                   ((1, 1), (2, 1), (0, 1)))   # side: forward horizontal, up vertical
    render_preview(placed, pages, os.path.join(ASSETS, "cop_preview_front.png"),
                   ((0, -1), (2, 1), (1, 1)))  # front: his right on the viewer's left
    render_preview(placed, pages, os.path.join(ASSETS, "cop_preview_top.png"),
                   ((0, 1), (1, 1), (2, 1)))
    print(json.dumps(meas, indent=1))


if __name__ == "__main__":
    main()
