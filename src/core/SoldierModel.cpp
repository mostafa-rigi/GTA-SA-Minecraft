// The Counter-Strike soldier's real model, posed and skinned in the core.
// Data comes from cs_soldier.bin format v3 (tools/convert_cs_soldier.py): bone-local
// corner vertices, per-bone baked animation tracks at 30 fps stored as SEPARATE
// gait and aim layers, and the model's own textures as separate PNGs the bridge
// loads. Skinning is the GoldSrc way: a vertex hangs on exactly one bone, and the
// bone's posed matrix is its whole transform.
//
// The two-layer compose (workstream M): the lower-body bones (root, pelvis, legs)
// are sampled from the gait layer, the spine-up bones from the aim layer, and the
// aim-vs-legs yaw difference is distributed over the three spine bones as a
// world-space twist while the hierarchy is composed - hips face the movement,
// shoulders face the target, exactly the read a real strafing soldier gives.

#include "SoldierModel.h"

#include <array>
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
    int layer = 0;
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
std::vector<int> gBoneLayer; // 0 = lower body (gait layer), 1 = upper (aim layer)
std::vector<float> gTwist;   // spine twist weights (sum 1 over the spine chain)
std::vector<Vert> gVerts;
std::vector<unsigned char> gTriTex;
std::vector<Seq> gSeqs;
// per sequence-frame-bone: position (3 floats) then quaternion (4 int16, / 32767)
std::vector<float> gAnimPos;
std::vector<short> gAnimQuat;
float gStrideWalk = 1.553f, gStrideRun = 2.970f, gStrideStrafe = 0.366f, gStrideBack = 2.322f;
bool gHasGun = false;
int gGunBone[2] = { -1, -1 }; // 0 muzzle, 1 butt
Vec3 gGunLocal[2];

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

float WrapAngle(float a) {
    while (a > 3.14159265f)
        a -= 6.28318531f;
    while (a < -3.14159265f)
        a += 6.28318531f;
    return a;
}

const Seq* FindSeq(int category, int layer, int variant) {
    const Seq* found = nullptr;
    int n = 0;
    for (const Seq& s : gSeqs)
        if (s.category == category && s.layer == layer) {
            if (n == variant)
                found = &s;
            ++n;
        }
    if (!found && n > 0) { // variant past the end: wrap onto the ones that exist
        int want = ((variant % n) + n) % n, k = 0;
        for (const Seq& s : gSeqs)
            if (s.category == category && s.layer == layer && k++ == want)
                found = &s;
    }
    return found;
}

// Samples every bone's local of one sequence at `time` seconds, interpolated
// between the baked 30 fps frames (positions lerped, quaternions nlerped the
// short way round) - the same smoothing the GoldSrc engine draws with.
bool PoseLocals(const Seq& s, float time, std::vector<SoldierLocal>& out) {
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
        out[b].pos = Vec3(pa[0] + (pb[0] - pa[0]) * frac, pa[1] + (pb[1] - pa[1]) * frac, pa[2] + (pb[2] - pa[2]) * frac);
        out[b].q = { x / len, y / len, z / len, w / len };
    }
    return true;
}

// The sequence time the leg layer means: the gaits do not run on the clock, they
// run on the ground he has covered - one cycle per stride length, so his feet
// neither skate nor moon-walk at any speed the brain walks him.
float LegSeqTime(const SoldierDrawState& st, const Seq& s) {
    if (st.legCategory == SP_WALK || st.legCategory == SP_RUN) {
        float phase = st.gait - std::floor(st.gait);
        return phase * s.duration;
    }
    return st.legTime;
}

// Composes bone locals down the hierarchy into model-space matrices, applying
// the spine twist on the way: each spine bone's world orientation is rotated
// about the world up axis (through its own origin) by its share of the twist, so
// the chain above it swings from the legs' facing toward the aim facing and the
// chain below it never moves. Rotations only - no bone can stretch.
void ComposeWorldMats(const std::vector<SoldierLocal>& locals, float twist, std::vector<Mat>& out) {
    out.resize(gBones);
    for (int b = 0; b < gBones; ++b) {
        const SoldierLocal& l = locals[b];
        Mat local = QuatMat(l.q[0], l.q[1], l.q[2], l.q[3], l.pos);
        Mat w = gParents[b] >= 0 ? Mul(out[gParents[b]], local) : local;
        if (twist != 0.0f && b < (int)gTwist.size() && gTwist[b] > 0.0f) {
            const float th = twist * gTwist[b];
            const float c = std::cos(th), s = std::sin(th);
            Mat rz;
            rz.a[0][0] = c;
            rz.a[0][1] = -s;
            rz.a[0][2] = 0;
            rz.a[1][0] = s;
            rz.a[1][1] = c;
            rz.a[1][2] = 0;
            rz.a[2][0] = 0;
            rz.a[2][1] = 0;
            rz.a[2][2] = 1;
            rz.t = w.t;
            // rotate the orientation about the bone's own origin: R' = Rz * R, t kept
            Mat spun;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    spun.a[i][j] = rz.a[i][0] * w.a[0][j] + rz.a[i][1] * w.a[1][j] + rz.a[i][2] * w.a[2][j];
            spun.t = w.t;
            w = spun;
        }
        out[b] = w;
    }
}

// The man's stand in the world, recovered from the EntityPose DrawMob built him.
// EntityPose stores its axes NEGATED times the mob scale (Minecraft's convention:
// model x is the left, y is down, z is back), so every axis is un-negated here -
// all three of them. (The first version of this code un-negated only two and read
// base.X as "right" when it is in fact -right: the drawn man came out mirrored,
// his rifle riding on the wrong shoulder.) The pose's yaw is his LEG yaw; the
// torso reaches the aim yaw through the spine twist.
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
    st.feet = base.o + base.Y * 1.501f; // the origin rides 1.501 m over his feet
    st.ok = true;
    return st;
}

// Animation space: +X is his forward, +Y is his left, +Z is up. His world frame:
// x right, y forward, z up.
Vec3 Place(const Stand& st, const Vec3& a) {
    return st.feet + st.right * (-a.y) + st.fwd * a.x + st.up * a.z;
}
Vec3 PlaceDir(const Stand& st, const Vec3& a) {
    return st.right * (-a.y) + st.fwd * a.x + st.up * a.z;
}

// The twist a draw state asks for: aim relative to legs, clamped to what a spine
// can honestly do (110 degrees); past that the brain turns the legs themselves.
float StateTwist(const SoldierDrawState& st) {
    return Clamp(WrapAngle(st.aimYaw - st.legYaw), -1.92f, 1.92f);
}

} // namespace

bool SoldierComposeLocals(const SoldierDrawState& st, SoldierLocal* out, int maxBones) {
    if (!gReady || !out || maxBones < gBones)
        return false;
    std::vector<SoldierLocal> locals;
    if (st.fullCategory >= 0) {
        const Seq* s = FindSeq(st.fullCategory, SL_FULL, st.fullVariant);
        if (!s || !PoseLocals(*s, st.fullTime, locals))
            return false;
    } else {
        const Seq* leg = FindSeq(st.legCategory, SL_LEG, st.legVariant);
        const Seq* up = FindSeq(st.upCategory, SL_UP, st.upVariant);
        if (!leg || !up)
            return false;
        std::vector<SoldierLocal> ll, uu;
        if (!PoseLocals(*leg, LegSeqTime(st, *leg), ll) || !PoseLocals(*up, st.upTime, uu))
            return false;
        locals.resize(gBones);
        for (int b = 0; b < gBones; ++b)
            locals[b] = (b < (int)gBoneLayer.size() && gBoneLayer[b] == 0) ? ll[b] : uu[b];
    }
    for (int b = 0; b < gBones; ++b)
        out[b] = locals[b];
    return true;
}

bool LoadSoldierModel(const unsigned char* data, size_t size) {
    UnloadSoldierModel();
    if (!data || size < 64) {
        Log("cs_soldier.bin: missing or empty - the Counter-Strike soldier cannot appear without his model file");
        return false;
    }
    Reader rd{ data, size };
    char magic[4];
    uint32_t version = 0, bones = 0, verts = 0, tris = 0, ntex = 0, nseq = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "CSM1", 4) != 0 || !rd.Get(version) ||
        !rd.Get(bones) || !rd.Get(verts) || !rd.Get(tris) || !rd.Get(ntex) ||
        bones == 0 || bones > 64 || verts == 0 || verts % 3 != 0 || tris != verts / 3 || ntex == 0 || ntex > 8) {
        Log("cs_soldier.bin: bad header - the Counter-Strike soldier cannot appear");
        return false;
    }
    if (version != 3) {
        Log("cs_soldier.bin: format version %u - this build needs the v3 layered file; "
            "rebake it with tools/convert_cs_soldier.py - the Counter-Strike soldier cannot appear",
            version);
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
        uint8_t cat = 0, layer = 0, loop = 0;
        uint16_t frames = 0;
        float dur = 0;
        if (!rd.Get(cat) || !rd.Get(layer) || !rd.Get(loop) || !rd.Get(frames) || !rd.Get(dur) ||
            frames == 0 || dur <= 0.0f || layer > 2) {
            Log("cs_soldier.bin: bad sequence entry - the Counter-Strike soldier cannot appear");
            return false;
        }
        Seq s;
        s.category = cat;
        s.layer = layer;
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
    float strides[4];
    int gunBone[2] = { -1, -1 };
    Vec3 gunLocal[2];
    if (!rd.Get(strides[0]) || !rd.Get(strides[1]) || !rd.Get(strides[2]) || !rd.Get(strides[3]) ||
        strides[0] <= 0.1f || strides[1] <= 0.1f ||
        !rd.Get(gunBone[0]) || !rd.Get(gunLocal[0].x) || !rd.Get(gunLocal[0].y) || !rd.Get(gunLocal[0].z) ||
        !rd.Get(gunBone[1]) || !rd.Get(gunLocal[1].x) || !rd.Get(gunLocal[1].y) || !rd.Get(gunLocal[1].z) ||
        gunBone[0] < 0 || gunBone[0] >= (int)bones || gunBone[1] < 0 || gunBone[1] >= (int)bones) {
        Log("cs_soldier.bin: bad stride/gun table - the Counter-Strike soldier cannot appear");
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
    std::vector<int> boneLayer(bones);
    std::vector<float> twist(bones);
    for (uint32_t i = 0; i < bones; ++i) {
        uint8_t v = 0;
        if (!rd.Get(v) || v > 1) {
            Log("cs_soldier.bin: bad bone layer table - the Counter-Strike soldier cannot appear");
            return false;
        }
        boneLayer[i] = v;
    }
    for (uint32_t i = 0; i < bones; ++i) {
        if (!rd.Get(twist[i]) || twist[i] < 0.0f || twist[i] > 1.0f) {
            Log("cs_soldier.bin: bad twist table - the Counter-Strike soldier cannot appear");
            return false;
        }
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
    gBoneLayer = std::move(boneLayer);
    gTwist = std::move(twist);
    gVerts = std::move(vv);
    gTriTex = std::move(tt);
    gSeqs = std::move(seqs);
    gAnimPos = std::move(animPos);
    gAnimQuat = std::move(animQuat);
    gStrideWalk = strides[0];
    gStrideRun = strides[1];
    gStrideStrafe = strides[2];
    gStrideBack = strides[3];
    gHasGun = true;
    gGunBone[0] = gunBone[0];
    gGunBone[1] = gunBone[1];
    gGunLocal[0] = gunLocal[0];
    gGunLocal[1] = gunLocal[1];
    gReady = true;
    Log("cs_soldier.bin v3 loaded: %d bones, %d triangles, %d layered sequences (scale %.4f m/unit, lifted %.3f m, strides %.2f/%.2f/%.2f/%.2f m)",
        gBones, (int)gTriTex.size(), (int)gSeqs.size(), infoScale, infoLift * infoScale,
        gStrideWalk, gStrideRun, gStrideStrafe, gStrideBack);
    return true;
}

void UnloadSoldierModel() {
    gReady = false;
    gBones = 0;
    gParents.clear();
    gBoneLayer.clear();
    gTwist.clear();
    gVerts.clear();
    gTriTex.clear();
    gSeqs.clear();
    gAnimPos.clear();
    gAnimQuat.clear();
    gHasGun = false;
    gGunBone[0] = gGunBone[1] = -1;
    gStrideWalk = 1.553f;
    gStrideRun = 2.970f;
    gStrideStrafe = 0.366f;
    gStrideBack = 2.322f;
}

bool SoldierModelReady() { return gReady; }
int SoldierBoneCount() { return gBones; }

float SoldierStride(int category) {
    if (category == SP_WALK)
        return gStrideWalk;
    if (category == SP_RUN)
        return gStrideRun;
    return 0.0f;
}

float SoldierGaitStride(int legCategory, int legVariant) {
    if (legCategory == SP_WALK)
        return gStrideWalk;
    if (legCategory == SP_RUN) {
        if (legVariant == 1 || legVariant == 2)
            return gStrideStrafe;
        if (legVariant == 3)
            return gStrideBack;
        return gStrideRun;
    }
    return 0.0f;
}

bool SoldierPoseSpan(int category, int layer, int variant, float time, float* loZ, float* hiZ) {
    if (!gReady)
        return false;
    const Seq* s = FindSeq(category, layer, variant);
    if (!s)
        return false;
    std::vector<SoldierLocal> locals;
    if (!PoseLocals(*s, time, locals))
        return false;
    std::vector<Mat> mats;
    ComposeWorldMats(locals, 0.0f, mats);
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

bool SoldierGunPose(const Pose& base, const SoldierDrawState& st, Vec3* muzzle, Vec3* butt) {
    if (!gReady || !gHasGun)
        return false;
    const Stand stand = RecoverStand(base);
    if (!stand.ok)
        return false;
    SoldierLocal locals[64];
    if (!SoldierComposeLocals(st, locals, 64))
        return false;
    std::vector<SoldierLocal> lv(locals, locals + gBones);
    std::vector<Mat> mats;
    ComposeWorldMats(lv, StateTwist(st), mats);
    if (muzzle)
        *muzzle = Place(stand, Apply(mats[gGunBone[0]], gGunLocal[0]));
    if (butt)
        *butt = Place(stand, Apply(mats[gGunBone[1]], gGunLocal[1]));
    return true;
}

bool SoldierLegDir(const Pose& base, const SoldierDrawState& st, Vec3* dir) {
    if (!gReady)
        return false;
    const Stand stand = RecoverStand(base);
    if (!stand.ok)
        return false;
    SoldierLocal locals[64];
    if (!SoldierComposeLocals(st, locals, 64))
        return false;
    std::vector<SoldierLocal> lv(locals, locals + gBones);
    std::vector<Mat> mats;
    ComposeWorldMats(lv, 0.0f, mats); // the legs never twist
    // the pelvis: the lower-layer bone (past the root) the thighs hang from
    int pelvis = -1;
    for (int b = 1; b < gBones; ++b)
        if (gBoneLayer[b] == 0 && gParents[b] == 0) {
            pelvis = b;
            break;
        }
    if (pelvis < 0)
        return false;
    Vec3 sum(0, 0, 0);
    int legs = 0;
    for (int b = 0; b < gBones; ++b) {
        if (gBoneLayer[b] != 0 || gParents[b] != pelvis)
            continue;
        // walk down this leg's chain to its deepest bone (the toe)
        int toe = b, guard = 0;
        for (;;) {
            int next = -1;
            for (int c = 0; c < gBones; ++c)
                if (gParents[c] == toe && gBoneLayer[c] == 0) {
                    next = c;
                    break;
                }
            if (next < 0 || ++guard > 8)
                break;
            toe = next;
        }
        sum = sum + (mats[toe].t - mats[b].t);
        ++legs;
    }
    if (legs == 0)
        return false;
    Vec3 d = PlaceDir(stand, sum);
    d.z = 0;
    float len = d.Length();
    if (len < 1e-6f)
        return false;
    *dir = d * (1.0f / len);
    return true;
}

void DrawSoldierModel(const Pose& base, const SoldierDrawState& st, float light, float r, float g, float b) {
    if (!gReady)
        return;
    SoldierLocal buf[64];
    if (!SoldierComposeLocals(st, buf, 64))
        return;
    std::vector<SoldierLocal> locals(buf, buf + gBones);
    // Cross-fade out of the previous composed pose: the brain snapshots the
    // composed locals whenever the pose changes; blend toward the new compose
    // (positions lerped, quaternions nlerped the short way) so no transition -
    // layer change, crouch, flinch, death - ever snaps.
    if (st.hasPrev && st.fade < 1.0f && st.prevBones == gBones) {
        const float w = Clamp(st.fade, 0.0f, 1.0f);
        const float s = w * w * (3.0f - 2.0f * w); // smoothstep: soft at both ends
        for (int i = 0; i < gBones; ++i) {
            const SoldierLocal& a = st.prevLocals[i];
            SoldierLocal& c = locals[i];
            float dot = a.q[0] * c.q[0] + a.q[1] * c.q[1] + a.q[2] * c.q[2] + a.q[3] * c.q[3];
            float sg = dot < 0.0f ? -1.0f : 1.0f;
            float x = a.q[0] + (c.q[0] * sg - a.q[0]) * s, y = a.q[1] + (c.q[1] * sg - a.q[1]) * s,
                  z = a.q[2] + (c.q[2] * sg - a.q[2]) * s, wq = a.q[3] + (c.q[3] * sg - a.q[3]) * s;
            float len = std::sqrt(x * x + y * y + z * z + w * wq);
            if (len > 1e-6f)
                c.q = { x / len, y / len, z / len, wq / len };
            c.pos = a.pos + (c.pos - a.pos) * s;
        }
    }
    std::vector<Mat> mats;
    ComposeWorldMats(locals, StateTwist(st), mats);
    const Stand stand = RecoverStand(base);
    if (!stand.ok)
        return;
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
            p[k] = Place(stand, Apply(m, vt.pos));
            Vec3 n = PlaceDir(stand, ApplyDir(m, vt.normal));
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

const int* SoldierBoneParents() { return gParents.empty() ? nullptr : gParents.data(); }

bool SoldierComposedSpan(const SoldierDrawState& st, float* lo, float* hi) {
    if (!gReady || !lo || !hi)
        return false;
    SoldierLocal locals[64];
    if (!SoldierComposeLocals(st, locals, 64))
        return false;
    std::vector<SoldierLocal> lv(locals, locals + gBones);
    std::vector<Mat> mats;
    const float twist = Clamp(WrapAngle(st.aimYaw - st.legYaw), -1.92f, 1.92f);
    ComposeWorldMats(lv, st.fullCategory >= 0 ? 0.0f : twist, mats);
    *lo = 1e9f;
    *hi = -1e9f;
    for (int b = 0; b < gBones; ++b) {
        *lo = std::min(*lo, mats[b].t.z);
        *hi = std::max(*hi, mats[b].t.z);
    }
    return true;
}

} // namespace mc
