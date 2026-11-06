// The Counter-Strike soldier's real model, posed and skinned in the core.
// Data comes from cs_soldier.bin (tools/convert_cs_soldier.py): bone-local corner
// vertices, per-bone baked animation tracks at 30 fps, and the model's own textures
// as separate PNGs the bridge loads. Skinning is the GoldSrc way: a vertex hangs on
// exactly one bone, and the bone's posed matrix is its whole transform.

#include "SoldierModel.h"

#include <cstring>

#include "McModel.h" // ModelSink, ModelColor, MT_SOLDIER

namespace mc {
namespace {

struct Reader {
    const unsigned char* p = nullptr;
    size_t left = 0;
    bool Take(void* out, size_t n) {
        if (n > left)
            return false;
        std::memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    template <typename T> bool Get(T& v) { return Take(&v, sizeof(T)); }
};

struct Seq {
    int category = 0;
    bool loop = false;
    int frames = 0;
    float duration = 0;
    size_t offset = 0; // into the animation block, in bone-frame units
};

struct Vert {
    Vec3 pos;      // bone-local, metres
    Vec3 normal;   // bone-local, unit
    float u = 0, v = 0;
    int bone = 0;
};

bool gReady = false;
int gBones = 0;
std::vector<int> gParents;
std::vector<Vert> gVerts;
std::vector<unsigned char> gTriTex;
std::vector<Seq> gSeqs;
// per sequence-frame-bone: position (3 floats) then quaternion (4 int16, / 32767)
std::vector<float> gAnimPos;
std::vector<short> gAnimQuat;
size_t gFrameStride = 0; // bones per frame across all sequences laid end to end

struct Mat {
    float a[3][3];
    Vec3 t;
};

Mat QuatMat(float x, float y, float z, float w, const Vec3& pos) {
    Mat m;
    m.a[0][0] = 1 - 2 * (y * y + z * z);
    m.a[0][1] = 2 * (x * y - z * w);
    m.a[0][2] = 2 * (x * z + y * w);
    m.a[1][0] = 2 * (x * y + z * w);
    m.a[1][1] = 1 - 2 * (x * x + z * z);
    m.a[1][2] = 2 * (y * z - x * w);
    m.a[2][0] = 2 * (x * z - y * w);
    m.a[2][1] = 2 * (y * z + x * w);
    m.a[2][2] = 1 - 2 * (x * x + y * y);
    m.t = pos;
    return m;
}

Mat Mul(const Mat& A, const Mat& B) { // A * B (B applied first)
    Mat m;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            m.a[i][j] = A.a[i][0] * B.a[0][j] + A.a[i][1] * B.a[1][j] + A.a[i][2] * B.a[2][j];
    m.t = Vec3(A.a[0][0] * B.t.x + A.a[0][1] * B.t.y + A.a[0][2] * B.t.z + A.t.x,
               A.a[1][0] * B.t.x + A.a[1][1] * B.t.y + A.a[1][2] * B.t.z + A.t.y,
               A.a[2][0] * B.t.x + A.a[2][1] * B.t.y + A.a[2][2] * B.t.z + A.t.z);
    return m;
}

Vec3 Apply(const Mat& m, const Vec3& v) {
    return Vec3(m.a[0][0] * v.x + m.a[0][1] * v.y + m.a[0][2] * v.z + m.t.x,
                m.a[1][0] * v.x + m.a[1][1] * v.y + m.a[1][2] * v.z + m.t.y,
                m.a[2][0] * v.x + m.a[2][1] * v.y + m.a[2][2] * v.z + m.t.z);
}

Vec3 ApplyDir(const Mat& m, const Vec3& v) {
    return Vec3(m.a[0][0] * v.x + m.a[0][1] * v.y + m.a[0][2] * v.z,
                m.a[1][0] * v.x + m.a[1][1] * v.y + m.a[1][2] * v.z,
                m.a[2][0] * v.x + m.a[2][1] * v.y + m.a[2][2] * v.z);
}

const Seq* FindSeq(int category, int variant) {
    const Seq* found = nullptr;
    int n = 0;
    for (const Seq& s : gSeqs)
        if (s.category == category) {
            if (n == variant)
                found = &s;
            ++n;
        }
    if (!found && n > 0) { // variant past the end: wrap onto the ones that exist
        int want = variant % n, k = 0;
        for (const Seq& s : gSeqs)
            if (s.category == category && k++ == want)
                found = &s;
    }
    return found;
}

// Poses every bone of one sequence at `time` seconds into world (model-space) matrices.
bool PoseBones(const Seq& s, float time, std::vector<Mat>& out) {
    if (gBones <= 0 || s.frames <= 0)
        return false;
    float ft = s.duration > 0.0f ? time / s.duration * (float)s.frames : 0.0f;
    if (s.loop) {
        ft = std::fmod(ft, (float)s.frames);
        if (ft < 0)
            ft += s.frames;
    } else {
        ft = Clamp(ft, 0.0f, (float)s.frames - 1.0f);
    }
    int f0 = (int)std::floor(ft);
    float frac = ft - (float)f0;
    int f1 = s.loop ? (f0 + 1) % s.frames : (f0 + 1 < s.frames ? f0 + 1 : f0);
    out.resize(gBones);
    for (int b = 0; b < gBones; ++b) {
        const size_t i0 = (s.offset + (size_t)f0 * gBones + b);
        const size_t i1 = (s.offset + (size_t)f1 * gBones + b);
        const float* pa = &gAnimPos[i0 * 3];
        const float* pb = &gAnimPos[i1 * 3];
        const short* qa = &gAnimQuat[i0 * 4];
        const short* qb = &gAnimQuat[i1 * 4];
        float qx = qa[0] / 32767.0f, qy = qa[1] / 32767.0f, qz = qa[2] / 32767.0f, qw = qa[3] / 32767.0f;
        float rx = qb[0] / 32767.0f, ry = qb[1] / 32767.0f, rz = qb[2] / 32767.0f, rw = qb[3] / 32767.0f;
        if (qx * rx + qy * ry + qz * rz + qw * rw < 0.0f) {
            rx = -rx;
            ry = -ry;
            rz = -rz;
            rw = -rw;
        }
        float x = qx + (rx - qx) * frac, y = qy + (ry - qy) * frac, z = qz + (rz - qz) * frac,
              w = qw + (rw - qw) * frac;
        float len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len < 1e-6f) {
            x = qx;
            y = qy;
            z = qz;
            w = qw;
            len = std::sqrt(x * x + y * y + z * z + w * w);
            if (len < 1e-6f) {
                x = 0;
                y = 0;
                z = 0;
                w = 1;
                len = 1;
            }
        }
        Vec3 pos(pa[0] + (pb[0] - pa[0]) * frac, pa[1] + (pb[1] - pa[1]) * frac, pa[2] + (pb[2] - pa[2]) * frac);
        Mat local = QuatMat(x / len, y / len, z / len, w / len, pos);
        out[b] = gParents[b] >= 0 ? Mul(out[gParents[b]], local) : local;
    }
    return true;
}

} // namespace

bool LoadSoldierModel(const unsigned char* data, size_t size) {
    UnloadSoldierModel();
    if (!data || size < 64) {
        Log("cs_soldier.bin: missing or empty - the Counter-Strike soldier cannot appear without his model file");
        return false;
    }
    Reader rd{ data, size };
    char magic[4];
    uint32_t version = 0, bones = 0, verts = 0, tris = 0, ntex = 0, nseq = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "CSM1", 4) != 0 || !rd.Get(version) || version != 1 ||
        !rd.Get(bones) || !rd.Get(verts) || !rd.Get(tris) || !rd.Get(ntex) ||
        bones == 0 || bones > 256 || verts == 0 || verts % 3 != 0 || tris != verts / 3 || ntex == 0 || ntex > 8) {
        Log("cs_soldier.bin: bad header - the Counter-Strike soldier cannot appear");
        return false;
    }
    for (uint32_t i = 0; i < ntex; ++i) {
        uint16_t w = 0, h = 0;
        if (!rd.Get(w) || !rd.Get(h) || w == 0 || h == 0) {
            Log("cs_soldier.bin: bad texture table - the Counter-Strike soldier cannot appear");
            return false;
        }
    }
    if (!rd.Get(nseq) || nseq == 0 || nseq > 64) {
        Log("cs_soldier.bin: bad sequence table - the Counter-Strike soldier cannot appear");
        return false;
    }
    std::vector<Seq> seqs;
    size_t frameCursor = 0;
    for (uint32_t i = 0; i < nseq; ++i) {
        uint8_t cat = 0, loop = 0;
        uint16_t frames = 0;
        float dur = 0;
        if (!rd.Get(cat) || !rd.Get(loop) || !rd.Get(frames) || !rd.Get(dur) || frames == 0 || dur <= 0.0f) {
            Log("cs_soldier.bin: bad sequence entry - the Counter-Strike soldier cannot appear");
            return false;
        }
        Seq s;
        s.category = cat;
        s.loop = loop != 0;
        s.frames = frames;
        s.duration = dur;
        s.offset = frameCursor * bones;
        frameCursor += frames;
        seqs.push_back(s);
    }
    float infoScale = 0, infoLift = 0; // the converter's measurements, kept for the log line
    if (!rd.Get(infoScale) || !rd.Get(infoLift)) {
        Log("cs_soldier.bin: truncated - the Counter-Strike soldier cannot appear");
        return false;
    }
    std::vector<int> parents(bones);
    for (uint32_t i = 0; i < bones; ++i) {
        int32_t p = -2;
        if (!rd.Get(p) || p < -1 || p >= (int32_t)i) {
            Log("cs_soldier.bin: bad bone table - the Counter-Strike soldier cannot appear");
            return false;
        }
        parents[i] = p;
    }
    std::vector<Vert> vv(verts);
    for (uint32_t i = 0; i < verts; ++i) {
        float px, py, pz, u, v;
        int16_t nx, ny, nz;
        uint16_t bone;
        if (!rd.Get(px) || !rd.Get(py) || !rd.Get(pz) || !rd.Get(nx) || !rd.Get(ny) || !rd.Get(nz) ||
            !rd.Get(u) || !rd.Get(v) || !rd.Get(bone) || bone >= bones) {
            Log("cs_soldier.bin: bad vertex data - the Counter-Strike soldier cannot appear");
            return false;
        }
        vv[i].pos = Vec3(px, py, pz);
        vv[i].normal = Vec3(nx / 32767.0f, ny / 32767.0f, nz / 32767.0f);
        vv[i].u = u;
        vv[i].v = v;
        vv[i].bone = bone;
    }
    std::vector<unsigned char> tt(tris);
    for (uint32_t i = 0; i < tris; ++i) {
        uint8_t t = 0;
        if (!rd.Get(t) || t >= ntex) {
            Log("cs_soldier.bin: bad triangle data - the Counter-Strike soldier cannot appear");
            return false;
        }
        tt[i] = t;
    }
    const size_t totalBoneFrames = frameCursor * bones;
    std::vector<float> animPos(totalBoneFrames * 3);
    std::vector<short> animQuat(totalBoneFrames * 4);
    for (size_t i = 0; i < totalBoneFrames; ++i) {
        float px, py, pz;
        int16_t qx, qy, qz, qw;
        if (!rd.Get(px) || !rd.Get(py) || !rd.Get(pz) || !rd.Get(qx) || !rd.Get(qy) || !rd.Get(qz) || !rd.Get(qw)) {
            Log("cs_soldier.bin: truncated animation - the Counter-Strike soldier cannot appear");
            return false;
        }
        animPos[i * 3] = px;
        animPos[i * 3 + 1] = py;
        animPos[i * 3 + 2] = pz;
        animQuat[i * 4] = qx;
        animQuat[i * 4 + 1] = qy;
        animQuat[i * 4 + 2] = qz;
        animQuat[i * 4 + 3] = qw;
    }
    if (rd.left != 0) {
        Log("cs_soldier.bin: trailing bytes - the Counter-Strike soldier cannot appear");
        return false;
    }
    gBones = (int)bones;
    gParents = std::move(parents);
    gVerts = std::move(vv);
    gTriTex = std::move(tt);
    gSeqs = std::move(seqs);
    gAnimPos = std::move(animPos);
    gAnimQuat = std::move(animQuat);
    gFrameStride = frameCursor;
    gReady = true;
    Log("cs_soldier.bin loaded: %d bones, %d triangles, %d sequences (scale %.4f m/unit, lifted %.3f m)",
        gBones, (int)gTriTex.size(), (int)gSeqs.size(), infoScale, infoLift * infoScale);
    return true;
}

void UnloadSoldierModel() {
    gReady = false;
    gBones = 0;
    gParents.clear();
    gVerts.clear();
    gTriTex.clear();
    gSeqs.clear();
    gAnimPos.clear();
    gAnimQuat.clear();
    gFrameStride = 0;
}

bool SoldierModelReady() { return gReady; }

bool SoldierPoseSpan(int category, int variant, float time, float* loZ, float* hiZ) {
    if (!gReady)
        return false;
    const Seq* s = FindSeq(category, variant);
    if (!s)
        return false;
    std::vector<Mat> mats;
    if (!PoseBones(*s, time, mats))
        return false;
    float lo = 1e9f, hi = -1e9f;
    for (const Vert& v : gVerts) {
        float z = Apply(mats[v.bone], v.pos).z;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
    *loZ = lo;
    *hiZ = hi;
    return true;
}

void DrawSoldierModel(const Pose& base, int category, int variant, float time, float light, float r, float g, float b) {
    if (!gReady)
        return;
    const Seq* s = FindSeq(category, variant);
    if (!s)
        return;
    std::vector<Mat> mats;
    if (!PoseBones(*s, time, mats))
        return;
    // Recover his stand from the EntityPose: its axes are right / -up / -forward times
    // the mob scale, and its origin rides 1.501 m over his feet (see EntityPose).
    const float sc = base.Z.Length();
    if (sc < 1e-6f)
        return;
    const Vec3 rightv = base.X * (1.0f / sc);
    const Vec3 upv = base.Y * (-1.0f / sc);
    const Vec3 fwdv = base.Z * (-1.0f / sc);
    const Vec3 feet = base.o + base.Y * 1.501f;
    // Animation space: +X is his forward, +Y is his left, +Z is up. His own frame:
    // x right, y forward, z up.
    auto place = [&](const Vec3& a) {
        return feet + rightv * (-a.y) + fwdv * a.x + upv * a.z;
    };
    auto placeDir = [&](const Vec3& a) {
        return rightv * (-a.y) + fwdv * a.x + upv * a.z;
    };
    int curTex = -1;
    const size_t tris = gTriTex.size();
    for (size_t ti = 0; ti < tris; ++ti) {
        const int tex = gTriTex[ti];
        if (tex != curTex) {
            TheModelSink().Texture(MT_SOLDIER + tex);
            curTex = tex;
        }
        Vec3 p[4];
        float u[4], v[4];
        uint32_t col = 0;
        for (int k = 0; k < 3; ++k) {
            const Vert& vt = gVerts[ti * 3 + k];
            const Mat& m = mats[vt.bone];
            p[k] = place(Apply(m, vt.pos));
            Vec3 n = placeDir(ApplyDir(m, vt.normal));
            float nl = n.Length();
            float up = nl > 1e-6f ? Clamp(n.z / nl, 0.0f, 1.0f) : 0.0f;
            // Flat-shaded like every model the core draws: the top of a man catches
            // more light than his sides. GTA does no per-vertex lighting for him -
            // this fixed gradient is the honest stand-in, named in the report.
            float shade = 0.60f + 0.40f * up;
            if (k == 0)
                col = ModelColor(Clamp((int)(255.0f * light * r * shade), 0, 255),
                                 Clamp((int)(255.0f * light * g * shade), 0, 255),
                                 Clamp((int)(255.0f * light * b * shade), 0, 255), 255);
            u[k] = vt.u;
            v[k] = vt.v;
        }
        p[3] = p[2];
        u[3] = u[2];
        v[3] = v[2];
        TheModelSink().Quad(p, u, v, col);
    }
}

} // namespace mc
