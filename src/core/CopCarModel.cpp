// The Driver 2 cop's own body, drawn in the core. Data comes from
// cop_car.bin format v1 (tools/convert_driver2_cop.py) - see
// CopCarModel.h for what the four meshes are and where they came from.
//
// Shading is flat per triangle, the way the core shades the soldier:
// the top of the car catches more light than its sides, judged by the
// triangle normal's world up component. The original's GT polys carry
// point normals (Gouraud); flat is the honest simple stand-in and is
// named as such in the piece-3 report.

#include "CopCarModel.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "Cop.h"    // CopProfile: his wheel placements live there
#include "McModel.h" // ModelSink, ModelColor, MT_COP

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

struct Corner {
    uint16_t vert = 0;
    float u = 0, v = 0;
    uint8_t tex = 255, spin = 0;
    uint8_t r = 0, g = 0, b = 0;
};

struct Mesh {
    std::vector<Vec3> verts;
    std::vector<Corner> corners;
};

bool gReady = false;
int gTexCount = 0;
Mesh gMeshes[4]; // 0 clean body, 1 dent target (verts only), 2 wheel clean, 3 wheel fast

bool Finite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 100.0f && std::fabs(v.y) < 100.0f && std::fabs(v.z) < 100.0f;
}

} // namespace

bool LoadCopCarModel(const unsigned char* data, size_t size) {
    UnloadCopCarModel();
    if (!data || size < 24) {
        Log("cop_car.bin: missing or empty - the Driver 2 cop keeps the host body");
        return false;
    }
    Reader rd{ data, size };
    char magic[4];
    uint32_t version = 0, ntex = 0;
    if (!rd.Get(magic) || std::memcmp(magic, "D2C1", 4) != 0 || !rd.Get(version) ||
        version != 1 || !rd.Get(ntex) || ntex == 0 || ntex > 64) {
        Log("cop_car.bin: bad header - the Driver 2 cop keeps the host body");
        return false;
    }
    Mesh meshes[4];
    for (int m = 0; m < 4; ++m) {
        uint32_t nv = 0, nc = 0, nt = 0;
        if (!rd.Get(nv) || !rd.Get(nc) || !rd.Get(nt) || nv == 0 || nv > 8192 ||
            nc % 3 != 0 || nc > 3 * 8192 || nt != nc / 3) {
            Log("cop_car.bin: bad mesh table - the Driver 2 cop keeps the host body");
            return false;
        }
        if (m == 1 && (nv != (uint32_t)meshes[0].verts.size() || nc != 0)) {
            Log("cop_car.bin: the dent target must mirror the body's verts and carry "
                "no corners - the Driver 2 cop keeps the host body");
            return false;
        }
        meshes[m].verts.resize(nv);
        for (uint32_t i = 0; i < nv; ++i) {
            Vec3& v = meshes[m].verts[i];
            if (!rd.Get(v.x) || !rd.Get(v.y) || !rd.Get(v.z) || !Finite(v)) {
                Log("cop_car.bin: bad vertex - the Driver 2 cop keeps the host body");
                return false;
            }
        }
        meshes[m].corners.resize(nc);
        for (uint32_t i = 0; i < nc; ++i) {
            Corner& c = meshes[m].corners[i];
            uint8_t tex = 255, spin = 0;
            if (!rd.Get(c.vert) || !rd.Get(c.u) || !rd.Get(c.v) || !rd.Get(tex) ||
                !rd.Get(spin) || !rd.Get(c.r) || !rd.Get(c.g) || !rd.Get(c.b) ||
                c.vert >= nv || tex >= ntex || spin > 1 ||
                !std::isfinite(c.u) || !std::isfinite(c.v)) {
                Log("cop_car.bin: bad corner - the Driver 2 cop keeps the host body");
                return false;
            }
            c.tex = tex;
            c.spin = spin;
        }
    }
    if (rd.left != 0) {
        Log("cop_car.bin: trailing bytes - the Driver 2 cop keeps the host body");
        return false;
    }
    for (int m = 0; m < 4; ++m)
        gMeshes[m] = std::move(meshes[m]);
    gTexCount = (int)ntex;
    gReady = true;
    Log("cop_car.bin: his body loaded - %d verts, %d tris, %d textures",
        (int)gMeshes[0].verts.size(), (int)gMeshes[0].corners.size() / 3, gTexCount);
    return true;
}

void UnloadCopCarModel() {
    gReady = false;
    gTexCount = 0;
    for (int m = 0; m < 4; ++m)
        gMeshes[m] = Mesh();
}

bool CopCarModelReady() { return gReady; }
int CopCarTextureCount() { return gReady ? gTexCount : 0; }
int CopCarBodyTris() { return gReady ? (int)gMeshes[0].corners.size() / 3 : 0; }

bool CopCarBodyExtents(Vec3* lo, Vec3* hi) {
    if (!gReady || !lo || !hi)
        return false;
    *lo = *hi = gMeshes[0].verts[0];
    for (const Vec3& v : gMeshes[0].verts) {
        lo->x = v.x < lo->x ? v.x : lo->x;
        lo->y = v.y < lo->y ? v.y : lo->y;
        lo->z = v.z < lo->z ? v.z : lo->z;
        hi->x = v.x > hi->x ? v.x : hi->x;
        hi->y = v.y > hi->y ? v.y : hi->y;
        hi->z = v.z > hi->z ? v.z : hi->z;
    }
    return true;
}

// The drawn wheel centres, from the game's own placement law
// (CARS.C DrawCarWheels), re-derived in fix round 3 - piece 3 placed
// them at the raw LCF wheelDisp with only the frame map applied, which
// dropped two terms of the law and glued the wheels on:
// - SIDE: DrawCarWheels places a wheel at sWheelPos.x = 17 - vx for
//   wheelnums 0,1 and -17 - vx for 2,3 (the 17 is the wheel model's
//   own half-width: the LCF vx is not the centre). Centres land at
//   |x| = 105..109 of his units, the tyre's outer face at 122.5 - just
//   inside the body slab (132), as the original sits. Piece 3's
//   centres (122..126, outer face up to 143) poked the tyres ~0.07 m
//   OUTSIDE the slab: the eye test's "wheels glued on".
// - FORWARD: sWheelPos.z = -vz with vz post-FixCarCos, and the body
//   frame's cog z is 0 in the data (FixCarCos never touches cog), so
//   the core's forward is vz itself: wheels 0,2 at +222 (the front
//   axle - even wheelnums steer, DrawCarWheels' front model), 1,3
//   at -207. Wheelbase 429 units = 2.79 m at the convicted factor.
// - HEIGHT: the law's vy term puts the centre at (-wheelSize - vy)
//   - sus + 14 = -5 - sus under the physics origin; the model origin
//   rides cog y = -95 over that origin, so in the body frame the
//   centre is 90 - sus, and the measured rest compression (26) fixes
//   it at 64 units under the model origin. Piece 3's arch measurement
//   (lip at core z -0.017, pan at -0.573 at this factor) fixed the
//   same 64 independently - formula and measurement agree, and the
//   tyre's swept radius is CARS.C's sizeScale (wheelSize 49 * 14142
//   / 10000 = 69 units = 0.449 m), so the contact patch sits at
//   -0.866 m in the body frame, 0.29 m under the pan.
constexpr float kHisUnitsPerMetre = 153.6f; // the profile's own factor
constexpr float kCopWheelCenterZ = -64.0f / kHisUnitsPerMetre;

void CopWheelCenters(Vec3 out[4]) {
    const CopProfileData& prof = CopProfile();
    for (int k = 0; k < 4; ++k) {
        // back to his units for the law's own arithmetic, then out in
        // metres: drawn x = (k & 2) == 0 ? 17 - vx : -17 - vx, and the
        // frame map negates x (see the converter's docstring); forward
        // is the profile's post-fix vz, unchanged by the law's frame.
        const float vx = prof.wheelDisp[k][0] * kHisUnitsPerMetre;
        const float drawnX = ((k & 2) == 0 ? 17.0f : -17.0f) - vx;
        out[k] = Vec3(-drawnX / kHisUnitsPerMetre, prof.wheelDisp[k][2],
                      kCopWheelCenterZ);
    }
}

namespace {

// Emits one mesh's triangles through the sink. `place` maps a stored
// vertex (already dent-blended for the body) into model space.
void EmitMesh(const Mesh& mesh, const Pose& pose, float light, float r, float g, float b,
              const Vec3* blended) {
    int curTex = -1;
    const size_t corners = mesh.corners.size();
    for (size_t i = 0; i + 2 < corners; i += 3) {
        Vec3 mp[3];
        for (int k = 0; k < 3; ++k) {
            const Corner& c = mesh.corners[i + k];
            mp[k] = blended ? blended[c.vert] : mesh.verts[c.vert];
        }
        // flat shade from the triangle's own normal, soldier-style
        Vec3 n = Cross(mp[1] - mp[0], mp[2] - mp[0]);
        Vec3 wn = pose.D(n.x, n.y, n.z);
        float nl = wn.Length();
        float up = nl > 1e-6f ? Clamp(wn.z / nl, 0.0f, 1.0f) : 0.0f;
        float shade = 0.60f + 0.40f * up;
        Vec3 p[4];
        float u[4], v[4];
        uint32_t col = 0;
        for (int k = 0; k < 3; ++k) {
            const Corner& c = mesh.corners[i + k];
            if ((int)c.tex != curTex) {
                TheModelSink().Texture(MT_COP + c.tex);
                curTex = c.tex;
            }
            p[k] = pose.P(mp[k].x, mp[k].y, mp[k].z);
            u[k] = c.u;
            v[k] = c.v;
            if (k == 0)
                col = ModelColor(Clamp((int)(255.0f * light * r * shade * (c.r / 128.0f)), 0, 255),
                                 Clamp((int)(255.0f * light * g * shade * (c.g / 128.0f)), 0, 255),
                                 Clamp((int)(255.0f * light * b * shade * (c.b / 128.0f)), 0, 255), 255);
        }
        p[3] = p[2];
        u[3] = u[2];
        v[3] = v[2];
        TheModelSink().Quad(p, u, v, col);
    }
}

} // namespace

void DrawCopCar(const Pose& pose, float dent01, const Vec3 wheelCenters[4],
                float steer, float spin, bool fastWheels,
                float light, float r, float g, float b) {
    if (!gReady)
        return;
    // the body, dent-blended (DENTING.C's law at a uniform factor)
    const Mesh& body = gMeshes[0];
    const Mesh& dent = gMeshes[1];
    float k = Clamp(dent01, 0.0f, 1.0f) * 0.5f;
    std::vector<Vec3> blended(body.verts.size());
    for (size_t i = 0; i < body.verts.size(); ++i) {
        const Vec3& c = body.verts[i];
        const Vec3& d = dent.verts[i];
        blended[i] = Vec3(c.x + (d.x - c.x) * k, c.y + (d.y - c.y) * k, c.z + (d.z - c.z) * k);
    }
    EmitMesh(body, pose, light, r, g, b, blended.data());
    // the wheels: spin the flagged verts about the axle, steer the
    // front pair about the vertical, then place at his centres
    const Mesh& wheel = gMeshes[fastWheels ? 3 : 2];
    float cs = std::cos(spin), sn = std::sin(spin);
    for (int w = 0; w < 4; ++w) {
        float yaw = (w % 2 == 0) ? steer : 0.0f; // even wheelnums steer (DrawCarWheels)
        float cy = std::cos(yaw), sy = std::sin(yaw);
        // spin membership is a per-vertex fact the corners carry (the
        // converter flags verts 8..23 on every corner that names them)
        std::vector<char> spinFlag(wheel.verts.size(), 0);
        for (const Corner& c : wheel.corners)
            if (c.spin)
                spinFlag[c.vert] = 1;
        std::vector<Vec3> placed(wheel.verts.size());
        for (size_t i = 0; i < wheel.verts.size(); ++i) {
            Vec3 v = wheel.verts[i];
            if (spinFlag[i]) {
                float y = v.y * cs - v.z * sn;
                float z = v.y * sn + v.z * cs;
                v.y = y;
                v.z = z;
            }
            if (yaw != 0.0f) {
                float x = v.x * cy - v.y * sy;
                float y = v.x * sy + v.y * cy;
                v.x = x;
                v.y = y;
            }
            placed[i] = v + wheelCenters[w];
        }
        EmitMesh(wheel, pose, light, r, g, b, placed.data());
    }
}

} // namespace mc
