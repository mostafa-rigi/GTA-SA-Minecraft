#pragma once
// Counter-Strike 1.6's own rules, as data and as small pure laws the soldier's
// brain prices his fight by: the full armory's numbers, the range falloff,
// the hit-zone multipliers, the kevlar split, wall penetration, the recoil
// patterns of the two weapons he carries, and the HE grenade's damage curve.
//
// Every number here was compiled from measured/open sources only - the
// community-measured weapon tables (the Steam "Detailed Weapon Guide", the
// Counter-Strike wiki's damage-dropoff table), the game's own weapon data
// file for the M4A1, and the values readable in the open-source ReGameDLL_CS
// reimplementation (read as a reference; no Valve code is copied). The full
// dossier, with the source named per table, is CS-RULEBOOK-fa.md in the
// project folder. A number that could not be sourced reliably is 0 here and
// marked as unrecorded in the dossier - never invented.
//
// The scale anchor: the M4A1's chest hit at point blank is 33 CS damage and
// has always been 6 half hearts on this host, so 1 CS damage = 6/33 hh.

#include "Core.h"
#include "Host.h" // HitSurface rides on the host's trace answer

namespace mc {

// 1 CS damage point in half hearts (the M4A1 anchor: 33 -> 6)
inline constexpr float kCsToHh = 6.0f / 33.0f;
// GoldSrc units to metres (1 unit = 1 inch)
inline constexpr float kCsUnitM = 0.0254f;

// one row of the armory: everything the dossier knows about a weapon
struct CsWeapon {
    const char* name;
    float baseDamage;   // CS damage per bullet (pellet for the shotguns' total), unsilenced
    float baseDamageSil; // with the silencer on (only the USP and the M4A1 have one; else = baseDamage)
    float rangeMod;     // the range modifier: damage *= rangeMod ^ (distance in units / 500)
    float rangeModSil;
    float maxRangeM;    // beyond this the bullet simply does not arrive
    float cycleSec;     // seconds between shots at the weapon's own cap; 0 = not reliably sourced
    int mag;            // rounds in a magazine
    int reserve;        // rounds carried in reserve
    float reloadSec;
    int price;          // dollars
    float moveSpeed;    // GoldSrc units per second while held (250 = the knife pace)
    float armorRatio;   // kevlar law: an armoured zone takes baseDamage * armorRatio / 2 to health
    int penPower;       // the calibre's penetration power, GoldSrc units (FireBullets3, per ammo)
    int penSurfaces;    // how many surfaces one bullet may pass (the weapon's iPenetration)
    bool thinOnly;      // pre-Source law: pistols (bar the Deagle), SMGs and shotguns never
                        // wallbang real walls - thin surfaces only (see CsPenetrate)
};

enum CsWeaponIdx {
    CSW_GLOCK = 0, CSW_USP, CSW_P228, CSW_DEAGLE, CSW_FIVESEVEN, CSW_DUALS,
    CSW_M3, CSW_XM1014,
    CSW_MAC10, CSW_TMP, CSW_MP5, CSW_UMP45, CSW_P90,
    CSW_GALIL, CSW_FAMAS, CSW_AK47, CSW_M4A1, CSW_SG552, CSW_AUG,
    CSW_SCOUT, CSW_AWP, CSW_G3SG1, CSW_SG550,
    CSW_M249, CSW_KNIFE, CSW_COUNT
};
extern const CsWeapon kCsWeapons[CSW_COUNT];
const CsWeapon& CsWeaponAt(int idx);

// ---- the range law: Damage x RangeModifier ^ (Distance / 500 units)
float CsRangeFalloff(const CsWeapon& w, float distM);

// ---- the hit zones: head 4x, chest & arms 1x, stomach 1.25x, legs 0.75x
enum CsZone { CSZ_HEAD = 0, CSZ_CHEST, CSZ_STOMACH, CSZ_LEGS };
// which zone a hit at this height above the target's feet belongs to
// (fractions of a 1.8 m man: legs under 0.78, stomach to 1.04, chest to 1.42)
int CsZoneFromHeight(float heightM);
float CsZoneMul(int zone);
// kevlar covers the chest and stomach; the head only under a helmet; the
// legs never (every measured table agrees: armoured legs = unarmoured legs)
bool CsZoneArmored(int zone, bool helmet);
// the height above `feet` where the shot line passes closest to the body's
// vertical axis - the honest height of the hit, for zone pricing
float CsHitHeight(const Vec3& from, const Vec3& dir, const Vec3& feet, float fallbackT);

// ---- the kevlar split, the game's own shape: of `dmg` (in whatever unit
// the caller works in), the health share is dmg * ratio / 2, and the armour
// pool wears by (dmg - health) * 0.5. If the pool cannot cover the wear, it
// wears out and the uncovered rest reaches health at the unarmoured rate.
// Returns the health share; *wear takes the pool's loss (same units).
float CsArmorSplit(float dmg, float ratio, float pool, float* wear);

// ---- one bullet priced whole, in half hearts: base damage, range falloff,
// zone multiplier, and - for an armoured target zone - the kevlar split
// (poolHh/wearHh in half hearts; SA armour carries no helmet, so the head
// is never an armoured zone on this host)
float CsShotHh(const CsWeapon& w, float distM, int zone, bool armored, float poolHh, float* wearHh);

// ---- wall penetration (FireBullets3's shape, on the surfaces the host can
// report): may this weapon's bullet pass this surface, how much damage
// survives the pass, and how thick the material counts as (the re-trace
// steps past it by this much). Thickness comes from the trace's own depth
// when it has one; a world surface of unknown thickness counts as thick.
struct CsPen {
    bool passes = false;
    float dmgMul = 1.0f;     // damage multiplier for having passed this surface
    float thicknessM = 0.0f; // the material's thickness as the bullet meets it
};
CsPen CsPenetrate(const CsWeapon& w, int surface, float depthM);

// ---- recoil: where shot number `shotIndex` of a spray lands relative to
// the aim, before the shooter's own compensation. 1.6's patterns are
// semi-random: a fixed shape (an inverted triangle for the rifles - climb,
// then a seeded left/right sway inside the triangle; a straight vertical
// line for the USP) whose sway is rolled per spray from `seed`. Pure and
// deterministic in (weapon, index, seed), so tests can pin the shape.
struct CsRecoil {
    float pitchDeg = 0.0f; // up
    float yawDeg = 0.0f;   // sideways, + = right of the aim
};
CsRecoil CsRecoilPattern(int weaponIdx, int shotIndex, uint32_t seed);

// ---- the HE grenade: max damage at the heart, falling linearly to nothing
// at the radius; armour halves what reaches health (the measured 96 / 48).
// In 1.6 the blast prices through walls - no line-of-sight term by design.
// Returns health damage in half hearts; *wearHh the armour's loss.
float CsHeHh(float distM, float radiusM, float maxCs, bool armored, float poolHh, float* wearHh);

} // namespace mc
