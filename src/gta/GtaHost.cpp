#include "GtaHost.h"

#include "CClock.h"
#include "CColPoint.h"
#include "CPedIntelligence.h"
#include "CTaskManager.h"
#include "CEntity.h"
#include "CExplosion.h"
#include "CFireManager.h"
#include "CGame.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CWaterLevel.h"
#include "CWeapon.h"
#include "CWeather.h"
#include "CWorld.h"
#include "common.h"

#include "Blocks.h"
#include "Carve.h"
#include "Collision.h"
#include "Entities.h"
#include "Game.h"
#include "GameState.h"
#include "GtaCombat.h"
#include "GtaCop.h"
#include "GtaFishing.h"
#include "GtaMining.h"
#include "GtaMobs.h"
#include "Host.h"
#include "ModCommon.h"
#include "Movement.h"
#include "PedSkins.h"
#include "Terrain.h"

namespace mc {

namespace {
// explosions we made ourselves: GTA shows them, but their blocks are already broken
struct OwnBlast {
    CVector pos;
    float age;
};
std::vector<OwnBlast> gOwnBlasts;
uint8_t gExpCounter[16] = {};
CVector gExpPos[16];

// Is the player flying on the game's own jetpack right now? The jetpack
// runs as a simple task on his own task manager (CTaskSimpleJetPack,
// TASK_SIMPLE_JETPACK); the scan reads his slots the way the boss's body
// duties read the street's kill tasks. This state is worth naming exactly:
// it is the one state in which the game's damage-event processing
// swallows the fall-type event HurtPlayer pays through (see there) - on
// foot the same event demonstrably lands, up there it never did.
bool PlayerJetpackActive(CPlayerPed* ped) {
    if (!ped || !ped->m_pIntelligence)
        return false;
    CTaskManager& tm = ped->m_pIntelligence->m_TaskMgr;
    for (int s = 0; s < TASK_PRIMARY_MAX; ++s) {
        CTask* t = tm.m_aPrimaryTasks[s];
        if (t && t->GetId() == TASK_SIMPLE_JETPACK)
            return true;
    }
    for (int s = 0; s < TASK_SECONDARY_MAX; ++s) {
        CTask* t = tm.m_aSecondaryTasks[s];
        if (t && t->GetId() == TASK_SIMPLE_JETPACK)
            return true;
    }
    return false;
}

struct GtaHost : Host {
    // ---- the GTA map
    bool SolidCell(const Int3& c) override { return GtaSolidCell(c); }
    bool WallBetween(const Int3& a, const Int3& b) override { return GtaWallBetween(a, b); }
    bool Supports(const Int3& c) override { return GtaSupports(c); }
    bool CapAt(const Int3& c, float* top) override { return TerrainCapAt(c.x, c.y, c.z, top); }
    bool GroundBelow(const Vec3& from, float maxDrop, float* z) override {
        bool found = false;
        float gz = CWorld::FindGroundZFor3DCoord(from.x, from.y, from.z, &found, nullptr);
        const int bx = FloorI(from.x), by = FloorI(from.y);
        if (found && TerrainIsOpened(bx, by) && std::fabs(gz - TerrainSurface(bx, by)) < 0.8f)
            found = false; // that ground has been dug away
        if (!found || from.z - gz > maxDrop)
            return false;
        *z = gz;
        return true;
    }
    bool LineBlocked(const Vec3& a, const Vec3& b) override {
        CColPoint cp;
        CEntity* e = nullptr;
        return CWorld::ProcessLineOfSight(a, b, cp, e, true, true, false, true, false, false, false, false) &&
               !IsCollisionObject(e);
    }
    bool LineBlockedSkipVeh(const Vec3& a, const Vec3& b, int vehRef) override {
        // the driver's own car is not a wall between an eye and the driver:
        // the line runs with that one vehicle ignored, exactly the trick the
        // shot trace's LineHit already plays for a shooter's own vehicle
        CVehicle* veh = vehRef >= 0 && CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAtRef(vehRef) : nullptr;
        if (!veh)
            return LineBlocked(a, b);
        CColPoint cp;
        CEntity* e = nullptr;
        CEntity* old = CWorld::pIgnoreEntity;
        CWorld::pIgnoreEntity = veh;
        const bool blocked = CWorld::ProcessLineOfSight(a, b, cp, e, true, true, false, true, false, false, false, false) &&
                             !IsCollisionObject(e);
        CWorld::pIgnoreEntity = old;
        return blocked;
    }
    bool WaterLevel(const Vec3& at, float* level) override {
        return CWaterLevel::GetWaterLevelNoWaves(at.x, at.y, at.z, level);
    }
    bool FlammableUnder(const Int3& c) override { return GtaFlammableUnder(c); }
    bool SoilBelow(const Vec3& from, bool sandToo, float* z) override {
        return GtaSoilBelow(from.x, from.y, from.z, sandToo, z);
    }
    bool CellBlocked(const Int3& c) override { return GtaCellBlocked(c); }
    bool Outdoors() override { return CGame::currArea == 0; }
    bool Dark() override { return Outdoors() && (CClock::ms_nGameClockHours >= 21 || CClock::ms_nGameClockHours < 5); }
    bool OwnGround(const Int3& c, float* depthShade) override {
        if (!TerrainOwnsCell(c.x, c.y, c.z))
            return false;
        *depthShade = TerrainDepthShade(c.x, c.y, c.z);
        return true;
    }
    bool Raining() override { return Outdoors() && CWeather::Rain > 0.2f; }
    bool SpawnGround(const Vec3& from, Vec3* ground) override {
        CVector out;
        if (!GtaAnimalSpawnGround(from, out))
            return false;
        *ground = out;
        return true;
    }

    // ---- what is under the crosshair, when it is GTA's (GtaMining.cpp)
    bool Pick(const PickRay& ray, const VoxelHit& blocks, Target& out) override { return GtaPick(ray, blocks, out); }
    void BreakTarget() override { GtaBreakTarget(); }

    // ---- the player is GTA's ped
    bool PlayerPos(Vec3* pos) override {
        CPlayerPed* ped = FindPlayerPed();
        if (!ped)
            return false;
        *pos = ped->GetPosition();
        return true;
    }

    // ---- what only GTA can do
    void FluidPlaced(const Int3& c) override { GtaSolidCellForget(c); }
    Vec3 PlayerVelocity() override { return mc::PlayerVelocity(FindPlayerPed()); }
    bool PlayerOnGround() override {
        CPlayerPed* ped = FindPlayerPed();
        return ped && mc::PlayerOnGround(ped);
    }
    bool PlayerFalling() override {
        CPlayerPed* ped = FindPlayerPed();
        return ped && (gGame.jumping ? gGame.flyVel.z < 0.0f : (!ped->bIsStanding && ped->m_vecMoveSpeed.z < 0.0f));
    }
    int PlayerVehicle() override {
        CPlayerPed* ped = FindPlayerPed();
        return ped && ped->bInVehicle && ped->m_pVehicle ? CPools::GetVehicleRef(ped->m_pVehicle) : -1;
    }
    void MovePlayer(const Vec3& to) override {
        if (CPlayerPed* ped = FindPlayerPed()) {
            StopFlying(ped);
            ped->Teleport(to, false);
        }
    }
    void HurtPlayer(float halfHearts) override {
        if (CPlayerPed* ped = FindPlayerPed()) {
            const float maxH = ped->m_fMaxHealth > 1.0f ? ped->m_fMaxHealth : 100.0f;
            const int dmg = (int)(maxH * halfHearts / 20.0f);
            if (!PlayerJetpackActive(ped)) {
                // the standing road, bit-for-bit: a generated fall-type
                // damage event on the player ped. On foot it lands - the
                // user has died to this channel, in this build, twice in
                // one session - and the game's armour never answers a
                // fall-type event, which is why the core keeps the vest's
                // books itself (PlayerArmor/WearPlayerArmor below).
                CWeapon::GenerateDamageEvent(ped, nullptr, WEAPONTYPE_FALL, dmg, (ePedPieceTypes)3, 0);
                return;
            }
            // THE JETPACK GATE (the user's verdicts, 2026-11-07, twice):
            // on the jetpack the same event never moved his health at
            // all. The whole path was read before this line was written:
            // the core's senses, target and pricing are honest 3D for an
            // airborne man (feet and chest at his true position, the
            // bullet's gates on the true line since the first act's fix,
            // proven in sims), and this function is the only application
            // there is - so the gate is the game's own processing of a
            // fall-type damage event for a ped whose jetpack task owns
            // him: fall damage is the one hurt a jetpack flight is built
            // never to take, and the event is swallowed whole. The
            // routing follows the channel's own design - the core keeps
            // the books (it priced this delta, vest and all), so the
            // bridge pays the same priced delta straight onto his
            // health. The event is still generated, for whatever
            // reaction the game gives it; only the shortfall it did not
            // pay synchronously is written directly, so a game build
            // whose event DOES land up there is never charged twice.
            // The line is the conviction instrument, the way boss state:
            // was: priced, event-paid, direct-paid, health either side.
            const float before = ped->m_fHealth;
            CWeapon::GenerateDamageEvent(ped, nullptr, WEAPONTYPE_FALL, dmg, (ePedPieceTypes)3, 0);
            const float afterEvent = ped->m_fHealth;
            const float eventPaid = std::max(0.0f, before - afterEvent);
            float direct = 0.0f;
            if (eventPaid < (float)dmg - 0.01f) {
                direct = (float)dmg - eventPaid;
                ped->m_fHealth = std::max(0.0f, afterEvent - direct);
            }
            Log("boss dmg: jetpack application - priced %.1f hh (event %d), health %.1f -> %.1f (event paid %.1f, direct %.1f)",
                halfHearts, dmg, before, ped->m_fHealth, eventPaid, direct);
        }
    }
    // the player's vest, for the core's kevlar books: the channel above is a
    // fall-type event, which the game's own armour never answers - so the
    // core reads the vest here and wears it down there, at the same scale
    // (20 half hearts = the vest's full 100 points)
    float PlayerArmor() override {
        CPlayerPed* ped = FindPlayerPed();
        return ped ? ped->m_fArmour : 0.0f;
    }
    void WearPlayerArmor(float halfHearts) override {
        if (CPlayerPed* ped = FindPlayerPed())
            ped->m_fArmour = std::max(0.0f, ped->m_fArmour - halfHearts * 5.0f);
    }
    float AimDistance(const Vec3& origin, const Vec3& dir, float reach, float nothing) override {
        return GtaAimDistance(origin, dir, reach, nothing);
    }
    HostHit BlowTrace(const Vec3& origin, const Vec3& dir, float reach) override { return GtaBlowTrace(origin, dir, reach); }
    HostHit ShotTrace(const Vec3& from, const Vec3& dir, float len, const ShotOwner& by) override {
        return GtaShotTrace(from, dir, len, by);
    }
    void HurtBeing(int being, float halfHearts, int how, const ShotOwner* by) override { GtaHurtBeing(being, halfHearts, how, by); }
    void PushBeing(int being, const Vec3& velocity) override { GtaPushBeing(being, velocity); }
    void NearbyBeings(const Vec3& centre, float radius, std::vector<BeingInfo>& out) override {
        GtaNearbyBeings(centre, radius, out);
    }
    void HurtVehicle(int vehicle, float halfHearts) override { GtaHurtVehicle(vehicle, halfHearts); }
    // ---- the Driver 2 cop's body (thin forwards into GtaCop.cpp)
    int SpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face) override {
        return GtaSpawnCopBody(at, headingDeg, face);
    }
    bool CopBodyReport(int veh, CopBody* out) override { return GtaCopBodyReport(veh, out); }
    void CopBodyDrive(int veh, float gas, float brake, float steer, bool siren) override {
        GtaCopBodyDrive(veh, gas, brake, steer, siren);
    }
    void CopBodySetHealth(int veh, float book01) override { GtaCopBodySetHealth(veh, book01); }
    void CopBodyRetire(int veh, bool burnOut) override { GtaCopBodyRetire(veh, burnOut); }
    float PlayerVehicleImpact() override { return GtaPlayerVehicleImpact(); }
    // ---- the boss's vehicle senses (workstream L)
    bool PlayerVehiclePose(Vec3* centre, Vec3* forward, Vec3* driverChest, Vec3* velocity) override {
        CPlayerPed* ped = FindPlayerPed();
        if (!ped || !ped->bInVehicle || !ped->m_pVehicle)
            return false;
        CVehicle* veh = ped->m_pVehicle;
        CMatrix& mx = veh->GetMatrix();
        *centre = mx.pos;
        *forward = mx.at;
        // the driver's chest is where the game itself seats him: his own ped
        // position rides at the seat - plus a hand's breadth up his spine
        *driverChest = ped->GetPosition() + CVector(0, 0, 0.35f);
        *velocity = veh->m_vecMoveSpeed * 50.0f; // per-frame at 50 fps -> per second
        return true;
    }
    // smart pack: any vehicle's frame, for the boss's zoned vehicle damage -
    // the same matrix the player-car pose reads, resolved from the pool ref
    bool VehiclePose(int vehicle, Vec3* centre, Vec3* forward) override {
        CVehicle* veh = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAtRef(vehicle) : nullptr;
        if (!veh)
            return false;
        CMatrix& mx = veh->GetMatrix();
        *centre = mx.pos;
        *forward = mx.at;
        return true;
    }
    void VehicleShotEffect(int vehicle, const Vec3& hitPoint, int effect) override {
        if (effect != 1)
            return; // 1 is the only effect: burst the tyre nearest the hit
        CVehicle* veh = CPools::ms_pVehiclePool ? CPools::ms_pVehiclePool->GetAtRef(vehicle) : nullptr;
        if (!veh)
            return;
        CMatrix& mx = veh->GetMatrix();
        const Vec3 d = hitPoint - Vec3(mx.pos);
        const float along = d.Dot(Vec3(mx.at));    // > 0: the front half
        const float side = d.Dot(Vec3(mx.right));  // > 0: the car's own right
        // a bike has two wheels, not four corners: 0 front, 1 rear in the
        // game's own component ids (appearance 2 is the bike family)
        if (veh->GetVehicleAppearance() == 2) {
            veh->BurstTyre((unsigned char)(along > 0.0f ? 0 : 1), true);
            return;
        }
        // SA's wheel component ids for BurstTyre: 0 front-left, 1 rear-left,
        // 2 front-right, 3 rear-right
        const int id = along > 0.0f ? (side < 0.0f ? 0 : 2) : (side < 0.0f ? 1 : 3);
        veh->BurstTyre((unsigned char)id, true);
    }
    bool PlayerAimingAt(const Vec3& point, float radius) override {
        CPlayerPed* ped = FindPlayerPed();
        if (!ped || !ped->bIsAimingGun)
            return false; // the game's own flag: he is pointing a gun at something
        const Vec3 rel = point - gGame.eyePos;
        const float t = rel.Dot(gGame.lookDir);
        if (t < 0.0f)
            return false;
        const Vec3 closest = gGame.eyePos + gGame.lookDir * t;
        return (closest - point).Length() < radius;
    }
    HostHit BeingTrace(const Vec3& origin, const Vec3& dir, float reach) override {
        HostHit h;
        const PedHit ph = RaycastPeds(origin, dir, reach, FindPlayerPed(), false);
        if (ph.ped) {
            h.being = CPools::GetPedRef(ph.ped);
            h.beingDist = ph.dist;
            h.beingPoint = ph.point;
        }
        return h;
    }
    bool UseOnBeing(int being) override {
        return VillagerInteract(CPools::ms_pPedPool ? CPools::ms_pPedPool->GetAtRef(being) : nullptr);
    }
    HostHit HookTrace(const Vec3& from, const Vec3& dir, float len) override { return GtaHookTrace(from, dir, len); }
    bool HookPoint(const HostHit& hooked, Vec3* at) override {
        CVector p;
        if (!GtaHookPoint(hooked, p))
            return false;
        *at = p;
        return true;
    }
    bool Fling(const HostHit& what, const Vec3& velocity) override { return GtaFling(what, velocity); }
    void Gust(const Vec3& centre, float radius, float side, float up, bool playerToo) override {
        GtaGust(centre, radius, side, up, playerToo);
    }
    void Douse(const Vec3& at, float radius) override { gFireManager.ExtinguishPoint(at, radius); }
    void Ignite(const Vec3& at, float seconds, int spread) override { GtaIgnite(at, seconds, spread); }
    bool Thunderstorm() override { return CWeather::Rain > 0.1f; }
    std::string PlayerName() override { return gGta.steve ? "Steve" : "CJ"; }
    void KillPlayer() override {
        if (CPlayerPed* ped = FindPlayerPed())
            ped->m_fHealth = 0.0f;
    }
    void SetWeather(int kind) override {
        // GTA's weathers: sunny, rainy, and the rainy one of San Fierro with its thunder
        static const short kTypes[3] = { WEATHER_SUNNY_LA, WEATHER_RAINY_COUNTRYSIDE, WEATHER_RAINY_SF };
        CWeather::ForceWeatherNow(kTypes[std::clamp(kind, 0, 2)]);
        CWeather::ReleaseWeather(); // (and it goes on from there)
        if (kind == 0)
            CWeather::Rain = 0.0f;
    }
    void SetWanted(int level) override {
        if (CPlayerPed* ped = FindPlayerPed())
            ped->SetWantedLevel(level);
    }
    float ClockHours() override { return CClock::ms_nGameClockHours + CClock::ms_nGameClockMinutes / 60.0f; }
    void SetClock(float hours, bool nextDay) override {
        const int h = (int)hours, m = (int)((hours - h) * 60.0f);
        CClock::SetGameClock((unsigned char)h, (unsigned char)m, nextDay ? (unsigned char)(CClock::CurrentDay % 7 + 1) : 0);
    }
    void ClearWeather() override {
        CWeather::ForceWeatherNow(WEATHER_SUNNY_LA);
        CWeather::ReleaseWeather(); // (the weather goes on from a sunny one)
        CWeather::Rain = 0.0f;
    }
    bool PlaceVehicle(int kind, const Vec3& at, float headingDeg) override { return GtaPlaceVehicle(kind, at, headingDeg); }
    void BoostVehicle(float seconds) override { GtaBoostVehicle(seconds); }
    bool ItemAttack(int special) override { return GtaItemAttack(special); }
    bool ItemUse(int special) override { return GtaItemUse(special); }
    void Explosion(const Vec3& at, int kind) override {
        static const eExplosionType kTypes[] = { EXPLOSION_GRENADE, EXPLOSION_SMALL, EXPLOSION_MOLOTOV }; // BlastKind
        gOwnBlasts.push_back({ at, 0.0f });
        CExplosion::AddExplosion(nullptr, FindPlayerPed(), kTypes[kind], at, 0, true, -1.0f, false);
    }
    void BlastMap(const Vec3& at, float radius, std::vector<BlastedCell>& opened) override {
        TerrainExplode(at, radius);
        // GTA buildings next to the blast: a crater in them too
        for (const CarveOpened& o : CarveExplode(at, radius))
            opened.push_back({ o.cell, o.block, o.surface });
    }
};
GtaHost gGtaHost;
const bool gGtaHostSet = (SetHost(&gGtaHost), true);
} // namespace

void PollExplosions(float dt) {
    for (size_t i = 0; i < gOwnBlasts.size();) {
        gOwnBlasts[i].age += dt;
        if (gOwnBlasts[i].age > 2.0f) {
            gOwnBlasts[i] = gOwnBlasts.back();
            gOwnBlasts.pop_back();
        } else {
            ++i;
        }
    }
    for (int i = 0; i < 16; ++i) {
        CExplosion& e = aExplosions[i];
        // the "active counter" byte lives at offset 0x28 (starts at 1, counts frames, 0 = free slot)
        const uint8_t counter = *(reinterpret_cast<const uint8_t*>(&e) + 0x28);
        const bool fresh = counter != 0 && (gExpCounter[i] == 0 || counter < gExpCounter[i] ||
                                            (e.m_vecPosition - gExpPos[i]).Magnitude() > 0.01f);
        gExpCounter[i] = counter;
        gExpPos[i] = e.m_vecPosition;
        if (!fresh)
            continue;
        bool own = false;
        for (auto& o : gOwnBlasts)
            if ((o.pos - e.m_vecPosition).Magnitude() < 0.05f)
                own = true;
        if (own)
            continue;
        float radius = 0.0f;
        switch (e.m_nType) {
        case EXPLOSION_GRENADE: radius = 3.2f; break;
        case EXPLOSION_ROCKET: case EXPLOSION_TANK_FIRE: radius = 3.5f; break;
        case EXPLOSION_WEAK_ROCKET: case EXPLOSION_MINE: radius = 2.5f; break;
        case EXPLOSION_CAR: case EXPLOSION_QUICK_CAR: case EXPLOSION_BOAT: radius = 3.0f; break;
        case EXPLOSION_AIRCRAFT: radius = 4.5f; break;
        case EXPLOSION_SMALL: case EXPLOSION_RC_VEHICLE: case EXPLOSION_OBJECT: radius = 1.5f; break;
        default: radius = 0.0f; break;
        }
        if (radius > 0)
            ExplodeAt(e.m_vecPosition, radius, false);
    }
}

} // namespace mc
