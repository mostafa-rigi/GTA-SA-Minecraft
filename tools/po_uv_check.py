#!/usr/bin/env python3
"""po_uv_check.py - which texture region do Po's face/eye UVs sample?

Parses po/po.bin (POM1 v1, the loader's own layout), decodes
po/po_tex0.png, finds the eye-height front-of-head vertices, and samples
the atlas under both V conventions (v as stored, and 1-v). The eyes are
near-black patches on the atlas; the right convention puts the eye
vertices' texels on near-black, the wrong one puts them somewhere else.
"""
import struct
import sys
import zlib

import numpy as np

REPO = '/home/hatch/workspace/gta-minecraft-mod/src-repo'


def decode_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos = 8
    idat = b''
    ihdr = None
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', chunk)
        if typ == b'IDAT':
            idat += chunk
        pos += 12 + ln
    w, h, bd, ct = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    bpp = ch * (bd // 8)
    stride = w * bpp
    rows = []
    prev = bytearray(stride)
    off = 0
    for _ in range(h):
        ft = raw[off]
        off += 1
        line = bytearray(raw[off:off + stride])
        off += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(bytes(line))
        prev = line
    return np.frombuffer(b''.join(rows), dtype=np.uint8).reshape(h, w, ch)


def parse_po_bin(path):
    d = open(path, 'rb').read()
    o = 0

    def take(fmt):
        nonlocal o
        n = struct.calcsize(fmt)
        v = struct.unpack_from(fmt, d, o)
        o += n
        return v

    magic = d[:4]
    o = 4
    version, bones, verts, tris, ntex = take('<IIIII')
    texw, texh = take('<HH')
    (nseq,) = take('<I')
    for _ in range(nseq):
        take('<BBHf')
    take('<ff')  # scale, lift
    take('<ff')  # strides
    parents = [take('<i')[0] for _ in range(bones)]
    for _ in range(bones):
        take('<12f')
    V = []
    for _ in range(verts):
        px, py, pz = take('<3f')
        take('<3h')
        u, v = take('<2f')
        bn = take('<4B')
        wt = take('<4B')
        V.append((px, py, pz, u, v, bn, wt))
    return parents, V


def main():
    img = decode_png(REPO + '/po/po_tex0.png')
    lum = img[..., :3].astype(int).mean(axis=2)
    print('atlas luminance: mean %.1f, frac<40: %.4f' % (lum.mean(), (lum < 40).mean()))
    parents, V = parse_po_bin(REPO + '/po/po.bin')
    P = np.array([[v[0], v[1], v[2]] for v in V])       # bind pos, mod space (x fwd, y left, z up)
    UV = np.array([[v[3], v[4]] for v in V])
    print('verts', len(V), 'bind z range', P[:, 2].min(), P[:, 2].max())

    # head region: top of the figure; face front: forward (+x) side
    head = (P[:, 2] > 1.05)
    face = head & (P[:, 0] > 0.05)
    # eye band: the upper-front of the head (eyes sit high on the face)
    eye = face & (P[:, 2] > 1.28)
    print('head verts', int(head.sum()), 'face verts', int(face.sum()), 'eye-band verts', int(eye.sum()))

    def sample(mask, flip):
        u = np.clip(UV[mask, 0], 0, 0.9999)
        v = UV[mask, 1]
        v = 1.0 - v if flip else v
        v = np.clip(v, 0, 0.9999)
        tx = (u * 1024).astype(int)
        ty = (v * 1024).astype(int)
        vals = lum[ty, tx]
        return vals.mean(), (vals < 40).mean(), (vals < 90).mean()

    for name, mask in (('head', head), ('face', face), ('eye-band', eye)):
        for flip in (False, True):
            mean, f40, f90 = sample(mask, flip)
            print('%-8s v=%s: mean lum %6.1f  frac<40 %.3f  frac<90 %.3f'
                  % (name, '1-v' if flip else 'as-is', mean, f40, f90))

    # where do the eye-band UVs point, as a cluster?
    eu = UV[eye]
    print('eye-band uv centroid as-is: (%.3f, %.3f)  range u %.3f..%.3f v %.3f..%.3f'
          % (eu[:, 0].mean(), eu[:, 1].mean(), eu[:, 0].min(), eu[:, 0].max(), eu[:, 1].min(), eu[:, 1].max()))
    # the darkest texels of the atlas (eye patch candidates)
    ys, xs = np.where(lum < 25)
    if len(xs):
        print('dark texels (<25): %d, centroid (u %.3f, v_from_top %.3f), u %.3f..%.3f, v %.3f..%.3f'
              % (len(xs), xs.mean() / 1024, ys.mean() / 1024, xs.min() / 1024, xs.max() / 1024,
                 ys.min() / 1024, ys.max() / 1024))


if __name__ == '__main__':
    main()
