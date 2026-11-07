// Counter-Strike 1.6's rules as data and pure laws - see CsRules.h for the
// sourcing. Nothing in this file knows about the boss or the host's world:
// the same numbers the game will pay are pinned in the tests from here.

#include "CsRules.h"

#include <cmath>

namespace mc {

// The armory. Damage and range modifiers: the wiki's damage-dropoff table
// (the game's constants; silencer pairs where the weapon has one). Price,
// magazine, reserve, reload and move speed: the Steam measured guide.
// Armour ratios: derived from that guide's armoured/unarmoured chest pairs
// (ratio = 2 x armoured / unarmoured). Penetration power: ReGameDLL_CS's
// per-ammo table. Penetration surfaces: the weapon data (M4A1's 2 is from
// its own data file; the AWP's 3 is the commonly tabulated value, flagged
// in the dossier). Cycle times are 0 except the M4A1's 0.09, the one value
// the weapon data file states outright - the rest were not reliably
// sourced in this pass and are not invented here.
const CsWeapon kCsWeapons[CSW_COUNT] = {
    // name, dmg, dmgSil, RM, RMSil, maxRangeM, cycle, mag, reserve, reload, price, speed, armorRatio, penPower, penSurfaces, thinOnly
    { "Glock-18",     25.0f, 25.0f, 0.75f,  0.75f,  208.08f, 0.0f,  20, 120, 2.27f,  400, 250.0f, 1.00f, 21, 1, true },
    { "USP",          34.0f, 30.0f, 0.79f,  0.79f,  104.04f, 0.0f,  12, 100, 2.70f,  500, 250.0f, 1.00f, 15, 1, true },
    { "P228",         32.0f, 32.0f, 0.80f,  0.80f,  104.04f, 0.0f,  13,  52, 2.70f,  600, 250.0f, 1.23f, 25, 1, true },
    { "Desert Eagle", 54.0f, 54.0f, 0.81f,  0.81f,  104.04f, 0.0f,   7,  35, 2.20f,  650, 250.0f, 1.47f, 30, 2, false },
    { "Five-SeveN",   20.0f, 20.0f, 0.885f, 0.885f, 104.04f, 0.0f,  20, 100, 2.70f,  750, 250.0f, 1.47f, 30, 1, true },
    { "Dual Berettas",36.0f, 36.0f, 0.75f,  0.75f,  208.08f, 0.0f,  30, 120, 4.60f,  800, 250.0f, 1.03f, 21, 1, true },
    { "M3 Super 90", 180.0f,180.0f, 1.00f,  1.00f,   76.20f, 0.0f,   8,  32, 4.60f, 1700, 230.0f, 0.99f,  0, 0, true },
    { "XM1014",      120.0f,120.0f, 1.00f,  1.00f,   77.42f, 0.0f,   7,  32, 2.80f, 3000, 240.0f, 1.14f,  0, 0, true },
    { "MAC-10",       29.0f, 29.0f, 0.82f,  0.82f,  208.08f, 0.0f,  30, 100, 3.20f, 1400, 250.0f, 0.93f, 15, 1, true },
    { "TMP",          20.0f, 20.0f, 0.85f,  0.85f,  208.08f, 0.0f,  30, 120, 2.10f, 1250, 250.0f, 0.95f, 21, 1, true },
    { "MP5 Navy",     26.0f, 26.0f, 0.84f,  0.84f,  208.08f, 0.0f,  30, 120, 2.60f, 1500, 250.0f, 0.96f, 21, 1, true },
    { "UMP45",        30.0f, 30.0f, 0.82f,  0.82f,  208.08f, 0.0f,  25, 120, 3.50f, 1700, 250.0f, 0.97f, 15, 1, true },
    { "P90",          21.0f, 21.0f, 0.885f, 0.885f, 208.08f, 0.0f,  50, 100, 3.30f, 2350, 245.0f, 1.50f, 30, 1, true },
    { "Galil",        30.0f, 30.0f, 0.98f,  0.98f,  208.08f, 0.0f,  35,  90, 2.60f, 2000, 240.0f, 1.59f, 35, 2, false },
    { "FAMAS",        30.0f, 30.0f, 0.96f,  0.96f,  208.08f, 0.0f,  25,  90, 3.30f, 2250, 240.0f, 1.40f, 35, 2, false },
    { "AK-47",        36.0f, 36.0f, 0.98f,  0.98f,  208.08f, 0.0f,  30,  90, 2.43f, 2500, 221.0f, 1.54f, 39, 2, false },
    { "M4A1",         33.0f, 32.0f, 0.97f,  0.95f,  208.08f, 0.09f, 30,  90, 3.07f, 3100, 230.0f, 1.35f, 35, 2, false },
    { "SG-552",       33.0f, 33.0f, 0.955f, 0.955f, 208.08f, 0.0f,  30,  90, 2.90f, 3500, 235.0f, 1.38f, 35, 2, false },
    { "AUG",          32.0f, 32.0f, 0.96f,  0.96f,  208.08f, 0.0f,  30,  90, 3.30f, 3500, 240.0f, 1.42f, 35, 2, false },
    { "Scout",        75.0f, 75.0f, 0.98f,  0.98f,  208.08f, 0.0f,  10,  90, 2.00f, 2750, 260.0f, 1.70f, 39, 2, false },
    { "AWP",         115.0f,115.0f, 0.99f,  0.99f,  208.08f, 0.0f,  10,  30, 2.50f, 4750, 210.0f, 1.95f, 45, 3, false },
    { "G3SG1",        80.0f, 80.0f, 0.98f,  0.98f,  208.08f, 0.0f,  20,  90, 3.50f, 5000, 210.0f, 1.65f, 39, 2, false },
    { "SG-550",       70.0f, 70.0f, 0.98f,  0.98f,  208.08f, 0.0f,  30,  90, 3.80f, 4200, 210.0f, 1.45f, 35, 2, false },
    { "M249",         32.0f, 32.0f, 0.97f,  0.97f,  208.08f, 0.0f, 100, 200, 4.70f, 5750, 220.0f, 1.48f, 35, 2, false },
    { "Knife",        65.0f, 65.0f, 1.00f,  1.00f,    1.00f, 0.40f,  0,   0, 0.0f,     0, 250.0f, 1.00f,  0, 0, true },
};

const CsWeapon& CsWeaponAt(int idx) {
    if (idx < 0 || idx >= CSW_COUNT)
        idx = CSW_M4A1;
    return kCsWeapons[idx];
}

float CsRangeFalloff(const CsWeapon& w, float distM) {
    if (distM <= 0.0f)
        return 1.0f;
    if (distM > w.maxRangeM)
        return 0.0f;
    // Damage x RangeModifier ^ (Distance in units / 500); 500 units = 12.7 m
    return std::pow(w.rangeMod, distM / (500.0f * kCsUnitM));
}

int CsZoneFromHeight(float heightM) {
    if (heightM >= 1.42f)
        return CSZ_HEAD;
    if (heightM >= 1.04f)
        return CSZ_CHEST;
    if (heightM >= 0.78f)
        return CSZ_STOMACH;
    return CSZ_LEGS;
}

float CsZoneMul(int zone) {
    switch (zone) {
    case CSZ_HEAD: return 4.0f;
    case CSZ_STOMACH: return 1.25f;
    case CSZ_LEGS: return 0.75f;
    default: return 1.0f;
    }
}

bool CsZoneArmored(int zone, bool helmet) {
    if (zone == CSZ_CHEST || zone == CSZ_STOMACH)
        return true;
    if (zone == CSZ_HEAD)
        return helmet;
    return false; // the legs are never armoured
}

float CsHitHeight(const Vec3& from, const Vec3& dir, const Vec3& feet, float fallbackT) {
    // closest approach of the shot line to the body's vertical axis: solve
    // for the axis height s at the line parameter where the two pass nearest
    const Vec3 u = from - feet;
    const float denom = 1.0f - dir.z * dir.z;
    if (denom < 1e-4f) // a near-vertical shot: price it where it lands
        return (from + dir * fallbackT).z - feet.z;
    const float t = (dir.z * u.z - dir.Dot(u)) / denom;
    return u.z + dir.z * t;
}

float CsArmorSplit(float dmg, float ratio, float pool, float* wear) {
    if (wear)
        *wear = 0.0f;
    if (dmg <= 0.0f)
        return 0.0f;
    if (pool <= 0.0f)
        return dmg;
    float health = dmg * ratio * 0.5f;
    float worn = (dmg - health) * 0.5f;
    if (worn > pool) {
        // the armour dies inside this hit: what it could not wear reaches
        // health at the full rate (the original formula's spill)
        health = dmg - pool / 0.5f;
        worn = pool;
    }
    if (wear)
        *wear = worn;
    return health;
}

float CsShotHh(const CsWeapon& w, float distM, int zone, bool armored, float poolHh, float* wearHh) {
    if (wearHh)
        *wearHh = 0.0f;
    const float cs = w.baseDamage * CsRangeFalloff(w, distM) * CsZoneMul(zone);
    if (cs <= 0.0f)
        return 0.0f;
    if (!armored || !CsZoneArmored(zone, false))
        return cs * kCsToHh;
    // the split is computed in CS points (the pool converted), then priced back
    float wearCs = 0.0f;
    const float healthCs = CsArmorSplit(cs, w.armorRatio, poolHh / kCsToHh, &wearCs);
    if (wearHh)
        *wearHh = wearCs * kCsToHh;
    return healthCs * kCsToHh;
}

CsPen CsPenetrate(const CsWeapon& w, int surface, float depthM) {
    CsPen out;
    if (w.penSurfaces <= 0 || surface == HS_NONE)
        return out;
    // the pre-Source class law: a thin-only weapon never passes a real wall,
    // however the trace prices its thickness
    if (w.thinOnly && (surface == HS_WOOD || surface == HS_METAL || surface == HS_STONE))
        return out;
    const float powerM = (float)w.penPower * kCsUnitM;
    float powerMul = 1.0f;   // how the surface's texture taxes the power (FireBullets3)
    float nominal = 0.1f;    // thickness when the trace has no depth to give
    switch (surface) {
    case HS_GLASS:   out.dmgMul = 0.75f; nominal = 0.02f; break;              // the object path's price
    case HS_WOOD_THIN: out.dmgMul = 0.6f; nominal = 0.05f; break;             // wood
    case HS_WOOD:    out.dmgMul = 0.6f; nominal = 0.15f; break;               // wood
    case HS_FENCE:   out.dmgMul = 0.4f; powerMul = 0.5f; nominal = 0.02f; break; // grate
    case HS_FOLIAGE: out.dmgMul = 0.7f; nominal = 0.40f; break;               // no CS texture: our mapping, flagged
    case HS_OBJECT:  out.dmgMul = 0.75f; nominal = 0.20f; break;              // non-BSP: power resets to 42 units
    case HS_METAL:   out.dmgMul = 0.2f; powerMul = 0.15f; nominal = 0.10f; break; // metal
    case HS_STONE:   out.dmgMul = 0.5f; powerMul = 0.25f; nominal = 1e9f; break;  // concrete: unknown depth = a wall
    default: return out;
    }
    float effPower = powerM * powerMul;
    if (surface == HS_OBJECT)
        effPower = std::min(effPower, 42.0f * kCsUnitM); // the non-BSP reset caps it
    out.thicknessM = depthM > 0.0f ? std::min(depthM, 3.0f) : nominal;
    out.passes = out.thicknessM <= effPower;
    if (!out.passes)
        out.dmgMul = 1.0f;
    return out;
}

namespace {
// a small deterministic hash to [0,1): the spray's seeded sway must be the
// same for the same (seed, shot) everywhere - tests, the game, replays
float CsHash01(uint32_t seed, int i) {
    uint32_t h = seed ^ (uint32_t)(i + 1) * 2654435761u;
    h ^= h >> 16;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}
} // namespace

CsRecoil CsRecoilPattern(int weaponIdx, int shotIndex, uint32_t seed) {
    CsRecoil r;
    if (shotIndex <= 0)
        return r;
    if (weaponIdx == CSW_USP) {
        // the USP's recoil is a straight vertical line; only the weapon's own
        // inaccuracy frets it sideways, a whisper either way
        r.pitchDeg = std::min((float)shotIndex * 0.55f, 4.4f);
        r.yawDeg = (CsHash01(seed, shotIndex) - 0.5f) * 0.3f;
        return r;
    }
    // the rifles' inverted triangle: a near-straight climb for the first ten
    // shots, then the pull goes sideways - seeded per spray, switching sides
    // as it pleases, and never leaving the triangle the climb has drawn
    const float climb = std::min((float)shotIndex, 10.0f) * 0.53f +
                        std::max(0.0f, (float)shotIndex - 10.0f) * 0.05f;
    r.pitchDeg = std::min(climb, 6.0f);
    if (shotIndex < 7) {
        r.yawDeg = (CsHash01(seed, shotIndex) - 0.5f) * 0.35f * ((float)shotIndex / 7.0f);
    } else {
        float yaw = 0.0f;
        for (int i = 7; i <= shotIndex; ++i) {
            yaw += (CsHash01(seed, i) - 0.5f) * 1.15f;
            const float bound = std::min(2.4f, r.pitchDeg * 0.55f);
            yaw = Clamp(yaw, -bound, bound);
        }
        r.yawDeg = yaw;
    }
    return r;
}

float CsHeHh(float distM, float radiusM, float maxCs, bool armored, float poolHh, float* wearHh) {
    if (wearHh)
        *wearHh = 0.0f;
    if (distM >= radiusM || radiusM <= 0.0f)
        return 0.0f;
    const float cs = maxCs * (1.0f - distM / radiusM);
    if (!armored)
        return cs * kCsToHh;
    float wearCs = 0.0f;
    const float healthCs = CsArmorSplit(cs, 1.0f, poolHh / kCsToHh, &wearCs);
    if (wearHh)
        *wearHh = wearCs * kCsToHh;
    return healthCs * kCsToHh;
}

} // namespace mc
