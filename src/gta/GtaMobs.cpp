#include "GtaMobs.h"

#include <algorithm>
#include <chrono>

#include "CAtomicModelInfo.h"
#include "CCamera.h"
#include "CColModel.h"
#include "CColPoint.h"
#include "CCopPed.h"
#include "CDecisionMakerTypes.h"
#include "CModelInfo.h"
#include "CObject.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CTaskComplexJump.h"
#include "CTaskComplexKillPedOnFoot.h"
#include "CTaskComplexSeekEntityMove.h"
#include "CTaskSimpleDuck.h"
#include "CVehicle.h"
#include "CWaterLevel.h"
#include "CWorld.h"
#include "ePedType.h"
#include "extensions/ScriptCommands.h"

#include "BossTest.h"
#include "Config.h"
#include "Draw3D.h"
#include "Game.h"
#include "GtaWorld.h"
#include "Host.h"
#include "Inventory.h"
#include "McModel.h"
#include "Mobs.h"
#include "PedSkins.h"
#include "SoldierModel.h"
#include "PlayerAnim.h"
#include "Render3D.h"
#include "Textures.h"

// The GTA side of the animals: drawing them, GTA's vehicles running into them, and where on the GTA map a herd may
// appear. How they live is the core's business (src/core/Mobs.h).

namespace mc {

namespace {
// While this runs, the proxy sync does not relay damage: a vehicle impact
// the core has already priced through MobVehicleHit has also torn at the
// proxy's game health, and one blow must not be billed twice.
float gRamMuteT = 0.0f;

// vehicles hurt and throw the animals they run into
void CarsHitMobs() {
    auto* pool = CPools::ms_pVehiclePool;
    if (!pool || gMobs.empty())
        return;
    static float nearMissT = 0.0f;
    nearMissT -= 0.05f;
    for (int i = 0; i < pool->m_nSize; ++i) {
        CVehicle* v = pool->GetAt(i);
        if (!v)
            continue;
        CVector vel = v->m_vecMoveSpeed * 50.0f;
        float speed = vel.Magnitude();
        if (speed < 4.0f)
            continue;
        CVector vp = v->GetPosition();
        for (size_t j = 0; j < gMobs.size(); ++j) {
            Mob& m = gMobs[j];
            if (m.death >= 0.0f || m.hitByCar > 0.0f)
                continue;
            if (m.kind == MOB_SOLDIER && speed > 6.0f) {
                // the boss reads traffic the way a man does (workstream L3): a
                // fast car on a line for him is a dodge before it is a hit, and
                // one that just screamed past him is a sound in his ears
                CVector rel = m.pos - vp;
                const float tca = rel.x * vel.x + rel.y * vel.y;
                if (tca > 0.0f) {
                    const float tt = tca / (speed * speed);
                    CVector closest = CVector(rel.x - vel.x * tt, rel.y - vel.y * tt, 0.0f);
                    const float perp = closest.Magnitude();
                    const float ahead = speed * tt;
                    if (perp < 1.7f && ahead < 13.0f)
                        MobVehicleThreat((int)j, vp, vel, v->m_pDriver == (CPed*)FindPlayerPed());
                    else if (perp < 3.5f && ahead < 6.0f && nearMissT <= 0.0f) {
                        NotePlayerShot(m.pos, 22.0f);
                        nearMissT = 2.0f;
                    }
                }
            }
            CVector d = (m.pos + CVector(0, 0, MobHeight(m) * 0.5f)) - vp;
            // 2.6 m from the vehicle's centre: half a car's length plus his
            // own radius - the old 2.3 m let a bumper that visibly reached
            // him stop a hair short of counting (the eye verdict's ram)
            if (std::fabs(d.z) > 1.8f || d.x * d.x + d.y * d.y > 2.6f * 2.6f)
                continue;
            m.hitByCar = 0.6f;
            if (m.kind == MOB_SOLDIER) {
                // his own accounting: capped, never a one-shot, the driver known
                MobVehicleHit((int)j, speed, vp, v->m_pDriver == (CPed*)FindPlayerPed());
                gRamMuteT = 0.35f; // the same impact also wounds his perception
                                   // body in the game's books: the proxy sync
                                   // must not relay it a second time
            } else {
                MobHurt((int)j, speed * 0.45f, vp, 0.0f);
            }
            m.vel = vel * 1.1f + CVector(0, 0, 3.0f + speed * 0.25f);
            m.onGround = false;
        }
    }
}

// ------------------------------------------------------------------
// The boss's perception body (workstream K2): ONE invisible real ped of the
// game's own, glued to the boss. He exists so the game's own hostility has
// something to aim at: a cop-type body, because the gangs' hate for the law
// is written in the game's own relationship tables and needs no scripting
// from us. The fire the street pours into him is read back as health deltas
// and relayed into the boss's core health (MobProxyDamage, capped), and a
// ped whose own kill task names him is reported as an attacker
// (MobProvoked) - the boss's K1 self-defence does the rest. He is invisible
// in layers (the entity flag alone proved re-decidable by the game - the
// first eye test saw him; BossProxySync now also strips his clump's render
// flags every frame and the villager system is told to never skin him),
// taskless by construction (position and heading are re-glued every frame;
// he never gets to walk anywhere), and he dies with the boss: no orphans.
struct BossProxyState {
    CPed* ped = nullptr;
    int ref = 0; // his pool handle (slot index + generation byte): the only name
                 // the game itself trusts for a ped across frames
    uint32_t mobId = 0;
    bool createFailed = false; // creation produced a non-pool body once: latched
                               // off for the session rather than retried per frame
    float scanT = 0.0f;
    float cowerT = 0.0f;
};
BossProxyState gBossProxy;

// CPed::m_nCreatedBy, the game's own ownership bookkeeping: a ped the
// population system spawned for the street is RANDOM (1) and population may
// despawn it whenever the player leaves its area; a ped a script created is
// MISSION (2) and only its owner may remove it - every story character in
// the game lives across the whole map on exactly this marking.
constexpr unsigned char kCreatedByMission = 2;

// Where his body really comes from (this corrects the record of the crash
// audit, which read the C++ surface and stopped one layer too early):
// `new CPed` in this build does NOT reach the CRT heap. CPed declares a
// class operator new (plugin-sdk CPed.h), the build compiles the SDK's
// CPed.cpp in (CMake globs all of plugin_sa), and that operator new is a
// call into the game's own CPed::operator new (0x5E4720) - which, like
// every entity class in this engine generation (reversed III/VC sources
// spell it out: return CPools::GetPedPool()->New()), hands out the next
// slot of CPools::ms_pPedPool, an array of CCopPed-sized slots. He was a
// pool ped all along. The defects were in HOW he was put there:
//   (1) The bare CPed base was constructed in a CCopPed-sized slot, so the
//       cop tail (0x79C..0x7C4: cop type, partner pointer, the criminals
//       array) stayed stale bytes from the slot's previous tenant, for any
//       cop system that reaches a PED_TYPE_COP body by cast to read and
//       write through.
//   (2) His m_nCreatedBy kept the bare constructor's default, so in the
//       game's eyes he was population's own random cop, and population
//       could lawfully despawn him - at distance, on an area rebuild after
//       a hospital respawn - while we went on holding the pointer.
//   (3) He was held by raw pointer alone. After such a despawn the slot is
//       destructed (its intelligence and RwObject already freed or handed
//       to a new tenant) while the address still reads as a ped for a
//       while: every glued touch (Teleport, SetPedDrawn's clump walk,
//       UpdateRpHAnim) writes through freed-or-recycled structures, and a
//       late delete runs the destructor a second time on a body the game
//       already destroyed. Silent corruption, detonating wherever the
//       damaged pointer is next followed - the user-bisected signature.
// The fix is to build him the way the game builds its own and hold him the
// way scripts hold theirs: a CCopPed, marked MISSION at birth, resolved by
// pool ref before every touch. The field check below survives only as a
// second belt, and is only ever read after the ref has resolved.
bool BossProxyIsOurs(CPed* ped) {
    return ped && ped->m_nType == ENTITY_TYPE_PED && ped->m_nPedType == PED_TYPE_COP &&
           ped->m_nModelIndex == 0;
}

bool BossProxyAlive(CPed* ped) {
    return BossProxyIsOurs(ped) && ped->m_fHealth > 0.0f &&
           ped->m_ePedState != PEDSTATE_DIE && ped->m_ePedState != PEDSTATE_DEAD;
}

// Drop the pointer and the ref and touch NOTHING through either. For the
// states where the game has already taken the body (its slot freed or
// recycled under us): removing or deleting through the pointer in that
// state is a double-destruction waiting for a frame.
void BossProxyForget() {
    if (gBossProxy.ped)
        Log("boss proxy: his body no longer resolves by its pool ref - forgetting it, hands off");
    gBossProxy.ped = nullptr;
    gBossProxy.ref = 0;
    gBossProxy.mobId = 0;
}

// ------------------------------------------------------------------
// The driver (the hybrid step, 2026-11-07): the same body, promoted. While
// the driver mode is on (the default; /bosstest driver turns it off), the
// body is no longer glued to the boss - it IS his feet. The game's own ped
// locomotion walks it: one persistent CTaskComplexSeekEntityMove aimed at
// an invisible anchor object the bridge teleports to the objective point
// the core's brain issues (Mob::drvObj, written only by BossDriveObjective
// in the core). The brain stays the commander - decisions, memory, the
// aggro law, the Counter-Strike pricing, the atlas - and this layer only
// executes movement, the way the game's own peds flow around cars, fences
// and corners on the street's node graph. What the SDK honestly offers was
// inventoried before a line was designed: there is NO go-to-point task
// class in this SDK (TASK_COMPLEX_GO_TO_POINT_AND_STAND_STILL exists as an
// enum value with no class behind it), so the seek-an-entity task plus a
// movable anchor is the directed native locomotion that exists - the
// anchor is a point made into an entity, nothing more is claimed.
// The body's own will is contained at birth (BossProxyCreate): the EMPTY
// decision maker (the game's own choice for script peds that must never
// decide anything), senses zeroed so no event scanner can feed even that,
// and no weapon it could fire with. What containment cannot reach - a
// physical-contact event answer the engine gives below the decision maker
// (a shove answered with a brace) - is watched for, not hidden: the apply
// step below never stomps a foreign primary task, it waits it out and
// reclaims the slot only when the body has somewhere to be.
struct BossDriverState {
    bool active = false;     // the driver mode is carrying him right now
    bool didPre = false;     // the pre-tick reported for this frame (MobsUpdate's book)
    bool anchored = false;   // the CURRENT body has been placed at his feet for driving
    CObject* anchorObj = nullptr; // the seek target: an invisible mission object
    int anchorRef = 0;            // its pool handle, the only name trusted across frames
    int anchorModel = -1;         // the synthetic model id (process-lifetime registration)
    CTask* seekTask = nullptr;    // the task we issued - an identity to compare, NEVER to dereference
    CTask* jumpTask = nullptr;    // the hop we issued, same discipline: an identity, never a pointer to read
    float jumpHoldT = 0.0f;       // seconds our jump has held the primary slot: its deadline clock (a jump waited out without one held the statue's feet forever)
    eMoveState appliedMove = PEDMOVE_STILL; // the speed class the feet currently walk under
    Vec3 appliedObj{ 0, 0, 0 };   // the objective point the anchor currently sits at
    bool appliedValid = false;
    float watchT = 0.0f; // how long a foreign primary task has owned his feet
};
BossDriverState gBossDriver;

// The anchor dies with the driver state: delete it through its ref, only
// if the ref still resolves to it (the pool discipline, same as the body).
void BossDriverTeardown() {
    if (gBossDriver.anchorObj) {
        CObject* cur = CPools::GetObject(gBossDriver.anchorRef);
        if (cur == gBossDriver.anchorObj && cur->m_nModelIndex == gBossDriver.anchorModel)
            plugin::Command<plugin::Commands::DELETE_OBJECT>(gBossDriver.anchorRef);
        gBossDriver.anchorObj = nullptr;
        gBossDriver.anchorRef = 0;
    }
    gBossDriver.seekTask = nullptr;
    gBossDriver.jumpTask = nullptr;
    gBossDriver.jumpHoldT = 0.0f;
    gBossDriver.active = false;
    gBossDriver.anchored = false;
    gBossDriver.appliedValid = false;
    gBossDriver.watchT = 0.0f;
}

// Retire the driver: the core is told its feet are gone (MobDriverGone -
// it steers him itself again, from exactly where he stands) and the
// driver's own state is torn down. The body itself is NOT touched here:
// killing it is BossProxyKill's work, and a mode switch hands it back to
// the legacy glue alive.
void BossDriverRetire() {
    if (gBossProxy.mobId)
        for (size_t j = 0; j < gMobs.size(); ++j)
            if (gMobs[j].id == gBossProxy.mobId)
                MobDriverGone((int)j);
    BossDriverTeardown();
}

// Stand the anchor up (once per body loss): register its synthetic model
// the way the collision system registers its block columns - an id the
// game does not use, draw distance zero so the streamer never asks for
// files that do not exist - with an EMPTY collision model and collision
// off besides: the anchor is a point to walk to, never a wall.
bool BossDriverAnchorEnsure() {
    if (gBossDriver.anchorObj) {
        CObject* cur = CPools::GetObject(gBossDriver.anchorRef);
        if (cur == gBossDriver.anchorObj && cur->m_nModelIndex == gBossDriver.anchorModel)
            return true;
        gBossDriver.anchorObj = nullptr; // the game took it: rebuild below
        gBossDriver.anchorRef = 0;
        gBossDriver.appliedValid = false;
    }
    if (gBossDriver.anchorModel < 0) {
        int count = CModelInfo::ms_modelInfoCount;
        if (count <= 0 || count > 100000)
            count = 20000;
        int id = -1;
        for (int cand = count - 1; cand > 18630; --cand) {
            if (CModelInfo::ms_modelInfoPtrs[cand])
                continue;
            CStreamingInfo& si = CStreaming::ms_aInfoForModel[cand];
            if (si.m_nLoadState != 0 || si.m_nCdSize != 0)
                continue;
            id = cand;
            break;
        }
        if (id < 0)
            return false;
        CColModel* col = static_cast<CColModel*>(calloc(1, sizeof(CColModel)));
        CCollisionData* data = static_cast<CCollisionData*>(calloc(1, sizeof(CCollisionData)));
        col->m_nColSlot = 0; // "generic" slot, never streamed out
        col->m_bHasCollisionVolumes = 0;
        col->m_bIsActive = 1;
        col->m_pColData = data;
        CAtomicModelInfo* mi = CModelInfo::AddAtomicModel(id);
        mi->m_fDrawDistance = 0.0f;
        mi->SetColModel(col, false); // false: the game must not delete our col model
        gBossDriver.anchorModel = id;
        Log("boss driver: anchor model %d registered - an invisible point his feet will seek", id);
    }
    CObject* obj = CObject::Create(gBossDriver.anchorModel);
    if (!obj)
        return false;
    obj->m_nObjectType = OBJECT_MISSION;
    obj->bIsVisible = false;
    obj->bDontStream = true;
    obj->bUsesCollision = false;
    obj->SetPosn(CVector(0.0f, 0.0f, -50.0f)); // parked underground until the first objective
    obj->UpdateRwMatrix();
    obj->UpdateRwFrame();
    CWorld::Add(obj);
    gBossDriver.anchorObj = obj;
    gBossDriver.anchorRef = CPools::GetObjectRef(obj);
    gBossDriver.appliedValid = false;
    return true;
}

// The one lawful question about a ped held across frames: does his ref
// still resolve, and to the same body? GetAtRef compares the slot's full
// flag byte (generation + occupied) against the ref, so a freed slot and a
// recycled slot both answer no - without a single read through the pointer
// itself. (The game's refs have one documented weakness, its own: the
// generation byte wraps after 128 reuses of the same slot. Every script
// character in the game lives under the same limit.)
CPed* BossProxyResolve() {
    auto* pool = CPools::ms_pPedPool;
    if (!pool || !gBossProxy.ped || !gBossProxy.ref)
        return nullptr;
    CPed* p = pool->GetAtRef(gBossProxy.ref);
    return p == gBossProxy.ped ? p : nullptr;
}

void BossProxyKill() {
    BossDriverRetire(); // his feet go with his body: the core steers again
    if (gBossProxy.ped) {
        CPed* ped = BossProxyResolve();
        if (!ped) {
            BossProxyForget(); // the game already took him: hands off entirely
            return;
        }
        PedSkinsIncludePed(ped); // his ref goes back to the slot's next tenant clean
        CWorld::Remove(ped);
        delete ped; // CPed::operator delete returns the slot to the pool
        gBossProxy.ped = nullptr;
        gBossProxy.ref = 0;
    }
    gBossProxy.mobId = 0;
}

// Build him the way the game builds its own cops and hand him to us the
// way scripts hold their characters. Returns false when no body could be
// stood up this frame (pool exhausted is transient; a body that fails the
// pool check latches the createFailed flag instead - see BossProxyState).
bool BossProxyCreate(const Mob& m) {
    auto* pool = CPools::ms_pPedPool;
    if (!pool)
        return false;
    CCopPed* cop = new CCopPed(COP_TYPE_CITYCOP); // pool slot, fully constructed cop body
    CPed* ped = cop;
    if (!ped)
        return false; // pool full right now: the next sync tries again
    ped->SetModelIndex(0);
    ped->SetCharCreatedBy(kCreatedByMission); // ours alone to remove: population never despawns him
    ped->ClearWeapons();                      // a sensor, as before: the street shoots him, he never shoots
    CWorld::Add(ped);
    // The acceptance fact, checked with the pool's own arithmetic: his
    // address must be a live slot of the ped pool, and the slot must be him.
    const int idx = pool->GetIndex(ped);
    if (idx < 0 || idx >= pool->m_nSize || pool->GetAt(idx) != ped || pool->IsFreeSlotAtIndex(idx)) {
        CWorld::Remove(ped);
        delete ped;
        gBossProxy.createFailed = true;
        Log("boss proxy: creation did not land in the ped pool (index %d) - proxy stays off for this session", idx);
        return false;
    }
    // Containment, from birth, whatever mode he serves in: the EMPTY
    // decision maker (the game's own answer for a script ped that must
    // never decide anything - no wanted-level reactions, no fleeing, no
    // fighting of its own) and his senses zeroed, so the event scanners
    // that feed a decision maker have nothing to feed it with. As a sensor
    // he was taskless by glue; as the driver he walks only where the one
    // seek task we issue takes him - and he carries no weapon either way.
    if (ped->m_pIntelligence) {
        ped->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        ped->m_pIntelligence->SetSeeingRange(0.0f);
        ped->m_pIntelligence->SetHearingRange(0.0f);
    }
    gBossDriver.anchored = false; // a fresh body has not been placed at his feet yet
    gBossDriver.seekTask = nullptr; // ...and it carries no seek of ours: the old body's
                                    // task pointer is a stale identity from here on
    gBossDriver.jumpTask = nullptr; // ...nor a hop of ours, for the same reason
    gBossDriver.jumpHoldT = 0.0f;
    gBossDriver.appliedValid = false;
    gBossProxy.ped = ped;
    gBossProxy.ref = pool->GetRef(ped);
    gBossProxy.mobId = m.id;
    ped->bIsVisible = false;
    PedSkinsExcludePed(ped);  // by ref, like every pool ped: the villager system must never adopt or draw him
    SetPedDrawn(ped, false);  // ...and his clump's atomics lose their render flags at once
    ped->m_fMaxHealth = 1000.0f;
    ped->m_fHealth = 1000.0f;
    Log("boss proxy: pool index %d, ref %d - one invisible mission cop body carries the street's fire for him",
        idx, gBossProxy.ref);
    return true;
}

int BossFindSoldier() {
    for (size_t j = 0; j < gMobs.size(); ++j)
        if (gMobs[j].kind == MOB_SOLDIER && gMobs[j].death < 0.0f)
            return (int)j;
    return -1;
}

// Stand his body up and prove it lives, the one way both modes share:
// another soldier's body is retired, a body the game recycled is
// forgotten untouched and rebuilt, a body that died in the game is
// retired for a fresh one next frame. False means: no live body this
// frame, the caller stands down.
bool BossBodyEnsure(Mob& m) {
    if (gBossProxy.ped && gBossProxy.mobId != m.id)
        BossProxyKill();
    if (gBossProxy.ped && !BossProxyResolve()) {
        // The game took him after all. A MISSION body should be beyond
        // population's reach, but a session teardown clears everyone, and
        // any path that did take him must find us holding nothing: resolve
        // first, touch second, and here the answer was no - never touch
        // that pointer again, obtain a fresh body below.
        Log("boss proxy: the game recycled his body - obtaining a fresh one");
        BossProxyForget();
    }
    if (!gBossProxy.ped && !gBossProxy.createFailed) {
        if (!BossProxyCreate(m))
            return false;
    }
    if (!gBossProxy.ped)
        return false;
    if (!BossProxyAlive(gBossProxy.ped)) {
        // He died in the game: the health top-up only runs while we sync,
        // and a single explosion-class hit can take the whole 1000 between
        // frames. A corpse is no perception body - the game is already
        // running its own death on him, gangs stop aiming at the dead, and
        // the relay below would read a stuck health value. Retire ours and
        // let the creation above build a fresh body next frame.
        Log("boss proxy: he died in the game - retiring the body, a fresh one is built next sync");
        BossProxyKill();
        return false;
    }
    return true;
}

// The body's standing duties, identical in both modes: who is fighting
// him (read off the street's own kill tasks), the damage relay into the
// core's books, and the street's fear. Only the feet differ between the
// sensor and the driver - everything the body is FOR stays one code path.
void BossBodyDuties(float dt, CPlayerPed* player, int bi, CPed* ped) {
    Mob& m = gMobs[bi];
    // who is fighting him, by the game's own books: a ped whose kill task
    // names the proxy. Throttled to 4 Hz and to the street around him.
    gBossProxy.scanT -= dt;
    int attacker = -1;
    Vec3 from = m.pos;
    if (gBossProxy.scanT <= 0.0f) {
        gBossProxy.scanT = 0.25f;
        auto* pool = CPools::ms_pPedPool;
        if (pool) {
            for (int i = 0; i < pool->m_nSize; ++i) {
                CPed* p = pool->GetAt(i);
                if (!p || p == ped || p == (CPed*)player || p->m_fHealth <= 0.0f || !p->m_pIntelligence)
                    continue;
                CVector d = p->GetPosition() - ped->GetPosition();
                if (d.x * d.x + d.y * d.y > 35.0f * 35.0f)
                    continue;
                CTaskManager& tm = p->m_pIntelligence->m_TaskMgr;
                for (int s = 0; s < TASK_PRIMARY_MAX; ++s) {
                    CTask* t = tm.m_aPrimaryTasks[s];
                    if (t && t->GetId() == TASK_COMPLEX_KILL_PED_ON_FOOT &&
                        ((CTaskComplexKillPedOnFoot*)t)->m_pTarget == ped) {
                        attacker = CPools::GetPedRef(p);
                        from = p->GetPosition();
                        MobProvoked(bi, attacker, from);
                        break;
                    }
                }
            }
        }
    }
    // how long since the player's own gun fired: attribution evidence for
    // the relay below (his bullets meet the proxy; no task names them, at
    // ANY range - the old 40 m guess silently un-attributed his long shots,
    // so damage from a sniper landed with no enemy made)
    static float sPlayerShotAge = 99.0f;
    {
        CWeapon& w = player->m_aWeapons[player->m_nSelectedWepSlot];
        if (w.m_nState == WEAPONSTATE_FIRING)
            sPlayerShotAge = 0.0f;
        else
            sPlayerShotAge += dt;
    }
    // the hurt he took since the last frame, relayed at sensor scale -
    // except a vehicle blow the core already priced this frame (CarsHitMobs
    // mutes the relay for it: one impact, one bill)
    gRamMuteT = std::max(0.0f, gRamMuteT - dt);
    const float h = ped->m_fHealth;
    if (h < 999.0f && gRamMuteT <= 0.0f) {
        int who = attacker;
        if (who < 0) {
            // no gang hand on it: the shot was the player's when he is near,
            // or when his gun fired within the last second at any range a
            // rifle reaches. Anything else stays unattributed - it lands on
            // the boss's health but makes no enemy of a bystander.
            CVector dp = player->GetPosition() - ped->GetPosition();
            const float d2 = dp.x * dp.x + dp.y * dp.y;
            if (d2 < 40.0f * 40.0f || (d2 < 120.0f * 120.0f && sPlayerShotAge < 1.0f))
                who = -2;
        }
        MobProxyDamage(bi, (1000.0f - h) * 2.0f, from, who);
    }
    // the street's fear while he fights: nearby civilians cower with the
    // game's own duck, a few at a time, four times a second at most
    gBossProxy.cowerT -= dt;
    if (gBossProxy.cowerT <= 0.0f) {
        gBossProxy.cowerT = 0.4f;
        if (m.bossState >= 2) {
            auto* pool = CPools::ms_pPedPool;
            int done = 0;
            if (pool) {
                for (int i = 0; i < pool->m_nSize && done < 3; ++i) {
                    CPed* p = pool->GetAt(i);
                    if (!p || p == ped || p == (CPed*)player || p->bInVehicle || p->m_fHealth <= 0.0f || !p->m_pIntelligence)
                        continue;
                    if (p->m_nPedType != PED_TYPE_CIVMALE && p->m_nPedType != PED_TYPE_CIVFEMALE)
                        continue;
                    CVector d = p->GetPosition() - ped->GetPosition();
                    if (d.x * d.x + d.y * d.y > 14.0f * 14.0f)
                        continue;
                    if (!p->m_pIntelligence->m_TaskMgr.GetTaskSecondary(TASK_SECONDARY_DUCK)) {
                        p->m_pIntelligence->m_TaskMgr.SetTaskSecondary(
                            new CTaskSimpleDuck(DUCK_STANDALONE, 900, -1), TASK_SECONDARY_DUCK);
                        ++done;
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------
// The driver frame runs in two halves around the core's tick, because the
// command flows one way: the body reports where the game's feet put him,
// the brain reads it and answers with an objective, the objective is
// applied to his feet. MobsUpdate calls the first half before MobsTick
// and BossProxySync (below) runs the second after it.

// First half: stand the body up, place it at his feet the first time,
// run its standing duties, and report its transform to the core. True
// means the frame is the driver's; false hands the frame to the legacy
// sync (no body could be stood up, or the anchor would not build - the
// glued sensor is the graceful degradation, never a frozen soldier).
bool BossDriverPreTick(float dt, CPlayerPed* player) {
    const int bi = BossFindSoldier();
    if (bi < 0) {
        BossProxyKill();
        return true; // no soldier: the kill above is the whole duty
    }
    Mob& m = gMobs[bi];
    if (!BossBodyEnsure(m))
        return false;
    CPed* ped = gBossProxy.ped;
    if (m.reanchor) {
        // A summon re-anchored him (the core's single-soldier door): the
        // man now stands at the summon point, but this body still walks
        // where the old man stood. Run the mode entry below again for it -
        // the body is placed at his feet, invisibly, the anchor re-stands,
        // and the standing objective is re-applied under the new ground.
        // No second body is ever built and the seek is not stomped: it
        // seeks the anchor entity, wherever the apply step moves it. Same
        // mob id, same body, new ground.
        m.reanchor = false;
        if (gBossDriver.anchored) {
            gBossDriver.anchored = false;
            gBossDriver.appliedValid = false;
            Log("boss driver: re-anchored with his soldier - his body is placed at his feet again");
        }
    }
    if (!gBossDriver.anchored) {
        // Mode entry: the body has ridden glued above his feet as a
        // sensor; from this frame it IS his feet. It is placed at his
        // feet once - invisibly, so no eye can see a set-down - and the
        // anchor stands up with it. Every later frame the body is simply
        // where the game's locomotion walked it.
        if (!BossDriverAnchorEnsure())
            return false;
        ped->Teleport(CVector(m.pos.x, m.pos.y, m.pos.z + 0.9f), false);
        gBossDriver.anchored = true;
        gBossDriver.active = true;
        Log("boss driver: his body is his feet now - the game's own locomotion walks him, this brain commands");
    }
    BossBodyDuties(dt, player, bi, ped);
    if (m.death >= 0.0f) {
        BossProxyKill();
        return true;
    }
    // top the body back up (the relay already read what the street did)
    // and keep him invisible and hittable, the same layers as the sensor
    ped->m_fHealth = 1000.0f;
    ped->m_fMaxHealth = 1000.0f;
    SetPedDrawn(ped, false);
    if (ped->m_pRwObject)
        ped->UpdateRpHAnim();
    // the report: his feet are the body's feet (a ped's position rides
    // ~0.9 m above them, the codebase's own convention), his ground state
    // is the body's own standing flag
    const CVector pp = ped->GetPosition();
    MobDriverReport(bi, Vec3(pp.x, pp.y, pp.z - 0.9f), ped->bIsStanding != 0, dt);
    gBossDriver.didPre = true;
    return true;
}

// The bridge's speed classes: the core orders in metres per second, the
// game's feet walk in its own gaits. The classes are approximations by
// design - a patrol is a walk, a strafe a jog, the chase a sprint - and
// the exact pace is the game's own, which is the point of the exercise.
eMoveState BossDriverMoveClass(float speed) {
    if (speed <= 0.01f)
        return PEDMOVE_STILL;
    if (speed < 2.2f)
        return PEDMOVE_WALK;
    if (speed < 4.4f)
        return PEDMOVE_JOG;
    if (speed < 5.8f)
        return PEDMOVE_RUN;
    return PEDMOVE_SPRINT;
}

// Second half: hand the brain's objective to the feet. The anchor moves
// only when the order truly changes (a new point, a new class), and the
// seek task is issued once and then only watched: it re-reads the anchor
// on its own scan interval, so a streaming intercept costs no task churn.
// A speed-class change on the running task is a field write. A foreign
// primary task (the game's own get-up, a physical response) is never
// stomped: it is waited out, and the slot is reclaimed only when it
// overstays while he has somewhere to be.
void BossDriverApply(float dt) {
    const int bi = BossFindSoldier();
    if (bi < 0)
        return;
    Mob& m = gMobs[bi];
    if (m.death >= 0.0f) {
        BossProxyKill();
        return;
    }
    CPed* ped = BossProxyResolve();
    if (!ped || !ped->m_pIntelligence)
        return; // the pre-tick's body is gone mid-frame: next frame rebuilds
    if (!BossDriverAnchorEnsure())
        return;
    const Vec3 obj = m.drvObjValid ? m.drvObj : m.pos;
    const float spd = m.drvObjValid ? m.drvObjSpeed : 0.0f;
    const bool stand = spd <= 0.01f;
    const eMoveState ms = BossDriverMoveClass(spd);
    const bool classChanged = !gBossDriver.appliedValid || ms != gBossDriver.appliedMove;
    Vec3 dObj = obj - gBossDriver.appliedObj;
    dObj.z = 0.0f;
    const bool moved = !gBossDriver.appliedValid || dObj.Length() > 0.75f;
    if (classChanged || moved) {
        gBossDriver.anchorObj->SetPosn(CVector(obj.x, obj.y, obj.z + 0.9f));
        gBossDriver.anchorObj->UpdateRwMatrix();
        gBossDriver.anchorObj->UpdateRwFrame();
        gBossDriver.appliedObj = obj;
        gBossDriver.appliedMove = ms;
        gBossDriver.appliedValid = true;
    }
    CTaskManager& tm = ped->m_pIntelligence->m_TaskMgr;
    // The hop (the commander's order, executed on the body): one real
    // CTaskComplexJump as his primary task - the game's own jump, the
    // class its own peds hurdle fences and low walls with. The inventory
    // was read before a line was designed: CTaskComplexJump exists whole
    // in this SDK with a public constructor (0 = jump, 1 = climb), and it
    // is the task used here; CTaskSimpleClimb was NOT used - it wants a
    // climb entity and a handhold position the game's own obstacle
    // scanner computes, which we cannot honestly supply. Nothing about
    // the hop is our physics: no velocity is written, no arc is faked -
    // the task runs its course on the body, the report's ground state
    // tells the core he is airborne, and when the task leaves the slot
    // the ordinary flow below re-issues the seek at the standing
    // objective: he lands and walks on.
    if (m.drvHop) {
        m.drvHop = false;
        auto* jump = new CTaskComplexJump(0);
        tm.SetTask(jump, TASK_PRIMARY_PRIMARY, false);
        gBossDriver.jumpTask = jump;
        gBossDriver.jumpHoldT = 0.0f;
        gBossDriver.watchT = 0.0f;
        Log("boss driver: hop - the commander ordered a jump at (%.0f, %.0f)", m.pos.x, m.pos.y);
    }
    CTask* cur = tm.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY];
    if (gBossDriver.jumpTask && cur != gBossDriver.jumpTask) {
        gBossDriver.jumpTask = nullptr; // the jump ran its course and left the slot
        gBossDriver.jumpHoldT = 0.0f;
    }
    if (cur && cur == gBossDriver.jumpTask) {
        // our own jump owns his feet: wait it out like a seek - but under
        // a deadline. A jump is a second of physics; the foreign-task
        // reclaim below never applied to it, so a jump that is slow to
        // leave (or never leaves) this body would hold the slot - and
        // his feet - for the rest of the session: the statue of
        // 2026-11-07 stood exactly in this wait, the brain ordering and
        // shooting over a body that never walked. Past the deadline the
        // seek is issued over it, the same reclaim foreign tasks get,
        // and his feet are his again whatever the jump was doing.
        gBossDriver.jumpHoldT += dt;
        if (gBossDriver.jumpHoldT > 2.5f) {
            auto* seek = new CTaskComplexSeekEntityMove(gBossDriver.anchorObj,
                600000, 250, 1.0f, 7.0f, 3.0f, false, false);
            seek->m_moveState = ms;
            tm.SetTask(seek, TASK_PRIMARY_PRIMARY, false);
            gBossDriver.seekTask = seek;
            gBossDriver.jumpTask = nullptr;
            gBossDriver.jumpHoldT = 0.0f;
            Log("boss driver: the jump overstayed its welcome - the seek is re-issued over it");
        }
        gBossDriver.watchT = 0.0f;
    } else if (cur && cur == gBossDriver.seekTask) {
        if (classChanged)
            ((CTaskComplexSeekEntityMove*)cur)->m_moveState = ms;
        gBossDriver.watchT = 0.0f;
    } else if (!cur) {
        // the slot is free - he arrived, or a seek ran its course. A stand
        // he is already standing on needs no new task: the empty decision
        // maker keeps him standing, and churning seek objects under a
        // standing man would be pure waste.
        const CVector pp = ped->GetPosition();
        const CVector ap = gBossDriver.anchorObj->GetPosition();
        const float dx = pp.x - ap.x, dy = pp.y - ap.y;
        if (!stand || dx * dx + dy * dy > 1.2f * 1.2f) {
            auto* seek = new CTaskComplexSeekEntityMove(gBossDriver.anchorObj,
                600000, 250, 1.0f, 7.0f, 3.0f, false, false);
            seek->m_moveState = ms;
            tm.SetTask(seek, TASK_PRIMARY_PRIMARY, false);
            gBossDriver.seekTask = seek;
        }
        gBossDriver.watchT = 0.0f;
    } else {
        gBossDriver.watchT += dt;
        if (!stand && gBossDriver.watchT > 1.5f) {
            auto* seek = new CTaskComplexSeekEntityMove(gBossDriver.anchorObj,
                600000, 250, 1.0f, 7.0f, 3.0f, false, false);
            seek->m_moveState = ms;
            tm.SetTask(seek, TASK_PRIMARY_PRIMARY, false);
            gBossDriver.seekTask = seek;
            gBossDriver.watchT = 0.0f;
            Log("boss driver: a foreign task held his feet past its welcome - the seek is re-issued");
        }
    }
}

void BossProxySync(float dt, CPlayerPed* player) {
    if (gBossTest.proxyOff) {
        // /bosstest proxy: the bisect removes his perception body entirely -
        // the street has nothing of his to aim at and he takes no relayed
        // fire, for as long as the toggle stays off this session
        if (gBossProxy.ped) {
            BossProxyKill();
            Log("BOSS TEST: proxy removed - he has no perception body while the toggle is off");
        }
        return;
    }
    if (gBossDriver.didPre) {
        // the driver carried this frame: only the objective is still owed
        gBossDriver.didPre = false;
        BossDriverApply(dt);
        return;
    }
    if (gBossDriver.active)
        BossDriverRetire(); // the mode was switched off mid-session (or a
                            // frame the driver could not carry): the legacy
                            // glue below takes the body back over, alive,
                            // exactly where it stands
    const int bi = BossFindSoldier();
    if (bi < 0) {
        BossProxyKill();
        return;
    }
    Mob& m = gMobs[bi];
    if (!BossBodyEnsure(m))
        return;
    CPed* ped = gBossProxy.ped;
    // a summon's re-anchor needs no word in this mode: the glue below
    // re-places his body at his feet this very frame, by construction -
    // the flag is simply spent
    m.reanchor = false;
    BossBodyDuties(dt, player, bi, ped);
    if (m.death >= 0.0f) {
        BossProxyKill();
        return;
    }
    // top the sensor back up and glue him to the boss, every frame
    ped->m_fHealth = 1000.0f;
    ped->m_fMaxHealth = 1000.0f;
    ped->Teleport(CVector(m.pos.x, m.pos.y, m.pos.z + 0.9f), false);
    ped->SetHeading(m.yaw);
    ped->m_vecMoveSpeed = CVector(0, 0, 0);
    // He never shows - by layers, not by one flag. The entity flag alone was
    // the first build's only lock, and the game re-decides it (the first boss
    // eye test saw a separate skin standing on the boss, reaching up out of
    // his body: this body, rendered - his feet ride 0.9 m above the boss's,
    // so a shown proxy reads as a second man growing out of him). SetPedDrawn
    // re-strips the render flag from every atomic of his clump each frame, so
    // even a clump that streams in late has nothing drawable, whatever the
    // flag says; the PedSkins exclusion registered at creation covers the
    // villager overlay. And his skeleton is kept up to date by hand: a ped
    // the renderer skips stops updating the hit spheres the street's bullets
    // test against, and he must stay hittable.
    SetPedDrawn(ped, false);
    if (ped->m_pRwObject)
        ped->UpdateRpHAnim();
}
} // namespace

void BossProxySessionReset() {
    // a new session (load / new game): the game has torn every ped in the
    // world down, this body included - forget the pointer and the ref
    // without a single touch through either. PedSkinsForget has done
    // exactly this for the villager system since long ago; the proxy was
    // missing from that list, and the first sync after a load would have
    // killed through the stale pointer (Remove + delete on a body the game
    // already destroyed). A latched creation failure belongs to the old
    // session too: the new one gets its own attempt.
    BossProxyForget();
    gBossProxy.createFailed = false;
    // the driver's state belongs to the torn-down world too: pointers off,
    // nothing touched through them (the anchor object died with the world;
    // its model registration is process-lifetime and stays, the way the
    // collision system's does). The next session's first driver frame
    // stands a fresh body and a fresh anchor up.
    gBossDriver.anchorObj = nullptr;
    gBossDriver.anchorRef = 0;
    gBossDriver.seekTask = nullptr;
    gBossDriver.jumpTask = nullptr;
    gBossDriver.jumpHoldT = 0.0f;
    gBossDriver.active = false;
    gBossDriver.anchored = false;
    gBossDriver.appliedValid = false;
    gBossDriver.didPre = false;
    gBossDriver.watchT = 0.0f;
}

// Host::SpawnGround: grass, earth or sand of the GTA map (rarely anything else), not too steep, not under water
bool GtaAnimalSpawnGround(const CVector& from, CVector& out) {
    CColPoint cp;
    CEntity* e = nullptr;
    if (!CWorld::ProcessVerticalLine(from, from.z - 70.0f, cp, e, true, false, false, false, false, false, nullptr))
        return false;
    int block = VirtualBlockFor(cp, e);
    bool natural = block == ID_GRASS_BLOCK || block == ID_DIRT || block == ID_SAND;
    if (!natural && Rand01() > 0.12f)
        return false;
    if (cp.m_vecNormal.z < 0.75f)
        return false; // too steep
    float wl;
    if (CWaterLevel::GetWaterLevelNoWaves(cp.m_vecPoint.x, cp.m_vecPoint.y, cp.m_vecPoint.z, &wl) && wl > cp.m_vecPoint.z + 0.2f)
        return false;
    out = cp.m_vecPoint;
    float gz;
    if (GroundBelow(CVector(out.x, out.y, out.z + 3.0f), 6.0f, &gz))
        out.z = gz; // blocks placed on top of the ground
    return true;
}

void MobsUpdate(float dt, CPlayerPed* player) {
    if (dt <= 0.0f || !player)
        return; // (no player for a moment must not make every animal "too far away")
    const CVector playerPos = player->GetPosition();
    // natural spawns are the Minecraft world's; CleanSA has none (the core's
    // own spawn gates refuse them too - this just never asks)
    if (gGta.enabled && !gConfig.cleanSA)
        MobsSpawnTick(dt, playerPos);
    CarsHitMobs();
    std::vector<BeingInfo> beings; // the people around the player: the monsters may go for them too
    if (gGta.enabled)
        TheHost().NearbyBeings(playerPos, 20.0f, beings);
    // the hybrid step: while the driver mode is wanted (the default; the
    // /bosstest driver toggle is the A/B), his body reports BEFORE the
    // brain ticks - the brain reads the feet the game gave him and answers
    // with objectives, which BossProxySync applies after the tick
    gBossDriver.didPre = false;
    if (gGta.enabled && !gBossTest.proxyOff && !gBossTest.driverOff)
        BossDriverPreTick(dt, player);
    MobsTick(dt, playerPos, gGta.enabled ? gInv.Held().id : 0, !player->bInVehicle, &beings);
    if (gGta.enabled) {
        const auto proxyT0 = std::chrono::steady_clock::now();
        BossProxySync(dt, player);
        BossTestNoteProxyMs(
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - proxyT0).count());
        // a horn at him is a loud fact in his ears, like a shout in traffic
        if (player->bInVehicle) {
            CPad* pad = CPad::GetPad(0);
            if (pad && pad->HornJustDown())
                for (const Mob& m : gMobs)
                    if (m.kind == MOB_SOLDIER && m.death < 0.0f)
                        NotePlayerShot(m.pos, 18.0f);
        }
    }
}

void MobsRender(float light) {
    if (gMobs.empty() || !gEntityTex.tex)
        return;
    const CVector cam = TheCamera.GetPosition();
    for (auto& m : gMobs) {
        CVector centre = m.pos + CVector(0, 0, MobHeight(m) * 0.5f);
        CVector d = centre - cam;
        if (d.x * d.x + d.y * d.y + d.z * d.z > 120.0f * 120.0f)
            continue;
        if (!TheCamera.IsSphereVisible(centre, 1.6f))
            continue;
        CVector fwd(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
        CVector up(0, 0, 1);
        CVector right(fwd.y, -fwd.x, 0.0f);
        float r = 1.0f, g = 1.0f, b = 1.0f;
        if (m.death >= 0.0f && m.kind != MOB_SOLDIER) {
            Vec3 r2 = right, u2 = up;
            DeathTilt(m.death, r2, u2); // the dead animal tips over onto its side
            right = ToGta(r2);
            up = ToGta(u2);
        }
        // (the soldier falls with Counter-Strike's own death animation instead -
        // SoldierModel poses it; a Minecraft tip-over on top of it would double the fall)
        if (m.hurt > 0.0f || m.death >= 0.0f) {
            g = 0.45f;
            b = 0.45f;
        }
        Pose base = EntityPose(m.pos, right, up, fwd, MobScale(m));
        MobAnim a;
        a.limbSwing = m.limbSwing;
        a.limbAmount = m.limbAmount;
        a.headYaw = m.headYaw;
        a.headPitch = m.headPitch;
        a.wingFlap = (std::sin(m.flap) + 1.0f) * m.flapSpeed;
        a.sheared = m.sheared;
        a.saddled = m.saddled;
        a.swell = m.swell;
        a.attack = m.attackAnim;
        a.pulse = m.pulse;
        a.charge = m.boom;
        a.soldier = m.draw;
        if (m.death >= 0.0f) {
            // dying is a pose change too: Kill snapshotted the pose he died in
            // and set the fall; here the fall's clock and its fade simply run
            a.soldier.fullTime = m.death;
            a.soldier.fade = Clamp(m.death / kSoldierFadeSeconds, 0.0f, 1.0f);
        }
        DrawMob(m.kind, base, a, light, r, g, b);
        if (m.groundZ > -900.0f)
            AddShadow(CVector(m.pos.x, m.pos.y, m.groundZ), MobWidth(m) * 0.75f,
                      Clamp(1.0f - (m.pos.z - m.groundZ) / 6.0f, 0.0f, 1.0f));
    }
    // his grenades in flight: a small dark body riding its arc, drawn HERE -
    // the one render pass CleanSA keeps - because nothing else draws them:
    // the Minecraft projectile renderer belongs to the layer CleanSA turns
    // off, and a grenade nobody can see reads as no grenade at all (the eye
    // verdict). A plain dark cube from the mob sink: no Minecraft world
    // comes back with it, just the thrown object itself.
    if (!gBossNades.empty()) {
        TheModelSink().Texture(MT_ENTITY);
        for (const BossNade& n : gBossNades) {
            Pose p;
            p.o = n.pos;
            ModelStyle st;
            st.untextured = true;
            st.light = light;
            st.r = 0.13f;
            st.g = 0.15f;
            st.b = 0.09f; // dark olive: a grenade's own colour
            McCube(p, -1.5f, -1.5f, -1.5f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, st);
        }
    }
    d3::Flush();
}

} // namespace mc
