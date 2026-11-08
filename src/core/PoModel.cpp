// Po's real model, posed and skinned in the core.
// Data comes from po.bin format v1 (tools/convert_po.py): bind-space vertices
// with up to four weighted bone influences each, per-bone inverse bind
// matrices, and six whole-body baked sequences at 30 fps. Skinning is true
// linear-blend skinning: a vertex's posed position is the weight-blended sum
// of (bone world matrix x inverse bind matrix) over its influences.

#include "PoModel.h"

#include <array>
#include <cstring>

#include "McModel.h" // ModelSink, ModelColor, MT_PO

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
    Vec3 pos;    // bind (model) space, metres
    Vec3 normal; // bind space, unit
    float u = 0, v = 0;
    int bone[4] = { 0, 0, 0, 0 };
    float w[4] = { 0, 0, 0, 0 };
};

struct Mat {
    float a[3][3];
    Vec3 t;
};

bool gReady = false;
int gBones = 0;
std::vector<int> gParents;
std::vector<Mat> gIBM; // per bone: bind world -> bone local
std::vector<Vert> gVerts;
std::vector<Seq> gSeqs;
std::vector<float> gAnimPos;
std::vector<short> gAnimQuat;
float gStrideWalk = 0.264f, gStrideRun = 0.408f;
float gLift = 0.0f; // the bind pose's lowest point, lifted onto the ground

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

const Seq* FindSeq(int category) {
    for (const Seq& s : gSeqs)
        if (s.category == category)
            return &s;
    return nullptr;
}

// Samples every bone's local of one sequence at `time` seconds, interpolated
// between the baked 30 fps frames (positions lerped, quaternions nlerped the
// short way round).
bool PoseLocals(const Seq& s, float time, std::vector<PoLocal>& out) {
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
            x = 0;
            y = 0;
            z = 0;
            w = 1;
            len = 1;
        }
        out[b].pos = Vec3(pa[0] + (pb[0] - pa[0]) * frac, pa[1] + (pb[1] - pa[1]) * frac, pa[2] + (pb[2] - pa[2]) * frac);
        out[b].q = { x / len, y / len, z / len, w / len };
    }
    return true;
}

// The sequence time a draw state means: the gaits do not run on the clock,
// they run on the ground he has covered - one cycle per stride length.
float SeqTime(const PoDrawState& st, const Seq& s) {
    if (st.category == PP_WALK || st.category == PP_RUN) {
        float phase = st.gait - std::floor(st.gait);
        return phase * s.duration;
    }
    return st.time;
}

void ComposeWorldMats(const std::vector<PoLocal>& locals, std::vector<Mat>& out) {
    out.resize(gBones);
    for (int b = 0; b < gBones; ++b) {
        const PoLocal& l = locals[b];
        Mat local = QuatMat(l.q[0], l.q[1], l.q[2], l.q[3], l.pos);
        out[b] = gParents[b] >= 0 ? Mul(out[gParents[b]], local) : local;
    }
}

// The locals the RENDERER draws: the category pose plus the cross-fade out
// of the previous composed pose (the brain snapshots the composed locals
// whenever the pose changes). The blend normalizes by wq*wq - the soldier's
// fade bug (w*wq) is the standing lesson and is not repeated here.
bool ComposeRenderLocals(const PoDrawState& st, std::vector<PoLocal>& locals) {
    PoLocal buf[128];
    if (!PoComposeLocals(st, buf, 128))
        return false;
    locals.assign(buf, buf + gBones);
    if (st.hasPrev && st.fade < 1.0f && st.prevBones == gBones) {
        const float w = Clamp(st.fade, 0.0f, 1.0f);
        const float s = w * w * (3.0f - 2.0f * w); // smoothstep
        for (int i = 0; i < gBones; ++i) {
            const PoLocal& a = st.prevLocals[i];
            PoLocal& c = locals[i];
            float dot = a.q[0] * c.q[0] + a.q[1] * c.q[1] + a.q[2] * c.q[2] + a.q[3] * c.q[3];
            float sg = dot < 0.0f ? -1.0f : 1.0f;
            float x = a.q[0] + (c.q[0] * sg - a.q[0]) * s, y = a.q[1] + (c.q[1] * sg - a.q[1]) * s,
                  z = a.q[2] + (c.q[2] * sg - a.q[2]) * s, wq = a.q[3] + (c.q[3] * sg - a.q[3]) * s;
            float len = std::sqrt(x * x + y * y + z * z + wq * wq);
            if (len > 1e-6f)
                c.q = { x / len, y / len, z / len, wq / len };
            c.pos = a.pos + (c.pos - a.pos) * s;
        }
    }
    return true;
}

// His stand in the world, recovered from the EntityPose DrawMob built him -
// the same convention as the soldier's (axes negated by the pose, the origin
// riding 1.501 m over his feet at scale 1).
struct Stand {
    Vec3 feet, right, fwd, up;
    bool ok = false;
};

Stand RecoverStand(const Pose& base) {
    Stand st;
    const float sc = base.Z.Length();
    if (sc < 1e-6f)
        return st;
    st.right = base.X * (-1.0f / sc);
    st.up = base.Y * (-1.0f / sc);
    st.fwd = base.Z * (-1.0f / sc);
    st.feet = base.o + base.Y * 1.501f + st.up * gLift;
    st.ok = true;
    return st;
}

// Animation space: +X is his forward, +Y is his left, +Z is up.
Vec3 Place(const Stand& st, const Vec3& a) {
    return st.feet + st.right * (-a.y) + st.fwd * a.x + st.up * a.z;
}
Vec3 PlaceDir(const Stand& st, const Vec3& a) {
    return st.right * (-a.y) + st.fwd * a.x + st.up * a.z;
}

// Skins every vertex once, in model space (linear blend over each vertex's
// influences), and measures the figure exactly as the draw emits it.
void SkinAllVerts(const std::vector<Mat>& mats, std::vector<Vec3>& pos, std::vector<Vec3>& nrm,
                  float* maxRadial, float* loZ, float* hiZ, bool* finite) {
    std::vector<Mat> skin(gBones);
    for (int b = 0; b < gBones; ++b)
        skin[b] = Mul(mats[b], gIBM[b]);
    pos.resize(gVerts.size());
    nrm.resize(gVerts.size());
    float mr = 0.0f, lo = 1e9f, hi = -1e9f;
    bool fin = true;
    for (size_t i = 0; i < gVerts.size(); ++i) {
        const Vert& v = gVerts[i];
        Vec3 p(0, 0, 0), n(0, 0, 0);
        for (int k = 0; k < 4; ++k) {
            if (v.w[k] <= 0.0f)
                continue;
            const Mat& m = skin[v.bone[k]];
            p = p + Apply(m, v.pos) * v.w[k];
            n = n + ApplyDir(m, v.normal) * v.w[k];
        }
        pos[i] = p;
        nrm[i] = n;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            fin = false;
        const float r = std::sqrt(p.x * p.x + p.y * p.y);
        if (r > mr)
            mr = r;
        lo = std::min(lo, p.z);
        hi = std::max(hi, p.z);
    }
    *maxRadial = mr;
    *loZ = lo;
    *hiZ = hi;
    *finite = fin;
}

bool WatchdogTrips(float maxRadial, float loZ, float hiZ, bool finite) {
    return !finite || maxRadial > kPoWatchdogRadius || loZ < kPoWatchdogLoZ || hiZ > kPoWatchdogHiZ;
}

} // namespace

bool PoComposeLocals(const PoDrawState& st, PoLocal* out, int maxBones) {
    if (!gReady || !out || maxBones < gBones)
        return false;
    const Seq* s = FindSeq(st.category);
    if (!s)
        return false;
    std::vector<PoLocal> locals;
    if (!PoseLocals(*s, SeqTime(st, *s), locals))
        return false;
    for (int b = 0; b < gBones; ++b)
        out[b] = locals[b];
    return true;
}

bool LoadPoModel(const unsigned char* data, size_t size) {
    UnloadPoModel();
    if (!data || size < 64) {
        Log("po.bin: missing or empty - Po cannot appear without his model file");
        return false;
    }
    Reader rd{ data, size };
    char magic[4];
    uint32_t version = 0, bones = 0, verts = 0, tris = 0, ntex = 0, nseq = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "POM1", 4) != 0 || !rd.Get(version) ||
        !rd.Get(bones) || !rd.Get(verts) || !rd.Get(tris) || !rd.Get(ntex) ||
        bones == 0 || bones > 128 || verts == 0 || verts % 3 != 0 || tris != verts / 3 || ntex != 1) {
        Log("po.bin: bad header - Po cannot appear");
        return false;
    }
    if (version != 1) {
        Log("po.bin: format version %u - this build needs v1; rebake it with tools/convert_po.py - Po cannot appear",
            version);
        return false;
    }
    uint16_t texW = 0, texH = 0;
    if (!rd.Get(texW) || !rd.Get(texH) || texW == 0 || texH == 0) {
        Log("po.bin: bad texture table - Po cannot appear");
        return false;
    }
    if (!rd.Get(nseq) || nseq == 0 || nseq > 16) {
        Log("po.bin: bad sequence table - Po cannot appear");
        return false;
    }
    std::vector<Seq> seqs;
    size_t frameCursor = 0;
    for (uint32_t i = 0; i < nseq; ++i) {
        uint8_t cat = 0, loop = 0;
        uint16_t frames = 0;
        float dur = 0;
        if (!rd.Get(cat) || !rd.Get(loop) || !rd.Get(frames) || !rd.Get(dur) ||
            frames == 0 || dur <= 0.0f || cat >= 16) {
            Log("po.bin: bad sequence entry - Po cannot appear");
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
    float infoScale = 0, lift = 0;
    float strides[2];
    if (!rd.Get(infoScale) || !rd.Get(lift) || !rd.Get(strides[0]) || !rd.Get(strides[1]) ||
        strides[0] <= 0.01f || strides[1] <= 0.01f) {
        Log("po.bin: bad scale/stride table - Po cannot appear");
        return false;
    }
    std::vector<int> parents(bones);
    for (uint32_t i = 0; i < bones; ++i) {
        int32_t p = -2;
        if (!rd.Get(p) || p < -1 || p >= (int32_t)i) {
            Log("po.bin: bad bone table - Po cannot appear");
            return false;
        }
        parents[i] = p;
    }
    std::vector<Mat> ibm(bones);
    for (uint32_t i = 0; i < bones; ++i) {
        float v[12];
        for (int k = 0; k < 12; ++k)
            if (!rd.Get(v[k])) {
                Log("po.bin: truncated inverse binds - Po cannot appear");
                return false;
            }
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                ibm[i].a[r][c] = v[r * 3 + c];
        ibm[i].t = Vec3(v[9], v[10], v[11]);
    }
    std::vector<Vert> vv(verts);
    for (uint32_t i = 0; i < verts; ++i) {
        float px, py, pz, u, v;
        int16_t nx, ny, nz;
        uint8_t bn[4], wt[4];
        if (!rd.Get(px) || !rd.Get(py) || !rd.Get(pz) || !rd.Get(nx) || !rd.Get(ny) || !rd.Get(nz) ||
            !rd.Get(u) || !rd.Get(v))
            goto truncated;
        for (int k = 0; k < 4; ++k)
            if (!rd.Get(bn[k]) || bn[k] >= bones)
                goto truncated;
        {
            int wsum = 0;
            for (int k = 0; k < 4; ++k) {
                if (!rd.Get(wt[k]))
                    goto truncated;
                wsum += wt[k];
            }
            if (wsum < 250 || wsum > 255)
                goto truncated;
            vv[i].pos = Vec3(px, py, pz);
            vv[i].normal = Vec3(nx / 32767.0f, ny / 32767.0f, nz / 32767.0f);
            vv[i].u = u;
            vv[i].v = v;
            for (int k = 0; k < 4; ++k) {
                vv[i].bone[k] = bn[k];
                vv[i].w[k] = wt[k] / 255.0f;
            }
        }
    }
    {
        const size_t totalBoneFrames = frameCursor * bones;
        std::vector<float> animPos(totalBoneFrames * 3);
        std::vector<short> animQuat(totalBoneFrames * 4);
        for (size_t i = 0; i < totalBoneFrames; ++i) {
            float px, py, pz;
            int16_t qx, qy, qz, qw;
            if (!rd.Get(px) || !rd.Get(py) || !rd.Get(pz) || !rd.Get(qx) || !rd.Get(qy) || !rd.Get(qz) || !rd.Get(qw))
                goto truncated;
            animPos[i * 3] = px;
            animPos[i * 3 + 1] = py;
            animPos[i * 3 + 2] = pz;
            animQuat[i * 4] = qx;
            animQuat[i * 4 + 1] = qy;
            animQuat[i * 4 + 2] = qz;
            animQuat[i * 4 + 3] = qw;
        }
        if (rd.left != 0) {
            Log("po.bin: trailing bytes - Po cannot appear");
            return false;
        }
        gBones = (int)bones;
        gParents = std::move(parents);
        gIBM = std::move(ibm);
        gVerts = std::move(vv);
        gSeqs = std::move(seqs);
        gAnimPos = std::move(animPos);
        gAnimQuat = std::move(animQuat);
        gStrideWalk = strides[0];
        gStrideRun = strides[1];
        gLift = lift;
        gReady = true;
        Log("po.bin v1 loaded: %d bones, %d triangles, %d sequences (scale %.4f, lift %.3f m, strides %.2f/%.2f m)",
            gBones, (int)tris, (int)gSeqs.size(), infoScale, gLift, gStrideWalk, gStrideRun);
        return true;
    }
truncated:
    Log("po.bin: truncated or corrupt vertex/animation data - Po cannot appear");
    return false;
}

void UnloadPoModel() {
    gReady = false;
    gBones = 0;
    gParents.clear();
    gIBM.clear();
    gVerts.clear();
    gSeqs.clear();
    gAnimPos.clear();
    gAnimQuat.clear();
    gStrideWalk = 0.264f;
    gStrideRun = 0.408f;
    gLift = 0.0f;
}

bool PoModelReady() { return gReady; }
int PoBoneCount() { return gBones; }
int PoSequenceCount() { return (int)gSeqs.size(); }
const int* PoBoneParents() { return gParents.empty() ? nullptr : gParents.data(); }

float PoPoseDuration(int category) {
    const Seq* s = FindSeq(category);
    return s ? s->duration : 0.0f;
}

float PoStride(int category) {
    if (category == PP_WALK)
        return gStrideWalk;
    if (category == PP_RUN)
        return gStrideRun;
    return 0.0f;
}

void DrawPoModel(const Pose& base, const PoDrawState& st, float light, float r, float g, float b) {
    if (!gReady)
        return;
    std::vector<PoLocal> locals;
    if (!ComposeRenderLocals(st, locals))
        return;
    std::vector<Mat> mats;
    ComposeWorldMats(locals, mats);
    const Stand stand = RecoverStand(base);
    if (!stand.ok)
        return;
    // the render watchdog, the soldier's law: measure the exact buffer about
    // to be emitted; an exploded frame is not drawn and the state is logged.
    std::vector<Vec3> pos, nrm;
    float maxRadial = 0.0f, loZ = 0.0f, hiZ = 0.0f;
    bool finite = true;
    SkinAllVerts(mats, pos, nrm, &maxRadial, &loZ, &hiZ, &finite);
    static int watchdogQuiet = 0;
    if (WatchdogTrips(maxRadial, loZ, hiZ, finite)) {
        if (watchdogQuiet <= 0) {
            Log("PO POSE EXPLOSION: radial %.2f m, z [%.2f, %.2f], finite %d | cat %d t %.2f gait %.2f fade %.2f prev %d | frame NOT drawn",
                maxRadial, loZ, hiZ, finite ? 1 : 0, st.category, st.time, st.gait, st.fade, st.hasPrev ? 1 : 0);
            watchdogQuiet = 30;
        } else {
            --watchdogQuiet;
        }
        return;
    }
    watchdogQuiet = 0;
    TheModelSink().Texture(MT_PO);
    const size_t tris = gVerts.size() / 3;
    for (size_t ti = 0; ti < tris; ++ti) {
        Vec3 p[4];
        float u[4], v[4];
        uint32_t col = 0;
        for (int k = 0; k < 3; ++k) {
            const size_t vi = ti * 3 + k;
            const Vert& vt = gVerts[vi];
            p[k] = Place(stand, pos[vi]);
            Vec3 n = PlaceDir(stand, nrm[vi]);
            float nl = n.Length();
            float up = nl > 1e-6f ? Clamp(n.z / nl, 0.0f, 1.0f) : 0.0f;
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

bool PoRenderExtents(const PoDrawState& st, float* maxRadial, float* loZ, float* hiZ) {
    if (!gReady || !maxRadial || !loZ || !hiZ)
        return false;
    std::vector<PoLocal> locals;
    if (!ComposeRenderLocals(st, locals))
        return false;
    std::vector<Mat> mats;
    ComposeWorldMats(locals, mats);
    std::vector<Vec3> pos, nrm;
    bool finite = true;
    SkinAllVerts(mats, pos, nrm, maxRadial, loZ, hiZ, &finite);
    return finite;
}

bool PoPoseExplodes(const PoDrawState& st) {
    if (!gReady)
        return false;
    std::vector<PoLocal> locals;
    if (!ComposeRenderLocals(st, locals))
        return false;
    std::vector<Mat> mats;
    ComposeWorldMats(locals, mats);
    std::vector<Vec3> pos, nrm;
    float maxRadial = 0.0f, loZ = 0.0f, hiZ = 0.0f;
    bool finite = true;
    SkinAllVerts(mats, pos, nrm, &maxRadial, &loZ, &hiZ, &finite);
    return WatchdogTrips(maxRadial, loZ, hiZ, finite);
}

} // namespace mc
