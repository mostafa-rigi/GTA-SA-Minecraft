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
CAT_CROUCH, CAT_JUMP, CAT_FLINCH = 7, 8, 9
LYR_FULL, LYR_LEG, LYR_UP = 0, 1, 2
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


def centre_blend(seq):
    """The neutral cell of a sequence's blend grid (aim straight ahead). The weapon
    layer's grids are 3x3 (yaw x pitch); their centre is numblends // 2. A sequence
    with one blend has only cell 0. See mdl.bone_pose for why this is not cosmetic."""
    return seq.numblends // 2 if seq.numblends > 1 else 0


# The aim grid's centre column, measured off urban.mdl itself (gun line per cell):
# the column whose yaw stays on the man's facing while the pitch walks the three
# rows - blend 1 aims UP (+33 degrees), blend 4 is LEVEL (-13, the game's own
# relaxed hold), blend 7 aims DOWN (-31). The boss's aim and shoot poses are baked
# at all three so his rifle visibly pitches to a target above or below him; the
# brain picks the row by the target's elevation and blends at the change.
AIM_BLEND = {0: 4, 1: 1, 2: 7}  # pitch index (0 level, 1 up, 2 down) -> blend cell


def sample_locals(mdl, seq, t, clamp=False, blend=0):
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
    a = mdl.bone_pose(seq, f0, blend)
    if frac <= 0.0 or f1 == f0:
        return a
    b = mdl.bone_pose(seq, f1, blend)
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
    """plan: list of dicts {cat, layer, loop, frames, seq:(seq,stretch,clamp,blend)}
    -> baked tracks. V3 stores the layers SEPARATELY: a legs-layer track holds the
    gait sequence's full pose (the runtime reads only its lower-body bones), an
    upper-layer track holds the aim/shoot sequence's full pose sampled at one cell
    of its blend grid (the runtime reads only its spine-up bones), and a full track
    (deaths) is read whole. The runtime composes the two layers with the legs
    facing the movement and the torso twisted to the aim - the way GoldSrc itself
    separates gait from aim - instead of one merged statue pose per state."""
    seqs = []
    for spec in plan:
        nf = spec["frames"]
        frames = []
        seq, stretch, clamp, blend = spec["seq"]
        for i in range(nf):
            t = i / OUT_FPS
            loc = sample_locals(urban, seq, t * stretch, clamp, blend)
            frames.append(comb.pose(urban, loc))
        if spec.get("detrend_root") and len(frames) > 1:
            # The model's directional gaits (left / right / back) are NOT in-place
            # cycles in the file: their root bone carries the whole travel of the
            # move (up to 1.4 m) and sinks steadily to the floor across the cycle
            # (pelvis z 0.90 -> 0.12 m in baked terms). The game extracts that root
            # motion itself and plays the gait in place; a runtime that loops the
            # baked track as-is draws a man who swims off his true position, sinks
            # into the street and snaps back every cycle - the first boss eye test
            # called the result "a bit broken all over". Remove the root's linear
            # trend so the cycle starts and ends at the same root offset: what is
            # left is the stepping itself (bob and sway stay), loop-clean.
            p0 = frames[0][0][0]
            p1 = frames[-1][0][0]
            n = len(frames)
            for i, fr in enumerate(frames):
                k = i / (n - 1.0)
                pos = tuple(fr[0][0][j] - (p1[j] - p0[j]) * k for j in range(3))
                fr[0] = (pos, fr[0][1])
        if spec.get("root_dz"):
            # a constant root lift/drop for the whole sequence, in model units
            dz = spec["root_dz"]
            for fr in frames:
                pos = (fr[0][0][0], fr[0][0][1], fr[0][0][2] + dz)
                fr[0] = (pos, fr[0][1])
        seqs.append((spec["cat"], spec["layer"], 1 if spec["loop"] else 0, frames))
    return seqs


def measure_stride(mdl, seq, scale, axis=0, detrend_root=False):
    """The gait's own stride, measured off the model: how far the planted foot
    travels against the pelvis during its stance, per second of stance
    (that rate IS the ground speed at which the feet do not slip), times the cycle
    time. Returns (stride length in metres, no-slip speed in m/s). A stance is the
    longest run of frames where a toe stays within 3 cm of its lowest point.
    axis picks the travel coordinate: 0 for forward/back gaits, 1 for strafes."""
    names = [b.name for b in mdl.bones]
    i_pelvis = names.index("Bip01 Pelvis")
    toes = [names.index("Bip01 L Toe0"), names.index("Bip01 R Toe0")]
    frames = []
    for f in range(seq.numframes):
        mats = mdl.bone_matrices(seq, f)
        frames.append([np.array(mats[bi])[:3, 3] * scale for bi in range(len(mdl.bones))])
    frames = np.array(frames)  # (F, bones, 3) metres
    if detrend_root and len(frames) > 1:
        # measure the gait the way it is baked: with the root's linear trend
        # removed (see bake()), so the directional gaits' root travel and sink
        # do not corrupt the toe-vs-pelvis stance reading
        ramp = (frames[-1, 0] - frames[0, 0]) * (np.arange(len(frames)) / (len(frames) - 1.0))[:, None]
        frames = frames - ramp[:, None, :]
    speeds = []
    for ti in toes:
        x = frames[:, ti, axis] - frames[:, i_pelvis, axis]
        z = frames[:, ti, 2]
        planted = z < z.min() + 0.03
        idx = [f for f in range(seq.numframes) if planted[f]]
        if not idx:
            continue
        best = []
        for start in idx:
            run = [start]
            while planted[(run[-1] + 1) % seq.numframes] and len(run) <= seq.numframes:
                run.append((run[-1] + 1) % seq.numframes)
            if len(run) > len(best):
                best = run
        if len(best) >= 3:
            dx = abs(x[best[0]] - x[best[-1]])
            dur = len(best) / seq.fps
            if dur > 0 and dx > 0:
                speeds.append(dx / dur)
    if not speeds:
        raise SystemExit("no stance found in %s - cannot measure its stride" % seq.name)
    v0 = sum(speeds) / len(speeds)
    return v0 * (seq.numframes / seq.fps), v0


def gun_anchors(urban, weapon, comb, scale):
    """Two points of the rifle in the baked aim pose, as (combined bone, bone-local
    position in metres): the muzzle tip and the butt end. The runtime hangs its
    muzzle flash and the tests hang their aim proof on these. The weapon's corners
    all ride one bone (Line03), but nothing here assumes that."""
    S = urban.seq_by_name
    legs = leg_bone_set(urban)
    ll = sample_locals(urban, S["idle1"], 0.0, clamp=True)
    uu = sample_locals(urban, S["ref_aim_carbine"], 0.0, clamp=True, blend=centre_blend(S["ref_aim_carbine"]))
    loc = [ll[bi] if bi in legs else uu[bi] for bi in range(len(urban.bones))]
    full = comb.pose(urban, loc)
    mats = []
    for bi, (pos, q) in enumerate(full):
        mm = M._quat_matrix(pos, q)
        if comb.parents[bi] >= 0:
            mm = M._mat_mul(mats[comb.parents[bi]], mm)
        mats.append(np.array(mm))
    corners = []  # (posed position in metres, combined bone, bone-local metres)
    for _bp, models in weapon.bodyparts:
        for mo in models:
            if not mo.verts:
                continue
            for vi, v in enumerate(mo.verts):
                cb = comb.weapon_bone_map[mo.vertbone[vi]]
                p = mats[cb] @ np.array([v[0], v[1], v[2], 1.0])
                corners.append((p[:3] * scale, cb, (v[0] * scale, v[1] * scale, v[2] * scale)))
    pts = np.array([c[0] for c in corners])
    cen = pts.mean(axis=0)
    _w, V = np.linalg.eigh((pts - cen).T @ (pts - cen))
    axis = V[:, -1]
    proj = (pts - cen) @ axis
    ends = [corners[int(np.argmax(proj))], corners[int(np.argmin(proj))]]
    # the muzzle is the far end of the barrel: the end further from the man's centre
    muzzle, butt = (ends[0], ends[1]) if np.linalg.norm(ends[0][0]) > np.linalg.norm(ends[1][0]) else (ends[1], ends[0])
    d = muzzle[0] - butt[0]
    yaw = math.degrees(math.atan2(-d[1], d[0]))
    print("aim gun line: muzzle %s butt %s yaw %+.2f deg from the model's forward"
          % (np.round(muzzle[0], 3), np.round(butt[0], 3), yaw))
    if abs(yaw) > 10.0:
        raise SystemExit("the baked aim does not point where the man faces (yaw %.1f) - refusing to bake" % yaw)
    return muzzle, butt


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

    def spec(seq, stretch=1.0, clamp=False, blend=None):
        if blend is None:
            blend = centre_blend(seq)
        return (seq, stretch, clamp, blend)

    def native_frames(seq):
        return max(2, int(round(seq.numframes / seq.fps * OUT_FPS)))

    # crouch_idle is authored around a root that sits higher than the standing
    # root: baked as-is, the crouched man's soles hover over the street (the
    # first boss eye test's floating crouch). The game lowers the whole figure
    # when a player ducks; our runtime places his feet at the ground instead,
    # so the bake drops the crouch root by exactly the hover - measured here
    # off the model itself, as the crouch cycle's lowest skinned point above
    # the standing soles' line (model units; the ground lift is added later).
    crouch_min = min(float(skin_corners(urban, urban.bone_matrices(S["crouch_idle"], f))[:, 2].min())
                     for f in range(S["crouch_idle"].numframes))
    crouch_root_dz = -(crouch_min + zshift)
    print("crouch root drop %.2f units (%.3f m): the crouch cycle's soles hovered that far over the ground"
          % (crouch_root_dz, crouch_root_dz * scale))

    # V3 plan: layers stored separately. Legs variants: WALK 0 = forward; RUN
    # 0 = forward, 1 = strafe-left, 2 = strafe-right, 3 = back (all four carry
    # the run cycle - the file's directional sequences are events, not gaits;
    # see the note at the RUN entries below). Upper variants: AIM/SHOOT 0..2 = standing pitch rows
    # (level/up/down of the aim grid), 3..5 = the crouched rows; RELOAD 0 =
    # standing, 1 = crouched; FLINCH 0 = gut, 1 = head.
    plan = [
        # ---- legs layer ----
        dict(cat=CAT_IDLE, layer=LYR_LEG, loop=True, frames=122, seq=spec(S["idle1"])),
        dict(cat=CAT_WALK, layer=LYR_LEG, loop=True, frames=33, seq=spec(S["walk"])),
        dict(cat=CAT_RUN, layer=LYR_LEG, loop=True, frames=19, seq=spec(S["run"])),
        # RUN 1/2/3 (strafe-left, strafe-right, back) are baked from the model's
        # own "run" cycle, NOT from the file's "left"/"right"/"back" sequences:
        # measured off urban.mdl itself, those three are one-shot events, not
        # in-place gaits - their root carries up to 1.4 m of travel, the pelvis
        # sinks to the floor ("left": z 0.90 -> 0.12 m), and by the last frames
        # the whole figure has left the ground plane (toes 0.8 m up, head at
        # the floor in "back": a backward fall). Looping any of them drew a man
        # who sank, swam off his position and snapped back - a large part of
        # the first boss eye test's "a bit broken all over". Counter-Strike
        # itself strafes by yawing the leg layer to the movement while the run
        # cycle steps - which is exactly what the runtime's two-layer compose
        # does with legYaw - so the strafe and back variants carry the run
        # cycle and the yaw does the rest. (bake() still supports detrend_root
        # for any future sequence that only needs its root trend removed.)
        dict(cat=CAT_RUN, layer=LYR_LEG, loop=True, frames=19, seq=spec(S["run"])),
        dict(cat=CAT_RUN, layer=LYR_LEG, loop=True, frames=19, seq=spec(S["run"])),
        dict(cat=CAT_RUN, layer=LYR_LEG, loop=True, frames=19, seq=spec(S["run"])),
        dict(cat=CAT_CROUCH, layer=LYR_LEG, loop=True, frames=30, seq=spec(S["crouch_idle"]),
             root_dz=crouch_root_dz),
        dict(cat=CAT_JUMP, layer=LYR_LEG, loop=False, frames=native_frames(S["jump"]),
             seq=spec(S["jump"], 1.0, True)),
        # ---- upper layer ----
        dict(cat=CAT_IDLE, layer=LYR_UP, loop=True, frames=41,
             seq=spec(S["idle1"], 122.0 / 41.0)),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["ref_aim_carbine"], 1.0, False, AIM_BLEND[0])),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["ref_aim_carbine"], 1.0, False, AIM_BLEND[1])),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["ref_aim_carbine"], 1.0, False, AIM_BLEND[2])),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["crouch_aim_carbine"], 1.0, False, AIM_BLEND[0])),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["crouch_aim_carbine"], 1.0, False, AIM_BLEND[1])),
        dict(cat=CAT_AIM, layer=LYR_UP, loop=True, frames=2,
             seq=spec(S["crouch_aim_carbine"], 1.0, False, AIM_BLEND[2])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["ref_shoot_carbine"], 1.0, False, AIM_BLEND[0])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["ref_shoot_carbine"], 1.0, False, AIM_BLEND[1])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["ref_shoot_carbine"], 1.0, False, AIM_BLEND[2])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["crouch_shoot_carbine"], 1.0, False, AIM_BLEND[0])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["crouch_shoot_carbine"], 1.0, False, AIM_BLEND[1])),
        dict(cat=CAT_SHOOT, layer=LYR_UP, loop=True, frames=10,
             seq=spec(S["crouch_shoot_carbine"], 1.0, False, AIM_BLEND[2])),
        # the brain's reload takes 3.0 s; the game's own reload anim is 2.03 s, stretched to fit
        dict(cat=CAT_RELOAD, layer=LYR_UP, loop=False, frames=90,
             seq=spec(S["ref_reload_carbine"], (61 / 30.0) / 3.0, True)),
        dict(cat=CAT_RELOAD, layer=LYR_UP, loop=False, frames=90,
             seq=spec(S["crouch_reload_carbine"],
                      (S["crouch_reload_carbine"].numframes / S["crouch_reload_carbine"].fps) / 3.0, True)),
        dict(cat=CAT_FLINCH, layer=LYR_UP, loop=False, frames=native_frames(S["gut_flinch"]),
             seq=spec(S["gut_flinch"], 1.0, True)),
        dict(cat=CAT_FLINCH, layer=LYR_UP, loop=False, frames=native_frames(S["head_flinch"]),
             seq=spec(S["head_flinch"], 1.0, True)),
        # ---- full layer ----
        dict(cat=CAT_DEATH, layer=LYR_FULL, loop=False, frames=36, seq=spec(S["death1"], 1.0, True)),
        dict(cat=CAT_DEATH, layer=LYR_FULL, loop=False, frames=41, seq=spec(S["death2"], 1.0, True)),
        dict(cat=CAT_DEATH, layer=LYR_FULL, loop=False, frames=106, seq=spec(S["death3"], 1.0, True)),
    ]
    seqs = bake(urban, comb, plan)
    corners = collect_corners(urban, weapon, comb, scale)
    tris = len(corners) // 3
    print("corners %d, triangles %d, baked sequences %d, frames %d"
          % (len(corners), tris, len(seqs), sum(len(f) for _c, _ly, _l, f in seqs)))

    stride_walk, v_walk = measure_stride(urban, S["walk"], scale)
    stride_run, v_run = measure_stride(urban, S["run"], scale)
    # the strafe and back variants ARE the run cycle (see the plan), so they
    # share its stride by construction
    stride_strafe = stride_run
    stride_back = stride_run
    print("strides: walk %.3f m (feet match the ground at %.2f m/s), run %.3f m (at %.2f m/s), strafe %.3f m, back %.3f m"
          % (stride_walk, v_walk, stride_run, v_run, stride_strafe, stride_back))
    muzzle, butt = gun_anchors(urban, weapon, comb, scale)

    # per-bone layer + waist-twist weights for the runtime's two-layer compose:
    # lower bones (root, pelvis, legs) come from the gait layer; spine and up
    # come from the aim layer; the aim-vs-movement yaw difference is distributed
    # over the three spine bones so the torso twists like a real man's.
    legs_set = leg_bone_set(urban)
    bone_layer = []
    twist = []
    TWIST_BY_NAME = {"Bip01 Spine": 0.40, "Bip01 Spine1": 0.35, "Bip01 Spine2": 0.25}
    for i, name in enumerate(comb.names):
        bone_layer.append(0 if i in legs_set else 1)
        twist.append(TWIST_BY_NAME.get(name, 0.0))
    print("lower bones: %d of %d; twist weights sum %.2f"
          % (sum(1 for v in bone_layer if v == 0), comb.count, sum(twist)))

    textures = [(t.w, t.h) for t in urban.textures] + [(t.w, t.h) for t in weapon.textures]
    blob = bytearray()
    blob += b"CSM1"
    blob += struct.pack("<I", 3)  # version 3: layers stored separately + bone layer/twist tables
    blob += struct.pack("<IIII", comb.count, len(corners), tris, len(textures))
    for w, h in textures:
        blob += struct.pack("<HH", w, h)
    blob += struct.pack("<I", len(seqs))
    for cat, layer, loop, frames in seqs:
        blob += struct.pack("<BBBHf", cat, layer, loop, len(frames), len(frames) / OUT_FPS)
    blob += struct.pack("<ff", scale, zshift)
    blob += struct.pack("<ffff", stride_walk, stride_run, stride_strafe, stride_back)
    for _posed, bone, local in (muzzle, butt):
        blob += struct.pack("<i3f", bone, local[0], local[1], local[2])
    for p in comb.parents:
        blob += struct.pack("<i", p)
    for v in bone_layer:
        blob += struct.pack("<B", v)
    for wgt in twist:
        blob += struct.pack("<f", wgt)
    for pos, n, uv, bone, _tex in corners:
        nn = np.array(n, dtype=np.float64)
        ln = np.linalg.norm(nn) or 1.0
        nn = (nn / ln * 32767).astype(np.int16)
        blob += struct.pack("<3f3h2fH", pos[0], pos[1], pos[2],
                            int(nn[0]), int(nn[1]), int(nn[2]), uv[0], uv[1], bone)
    for i in range(tris):
        blob += struct.pack("<B", corners[3 * i][4])
    for _cat, _layer, _loop, frames in seqs:
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
