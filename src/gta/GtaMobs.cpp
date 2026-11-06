#include "GtaMobs.h"

#include <chrono>

#include "CCamera.h"
#include "CColPoint.h"
#include "CCopPed.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CPools.h"
#include "CStreaming.h"
#include "CTaskComplexKillPedOnFoot.h"
#include "CTaskSimpleDuck.h"
#include "CVehicle.h"
#include "CWaterLevel.h"
#include "CWorld.h"
#include "ePedType.h"

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
                        MobVehicleThreat((int)j, vp, vel);
                    else if (perp < 3.5f && ahead < 6.0f && nearMissT <= 0.0f) {
                        NotePlayerShot(m.pos, 22.0f);
                        nearMissT = 2.0f;
                    }
                }
            }
            CVector d = (m.pos + CVector(0, 0, MobHeight(m) * 0.5f)) - vp;
            if (std::fabs(d.z) > 1.8f || d.x * d.x + d.y * d.y > 2.3f * 2.3f)
                continue;
            m.hitByCar = 0.6f;
            if (m.kind == MOB_SOLDIER) {
                // his own accounting: capped, never a one-shot, the driver known
                MobVehicleHit((int)j, speed, vp, v->m_pDriver == (CPed*)FindPlayerPed());
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
    int bi = -1;
    for (size_t j = 0; j < gMobs.size(); ++j)
        if (gMobs[j].kind == MOB_SOLDIER && gMobs[j].death < 0.0f) {
            bi = (int)j;
            break;
        }
    if (bi < 0) {
        BossProxyKill();
        return;
    }
    Mob& m = gMobs[bi];
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
            return;
    }
    if (!gBossProxy.ped)
        return;
    CPed* ped = gBossProxy.ped;
    if (!BossProxyAlive(ped)) {
        // He died in the game: the health top-up only runs while we sync,
        // and a single explosion-class hit can take the whole 1000 between
        // frames. A corpse is no perception body - the game is already
        // running its own death on him, gangs stop aiming at the dead, and
        // the relay below would read a stuck health value. Retire ours and
        // let the creation above build a fresh body next frame.
        Log("boss proxy: he died in the game - retiring the body, a fresh one is built next sync");
        BossProxyKill();
        return;
    }
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
    // the hurt he took since the last frame, relayed at sensor scale
    const float h = ped->m_fHealth;
    if (h < 999.0f) {
        int who = attacker;
        if (who < 0) {
            // no gang hand on it: with the player near, the shot was most
            // likely his (his projectiles meet the proxy; no task names them)
            CVector dp = player->GetPosition() - ped->GetPosition();
            if (dp.x * dp.x + dp.y * dp.y < 40.0f * 40.0f)
                who = -2;
        }
        MobProxyDamage(bi, (1000.0f - h) * 2.0f, from, who);
    }
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
    d3::Flush();
}

} // namespace mc
