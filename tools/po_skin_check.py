#!/usr/bin/env python3
"""po_skin_check.py - the Po fix-round harness gate.

Diffs the ENGINE path (the C++ renderer's own composition, dumped by the
test suite under MCSA_PO_DUMP) against the REFERENCE skinning rebuilt
here from the source data (the verified rip + the real skeleton + the
decoded clips, through the same f16 machinery the known-good renders
used), probe for probe, vertex for vertex:

  probes (baked frame of the shipped 60 fps file = source key t):
    idle_f0 (52, t=0)  idle_f30 (52, t=60)  walk_f15 (14, t=30)
    attack_f22 (61, t=44)  die_f0 (18, t=0)

Per probe it prints: worst bone-origin error (bone named in both
numberings), worst skin-matrix error, vertex max/RMS error and the
dominant bones of the worst vertices. Then the smoothness check: the
C++ 240 Hz sweep's per-step joint-angle deltas vs the zero-order-hold
staircase of the baked frames and vs the native 60 fps keys.

Run:  tools-venv/bin/python tools/po_skin_check.py <dump-dir>
"""
import json
import math
import os
import struct
import sys

import numpy as np

KFP = '/home/hatch/workspace/kfp-panda'
sys.path.insert(0, os.path.join(KFP, 'str_research'))
import f16x_compose as C  # noqa: E402

M = np.array([[0.0, 1.0, 0.0], [-1.0, 0.0, 0.0], [0.0, 0.0, 1.0]])
M4 = np.eye(4)
M4[:3, :3] = M
TARGET_HEIGHT = 1.50

PROBES = [('idle_f0', 52, 0), ('idle_f30', 52, 60), ('walk_f15', 14, 30),
          ('attack_f22', 61, 44), ('die_f0', 18, 0)]


def build_reference():
    """Everything convert_po.py derived, recomputed from source - the
    permutation, the repaired bind worlds, the scale, the influences and
    the non-indexed expansion, in the file's own order."""
    raw = json.load(open(os.path.join(KFP, 'po_raw.json')))
    mapping = json.load(open(os.path.join(KFP, 'po_new_palette_mapping_final.json')))
    parents = [C.parent.get(b, -1) for b in range(108)]
    parents = [(-1 if p is None else int(p)) for p in parents]
    children = {}
    for b, p in enumerate(parents):
        children.setdefault(p, []).append(b)
    order = []
    stack = list(children.get(-1, []))
    while stack:
        b = stack.pop()
        order.append(b)
        stack.extend(children.get(b, []))
    assert len(order) == 108
    new_index = {old: new for new, old in enumerate(order)}

    W = [C.Wmat[b].copy() for b in range(108)]
    REPAIRED = {}
    for b in (85, 86):
        par = int(C.parent[b])
        W[b][:3, :3] = W[par][:3, :3]
        REPAIRED[b] = (np.linalg.inv(W[par]) @ np.append(W[b][:3, 3], 1.0))[:3]
    IBM = [np.linalg.inv(w) for w in W]

    meshes = [(name, raw[name]['verts'], raw[name]['faces'])
              for name in ('Mesh_0017.rip', 'Mesh_0016.rip')]

    def resolve(v, mesh_name):
        pairs = []
        unmapped = 0.0
        for slot, wgt in zip(v[3], v[4]):
            if wgt <= 0:
                continue
            key = '%s|%d' % (mesh_name, int(slot))
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

    bind_all = []
    for name, verts, faces in meshes:
        for v in verts:
            pos = np.array(v[0], dtype=float)
            acc = np.zeros(3)
            for bone, wgt in resolve(v, name):
                acc += wgt * (W[bone] @ IBM[bone] @ np.append(pos, 1.0))[:3]
            bind_all.append(acc)
    bind_all = np.array(bind_all)
    s = TARGET_HEIGHT / float(bind_all[:, 2].max() - bind_all[:, 2].min())

    def conv_world(Wm_, sc):
        W2 = M4 @ Wm_ @ M4.T
        W2[:3, 3] *= sc
        return W2

    Wm_bind = [conv_world(w, s) for w in W]
    IBMm = [np.linalg.inv(Wm_bind[old]) for old in order]  # file order

    # expanded bind vertices in file order (mod space), with file-order bones
    exp_pos, exp_bones, exp_w = [], [], []
    for name, verts, faces in meshes:
        conv = []
        for v in verts:
            pos = s * (M @ np.array(v[0], dtype=float))
            bones, wgts = [], []
            for bone, wgt in resolve(v, name):
                bones.append(new_index[bone])
                wgts.append(wgt)
            while len(bones) < 4:
                bones.append(0)
                wgts.append(0.0)
            conv.append((pos, bones[:4], wgts[:4]))
        for f in faces:
            for ci in f:
                exp_pos.append(conv[ci][0])
                exp_bones.append(conv[ci][1])
                exp_w.append(conv[ci][2])
    return {
        'order': order, 'new_index': new_index, 'parents_old': parents,
        'W': W, 'REPAIRED': REPAIRED, 's': s, 'conv_world': conv_world,
        'IBMm': IBMm, 'exp_pos': np.array(exp_pos),
        'exp_bones': exp_bones, 'exp_w': exp_w,
    }


def _nlerp_q(a, b, w):
    a = np.asarray(a, dtype=float)
    b = np.asarray(b, dtype=float)
    if np.dot(a, b) < 0.0:
        b = -b
    v = a + (b - a) * w
    n = np.linalg.norm(v)
    return v / n if n > 1e-9 else a


def _dev_from_smooth(clip_idx, bone):
    """Mirrors convert_po.py exactly: mean angle between the bone's
    local track and its own 5-key smoothed self."""
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


_FROZEN = None


def frozen_set():
    """The corrupt-track census, computed from the idle clip exactly as
    the converter computes it: deviation-from-smoothed > 2 deg AND an
    idle world-origin step over 5 mm (REPAIRED bones 85/86 excluded)."""
    global _FROZEN
    if _FROZEN is None:
        la_ = C.make_local_fn(52, 'F16B1', 'rotvec')
        S52 = C.SEG[52]['S']

        def step_max(bone):
            pts = [C.world_pos(la_, bone, t) for t in range(0, S52 + 1)]
            return max(float(np.linalg.norm(np.asarray(pts[i + 1]) - np.asarray(pts[i])))
                       for i in range(len(pts) - 1))

        _FROZEN = sorted(b for b in range(108)
                         if b not in (85, 86) and _dev_from_smooth(52, b) > 2.0
                         and step_max(b) > 0.005)
    return _FROZEN


def reference_pose(ref, clip, t):
    """World matrices (file order, mod space), skin matrices and skinned
    vertices for one clip at one source key time, composed by matrix FK
    from the f16 locals - the reference the renders were judged by. The
    converter's two declared repairs are applied identically: corrupt
    tracks frozen at bind, and the loop seam fade."""
    la = C.make_local_fn(clip, 'F16B1', 'rotvec')
    parents = ref['parents_old']
    frozen = frozen_set()
    is_loop = clip in (52, 14, 17)
    S = C.SEG[clip]['S']
    Wp = [None] * 108
    for b in ref['order']:  # topological: every parent composed before its children
        if b in ref['REPAIRED']:
            q = np.array([0.0, 0.0, 0.0, 1.0])
            tl = ref['REPAIRED'][b]
        else:
            if b in frozen:
                q = np.asarray(C.q0[b], dtype=float)
            else:
                q = np.asarray(la(b, t), dtype=float)
                if is_loop and t >= S - 12:
                    q_end = np.asarray(la(b, S), dtype=float)
                    q0v = np.asarray(la(b, 0), dtype=float)
                    d = abs(float(np.dot(q_end / np.linalg.norm(q_end),
                                         q0v / np.linalg.norm(q0v))))
                    closure = math.degrees(2 * math.acos(min(1.0, d)))
                    if closure > 20.0:
                        w = (t - (S - 12) + 1) / 13.0
                        q = _nlerp_q(q, q0v, w)
            tl = np.asarray(C.t_loc[b], dtype=float)
        L = np.eye(4)
        L[:3, :3] = C.R_from_q(np.asarray(q, dtype=float))
        L[:3, 3] = tl
        p = parents[b]
        Wp[b] = L if (p is None or p < 0) else Wp[p] @ L
    s = ref['s']
    world = [None] * 108
    for new, old in enumerate(ref['order']):
        world[new] = ref['conv_world'](Wp[old], s)
    skin = [world[i] @ ref['IBMm'][i] for i in range(108)]
    pos = np.zeros_like(ref['exp_pos'])
    for i in range(len(pos)):
        acc = np.zeros(3)
        v = np.append(ref['exp_pos'][i], 1.0)
        for k in range(4):
            w = ref['exp_w'][i][k]
            if w > 0:
                acc += w * (skin[ref['exp_bones'][i][k]] @ v)[:3]
        pos[i] = acc
    return world, skin, pos


def read_dump(path):
    d = open(path, 'rb').read()
    nb, nv = struct.unpack_from('<II', d, 0)
    o = 8
    w = np.frombuffer(d, dtype='<f4', count=nb * 12, offset=o).reshape(nb, 12)
    o += nb * 12 * 4
    sk = np.frombuffer(d, dtype='<f4', count=nb * 12, offset=o).reshape(nb, 12)
    o += nb * 12 * 4
    pos = np.frombuffer(d, dtype='<f4', count=nv * 3, offset=o).reshape(nv, 3)
    return w, sk, pos


def mat12(M4x4):
    out = np.zeros(12)
    out[:9] = M4x4[:3, :3].reshape(-1)
    out[9:] = M4x4[:3, 3]
    return out


def main():
    dumpdir = sys.argv[1]
    ref = build_reference()
    names = json.load(open(os.path.join(KFP, 'po_bones_named.json')))
    print('reference built: scale %.4f, %d expanded verts' % (ref['s'], len(ref['exp_pos'])))
    worst_all = 0.0
    for name, clip, t in PROBES:
        world, skin, pos = reference_pose(ref, clip, t)
        rw = np.array([mat12(world[i]) for i in range(108)])
        rs = np.array([mat12(skin[i]) for i in range(108)])
        cw, cs, cp = read_dump(os.path.join(dumpdir, 'po_skin_%s.bin' % name))
        origin_err = np.abs(cw[:, 9:] - rw[:, 9:])
        bone_worst = int(np.argmax(origin_err.max(axis=1)))
        vert_err = np.linalg.norm(cp - pos, axis=1)
        dom = [ref['exp_bones'][i][int(np.argmax(ref['exp_w'][i]))] for i in range(len(pos))]
        worst_idx = np.argsort(vert_err)[-5:][::-1]
        print('%s (clip %d t=%d): bone-origin max err %.5f m (file bone %d = old %d); '
              'skin-mat max err %.5f; vert max %.5f m, rms %.5f m'
              % (name, clip, t, origin_err.max(), bone_worst, ref['order'][bone_worst],
                 np.abs(cs - rs).max(), vert_err.max(), float(np.sqrt((vert_err ** 2).mean()))))
        print('   worst verts:', ', '.join('#%d err %.4f dom bone %d (old %d)'
              % (i, vert_err[i], dom[i], ref['order'][dom[i]]) for i in worst_idx))
        worst_all = max(worst_all, float(vert_err.max()))
    print('VERDICT: max vertex error over all probes: %.5f m' % worst_all)

    # ---- smoothness: the C++ sweep vs the staircases
    for clip_name, clip in (('idle', 52), ('walk', 14)):
        path = os.path.join(dumpdir, 'po_sweep_%s.bin' % clip_name)
        d = open(path, 'rb').read()
        ns, nb = struct.unpack_from('<II', d, 0)
        q = np.frombuffer(d, dtype='<f4', count=ns * nb * 4, offset=8).reshape(ns, nb, 4)
        dot = np.abs(np.sum(q[:-1] * q[1:], axis=2))
        ang = np.degrees(2 * np.arccos(np.clip(dot, 0, 1)))
        print('%s sweep (C++ sampler, %d Hz): per-step joint delta max %.3f deg, mean %.3f deg'
              % (clip_name, ns // 2, ang.max(), ang.mean()))
        # the shipped bake's own frame-to-frame jump: what a sampler with
        # NO interpolation would show as one step per baked frame, and the
        # floor the interpolated sampler is compared against. Step 1's
        # bake took every second key (30 fps); the fix round bakes the
        # native 60 fps keys.
        la = C.make_local_fn(clip, 'F16B1', 'rotvec')
        S = C.SEG[clip]['S']
        tags = list(range(0, S + 1, 1))
        prev = None
        jumps = []
        for tt in tags:
            cur = np.array([la(b, tt) for b in range(108)])
            if prev is not None:
                dd = np.abs(np.sum(prev * cur, axis=1))
                jumps.append(np.degrees(2 * np.arccos(np.clip(dd, 0, 1))).max())
            prev = cur
        print('%s baked-frame jump (what a snapping sampler would show): max %.3f deg'
              % (clip_name, max(jumps)))


if __name__ == '__main__':
    main()
