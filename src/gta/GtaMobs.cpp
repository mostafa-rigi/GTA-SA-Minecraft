#include "GtaMobs.h"

#include <chrono>

#include "CCamera.h"
#include "CColPoint.h"
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
    uint32_t mobId = 0;
    float scanT = 0.0f;
    float cowerT = 0.0f;
};
BossProxyState gBossProxy;

void BossProxyKill() {
    if (gBossProxy.ped) {
        PedSkinsIncludePed(gBossProxy.ped); // his pool ref may be reused: unmark before he goes
        CWorld::Remove(gBossProxy.ped);
        delete gBossProxy.ped;
        gBossProxy.ped = nullptr;
    }
    gBossProxy.mobId = 0;
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
    if (!gBossProxy.ped) {
        CPed* ped = new CPed(PED_TYPE_COP);
        if (!ped)
            return;
        ped->SetModelIndex(0);
        CWorld::Add(ped);
        ped->bIsVisible = false;
        PedSkinsExcludePed(ped);  // the villager system must never adopt or draw him
        SetPedDrawn(ped, false);  // ...and his clump's atomics lose their render flags at once
        ped->m_fMaxHealth = 1000.0f;
        ped->m_fHealth = 1000.0f;
        gBossProxy.ped = ped;
        gBossProxy.mobId = m.id;
        Log("boss proxy up: one invisible cop-type body carries the street's fire for him");
    }
    CPed* ped = gBossProxy.ped;
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
    if (gGta.enabled)
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
