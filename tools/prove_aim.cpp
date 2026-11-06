// Offline proof for the soldier's aim alignment (fix round, item A).
//
// Renders the CS soldier through the SAME chain the game uses - the bridge's
// stand construction (EntityPose from the mob's yaw, exactly as GtaMobs builds
// it) into the core's own DrawSoldierModel - with two overlays drawn in the same
// projection: a red arrow along the BRAIN's forward vector (where the hitscan
// actually goes) and a cyan line along the model's rifle (SoldierGunPose,
// muzzle extended past the butt line). If the red and cyan lines lie on top of
// each other and both leave the figure's front, the model's aim IS the brain's
// aim. The old offline preview never proved this: it drew the converter's own
// axes, not the bridge's transform.
//
// Build (from src-repo):
//   g++ -std=c++17 -O1 -w -I src/core src/core/*.cpp src/core/generated/*.cpp \
//       tools/prove_aim.cpp -o /tmp/prove_aim
// Run: /tmp/prove_aim cs-soldier/cs_soldier.bin out.ppm   (then PPM -> PNG)

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Host.h"
#include "McModel.h"
#include "Mobs.h"
#include "Pose.h"
#include "SoldierModel.h"
#include "World.h"

using namespace mc;

namespace mc {
void PlaySfx(SoundEvent, const Vec3*, float, float) {}
void SetLoopSfx(SoundEvent, bool, float) {}
// the seams the tests also stub: the log line goes to the terminal here, and a
// chunk's mesh is the bridge's business, not this proof's
struct ChunkMesh {};
Chunk::Chunk() = default;
Chunk::~Chunk() = default;
void Log(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "%s\n", buf);
}
} // namespace mc

namespace {

struct NullHost : Host {};

// ---- a sink that keeps the model's quads instead of rastering them away
struct CollectSink : ModelSink {
    struct Q {
        int tex;
        Vec3 p[4];
        uint32_t col;
    };
    std::vector<Q> quads;
    int tex = -1;
    void Texture(int t) override { tex = t; }
    void Quad(const Vec3* p, const float*, const float*, uint32_t c) override {
        Q q;
        q.tex = tex;
        q.col = c;
        for (int k = 0; k < 4; ++k)
            q.p[k] = p[k];
        quads.push_back(q);
    }
    bool Opaque(int, int, int) override { return true; }
};

// ---- a tiny software frame
struct Frame {
    int W, H;
    std::vector<unsigned char> px; // rgb
    std::vector<float> depth;      // smaller is closer
    Frame(int w, int h) : W(w), H(h), px((size_t)w * h * 3, 0), depth((size_t)w * h, 1e30f) {}
    void Clear(int r, int g, int b) {
        for (size_t i = 0; i + 2 < px.size(); i += 3) {
            px[i] = (unsigned char)r;
            px[i + 1] = (unsigned char)g;
            px[i + 2] = (unsigned char)b;
        }
        std::fill(depth.begin(), depth.end(), 1e30f);
    }
    void Put(int x, int y, float d, int r, int g, int b, bool testDepth) {
        if (x < 0 || y < 0 || x >= W || y >= H)
            return;
        const size_t i = (size_t)y * W + x;
        if (testDepth) {
            if (d >= depth[i])
                return;
            depth[i] = d;
        }
        px[i * 3] = (unsigned char)r;
        px[i * 3 + 1] = (unsigned char)g;
        px[i * 3 + 2] = (unsigned char)b;
    }
};

struct Projector {
    // mode 0: top-down, world X right, world Y up-screen, depth = -z (higher is nearer)
    // mode 1: perspective from an eye point
    int mode = 0;
    float scale = 120.0f, cx = 0, cy = 0;
    Vec3 eye, fwd, right, up;
    bool Project(const Vec3& w, float* sx, float* sy, float* d) const {
        if (mode == 0) {
            *sx = cx + w.x * scale;
            *sy = cy - w.y * scale;
            *d = -w.z;
            return true;
        }
        const Vec3 rel = w - eye;
        const float zc = rel.Dot(fwd);
        if (zc < 0.2f)
            return false;
        const float f = 340.0f;
        *sx = cx + rel.Dot(right) / zc * f;
        *sy = cy - rel.Dot(up) / zc * f;
        *d = zc;
        return true;
    }
};

void FillTri(Frame& fr, const Projector& pr, const Vec3& a, const Vec3& b, const Vec3& c, int r, int g, int bl) {
    float ax, ay, ad, bx, by, bd, cx2, cy2, cd;
    if (!pr.Project(a, &ax, &ay, &ad) || !pr.Project(b, &bx, &by, &bd) || !pr.Project(c, &cx2, &cy2, &cd))
        return;
    const float minx = std::min(ax, std::min(bx, cx2)), maxx = std::max(ax, std::max(bx, cx2));
    const float miny = std::min(ay, std::min(by, cy2)), maxy = std::max(ay, std::max(by, cy2));
    const float area = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
    if (std::fabs(area) < 1e-6f)
        return;
    for (int y = (int)std::floor(miny); y <= (int)std::ceil(maxy); ++y)
        for (int x = (int)std::floor(minx); x <= (int)std::ceil(maxx); ++x) {
            const float w0 = ((bx - x) * (cy2 - y) - (by - y) * (cx2 - x)) / area;
            const float w1 = ((cx2 - x) * (ay - y) - (cy2 - y) * (ax - x)) / area;
            const float w2 = 1.0f - w0 - w1;
            if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f)
                continue;
            fr.Put(x, y, w0 * ad + w1 * bd + w2 * cd, r, g, bl, true);
        }
}

void ThickLine(Frame& fr, const Projector& pr, const Vec3& a, const Vec3& b, int rad, int r, int g, int bl) {
    float ax, ay, ad, bx, by, bd;
    if (!pr.Project(a, &ax, &ay, &ad) || !pr.Project(b, &bx, &by, &bd))
        return;
    const int steps = (int)std::ceil(std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay))) + 1;
    for (int i = 0; i <= steps; ++i) {
        const float t = steps ? (float)i / steps : 0.0f;
        const int x = (int)std::lround(ax + (bx - ax) * t), y = (int)std::lround(ay + (by - ay) * t);
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx)
                if (dx * dx + dy * dy <= rad * rad)
                    fr.Put(x + dx, y + dy, 0.0f, r, g, bl, false);
    }
}

void Arrow(Frame& fr, const Projector& pr, const Vec3& from, const Vec3& dir, float len, int rad, int r, int g, int b) {
    const Vec3 tip = from + dir * len;
    Vec3 side(-dir.y, dir.x, 0.0f);
    if (side.Length() < 0.01f)
        side = Vec3(1, 0, 0);
    side = side * (1.0f / side.Length());
    ThickLine(fr, pr, from, tip, rad, r, g, b);
    const Vec3 base = tip - dir * 0.34f;
    ThickLine(fr, pr, tip, base + side * 0.20f, rad, r, g, b);
    ThickLine(fr, pr, tip, base - side * 0.20f, rad, r, g, b);
}

void RenderPanel(Frame& fr, int ox, int oy, int pw, int ph, float yaw, int mode) {
    const Vec3 fwd(-std::sin(yaw), std::cos(yaw), 0.0f);
    const Vec3 right(fwd.y, -fwd.x, 0.0f);
    // the stand, built exactly as the bridge builds it for a mob at the origin
    const Pose stand = EntityPose(Vec3(0, 0, 0), right, Vec3(0, 0, 1), fwd, 1.0f);

    CollectSink sink;
    SetModelSink(&sink);
    MobAnim anim;
    anim.soldierPose = SP_AIM;
    DrawMob(MOB_SOLDIER, stand, anim, 1.0f, 1.0f, 1.0f, 1.0f);
    SetModelSink(nullptr);

    Projector pr;
    pr.mode = mode;
    pr.cx = (float)ox + pw * 0.5f;
    pr.cy = (float)oy + ph * 0.55f;
    pr.scale = 118.0f;
    if (mode == 1) {
        pr.eye = Vec3(2.9f, -3.6f, 2.1f);
        const Vec3 target(0, 0.2f, 0.9f);
        pr.fwd = (target - pr.eye);
        pr.fwd = pr.fwd * (1.0f / pr.fwd.Length());
        pr.right = Vec3(pr.fwd.y, -pr.fwd.x, 0.0f);
        pr.right = pr.right * (1.0f / pr.right.Length());
        pr.up = Vec3(pr.right.y * pr.fwd.z - pr.right.z * pr.fwd.y,
                     pr.right.z * pr.fwd.x - pr.right.x * pr.fwd.z,
                     pr.right.x * pr.fwd.y - pr.right.y * pr.fwd.x);
        pr.cy = (float)oy + ph * 0.5f;
    }

    // the ground cross: his right axis in grey, a metre circle around his feet
    if (mode == 0) {
        ThickLine(fr, pr, right * -1.6f, right * 1.6f, 1, 90, 90, 96);
        for (int i = 0; i <= 48; ++i) {
            const float a0 = (float)i / 48.0f * 6.2831853f;
            ThickLine(fr, pr, Vec3(std::cos(a0), std::sin(a0), 0.0f), Vec3(std::cos(a0), std::sin(a0), 0.0f), 1, 70, 70, 76);
        }
    }

    for (const CollectSink::Q& q : sink.quads) {
        int r = (int)((q.col >> 16) & 255), g = (int)((q.col >> 8) & 255), b = (int)(q.col & 255);
        const int tex = q.tex - MT_SOLDIER;
        if (tex == 0) { // the uniform
            r = r * 82 / 100; g = g * 88 / 100; b = b * 60 / 100;
        } else if (tex == 1) { // the backpack
            r = r * 52 / 100; g = g * 46 / 100; b = b * 36 / 100;
        } else { // the rifle
            r = r * 34 / 100; g = g * 38 / 100; b = b * 48 / 100;
        }
        FillTri(fr, pr, q.p[0], q.p[1], q.p[2], r, g, b);
        FillTri(fr, pr, q.p[0], q.p[2], q.p[3], r, g, b);
    }

    // the brain's forward: a red arrow. The rifle's line: cyan, muzzle extended.
    Vec3 muzzle, butt;
    const float az = mode == 0 ? 0.06f : 0.05f;
    Arrow(fr, pr, Vec3(0, 0, az), fwd, 2.35f, 3, 235, 60, 40);
    if (SoldierGunPose(stand, SP_AIM, 0, 0.0f, &muzzle, &butt)) {
        Vec3 d = muzzle - butt;
        const float dl = d.Length();
        if (dl > 0.01f) {
            d = d * (1.0f / dl);
            ThickLine(fr, pr, butt, muzzle + d * 1.15f, 2, 40, 220, 235);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: prove_aim <cs_soldier.bin> <out.ppm>\n");
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes((size_t)std::max<long>(n, 0));
    if (n > 0 && std::fread(bytes.data(), 1, (size_t)n, f) != (size_t)n) {
        std::fprintf(stderr, "short read\n");
        return 2;
    }
    std::fclose(f);
    NullHost host;
    SetHost(&host);
    if (!LoadSoldierModel(bytes.data(), bytes.size())) {
        std::fprintf(stderr, "the model did not load\n");
        return 2;
    }

    // the numbers behind the picture: gun line vs brain forward, per quarter turn
    for (int qi = 0; qi < 4; ++qi) {
        const float yaw = qi * 1.5707963f;
        const Vec3 fwd(-std::sin(yaw), std::cos(yaw), 0.0f);
        const Vec3 right(fwd.y, -fwd.x, 0.0f);
        const Pose stand = EntityPose(Vec3(0, 0, 0), right, Vec3(0, 0, 1), fwd, 1.0f);
        Vec3 muzzle, butt;
        if (!SoldierGunPose(stand, SP_AIM, 0, 0.0f, &muzzle, &butt))
            continue;
        Vec3 d = muzzle - butt;
        d.z = 0.0f;
        const float dl = d.Length();
        const float dot = dl > 0.01f ? (d.x * fwd.x + d.y * fwd.y) / dl : 0.0f;
        const float ang = std::acos(std::max(-1.0f, std::min(1.0f, dot))) * 57.2957795f;
        const float side = muzzle.x * right.x + muzzle.y * right.y;
        std::fprintf(stderr, "yaw %3d: gun line %.2f deg off the brain forward, muzzle %.3f m to his right\n",
                     qi * 90, ang, side);
    }

    const int PW = 470, PH = 470;
    Frame fr(PW * 3, PH);
    fr.Clear(24, 25, 28);
    RenderPanel(fr, 0, 0, PW, PH, 0.0f, 0);          // top-down, facing +Y (up-screen)
    RenderPanel(fr, PW, 0, PW, PH, 1.5707963f, 0);   // top-down, facing -X (left)
    RenderPanel(fr, PW * 2, 0, PW, PH, 0.0f, 1);     // three-quarter view, facing +Y

    FILE* o = std::fopen(argv[2], "wb");
    if (!o)
        return 2;
    std::fprintf(o, "P6\n%d %d\n255\n", fr.W, fr.H);
    std::fwrite(fr.px.data(), 1, fr.px.size(), o);
    std::fclose(o);
    std::printf("wrote %s (%dx%d)\n", argv[2], fr.W, fr.H);
    return 0;
}
