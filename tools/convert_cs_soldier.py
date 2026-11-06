#!/usr/bin/env python3
"""Offline converter: Counter-Strike 1.6's own SEAL player model (urban.mdl) plus
its M4A1 world model (p_m4a1.mdl) -> the core's compact runtime file cs_soldier.bin.

The soldier is drawn from this file at runtime by src/core/SoldierModel.cpp. Nothing
here touches the mod's frozen atlases: the model's own textures are exported as
separate PNGs next to the .bin.

Baking choices, all measured on the real files (see CS-REALMODEL-fa.md):
- Vertices stay bone-local (the GoldSrc way); per-bone animation streams are baked
  to 30 fps position + quaternion tracks for all bones.
- Poses are composited exactly like the game does: a gait sequence drives the root,
  the pelvis and the legs; the weapon layer (ref_aim_carbine & friends) drives
  everything above. Deaths are whole-body sequences.
- Animation space puts the root bone at the pelvis, so the figure is lifted by the
  measured sole offset of the standing aim pose and scaled so the bind (reference)
  figure stands exactly 1.80 m - the soldier's height in the core.
- The weapon's own bones beyond the shared arm chain ('flash', 'Line03') are
  appended to the bone list with their constant base locals; the shared bones are
  driven by the player model's streams, which is how the engine attaches it.
"""
import math
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mdl as M

CAT_IDLE, CAT_WALK, CAT_RUN, CAT_AIM, CAT_SHOOT, CAT_RELOAD, CAT_DEATH = range(7)
OUT_FPS = 30.0


def slerp(q1, q2, t):
    d = sum(a * b for a, b in zip(q1, q2))
    if d < 0.0:
        q2 = tuple(-c for c in q2)
        d = -d
    if d > 0.9995:
        q = tuple(a + (b - a) * t for a, b in zip(q1, q2))
    else:
        th = math.acos(max(-1.0, min(1.0, d)))
        s = math.sin(th)
        a = math.sin((1.0 - t) * th) / s
        b = math.sin(t * th) / s
        q = tuple(a * x + b * y for x, y in zip(q1, q2))
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def sample_locals(mdl, seq, t, clamp=False):
    """Per-bone (pos, quat) locals of one sequence at time t seconds, interpolated."""
    f = t * seq.fps
    if clamp:
        f = min(f, seq.numframes - 1.0)
    else:
        f = f % seq.numframes
    f0 = int(math.floor(f))
    frac = f - f0
    f1 = f0 + 1
    if clamp:
        f1 = min(f1, seq.numframes - 1)
    else:
        f1 = f1 % seq.numframes
    a = mdl.bone_pose(seq, f0)
    if frac <= 0.0 or f1 == f0:
        return a
    b = mdl.bone_pose(seq, f1)
    out = []
    for (pa, qa), (pb, qb) in zip(a, b):
        pos = tuple(pa[i] + (pb[i] - pa[i]) * frac for i in range(3))
        out.append((pos, slerp(qa, qb, frac)))
    return out


class Combined:
    """Urban's 53 bones plus the weapon-only bones, one shared indexing.
    Each combined bone knows which file's stream drives it."""

    def __init__(self, urban, weapon):
        self.names = [b.name for b in urban.bones]
        self.parents = [b.parent for b in urban.bones]
        self.src = [("u", i) for i in range(len(urban.bones))]
        by_name = {b.name: i for i, b in enumerate(urban.bones)}
        self.weapon_only_base = {}
        combined_of_weapon = {}
        for wi, wb in enumerate(weapon.bones):
            if wb.name in by_name:
                combined_of_weapon[wi] = by_name[wb.name]
                u = urban.bones[by_name[wb.name]]
                dp = max(abs(u.value[k] - wb.value[k]) for k in range(3))
                dr = max(abs(u.value[k] - wb.value[k]) for k in range(3, 6))
                if dp > 0.05 or dr > 0.05:
                    raise SystemExit("weapon bone %s base differs from the player's (dp %.3f dr %.3f)"
                                     % (wb.name, dp, dr))
                continue
            idx = len(self.names)
            combined_of_weapon[wi] = idx
            self.names.append(wb.name)
            self.parents.append(combined_of_weapon[wb.parent] if wb.parent >= 0 else -1)
            self.src.append(("w", wi))
            pos = tuple(wb.value[0:3])
            quat = M._angle_quaternion(wb.value[3], wb.value[4], wb.value[5])
            self.weapon_only_base[idx] = (pos, quat)
        self.count = len(self.names)
        # weapon-file bone index -> combined index, for the weapon's vertices
        self.weapon_bone_map = combined_of_weapon

    def pose(self, urban, urban_locals):
        out = []
        for ci in range(self.count):
            kind, si = self.src[ci]
            out.append(urban_locals[si] if kind == "u" else self.weapon_only_base[ci])
        return out


def leg_bone_set(m):
    legs = {0}
    for i, b in enumerate(m.bones):
        if b.name == "Bip01 Pelvis":
            legs.add(i)
    for i, b in enumerate(m.bones):
        if "Thigh" in b.name:
            stack = [i]
            while stack:
                j = stack.pop()
                legs.add(j)
                stack += [k for k, bb in enumerate(m.bones) if bb.parent == j]
    return legs


def bake(urban, comb, plan):
    """plan: list of dicts {cat, loop, frames, legs:(seq,stretch,clamp)|None,
    upper:(seq,stretch,clamp)|None, full:(seq,stretch,clamp)|None} -> baked tracks."""
    legs_set = leg_bone_set(urban)
    seqs = []
    for spec in plan:
        nf = spec["frames"]
        dur = nf / OUT_FPS
        frames = []
        for i in range(nf):
            t = i / OUT_FPS
            if spec.get("full"):
                seq, stretch, clamp = spec["full"]
                loc = sample_locals(urban, seq, t * stretch, clamp)
            else:
                lseq, lstretch, lclamp = spec["legs"]
                useq, ustretch, uclamp = spec["upper"]
                ll = sample_locals(urban, lseq, t * lstretch, lclamp)
                uu = sample_locals(urban, useq, t * ustretch, uclamp)
                loc = [ll[bi] if bi in legs_set else uu[bi] for bi in range(len(urban.bones))]
            frames.append(comb.pose(urban, loc))
        seqs.append((spec["cat"], 1 if spec["loop"] else 0, frames))
    return seqs


def skin_corners(urban, mats):
    """All of the model's triangles as corner positions in model space (for measuring)."""
    pts = []
    for _bp, models in urban.bodyparts:
        for mo in models:
            for tri in mesh_all_tris(mo):
                for vi, _ni, _s, _t in tri:
                    v = np.array(mo.verts[vi])
                    bone = mo.vertbone[vi]
                    pts.append((mats[bone] @ np.array([v[0], v[1], v[2], 1.0]))[:3])
    return np.array(pts)


def mesh_all_tris(mo):
    out = []
    for mesh in mo.meshes:
        out.extend(mesh.tris)
    return out


def collect_corners(urban, weapon, comb, scale):
    """Corner-expanded geometry: (pos bone-local scaled, normal, uv, bone, tex)."""
    corners = []

    def add_model(mdl, mo, bone_map, tex_off):
        for mesh in mo.meshes:
            tex = tex_off + mesh.skinref
            for tri in mesh.tris:
                for vi, ni, s, t in tri:
                    texw, texh = mdl.textures[mesh.skinref].w, mdl.textures[mesh.skinref].h
                    v = mo.verts[vi]
                    n = mo.norms[ni]
                    corners.append(((v[0] * scale, v[1] * scale, v[2] * scale),
                                    n, (s / texw, t / texh), bone_map(mo.vertbone[vi]), tex))

    for _bp, models in urban.bodyparts:
        for mo in models:
            if mo.verts:
                add_model(urban, mo, lambda b: b, 0)
    for _bp, models in weapon.bodyparts:
        for mo in models:
            if mo.verts:
                add_model(weapon, mo, lambda b: comb.weapon_bone_map[b], 2)
    return corners


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    assets = os.path.join(here, "..", "..", "cs-assets")
    outdir = os.path.join(here, "..", "cs-soldier")
    os.makedirs(outdir, exist_ok=True)
    urban = M.MDL(open(os.path.join(assets, "models", "player", "urban", "urban.mdl"), "rb").read())
    weapon = M.MDL(open(os.path.join(assets, "models", "p_m4a1.mdl"), "rb").read())
    comb = Combined(urban, weapon)
    print("combined bones: %d (urban %d + weapon-only %s)"
          % (comb.count, len(urban.bones), [comb.names[i] for i in comb.weapon_only_base]))

    S = urban.seq_by_name
    # scale: the bind (reference) figure stands exactly 1.80 m
    bind_locals = [(tuple(b.value[0:3]), M._angle_quaternion(b.value[3], b.value[4], b.value[5]))
                   for b in urban.bones]
    bind = []
    for bi, b in enumerate(urban.bones):
        mm = M._quat_matrix(*bind_locals[bi])
        if b.parent >= 0:
            mm = M._mat_mul(bind[b.parent], mm)
        bind.append(mm)
    pts = skin_corners(urban, bind)
    h_bind = float(pts[:, 2].max() - pts[:, 2].min())
    scale = 1.8 / h_bind
    # ground shift: the standing aim pose's soles touch z = 0
    from prove_mdl import composite_mats
    aim = composite_mats(urban, S["idle1"], 0, S["ref_aim_carbine"], 0)
    pts = skin_corners(urban, aim)
    zshift = -float(pts[:, 2].min())
    print("bind height %.2f units -> scale %.5f m/unit; aim pose z %.2f..%.2f -> lift %.2f units (%.3f m)"
          % (h_bind, scale, float(pts[:, 2].min()), float(pts[:, 2].max()), zshift, zshift * scale))

    no_str = (1.0, False)
    plan = [
        dict(cat=CAT_IDLE, loop=True, frames=122, full=(S["idle1"],) + no_str),
        dict(cat=CAT_WALK, loop=True, frames=33, legs=(S["walk"],) + no_str,
             upper=(S["ref_aim_carbine"],) + no_str),
        dict(cat=CAT_RUN, loop=True, frames=19, legs=(S["run"],) + no_str,
             upper=(S["ref_aim_carbine"],) + no_str),
        dict(cat=CAT_AIM, loop=True, frames=2, legs=(S["idle1"],) + no_str,
             upper=(S["ref_aim_carbine"],) + no_str),
        dict(cat=CAT_SHOOT, loop=True, frames=10, legs=(S["idle1"],) + no_str,
             upper=(S["ref_shoot_carbine"],) + no_str),
        # the brain's reload takes 3.0 s; the game's own reload anim is 2.03 s, stretched to fit
        dict(cat=CAT_RELOAD, loop=False, frames=90, legs=(S["idle1"],) + no_str,
             upper=(S["ref_reload_carbine"], (61 / 30.0) / 3.0, True)),
        dict(cat=CAT_DEATH, loop=False, frames=36, full=(S["death1"],) + (1.0, True)),
        dict(cat=CAT_DEATH, loop=False, frames=41, full=(S["death2"],) + (1.0, True)),
        dict(cat=CAT_DEATH, loop=False, frames=106, full=(S["death3"],) + (1.0, True)),
    ]
    seqs = bake(urban, comb, plan)
    corners = collect_corners(urban, weapon, comb, scale)
    tris = len(corners) // 3
    print("corners %d, triangles %d, baked sequences %d, frames %d"
          % (len(corners), tris, len(seqs), sum(len(f) for _c, _l, f in seqs)))

    textures = [(t.w, t.h) for t in urban.textures] + [(t.w, t.h) for t in weapon.textures]
    blob = bytearray()
    blob += b"CSM1"
    blob += struct.pack("<I", 1)  # version
    blob += struct.pack("<IIII", comb.count, len(corners), tris, len(textures))
    for w, h in textures:
        blob += struct.pack("<HH", w, h)
    blob += struct.pack("<I", len(seqs))
    for cat, loop, frames in seqs:
        blob += struct.pack("<BBHf", cat, loop, len(frames), len(frames) / OUT_FPS)
    blob += struct.pack("<ff", scale, zshift)
    for p in comb.parents:
        blob += struct.pack("<i", p)
    for pos, n, uv, bone, _tex in corners:
        nn = np.array(n, dtype=np.float64)
        ln = np.linalg.norm(nn) or 1.0
        nn = (nn / ln * 32767).astype(np.int16)
        blob += struct.pack("<3f3h2fH", pos[0], pos[1], pos[2],
                            int(nn[0]), int(nn[1]), int(nn[2]), uv[0], uv[1], bone)
    for i in range(tris):
        blob += struct.pack("<B", corners[3 * i][4])
    for _cat, _loop, frames in seqs:
        for frame in frames:
            for bi, (pos, quat) in enumerate(frame):
                x, y, z = pos[0] * scale, pos[1] * scale, pos[2] * scale
                if bi == 0:
                    z += zshift * scale
                q = (np.array(quat) * 32767).astype(np.int16)
                blob += struct.pack("<3f4h", x, y, z, int(q[0]), int(q[1]), int(q[2]), int(q[3]))
    out = os.path.join(outdir, "cs_soldier.bin")
    open(out, "wb").write(bytes(blob))
    print("wrote %s: %d bytes" % (out, len(blob)))

    from PIL import Image
    for i, t in enumerate(list(urban.textures) + list(weapon.textures)):
        pal = np.frombuffer(t.palette, dtype=np.uint8).reshape(256, 3)
        idx = np.frombuffer(t.pixels, dtype=np.uint8).reshape(t.h, t.w)
        img = Image.fromarray(pal[idx], "RGB")
        p = os.path.join(outdir, "cs_soldier_tex%d.png" % i)
        img.save(p)
        print("wrote %s: %dx%d" % (p, t.w, t.h))


if __name__ == "__main__":
    main()
