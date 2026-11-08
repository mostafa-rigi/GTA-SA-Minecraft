#!/usr/bin/env python3
# convert_po_dff.py - the standard-format export of Po step 1: po.dff + po.txd
# built from the SAME verified data as po.bin (the file the game consumes):
# the verified Ninja Ripper rip of Po, his real 108-bone skeleton from the
# Hong Kong .4gw, and his own texture atlas.
#
# This is an offline interchange artifact - structurally validated here and
# by an rwfury read-back, NOT engine-proven; the mod's live route is
# po.bin + po_tex0.png (the soldier precedent). The section layout mirrors
# the project's proven DFF/TXD writer lineage (build_step5_dff_txd_v6.py in
# the kfp workspace): same clump/frame/geometry/material/binmesh/skin/txd
# shapes, with Po's real data in them.
#
# Space: RenderWare/SA model space, SA = (modY, modZ, modX) from the core's
# mod space - a pure axis permutation (det +1), ground origin kept, the same
# 0.7078 presentation scale baked in po.bin (1.50 m standing height).
# Matrices are stored in RenderWare's row-vector form (row = v * M).
#
# Run with the system python3 (needs numpy + PIL):
#   python3 tools/convert_po_dff.py
import json
import struct
import sys

import numpy as np
from PIL import Image

REPO = '/home/hatch/workspace/gta-minecraft-mod/src-repo'
KFP = '/home/hatch/workspace/kfp-panda'
sys.path.insert(0, KFP + '/str_research')
import f16x_compose as C  # the settled skeleton globals (Wmat, parent, ...)

LIBID = 0x1803FFFF


def sec(typ, body):
    return struct.pack('<III', typ, len(body), LIBID) + body


def rw_string(name):
    raw = name.encode('latin-1')
    pad = 4 - len(raw) % 4 if len(raw) % 4 else 4
    return raw + b'\x00' * pad


# ---------------------------------------------------------------- po.bin read
def read_po_bin(path):
    data = open(path, 'rb').read()
    off = 0

    def take(fmt):
        nonlocal off
        vals = struct.unpack_from(fmt, data, off)
        off += struct.calcsize(fmt)
        return vals

    magic = data[:4]
    off = 4
    assert magic == b'POM1', magic
    ver, bones, verts, tris, ntex = take('<5I')
    assert ver == 1 and ntex == 1
    take('<2H')  # texture size
    (nseq,) = take('<I')
    for _ in range(nseq):
        take('<BBHf')
    scale, lift, s0, s1 = take('<4f')
    parents = np.array(take('<%di' % bones), dtype=np.int64)
    ibm = np.zeros((bones, 3, 4))
    for b in range(bones):
        f = take('<12f')  # 9 rotation floats (row-major 3x3) + 3 translation
        ibm[b, :, :3] = np.array(f[:9]).reshape(3, 3)
        ibm[b, :, 3] = f[9:]
    vpos = np.zeros((verts, 3))
    vnrm = np.zeros((verts, 3))
    vuv = np.zeros((verts, 2))
    vbone = np.zeros((verts, 4), dtype=np.int64)
    vwt = np.zeros((verts, 4))
    for i in range(verts):
        vpos[i] = take('<3f')
        n = take('<3h')
        vnrm[i] = [x / 32767.0 for x in n]
        vuv[i] = take('<2f')
        vbone[i] = take('<4B')
        vwt[i] = [x / 255.0 for x in take('<4B')]
    return dict(bones=bones, verts=verts, tris=tris, parents=parents, ibm=ibm,
                pos=vpos, nrm=vnrm, uv=vuv, bone=vbone, wt=vwt, scale=scale)


def main():
    pb = read_po_bin(REPO + '/po/po.bin')
    nb = pb['bones']
    parents_topo = pb['parents']

    # ---- the topological permutation (po.bin bone k = skeleton bone
    # perm[k]): recomputed with convert_po.py's own ordering algorithm and
    # then PROVEN against the file - every stored inverse bind must equal
    # the inverse of the converted bind world of its skeleton bone, where
    # the conversion is convert_po.conv_world exactly (rotation conjugated
    # by M, translation scaled by s; bones 85/86 rotation repaired).
    s = pb['scale']
    M = np.array([[0, 1, 0], [-1, 0, 0], [0, 0, 1]], dtype=np.float64)
    children = {}
    for b in range(nb):
        p = C.parent.get(b, -1)
        p = -1 if p is None else int(p)
        children.setdefault(p, []).append(b)
    order_skel = []
    stack = list(children.get(-1, []))
    while stack:
        b = stack.pop()
        order_skel.append(b)
        stack.extend(children.get(b, []))
    assert len(order_skel) == nb
    perm = [int(b) for b in order_skel]
    Wc_mod = []
    for b in range(nb):
        W = np.array(C.Wmat[b], dtype=np.float64)
        if b in (85, 86):
            W[:3, :3] = np.array(C.Wmat[C.parent[b]], dtype=np.float64)[:3, :3]
        W2 = np.eye(4)
        W2[:3, :3] = M @ W[:3, :3] @ M.T
        W2[:3, 3] = (M @ W[:3, 3]) * s
        Wc_mod.append(W2)
    maxerr = 0.0
    for k in range(nb):
        cand = np.linalg.inv(Wc_mod[perm[k]])[:3, :]
        maxerr = max(maxerr, float(np.abs(cand - pb['ibm'][k]).max()))
    assert maxerr < 1e-3, maxerr
    print('permutation proven against the stored inverse binds, max err %.2e' % maxerr)

    names = {}
    for e in json.load(open(KFP + '/po_bones_named.json'))['bones']:
        if e.get('name'):
            names[e['index']] = e['name']
    for e in json.load(open(KFP + '/po_bones_named.json'))['bones']:
        if 'alias_of' in e and e['alias_of'] in names:
            names.setdefault(e['index'], names[e['alias_of']] + '_alias')
    bone_name = [names.get(perm[k], 'bone%d' % perm[k]) for k in range(nb)]

    # ---- bind worlds in SA space, row-vector form
    P = np.array([[0, 1, 0], [0, 0, 1], [1, 0, 0]], dtype=np.float64)  # SA = P * mod
    Wsa = []
    for b in range(nb):
        Wp = Wc_mod[b]             # mod space, converted (as baked)
        Wc = np.eye(4)             # SA space, column-vector form
        Wc[:3, :3] = P @ Wp[:3, :3] @ P.T
        Wc[:3, 3] = P @ Wp[:3, 3]
        Wsa.append(Wc)
    Wr = [W.T.copy() for W in Wsa]  # row-vector form

    hips_topo = perm.index(0)
    assert parents_topo[hips_topo] == -1

    # frame list order: frame 0 = clump root, frame 1 = hips, then the rest
    # of the topo order. Aux roots re-parent onto hips in the frame tree
    # (their locals are solved below, so their worlds are unchanged).
    frame_of_topo = {}
    order = [hips_topo] + [k for k in range(nb) if k != hips_topo]
    for i, k in enumerate(order):
        frame_of_topo[k] = i + 1
    frame_parent = [0] * (nb + 1)
    frame_local = [None] * (nb + 1)
    frame_local[0] = np.eye(4)
    for k in range(nb):
        pk = parents_topo[k]
        if k == hips_topo or pk == -1:
            # local solved against the frame parent's world (row form):
            if k == hips_topo:
                frame_parent[frame_of_topo[k]] = 0
                frame_local[frame_of_topo[k]] = Wr[perm[k]].copy()
            else:
                frame_parent[frame_of_topo[k]] = frame_of_topo[hips_topo]
                frame_local[frame_of_topo[k]] = Wr[perm[k]] @ np.linalg.inv(Wr[perm[hips_topo]])
        else:
            frame_parent[frame_of_topo[k]] = frame_of_topo[pk]
            frame_local[frame_of_topo[k]] = Wr[perm[k]] @ np.linalg.inv(Wr[perm[pk]])

    # validation: compose the frame tree (row form) and compare worlds
    world = [None] * (nb + 1)
    world[0] = np.eye(4)
    for f in range(1, nb + 1):
        world[f] = frame_local[f] @ world[frame_parent[f]]
    maxerr = 0.0
    for k in range(nb):
        err = float(np.abs(world[frame_of_topo[k]] - Wr[perm[k]]).max())
        maxerr = max(maxerr, err)
    assert maxerr < 1e-4, maxerr
    print('frame tree composes to the bind worlds, max err %.2e' % maxerr)

    # ---- frame list section
    fl = struct.pack('<I', nb + 1)
    for f in range(nb + 1):
        L = frame_local[f]
        rot = L[:3, :3].reshape(-1)
        pos = L[3, :3]
        flags = 131075 if f == 0 else 3
        fl += struct.pack('<9f3fiI', *rot, *pos, frame_parent[f] if f else -1, flags)
    exts = [sec(0x03, b'')]
    hanim = struct.pack('<5I', 256, 0, nb, 0, 36)
    for k in range(nb):
        hanim += struct.pack('<3I', k, k, 0)  # boneId = nodeIndex = topo index
    root_ext = sec(0x11E, hanim) + sec(0x253F2FE, rw_string('Root'))
    exts.append(sec(0x03, root_ext))
    for i, k in enumerate(order):
        if k == hips_topo:
            continue
        e = sec(0x11E, struct.pack('<3I', 256, k, 0))
        e += sec(0x253F2FE, rw_string(bone_name[k]))
        exts.append(sec(0x03, e))
    # exts must align 1:1 with frames: frame0 empty, frame1 root, then bones
    assert len(exts) == nb + 1, len(exts)
    framelist = sec(0x0E, sec(0x01, fl) + b''.join(exts))

    # ---- geometry (SA space), one material "po"
    pos_sa = pb['pos'] @ P.T
    nrm_sa = pb['nrm'] @ P.T
    nvert, ntri = pb['verts'], pb['tris']
    cen = pos_sa.mean(axis=0)
    rad = float(np.sqrt(((pos_sa - cen) ** 2).sum(axis=1)).max())
    g = struct.pack('<HHIII', 0x36, 1, ntri, nvert, 1)
    for (u, v) in pb['uv']:
        g += struct.pack('<2f', float(u), float(v))
    for t in range(ntri):
        f0, f1, f2 = 3 * t, 3 * t + 1, 3 * t + 2
        g += struct.pack('<4H', f0, f2, 0, f1)
    g += struct.pack('<4f', float(cen[0]), float(cen[1]), float(cen[2]), rad)
    g += struct.pack('<2I', 1, 1)
    for p in pos_sa:
        g += struct.pack('<3f', *p)
    for n in nrm_sa:
        g += struct.pack('<3f', *n)
    tex = sec(0x06, sec(0x01, bytes([6, 0x11, 1, 0])) +
              sec(0x02, rw_string('po')) + sec(0x02, b'\x00' * 4) + sec(0x03, b''))
    mstruct = struct.pack('<I4BII3f', 0, 255, 255, 255, 255, 4, 1, 1.0, 0.0, 1.0)
    mat = sec(0x07, sec(0x01, mstruct) + tex + sec(0x03, b''))
    matlist = sec(0x08, sec(0x01, struct.pack('<2i', 1, -1)) + mat)
    idx = []
    for t in range(ntri):
        idx.extend((3 * t, 3 * t + 1, 3 * t + 2))
    bm = struct.pack('<3I', 0, 1, len(idx)) + struct.pack('<2I', len(idx), 0) + \
        struct.pack('<%dI' % len(idx), *idx)
    binmesh = sec(0x50E, bm)
    used_list = sorted({int(x) for row in pb['bone'] for x in row})
    skin = struct.pack('<4B', nb, len(used_list), 4, 0) + bytes(used_list)
    for row in pb['bone']:
        skin += bytes(int(x) for x in row)
    for row in pb['wt']:
        skin += struct.pack('<4f', *[float(x) for x in row])
    for k in range(nb):
        ibm_row = np.eye(4)
        ibm_c = np.eye(4)
        ibm_c[:3, :] = pb['ibm'][k]
        ibm_row = ibm_c.T  # stored row-vector form
        skin += struct.pack('<16f', *ibm_row.reshape(-1))
    skin += struct.pack('<3I', 0, 0, 0)
    gext = sec(0x03, binmesh + sec(0x116, skin) + sec(0x253F2FD, struct.pack('<I', 0)))
    geom = sec(0x0F, sec(0x01, g) + matlist + gext)
    geolist = sec(0x1A, sec(0x01, struct.pack('<I', 1)) + geom)
    atomic = sec(0x14, sec(0x01, struct.pack('<4I', 1, 0, 5, 0)) +
                 sec(0x03, sec(0x1F, bytes([0x16, 1, 0, 0, 1, 0, 0, 0]))))
    clump = sec(0x10, sec(0x01, struct.pack('<3I', 1, 0, 0)) + framelist + geolist +
                atomic + sec(0x03, b''))
    open(REPO + '/po/po.dff', 'wb').write(clump)
    print('po.dff:', len(clump), 'bytes; frames', nb + 1, '; verts', nvert, '; tris', ntri,
          '; used bones', len(used_list))

    # ---- TXD: the one atlas texture, v6's proven native shape
    img = Image.open(REPO + '/po/po_tex0.png').convert('RGBA')
    w, h = img.size
    rgba = np.asarray(img, dtype=np.uint8)
    bgra = np.ascontiguousarray(rgba[:, :, [2, 1, 0, 3]]).tobytes()
    body = struct.pack('<2I', 9, 0x1106)
    body += 'po'.encode('latin-1').ljust(32, b'\x00')
    body += b'\x00' * 32
    body += struct.pack('<I', 1536) + struct.pack('<I', 22)
    body += struct.pack('<2H', w, h) + bytes([0x20, 1, 4, 0])
    body += struct.pack('<I', len(bgra)) + bgra
    native = sec(0x15, sec(0x01, body))
    txd = sec(0x16, sec(0x01, struct.pack('<HH', 1, 2)) + native)
    open(REPO + '/po/po.txd', 'wb').write(txd)
    print('po.txd:', len(txd), 'bytes; texture po %dx%d' % (w, h))


if __name__ == '__main__':
    main()
