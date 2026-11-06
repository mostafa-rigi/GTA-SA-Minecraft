#!/usr/bin/env python3
"""Phase zero of the real-model soldier: prove, on the user's own urban.mdl, that the
from-scratch MDL reader (tools/mdl.py) truly understands the file - bones, sequences,
meshes - and that posing it gives a man, measured with numbers, before any of it is
allowed near the game. With --render it also draws a software preview PNG of three
poses with the model's own textures, so an eye can judge what the numbers claim.

Usage: prove_mdl.py <model.mdl> [--render out.png]
"""

import sys
import math
import numpy as np

import mdl as M


def skin(model, mats, inv_bind=None):
    """Pose one Model's vertices: returns posed positions (N,3)."""
    v = np.array(model.verts, dtype=np.float64)
    bones = np.array(model.vertbone, dtype=np.int32)
    out = np.zeros_like(v)
    mats_np = [np.array(m, dtype=np.float64) for m in mats]
    if inv_bind is not None:
        mats_np = [mats_np[b] @ inv_bind[b] for b in range(len(mats_np))]
    for b in range(len(mats_np)):
        sel = bones == b
        if not sel.any():
            continue
        out[sel] = v[sel] @ mats_np[b][:3, :3].T + mats_np[b][:3, 3]
    return out


def corner_data(mdl, mats, inv_bind=None):
    """All triangles of all body parts, posed: (P (T,3,3), UV (T,3,2), tex (T,),
    N (T,3,3)) with UV already normalised per texture. A corner's normal is the
    stream's normal for that corner, rotated by the corner vertex's bone."""
    P, UV, TX, NR = [], [], [], []
    mats_np = [np.array(m, dtype=np.float64) for m in mats]
    if inv_bind is not None:
        mats_np = [mats_np[b] @ inv_bind[b] for b in range(len(mats_np))]
    for _bpname, models in mdl.bodyparts:
        for model in models:
            posed = skin(model, mats, inv_bind)
            norms = np.array(model.norms, dtype=np.float64)
            for mesh in model.meshes:
                tex = mdl.textures[mesh.skinref]
                for tri in mesh.tris:
                    P.append([posed[c[0]] for c in tri])
                    NR.append([norms[c[1]] @ mats_np[model.vertbone[c[0]]][:3, :3].T for c in tri])
                    UV.append([(c[2] / tex.w, c[3] / tex.h) for c in tri])
                    TX.append(mesh.skinref)
    return np.array(P), np.array(UV), np.array(TX), np.array(NR)


def bbox(P):
    lo = P.reshape(-1, 3).min(axis=0)
    hi = P.reshape(-1, 3).max(axis=0)
    return lo, hi


def fmt(v):
    return "(%.1f, %.1f, %.1f)" % (v[0], v[1], v[2])


def main():
    path = sys.argv[1]
    render = None
    if "--render" in sys.argv:
        render = sys.argv[sys.argv.index("--render") + 1]
    data = open(path, "rb").read()
    m = M.MDL(data)
    print("file: %s (%d bytes)" % (path, len(data)))
    print("name: %r  header bbox: %s .. %s" % (m.name, fmt(m.bbmin), fmt(m.bbmax)))
    print("bones: %d" % len(m.bones))
    for b in m.bones[:8]:
        print("   bone %-24s parent %2d  value %s" % (b.name, b.parent,
              "(" + ", ".join("%.2f" % x for x in b.value) + ")"))
    print("controllers: %d  attachments: %s" % (len(m.controllers),
          [(a[0], a[2], m.bones[a[2]].name if 0 <= a[2] < len(m.bones) else "?") for a in m.attachments]))
    print("sequences: %d" % len(m.seqs))
    for s in m.seqs:
        print("   seq %-28s fps %5.1f frames %3d" % (s.name, s.fps, s.numframes))
    print("textures: %s" % [(t.name, t.w, t.h) for t in m.textures])
    total_tris = 0
    for bpname, models in m.bodyparts:
        for mo in models:
            nt = sum(len(me.tris) for me in mo.meshes)
            total_tris += nt
            print("bodypart %-16s model %-16s verts %5d norms %5d meshes %d tris %d"
                  % (bpname, mo.name, len(mo.verts), len(mo.norms), len(mo.meshes), nt))
    print("triangles total: %d" % total_tris)

    # ---- which skinning convention does this file use?
    # H1: stored vertices are bone-local already; world = W_anim * v.
    # H2: stored vertices are model-space; world = W_anim * W_bind^-1 * v.
    bind_mats = []
    for bi, bone in enumerate(m.bones):
        mm = M._quat_matrix(bone.value[:3], M._angle_quaternion(*bone.value[3:6]))
        if bone.parent >= 0:
            mm = M._mat_mul(bind_mats[bone.parent], mm)
        bind_mats.append(mm)
    inv_bind = [np.linalg.inv(np.array(x, dtype=np.float64)) for x in bind_mats]

    idle = m.seq_by_name.get("idle1") or m.seqs[0]
    mats = m.bone_matrices(idle, 0)
    for label, ib in (("H1 raw bone matrices", None), ("H2 with inverse bind", inv_bind)):
        P, _UV, _TX, _NR = corner_data(m, mats, ib)
        lo, hi = bbox(P)
        print("%s at %s f0: bbox %s .. %s  height %.1f  feet z %.2f"
              % (label, idle.name, fmt(lo), fmt(hi), hi[2] - lo[2], lo[2]))
    print("(header model bbox height is %.1f, z from %.1f to %.1f)"
          % (m.bbmax[2] - m.bbmin[2], m.bbmin[2], m.bbmax[2]))

    if render:
        render_preview(m, inv_bind if _h2_wins(m, mats, inv_bind) else None, render)


def _h2_wins(m, mats, inv_bind):
    """H2 wins when its posed height is the sane one (a man, 60..80 units)."""
    def height(ib):
        P, _u, _t, _n = corner_data(m, mats, ib)
        lo, hi = bbox(P)
        return hi[2] - lo[2]
    h1, h2 = height(None), height(inv_bind)
    return abs(h2 - 72.0) < abs(h1 - 72.0)


def leg_bone_set(m):
    """The gait half of a GoldSrc player: the root, the pelvis, and everything under
    the two thighs. The weapon layer (ref_aim / ref_shoot / ref_reload) drives the
    rest - that split is how Counter-Strike itself composites its players."""
    legs = {0}
    for i, b in enumerate(m.bones):
        if b.name in ("Bip01 Pelvis",):
            legs.add(i)
    for i, b in enumerate(m.bones):
        if "Thigh" in b.name:
            stack = [i]
            while stack:
                j = stack.pop()
                legs.add(j)
                stack += [k for k, bb in enumerate(m.bones) if bb.parent == j]
    return legs


def composite_mats(m, legs_seq, legs_frame, upper_seq, upper_frame):
    legs = leg_bone_set(m)
    lm = m.bone_pose(legs_seq, legs_frame)
    um = m.bone_pose(upper_seq, upper_frame)
    mats = []
    for bi, b in enumerate(m.bones):
        pos, q = lm[bi] if bi in legs else um[bi]
        mm = M._quat_matrix(pos, q)
        if b.parent >= 0:
            mm = M._mat_mul(mats[b.parent], mm)
        mats.append(mm)
    return mats


def render_preview(m, inv_bind, out_path):
    """The soldier the way the game itself shows him: gait sequences below the
    pelvis, the M4A1 weapon layer above it - idle stance, a walking step, the aim,
    and a death - front and side, framed on the figures themselves."""
    from PIL import Image
    tex_imgs = []
    for t in m.textures:
        pal = np.frombuffer(t.palette, dtype=np.uint8).reshape(256, 3)
        idx = np.frombuffer(t.pixels, dtype=np.uint8).reshape(t.h, t.w)
        tex_imgs.append(pal[idx])  # (h, w, 3)

    S = m.seq_by_name
    plan = [
        ("idle (idle1)", m.bone_matrices(S["idle1"], 0)),
        ("walk + aim", composite_mats(m, S["walk"], 8, S["ref_aim_carbine"], 0)),
        ("aim (carbine)", composite_mats(m, S["idle1"], 0, S["ref_aim_carbine"], 0)),
        ("death1 mid", m.bone_matrices(S["death1"], 20)),
    ]
    views = []
    zlo, zhi, hlo, hhi = 1e9, -1e9, 1e9, -1e9
    for name, mats in plan:
        P, UV, TX, NR = corner_data(m, mats, inv_bind)
        views.append((name, P, UV, TX, NR))
        lo, hi = bbox(P)
        zlo, zhi = min(zlo, lo[2]), max(zhi, hi[2])
    W, H = 340, 520
    sheet = Image.new("RGB", (W * len(views) * 2, H), (24, 24, 28))
    for vi, (name, P, UV, TX, NR) in enumerate(views):
        for side in range(2):  # 0: front (Y across, X depth)  1: side (X across)
            img = _draw(P, UV, TX, NR, tex_imgs, side, W, H, zlo, zhi)
            sheet.paste(img, ((vi * 2 + side) * W, 0))
    sheet.save(out_path)
    print("preview written: %s (posed z range %.1f..%.1f)" % (out_path, zlo, zhi))


def _draw(P, UV, TX, NR, tex_imgs, side, W, H, zlo, zhi):
    from PIL import Image
    if side == 0:
        horiz = P[:, :, 1]
        depth = P[:, :, 0]
    else:
        horiz = P[:, :, 0]
        depth = -P[:, :, 1]
    vert = P[:, :, 2]
    hspan = max(30.0, float(np.abs(horiz).max()) + 4.0)
    zspan = (zhi - zlo) + 6.0
    scale = min((W - 8) / (2 * hspan), (H - 8) / zspan)
    sx = W / 2 + horiz * scale
    sy = H - 4 - (vert - zlo + 3.0) * scale
    # painter: far triangles first. Front view: camera on +X, so larger X is
    # NEARER - ascending X draws far first. Side view: camera on +Y (his left),
    # depth axis is Y the same way.
    order = np.argsort(P[:, :, 0].mean(axis=1)) if side == 0 else np.argsort(P[:, :, 1].mean(axis=1))
    img = np.zeros((H, W, 3), dtype=np.float64)
    img[:] = (24, 24, 28)
    light_dir = np.array([0.3, -0.5, 0.8])
    light_dir /= np.linalg.norm(light_dir)
    for ti in order:
        pts = np.stack([sx[ti], sy[ti]], axis=1)  # (3,2)
        n = np.cross(P[ti][1] - P[ti][0], P[ti][2] - P[ti][0])
        ln = np.linalg.norm(n)
        if ln < 1e-9:
            continue
        n = n / ln
        shade = 0.45 + 0.55 * abs(np.dot(n, light_dir))
        tex = tex_imgs[TX[ti]]
        th, tw = tex.shape[:2]
        uv = UV[ti].copy()
        uv[:, 0] = np.clip(uv[:, 0], 0.0, 0.9999) * tw
        uv[:, 1] = np.clip(uv[:, 1], 0.0, 0.9999) * th
        _fill(img, pts, uv, tex, shade)
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def _fill(img, pts, uv, tex, shade):
    H, W = img.shape[:2]
    lo = np.maximum(pts.min(axis=0).astype(int), [0, 0])
    hi = np.minimum(pts.max(axis=0).astype(int) + 1, [W, H])
    if (hi <= lo).any():
        return
    (x0, y0), (x1, y1), (x2, y2) = pts
    den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    if abs(den) < 1e-9:
        return
    ys, xs = np.mgrid[lo[1]:hi[1], lo[0]:hi[0]]
    w0 = ((y1 - y2) * (xs - x2) + (x2 - x1) * (ys - y2)) / den
    w1 = ((y2 - y0) * (xs - x2) + (x0 - x2) * (ys - y2)) / den
    w2 = 1.0 - w0 - w1
    inside = (w0 >= -0.001) & (w1 >= -0.001) & (w2 >= -0.001)
    if not inside.any():
        return
    u = (w0 * uv[0][0] + w1 * uv[1][0] + w2 * uv[2][0]).astype(int)
    v = (w0 * uv[0][1] + w1 * uv[1][1] + w2 * uv[2][1]).astype(int)
    th, tw = tex.shape[:2]
    u = np.clip(u, 0, tw - 1)
    v = np.clip(v, 0, th - 1)
    region = img[lo[1]:hi[1], lo[0]:hi[0]]
    region[inside] = tex[v[inside], u[inside]] * shade


if __name__ == "__main__":
    main()
