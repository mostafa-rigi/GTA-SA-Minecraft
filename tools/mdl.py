#!/usr/bin/env python3
"""A GoldSrc (Half-Life / Counter-Strike 1.6) MDL reader, written from scratch from the
format as it lies in the file: header, bones, bone controllers, animation sequences with
their RLE-compressed value streams, body parts / models / meshes with the fan-and-strip
triangle stream, embedded palette textures, and attachments.

On top of the reader sits a poser: it evaluates a sequence frame the way the game's own
studio renderer does - per-bone local position and quaternion from the base values plus
the frame's animation offsets, world matrices down the bone hierarchy, and every vertex
rigidly carried by its one bone (GoldSrc has no vertex weights).

Nothing here is copied from another parser; every struct size is asserted against the
file in front of us, and the proof script (prove_mdl.py) checks the posed result with
numbers before anything is trusted.
"""

import struct
import math


def _str(raw):
    return raw.split(b"\0")[0].decode("ascii", "replace")


class Bone:
    __slots__ = ("name", "parent", "controllers", "value", "scale")


class Seq:
    __slots__ = ("name", "fps", "numframes", "anim_off", "numbones", "linearmovement", "numblends")


class Texture:
    __slots__ = ("name", "flags", "w", "h", "pixels", "palette")


class Mesh:
    # tris: list of triangles; a triangle is three corners; a corner is
    # (vertex index, normal index, s, t) exactly as the stream carries it
    __slots__ = ("skinref", "tris")


class Model:
    __slots__ = ("name", "verts", "vertbone", "norms", "meshes")


class MDL:
    def __init__(self, data: bytes):
        self.data = data
        d = data
        if len(d) < 236 or d[0:4] != b"IDST":
            raise ValueError("not an IDST model file")
        (self.version,) = struct.unpack_from("<i", d, 4)
        if self.version != 10:
            raise ValueError("MDL version %d, expected 10" % self.version)
        self.name = _str(d[8:72])
        (self.length,) = struct.unpack_from("<i", d, 72)
        self.eye = struct.unpack_from("<3f", d, 76)
        self.bbmin = struct.unpack_from("<3f", d, 112)
        self.bbmax = struct.unpack_from("<3f", d, 124)
        i = lambda off: struct.unpack_from("<i", d, off)[0]
        self.flags = i(136)
        numbones, boneindex = i(140), i(144)
        nctrl, ctrlindex = i(148), i(152)
        numseq, seqindex = i(164), i(168)
        numtex, texindex = i(180), i(184)
        numbodyparts, bodypartindex = i(204), i(208)
        numattach, attachindex = i(212), i(216)
        if not (0 < numbones <= 256 and 236 <= boneindex < len(d)):
            raise ValueError("header bone table does not check out (numbones=%d, boneindex=%d)"
                             % (numbones, boneindex))

        # ---- bones (mstudiobone_t, 112 bytes)
        self.bones = []
        for n in range(numbones):
            off = boneindex + n * 112
            b = Bone()
            b.name = _str(d[off:off + 32])
            b.parent, = struct.unpack_from("<i", d, off + 32)
            b.controllers = struct.unpack_from("<6i", d, off + 40)
            b.value = struct.unpack_from("<6f", d, off + 64)
            b.scale = struct.unpack_from("<6f", d, off + 88)
            self.bones.append(b)
        if not self.bones[0].name:
            raise ValueError("first bone has no name - header layout is wrong")

        # ---- bone controllers (mstudiobonecontroller_t, 24 bytes)
        self.controllers = []
        for n in range(nctrl):
            off = ctrlindex + n * 24
            bone, typ = struct.unpack_from("<2i", d, off)
            start, end = struct.unpack_from("<2f", d, off + 8)
            rest, index = struct.unpack_from("<2i", d, off + 16)
            self.controllers.append((bone, typ, start, end, rest, index))

        # ---- sequences (mstudioseqdesc_t, 176 bytes)
        self.seqs = []
        for n in range(numseq):
            off = seqindex + n * 176
            s = Seq()
            s.name = _str(d[off:off + 32])
            s.fps, = struct.unpack_from("<f", d, off + 32)
            s.numframes, = struct.unpack_from("<i", d, off + 56)
            s.linearmovement = struct.unpack_from("<3f", d, off + 76)
            numblends, animindex = struct.unpack_from("<2i", d, off + 120)
            s.anim_off = animindex
            s.numblends = numblends
            s.numbones = numbones
            self.seqs.append(s)
        self.seq_by_name = {s.name: s for s in self.seqs}

        # ---- textures (mstudiotexture_t, 80 bytes)
        self.textures = []
        for n in range(numtex):
            off = texindex + n * 80
            t = Texture()
            t.name = _str(d[off:off + 64])
            t.flags, t.w, t.h, dataoff = struct.unpack_from("<4i", d, off + 64)
            npix = t.w * t.h
            t.pixels = d[dataoff:dataoff + npix]
            t.palette = d[dataoff + npix:dataoff + npix + 768]
            if len(t.pixels) != npix or len(t.palette) != 768:
                raise ValueError("texture %s runs past the end of the file" % t.name)
            self.textures.append(t)

        # ---- body parts -> models -> meshes
        self.bodyparts = []  # (name, [Model])
        for bp in range(numbodyparts):
            off = bodypartindex + bp * 76
            bpname = _str(d[off:off + 64])
            nummodels, base, modelindex = struct.unpack_from("<3i", d, off + 64)
            models = []
            for mi in range(nummodels):
                moff = modelindex + mi * 112
                m = Model()
                m.name = _str(d[moff:moff + 64])
                nummesh, meshindex = struct.unpack_from("<2i", d, moff + 72)
                numverts, vertinfoindex, vertindex = struct.unpack_from("<3i", d, moff + 80)
                numnorms, norminfoindex, normindex = struct.unpack_from("<3i", d, moff + 92)
                verts = struct.unpack_from("<%df" % (numverts * 3), d, vertindex)
                m.verts = [(verts[k * 3], verts[k * 3 + 1], verts[k * 3 + 2]) for k in range(numverts)]
                m.vertbone = list(d[vertinfoindex:vertinfoindex + numverts])
                norms = struct.unpack_from("<%df" % (numnorms * 3), d, normindex)
                m.norms = [(norms[k * 3], norms[k * 3 + 1], norms[k * 3 + 2]) for k in range(numnorms)]
                m.meshes = []
                for shi in range(nummesh):
                    soff = meshindex + shi * 20
                    numtris, triindex, skinref = struct.unpack_from("<3i", d, soff)
                    mesh = Mesh()
                    mesh.skinref = skinref
                    mesh.tris = self._read_tris(triindex, m)
                    if len(mesh.tris) != numtris:
                        raise ValueError("mesh of %s: header says %d triangles, stream gives %d"
                                         % (m.name, numtris, len(mesh.tris)))
                    m.meshes.append(mesh)
                models.append(m)
            self.bodyparts.append((bpname, models))

        # ---- attachments (mstudioattachment_t, 88 bytes)
        self.attachments = []
        for n in range(numattach):
            off = attachindex + n * 88
            aname = _str(d[off:off + 32])
            atype, abone = struct.unpack_from("<2i", d, off + 32)
            org = struct.unpack_from("<3f", d, off + 40)
            self.attachments.append((aname, atype, abone, org))

    def _read_tris(self, off, model):
        """The triangle stream: runs of <count> then count * (vert, norm, s, t).
        A negative count is a fan, a positive count a strip."""
        d = self.data
        tris = []
        while True:
            (count,) = struct.unpack_from("<h", d, off)
            off += 2
            if count == 0:
                break
            fan = count < 0
            n = abs(count)
            strip = []
            for k in range(n):
                strip.append(struct.unpack_from("<4h", d, off))
                off += 8
            if fan:
                for k in range(1, n - 1):
                    tris.append((strip[0], strip[k], strip[k + 1]))
            else:
                for k in range(n - 2):
                    if k % 2 == 0:
                        tris.append((strip[k], strip[k + 1], strip[k + 2]))
                    else:
                        tris.append((strip[k + 1], strip[k], strip[k + 2]))
        return tris

    # ------------------------------------------------------------ animation
    def _anim_value(self, stream_off, frame):
        """One channel's value at a frame, out of the RLE stream at stream_off."""
        d = self.data
        off = stream_off
        while True:
            valid = d[off]
            total = d[off + 1]
            if frame < total:
                if frame < valid:
                    (v,) = struct.unpack_from("<h", d, off + 2 + frame * 2)
                else:
                    (v,) = struct.unpack_from("<h", d, off + 2 + (valid - 1) * 2)
                return v
            frame -= total
            off += 2 + valid * 2

    def bone_pose(self, seq, frame):
        """(pos, quat) per bone at a frame of a sequence, locals relative to the parent.
        quat is (x, y, z, w). Bone controllers are not driven (they steer mouths and
        eyes on these models; their channels rest at the base value)."""
        d = self.data
        out = []
        anim_base = seq.anim_off
        for bi, bone in enumerate(self.bones):
            aoff = anim_base + bi * 12  # mstudioanim_t: 6 unsigned-short offsets
            offsets = struct.unpack_from("<6H", d, aoff)
            vals = list(bone.value)
            for ch in range(6):
                if offsets[ch]:
                    raw = self._anim_value(aoff + offsets[ch], frame)
                    vals[ch] += raw * bone.scale[ch]
            pos = (vals[0], vals[1], vals[2])
            out.append((pos, _angle_quaternion(vals[3], vals[4], vals[5])))
        return out

    def bone_matrices(self, seq, frame):
        """World matrix per bone (4x4, row-major, column-vector use: v' = M v)."""
        locals_ = self.bone_pose(seq, frame)
        mats = []
        for bi, bone in enumerate(self.bones):
            pos, q = locals_[bi]
            m = _quat_matrix(pos, q)
            if bone.parent >= 0:
                m = _mat_mul(mats[bone.parent], m)
            mats.append(m)
        return mats


def _angle_quaternion(x, y, z):
    """The studio AngleQuaternion: q = qz * qy * qx over the bone's (x, y, z) angles."""
    sx, cx = math.sin(x * 0.5), math.cos(x * 0.5)
    sy, cy = math.sin(y * 0.5), math.cos(y * 0.5)
    sz, cz = math.sin(z * 0.5), math.cos(z * 0.5)
    return (sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz)


def _quat_matrix(pos, q):
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    return [
        [1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy), pos[0]],
        [2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx), pos[1]],
        [2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy), pos[2]],
        [0.0, 0.0, 0.0, 1.0],
    ]


def _mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mat_point(m, p):
    x, y, z = p
    return (m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3],
            m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3],
            m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3])
