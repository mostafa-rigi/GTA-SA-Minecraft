#!/usr/bin/env python3
"""convert_po.py - bake Po (Kung Fu Panda) into po.bin (format POM1 v1) + po_tex0.png.

Inputs (all in ~/workspace/kfp-panda, the extraction audit's verified inventory):
  po_raw.json                          the verified live Ninja Ripper rip: two skinned
                                       meshes with full blend weights (bind/skeleton space)
  samples/skeleton_4gw.json            Po's 108-bone skeleton (bind world matrices,
                                       verified bone-by-bone against the .4gw tail)
  po_bones_named.json                  the authored parent hierarchy + bone names
  po_new_palette_mapping_final.json    rip palette slot -> skeleton bone (per mesh)
  clips_decoded_v2.json + clips_f16_v1.json
                                       the decoded animation, read through the settled
                                       hybrid reading (str_research/f16x_compose.py,
                                       mode F16B1 / rotvec) - the same reading PO_TEST3
                                       was built from
  po_tmr_flipped.png                   the 1024x1024 texture atlas of the rip

The six step-1 clips (identification evidence: PO-STEP1-fa.md):
  idle 52, walk 14, run 17, attack 61, hit 29, die 18.
The decoded keys are ROTATION-ONLY (segment quats + f16 rotation vectors; no
translation channel exists in either decoded file), so every clip plays IN
PLACE and the core entity's own movement carries him - the soldier's and the
Minecraft creatures' way. Local translations in the bake are the bind locals.

Space: Po's own space is x = his right, y = forward, z = up. The mod's
animation space is +X forward, +Y left, +Z up, so everything is conjugated by
M: (x, y, z) -> (y, -x, z) once, here, offline. The whole figure is then
scaled uniformly so his standing height is 1.50 m (his proportions untouched).

Run with the kfp tools venv python (numpy):
  ~/workspace/kfp-panda/tools-venv/bin/python tools/convert_po.py
Outputs: po/po.bin, po/po_tex0.png, po/po_bake_report.json (repo root layout,
mirroring cs-soldier/).
"""
import json
import math
import os
import shutil
import struct
import sys

import numpy as np

KFP = '/home/hatch/workspace/kfp-panda'
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(KFP, 'str_research'))
import f16x_compose as C  # the settled reading machinery (loads the skeleton + clips)

# clip index in the decoded animset -> (category, loop)
CLIPS = [
    (52, 0, True),   # PP_IDLE
    (14, 1, True),   # PP_WALK (provisional - see the report)
    (17, 2, True),   # PP_RUN (provisional - a forward-lean charge)
    (61, 3, False),  # PP_ATTACK
    (29, 4, False),  # PP_HIT
    (18, 5, False),  # PP_DIE (provisional - a knockback fall)
]
TARGET_HEIGHT = 1.50  # metres, presentation scale (proportions untouched)

M = np.array([[0.0, 1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, 1.0]])  # po -> mod anim space
M4 = np.eye(4)
M4[:3, :3] = M


def conv_world(W, s):
    """Bind world matrix -> mod space, uniformly scaled: R same form, t scaled."""
    W2 = M4 @ W @ M4.T
    W2[:3, 3] *= s
    return W2


def conv_quat(q, s_t=None):
    R = C.R_from_q(np.asarray(q, dtype=float))
    R2 = M @ R @ M.T
    return C.q_from_R(R2)


def main():
    raw = json.load(open(os.path.join(KFP, 'po_raw.json')))
    mapping = json.load(open(os.path.join(KFP, 'po_new_palette_mapping_final.json')))
    parents = [C.parent.get(b, -1) for b in range(108)]
    parents = [(-1 if p is None else int(p)) for p in parents]
    # The authored hierarchy is not index-ordered (skull_top 7 hangs on head 37,
    # neck 25 on spine_03 31, the hands on the wrist twists...). The file writes
    # bones in topological order (every parent before its children - the core
    # loader's invariant), and every bone reference in the file (palette,
    # inverse binds, animation tracks) is renumbered with the same permutation.
    children = {}
    for b, p in enumerate(parents):
        children.setdefault(p, []).append(b)
    order = []
    stack = list(children.get(-1, []))
    while stack:
        b = stack.pop()
        order.append(b)
        stack.extend(children.get(b, []))
    assert len(order) == 108, len(order)
    new_index = {old: new for new, old in enumerate(order)}
    parents = [(new_index[parents[old]] if parents[old] >= 0 else -1) for old in order]

    # ---- bind skinning in po space (for measurements) ----------------------
    W = [C.Wmat[b].copy() for b in range(108)]
    # Bones 85/86 (forearm_sub_l/r) are stored position-only in the .4gw tail:
    # their bind rotation is all zeros. Vertices do skin to them, and the only
    # sane reading of a position-only helper is "the parent's orientation at
    # my origin" - so their world rotation is inherited from the parent bone.
    # No clip keys them; their local stays the repaired bind local throughout.
    REPAIRED = {}
    for b in (85, 86):
        par = int(C.parent[b])
        W[b][:3, :3] = W[par][:3, :3]
        t_loc_rep = (np.linalg.inv(W[par]) @ np.append(W[b][:3, 3], 1.0))[:3]
        REPAIRED[b] = t_loc_rep
    IBM = [np.linalg.inv(w) for w in W]

    meshes = []
    for name in ('Mesh_0017.rip', 'Mesh_0016.rip'):
        m = raw[name]
        meshes.append((name, m['verts'], m['faces']))

    def resolve_influences(v, mesh_name):
        """(bone, weight) pairs for one rip vertex. The palette mapping covers
        every slot of the body mesh and all but three slots of the head mesh;
        those three (24/34/35, weight on 7 vertices of 5,939) fold into the
        vertex's strongest mapped influence."""
        pairs = []
        unmapped = 0.0
        for slot, wgt in zip(v[3], v[4]):
            if wgt <= 0:
                continue
            key = f'{mesh_name}|{int(slot)}'
            if key in mapping:
                pairs.append([int(mapping[key][0]), float(wgt)])
            else:
                unmapped += float(wgt)
        if not pairs:
            return [[0, 1.0]]
        if unmapped > 0:
            pairs.sort(key=lambda p: -p[1])
            pairs[0][1] += unmapped
        tot = sum(p[1] for p in pairs)
        return [[b, w / tot] for b, w in pairs]

    def skin_bind(verts_of_mesh, mesh_name):
        out = []
        for v in verts_of_mesh:
            pos = np.array(v[0], dtype=float)
            acc = np.zeros(3)
            for bone, wgt in resolve_influences(v, mesh_name):
                acc += wgt * (W[bone] @ IBM[bone] @ np.append(pos, 1.0))[:3]
            out.append(acc)
        return out

    bind_all = []
    for name, verts, faces in meshes:
        bind_all.extend(skin_bind(verts, name))
    bind_all = np.array(bind_all)
    height = float(bind_all[:, 2].max() - bind_all[:, 2].min())
    s = TARGET_HEIGHT / height
    lift = -float(bind_all[:, 2].min()) * s
    print(f'bind height {height:.3f} (skeleton units) -> scale {s:.4f}, lift {lift:.4f} m')

    # ---- converted skeleton -------------------------------------------------
    Wm = [conv_world(w, s) for w in W]
    IBMm = [np.linalg.inv(Wm[old]) for old in order]

    # ---- vertices (non-indexed expansion, soldier-style) --------------------
    out_verts = []  # (pos3, nrm3, uv2, bones4, weights4)
    for name, verts, faces in meshes:
        conv = []
        for v in verts:
            pos = s * (M @ np.array(v[0], dtype=float))
            nrm = M @ np.array(v[1], dtype=float)
            nrm = nrm / max(1e-9, float(np.linalg.norm(nrm)))
            bones, wgts = [], []
            for bone, wgt in resolve_influences(v, name):
                bones.append(new_index[bone])
                wgts.append(wgt)
            while len(bones) < 4:
                bones.append(0)
                wgts.append(0.0)
            conv.append((pos, nrm, v[2], bones[:4], wgts[:4]))
        for f in faces:
            for ci in f:
                out_verts.append(conv[ci])
    nverts = len(out_verts)
    ntris = nverts // 3
    print(f'verts {nverts} (non-indexed), tris {ntris}')

    # ---- sequences -----------------------------------------------------------
    seqs = []
    anim = []  # flat list of (pos3, quat4) per seq-frame-bone, in bake order
    strides = {}

    def nlerp_q(a, b, w):
        a = np.asarray(a, dtype=float)
        b = np.asarray(b, dtype=float)
        if np.dot(a, b) < 0.0:
            b = -b
        v = a + (b - a) * w
        n = np.linalg.norm(v)
        return v / n if n > 1e-9 else a

    def dev_from_smooth(clip_idx, bone):
        """Mean angle (deg) between a bone's local track and its own
        5-key smoothed self - the corruption discriminator: real motion,
        however fast, tracks its smoothing (tenths of a degree); a
        mis-sliced track record deviates everywhere, all the time."""
        la_ = C.make_local_fn(clip_idx, 'F16B1', 'rotvec')
        S_ = C.SEG[clip_idx]['S']
        qs = np.array([la_(bone, t) for t in range(0, S_ + 1)], dtype=float)
        qs = qs / np.linalg.norm(qs, axis=1, keepdims=True)
        for i in range(1, len(qs)):
            if np.dot(qs[i], qs[i - 1]) < 0.0:
                qs[i] = -qs[i]
        dev = []
        for i in range(len(qs)):
            lo_, hi_ = max(0, i - 2), min(len(qs), i + 3)
            v = qs[lo_:hi_].mean(axis=0)
            v = v / np.linalg.norm(v)
            d = abs(float(np.dot(qs[i], v)))
            dev.append(math.degrees(2 * math.acos(min(1.0, d))))
        return float(np.mean(dev))

    # Corrupt-track freeze (fix round): in the CALM idle clip, where the
    # whole body moves by tenths of a degree, five bones' decoded tracks
    # are convicted corrupt by TWO independent signals - they deviate
    # from their own smoothed selves by 2.6-21.5 degrees sustained, AND
    # their world origins teleport ~2 cm per key while a healthy bone's
    # moves 0.6 mm (old 42: 21.5 deg and 1.8 cm). Those records are
    # mis-sliced in the decoded animset, not motion. Their bones are
    # frozen at the bind local rotation in EVERY clip: the honest value
    # until the tracks are re-decoded (a named later step). The census
    # is computed here from the idle clip - never hand-picked - and a
    # bone failing either signal is left alone (old 2 deviates 2.25 deg
    # but its origin never moves: a subtle rotator, not corruption).
    def idle_world_step_max(bone):
        la_ = C.make_local_fn(52, 'F16B1', 'rotvec')
        pts = [C.world_pos(la_, bone, t) for t in range(0, C.SEG[52]['S'] + 1)]
        return max(float(np.linalg.norm(np.asarray(pts[i + 1]) - np.asarray(pts[i])))
                   for i in range(len(pts) - 1))

    FROZEN = sorted(b for b in range(108)
                    if b not in REPAIRED and dev_from_smooth(52, b) > 2.0
                    and idle_world_step_max(b) > 0.005)
    print('corrupt tracks frozen at bind (idle deviation + teleport census):', FROZEN)
    faded = {}
    for clip_idx, cat, loop in CLIPS:
        la = C.make_local_fn(clip_idx, 'F16B1', 'rotvec')
        S = C.SEG[clip_idx]['S']
        # fix round: bake at the clips' NATIVE 60 fps (step 1 took every
        # second key - 30 fps - which halved the motion detail and read as
        # stiffness in game). Loop clips drop the duplicate endpoint key:
        # frame k is the source key k and the period is S/60 exactly, so
        # the loader's wrap blend crosses the seam over one native step.
        tags = list(range(0, S)) if loop else list(range(0, S + 1))
        # per-bone key lists first, so the two declared repairs below act
        # on whole tracks, not on frames streaming past
        keys = {}
        for b in range(108):
            if b in FROZEN:
                keys[b] = [np.asarray(C.q0[b], dtype=float)] * len(tags)
            else:
                keys[b] = [np.asarray(la(b, t), dtype=float) for t in tags]
        if loop:
            # seam fade: these are cinematic clips forced to loop; several
            # healthy bones end 20-178 degrees away from their start, and
            # the loader's wrap blend would snap that whole error across
            # one step at every cycle. The last 12 keys are eased toward
            # frame 0 so the wrap crosses only a twelfth of the residue
            # per step. Declared here and in the report; the real cure is
            # the multiplayer animset's true gait clips (a later step).
            K = 12
            faded_here = []
            for b in range(108):
                if b in FROZEN or b in REPAIRED:
                    continue
                q_end = np.asarray(la(b, S), dtype=float)
                q0v = keys[b][0]
                d = abs(float(np.dot(q_end / np.linalg.norm(q_end), q0v / np.linalg.norm(q0v))))
                closure = math.degrees(2 * math.acos(min(1.0, d)))
                if closure > 20.0:
                    faded_here.append(b)
                    for idx, t in enumerate(tags):
                        if t >= S - K:
                            w = (t - (S - K) + 1) / (K + 1)
                            keys[b][idx] = nlerp_q(keys[b][idx], q0v, w)
            if faded_here:
                faded[clip_idx] = faded_here
                print(f'clip {clip_idx}: seam fade on bones {faded_here}')
        for fi, t in enumerate(tags):
            frame = []
            for b in range(108):
                if b in REPAIRED:
                    q = np.array([0.0, 0.0, 0.0, 1.0])
                    tl = REPAIRED[b]
                else:
                    q = keys[b][fi]  # the clips are rotation-only, so the
                    # local translation is the bind local (C.t_loc) always
                    tl = C.t_loc[b]
                qq = conv_quat(q)
                tt = s * (M @ np.asarray(tl, dtype=float))
                frame.append((tt, qq))
            for old in order:  # write the frame in the file's bone order
                anim.append(frame[old])
        seqs.append({'cat': cat, 'loop': loop, 'frames': len(tags), 'dur': S / 60.0,
                     'clip': clip_idx, 'S': S})
        # stride: foot fore-aft (po y) peak-to-peak, robust percentiles
        ys = {35: [], 36: []}
        diff = []
        for t in range(0, S + 1, 2):
            p35 = C.world_pos(la, 35, t)
            p36 = C.world_pos(la, 36, t)
            ys[35].append(p35[1])
            ys[36].append(p36[1])
            diff.append(p35[1] - p36[1])
        def rng(a):
            a = sorted(a)
            return a[int(0.95 * (len(a) - 1))] - a[int(0.05 * (len(a) - 1))]
        stride = 0.5 * (rng(ys[35]) + rng(ys[36])) * s
        cyc = sum(1 for i in range(1, len(diff)) if (diff[i - 1] < 0) != (diff[i] < 0)) / 2.0
        strides[cat] = (stride, cyc)
        print(f'clip {clip_idx} cat {cat}: S {S}, {len(tags)} frames, dur {S/60:.2f}s, '
              f'stride {stride:.3f} m, cycles {cyc:.1f}')

    # ---- write po.bin ---------------------------------------------------------
    outdir = os.path.join(REPO, 'po')
    os.makedirs(outdir, exist_ok=True)
    buf = bytearray()
    buf += b'POM1'
    buf += struct.pack('<IIII', 1, 108, nverts, ntris)
    buf += struct.pack('<I', 1)            # ntex
    buf += struct.pack('<HH', 1024, 1024)  # the atlas
    buf += struct.pack('<I', len(seqs))
    for sq in seqs:
        buf += struct.pack('<BBHf', sq['cat'], 1 if sq['loop'] else 0, sq['frames'], sq['dur'])
    buf += struct.pack('<ff', s, lift)
    buf += struct.pack('<ff', strides[1][0], strides[2][0])  # walk, run strides
    for p in parents:
        buf += struct.pack('<i', p)
    for ib in IBMm:
        buf += struct.pack('<12f', *ib[:3, :3].reshape(-1), *ib[:3, 3])
    for (pos, nrm, uv, bones, wgts) in out_verts:
        buf += struct.pack('<3f', *pos)
        buf += struct.pack('<3h', *[int(round(x * 32767)) for x in nrm])
        buf += struct.pack('<2f', uv[0], uv[1])
        buf += struct.pack('<4B', *bones)
        # weights to u8, largest remainder so they sum to exactly 255
        raw_w = [w * 255.0 for w in wgts]
        fl = [int(math.floor(x)) for x in raw_w]
        rem = 255 - sum(fl)
        order = sorted(range(4), key=lambda i: raw_w[i] - fl[i], reverse=True)
        for i in order[:rem]:
            fl[i] += 1
        buf += struct.pack('<4B', *fl)
    for (pos, q) in anim:
        buf += struct.pack('<3f', *pos)
        buf += struct.pack('<4h', *[int(round(float(x) * 32767)) for x in q])
    path = os.path.join(outdir, 'po.bin')
    open(path, 'wb').write(bytes(buf))
    shutil.copy(os.path.join(KFP, 'po_tmr_flipped.png'), os.path.join(outdir, 'po_tex0.png'))
    report = {'po_bin_bytes': len(buf), 'bones': 108, 'verts': nverts, 'tris': ntris,
              'scale': s, 'bind_height_units': height, 'lift': lift,
              'seqs': seqs, 'strides': {str(k): v for k, v in strides.items()},
              'frozen_corrupt_tracks': FROZEN,
              'seam_faded': {str(k): v for k, v in faded.items()}}
    json.dump(report, open(os.path.join(outdir, 'po_bake_report.json'), 'w'), indent=1)
    print('wrote', path, len(buf), 'bytes')


if __name__ == '__main__':
    main()
