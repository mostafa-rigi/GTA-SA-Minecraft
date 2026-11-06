#include "Mobs.h"

#include <cstdlib>

#include "Audio.h"
#include "BlockRules.h"
#include "Combat.h"
#include "Doors.h"
#include "Entities.h"
#include "GameState.h"
#include "Host.h"
#include "Interact.h"
#include "Inventory.h"
#include "Items.h"
#include "Particles.h"
#include "SoldierModel.h"

namespace mc {

namespace {
struct MobType {
    float width, height, health, walk, run;
    SoundEvent say, hurt, death;
    uint16_t food[3]; // what it follows and eats
    bool monster;     // hostile: no ambient sound, never runs away, 5 experience
};
const MobType kTypes[MOB_KIND_COUNT] = {
    { 0.9f, 1.4f, 10.0f, 1.25f, 3.4f, SND_COW_SAY, SND_COW_HURT, SND_COW_DEATH, { ID_WHEAT, 0, 0 } },
    { 0.9f, 0.9f, 10.0f, 1.25f, 3.4f, SND_PIG_SAY, SND_PIG_HURT, SND_PIG_DEATH, { ID_CARROT, ID_POTATO, ID_BEETROOT } },
    { 0.9f, 1.3f, 8.0f, 1.15f, 3.2f, SND_SHEEP_SAY, SND_SHEEP_HURT, SND_SHEEP_DEATH, { ID_WHEAT, 0, 0 } },
    { 0.4f, 0.7f, 4.0f, 1.0f, 2.8f, SND_CHICKEN_SAY, SND_CHICKEN_HURT, SND_CHICKEN_DEATH,
      { ID_WHEAT_SEEDS, ID_MELON_SEEDS, ID_PUMPKIN_SEEDS } },
    { 0.6f, 1.7f, 20.0f, 1.2f, 2.6f, SND_CREEPER_HURT, SND_CREEPER_HURT, SND_CREEPER_DEATH, { 0, 0, 0 }, true },
    { 0.9f, 2.9f, 500.0f, 1.6f, 5.0f, SND_WARDEN_AMBIENT, SND_WARDEN_HURT, SND_WARDEN_DEATH, { 0, 0, 0 }, true },
    // zombie, skeleton, spider: each with its own real voice from the sound table. Their speeds started as
    // Minecraft's own dry numbers (run 2.3 / 2.5 / 3.4, skeleton walk 1.25), but in this host those read slow:
    // San Andreas streets are wider and its people faster than Minecraft's, so next to the player the three
    // felt sluggish on the user's own screen. By his standing rule for this mod - exactly like the game, but
    // better where the dry number feels weak here - the run speeds are a quarter up (2.9 / 3.1 / 4.3) and the
    // skeleton's sideways drift while it aims is brisker (walk 1.7). Still under the player's own sprint.
    { 0.6f, 1.95f, 40.0f, 1.1f, 2.9f, SND_ZOMBIE_SAY, SND_ZOMBIE_HURT, SND_ZOMBIE_DEATH, { 0, 0, 0 }, true },
    { 0.6f, 1.9f, 40.0f, 1.7f, 3.1f, SND_SKELETON_SAY, SND_SKELETON_HURT, SND_SKELETON_DEATH, { 0, 0, 0 }, true },
    { 1.4f, 0.9f, 32.0f, 1.6f, 4.3f, SND_SPIDER_SAY, SND_SPIDER_HURT, SND_SPIDER_DEATH, { 0, 0, 0 }, true },
    // the Counter-Strike soldier: a man, not a monster of Minecraft's - 1.8 m tall, and his 100 health
    // points of Counter-Strike are counted the way this host counts everything, in half hearts (so 200),
    // with another 200 of kevlar on top (the `armor` field, set at spawn). His `say` is never played on
    // the ambient timer (monsters have none); his brain plays his footsteps itself, on his stride. His
    // walk is his patrol pace, his run his fighting pace; the whole of his difficulty lives in kSoldier.
    { 0.6f, 1.8f, 200.0f, 2.4f, 5.2f, SND_SOLDIER_STEP, SND_SOLDIER_HURT, SND_SOLDIER_DEATH, { 0, 0, 0 }, true },
};

uint32_t gNextMobId = 1;
float gSpawnTimer = 3.0f;
std::vector<Vec3> gReinforceCalls; // where hurt zombies have called for help (answered at the top of MobsTick)
} // namespace

std::vector<Mob> gMobs;
std::vector<ShotFx> gShotFx;

bool IsMonster(int kind) { return kind >= 0 && kind < MOB_KIND_COUNT && kTypes[kind].monster; }

float MobScale(const Mob& m) { return m.baby > 0.0f ? 0.5f : 1.0f; }
float MobWidth(const Mob& m) { return kTypes[m.kind].width * MobScale(m); }
float MobHeight(const Mob& m) { return kTypes[m.kind].height * MobScale(m); }

namespace {

Vec3 Flat(Vec3 v) {
    v.z = 0.0f;
    float m = v.Length();
    return m > 1e-4f ? v * (1.0f / m) : Vec3(0, 1, 0);
}

Vec3 RandomDir() {
    float a = Rand01() * 6.2831853f;
    return Vec3(std::cos(a), std::sin(a), 0.0f);
}

float WrapAngle(float a) {
    while (a > kPi)
        a -= 2.0f * kPi;
    while (a < -kPi)
        a += 2.0f * kPi;
    return a;
}

bool IsFood(const Mob& m, uint16_t id) {
    if (!id)
        return false;
    for (uint16_t f : kTypes[m.kind].food)
        if (f == id)
            return true;
    return false;
}

void Puff(const Vec3& at, uint16_t tile, int count, float spread, uint32_t color = 0xFFFFFFFF, bool smoke = false) {
    for (int i = 0; i < count; ++i) {
        Particle p;
        p.pos = at + Vec3((Rand01() - 0.5f) * spread, (Rand01() - 0.5f) * spread, (Rand01() - 0.5f) * spread);
        p.vel = Vec3((Rand01() - 0.5f) * 1.2f, (Rand01() - 0.5f) * 1.2f, Rand01() * 1.2f);
        p.maxLife = p.life = 0.6f + Rand01() * 0.6f;
        p.tile = tile;
        p.size = smoke ? 0.14f : 0.1f;
        p.gravity = smoke ? -0.6f : 0.0f;
        p.color = color;
        p.anim = smoke ? 2 : 0;
        SpawnParticle(p);
    }
}

void DropLoot(const Mob& m) {
    if (m.baby > 0.0f)
        return;
    Vec3 at = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
    SpawnXp(at, kTypes[m.kind].monster ? 5 : 1 + rand() % 3);
    auto drop = [&](uint16_t id, int lo, int hi) {
        int n = lo + (hi > lo ? rand() % (hi - lo + 1) : 0);
        if (m.burn > 0.0f) {
            // died on fire: the meat comes out cooked
            id = id == ID_BEEF ? ID_COOKED_BEEF : id == ID_PORKCHOP ? ID_COOKED_PORKCHOP
               : id == ID_MUTTON ? ID_COOKED_MUTTON : id == ID_CHICKEN ? ID_COOKED_CHICKEN : id;
        }
        if (n > 0)
            SpawnDropItem(at, id, n);
    };
    switch (m.kind) {
    case MOB_COW:
        drop(ID_LEATHER, 0, 2);
        drop(ID_BEEF, 1, 3);
        break;
    case MOB_PIG:
        drop(ID_PORKCHOP, 1, 3);
        break;
    case MOB_SHEEP:
        drop(ID_MUTTON, 1, 2);
        if (!m.sheared)
            drop(ID_WHITE_WOOL, 1, 1);
        break;
    case MOB_CHICKEN:
        drop(ID_FEATHER, 0, 2);
        drop(ID_CHICKEN, 1, 1);
        break;
    case MOB_CREEPER:
        drop(ID_GUNPOWDER, 0, 2);
        break;
    case MOB_WARDEN:
        drop(ID_SCULK_CATALYST, 1, 1);
        break;
    case MOB_ZOMBIE:
        drop(ID_ROTTEN_FLESH, 0, 2);
        break;
    case MOB_SKELETON:
        drop(ID_BONE, 0, 2);
        drop(ID_ARROW, 0, 2);
        break;
    case MOB_SPIDER:
        drop(ID_STRING, 0, 2);
        if (Rand01() < 0.33f)
            drop(ID_SPIDER_EYE, 1, 1);
        break;
    }
}

bool VoxelSolidAt(const Vec3& p) { return gWorld.IsSolid(FloorI(p.x), FloorI(p.y), FloorI(p.z)); }

void Kill(Mob& m);

void MoveMob(Mob& m, float dt) {
    const float h = MobHeight(m), r = MobWidth(m) * 0.5f;
    Vec3 np = m.pos + m.vel * dt;
    Vec3 hv(m.vel.x, m.vel.y, 0.0f);
    const float hs = hv.Length();
    bool blocked = false;
    if (hs > 0.05f) {
        const Vec3 dir = hv * (1.0f / hs);
        const Vec3 probe = Vec3(np.x, np.y, m.pos.z) + dir * r;
        const bool low = VoxelSolidAt(probe + Vec3(0, 0, 0.3f));
        const bool high = VoxelSolidAt(probe + Vec3(0, 0, 1.3f)) || VoxelSolidAt(m.pos + Vec3(0, 0, h + 0.9f));
        if (low) {
            blocked = true;
            if (!high && m.onGround && m.moving)
                m.vel.z = 8.4f; // hop onto the block
        } else if (h > 1.0f && VoxelSolidAt(probe + Vec3(0, 0, h - 0.1f))) {
            blocked = true;
        }
        if (!blocked) {
            // walls, cars and props of the host's map
            Vec3 a = m.pos + Vec3(0, 0, std::max(0.35f, h * 0.5f));
            Vec3 b = a + dir * (r + 0.2f + hs * dt);
            if (TheHost().LineBlocked(a, b))
                blocked = true;
        }
    }
    if (blocked) {
        np.x = m.pos.x;
        np.y = m.pos.y;
        if (m.kind == MOB_SPIDER && m.moving && m.death < 0.0f) {
            // a spider does not stop at a wall: it climbs it, a few metres a second, until it is over the top
            m.vel.z = std::max(m.vel.z, 4.2f);
            m.onGround = false;
        } else {
            m.vel.x *= 0.3f;
            m.vel.y *= 0.3f;
        }
        if (m.aiTimer > 0.4f)
            m.aiTimer = 0.4f; // think again soon
    }

    const bool movedFlat = np.x != m.pos.x || np.y != m.pos.y;
    if (movedFlat || !m.onGround || --m.groundTick <= 0) {
        m.groundTick = 12;
        float gz = -1000.0f;
        const bool sampled = GroundBelow(Vec3(np.x, np.y, np.z + 0.7f), 120.0f, &gz);
        // The soldier walks a real city on the host's collision, and that collision
        // has gaps: a vertical probe can slip past the street face he is visibly
        // standing on and land on a floor far below (an underpass, an interior), or
        // find nothing at all - and the old code handed him that depth as his
        // ground, so he sank through the street and was gone. A man standing on
        // known ground does not believe ONE sample that drops the world more than
        // a step from under him: he holds his last good ground and re-probes on
        // the next tick. Only when the deep (or missing) sample repeats three
        // times running does he accept that the edge is real - and then he steps
        // off and FALLS to it under gravity, like a man, never teleporting down.
        const bool soldierHold = m.kind == MOB_SOLDIER && m.onGround && m.groundZ > -900.0f &&
                                 (!sampled || gz < m.pos.z - 0.6f);
        if (soldierHold) {
            if (m.groundBad == 0)
                Log("soldier ground: a sample %s under his feet at (%.1f, %.1f) - holding his ground at %.2f",
                    sampled ? "dropped far" : "found nothing", np.x, np.y, m.groundZ);
            ++m.groundBad;
            m.groundTick = 1; // ask again next tick, not in twelve
            if (m.groundBad >= 3) {
                // the edge is real: believe it, and let the fall below be a fall
                m.groundBad = 0;
                m.groundZ = sampled ? gz : -1000.0f;
                m.onGround = false;
                m.fallFrom = np.z;
                Log("soldier ground: the drop under his feet at (%.1f, %.1f) is real - stepping off (ground %.2f)",
                    np.x, np.y, m.groundZ);
            }
        } else if (sampled) {
            m.groundBad = 0;
            if (m.onGround && gz > m.pos.z + 0.7f) {
                // a step that is too high: do not walk through it
                np.x = m.pos.x;
                np.y = m.pos.y;
                if (m.aiTimer > 0.4f)
                    m.aiTimer = 0.4f;
            } else {
                m.groundZ = gz;
            }
        } else {
            m.groundBad = 0;
            m.groundZ = -1000.0f;
        }
    }

    if (np.z <= m.groundZ + 0.001f && m.vel.z <= 0.0f) {
        if (!m.onGround && m.fallFrom > -900.0f && m.kind != MOB_CHICKEN && m.death < 0.0f) {
            float fall = m.fallFrom - m.groundZ;
            if (fall > 3.5f) {
                m.health -= fall - 3.0f;
                m.hurt = 0.5f;
            }
        }
        np.z = m.groundZ;
        m.vel.z = 0.0f;
        m.onGround = true;
        m.fallFrom = -1000.0f;
    } else {
        if (m.onGround || m.fallFrom < -900.0f || np.z > m.fallFrom)
            m.fallFrom = np.z;
        m.onGround = false;
    }

    // swim: keep the head above the water
    float wl;
    if (TheHost().WaterLevel(np, &wl) && wl > np.z + h * 0.55f) {
        float target = wl - h * 0.55f;
        m.vel.z = Clamp((target - np.z) * 6.0f, -3.0f, 3.0f);
        m.onGround = true; // can keep walking / turning
        m.fallFrom = -1000.0f;
        np.z += m.vel.z * dt;
    }
    // placed water, lava and fire
    const int fluid = FluidAt(np + Vec3(0, 0, h * 0.4f));
    const bool inFire = FireAt(np + Vec3(0, 0, 0.2f));
    float hostLevel;
    const bool hostWater = TheHost().WaterLevel(np, &hostLevel) && hostLevel > np.z + 0.2f;
    if (fluid == ID_WATER || hostWater) {
        m.burn = 0.0f; // water puts it out
        if (fluid == ID_WATER) {
            m.vel.z = std::min(m.vel.z + 40.0f * dt, 2.0f);
            m.vel.x *= std::pow(0.8f, dt * 20.0f);
            m.vel.y *= std::pow(0.8f, dt * 20.0f);
            const Vec3 flow = FluidFlowAt({ FloorI(np.x), FloorI(np.y), FloorI(np.z + h * 0.4f) });
            m.vel.x += flow.x * 5.6f * dt;
            m.vel.y += flow.y * 5.6f * dt;
            m.onGround = true;
            m.fallFrom = -1000.0f;
        }
    } else if ((fluid == ID_LAVA || inFire) && m.death < 0.0f) {
        m.burn = std::max(m.burn, fluid == ID_LAVA ? 15.0f : 8.0f);
        m.lavaTimer -= dt;
        if (m.lavaTimer <= 0.0f) {
            m.lavaTimer = 0.5f;
            m.health -= fluid == ID_LAVA ? 4.0f : 1.0f;
            m.hurt = 0.5f;
            m.panic = 4.0f;
            if (m.health <= 0.0f)
                Kill(m);
        }
    }
    if (m.burn > 0.0f && m.death < 0.0f) {
        // on fire: one point of damage a second, and flames all over
        const float before = m.burn;
        m.burn -= dt;
        if (std::floor(before) != std::floor(std::max(0.0f, m.burn))) {
            m.health -= 1.0f;
            m.hurt = 0.5f;
            m.panic = std::max(m.panic, 2.0f);
            if (m.health <= 0.0f)
                Kill(m);
        }
        if (Rand01() < 12.0f * dt) {
            Particle f;
            f.pos = np + Vec3((Rand01() - 0.5f) * MobWidth(m), (Rand01() - 0.5f) * MobWidth(m), Rand01() * h);
            f.vel = Vec3(0, 0, 0.6f + Rand01() * 0.6f);
            f.maxLife = f.life = 0.4f + Rand01() * 0.3f;
            f.tile = TILE_P_FLAME;
            f.size = 0.08f + Rand01() * 0.06f;
            f.gravity = 0.0f;
            f.glow = true;
            SpawnParticle(f);
        }
    }
    m.pos = np;
}

// Creeper (SwellGoal): with a player in survival within 16 m it goes for him; from 3 m it stands and swells for 1.5 s,
// then blows up; it only gives up when he gets more than 7 m away. The host's own people are prey too: it goes for
// whoever of them is nearest (its blast is the host's own explosion, so a crowd around it is hurt either way).
// True while it is after someone.
bool CreeperFuse(Mob& m, float dt, const Vec3& playerPos, const Vec3& toPlayer, float* speed, bool* looking,
                 const std::vector<BeingInfo>* beings) {
    float dist = (playerPos - Vec3(0, 0, 1.0f) - m.pos).Length(); // (the host's player position is his middle)
    Vec3 to = toPlayer;
    bool target = gGame.gameMode == MODE_SURVIVAL && gGame.deathTime < 0.0f && dist < 16.0f;
    if (beings)
        for (const BeingInfo& b : *beings) {
            const Vec3 bFeet = b.pos - Vec3(0, 0, 1.0f);
            const float bd = (bFeet - m.pos).Length();
            if (bd < 16.0f && (!target || bd < dist)) {
                target = true;
                dist = bd;
                to = bFeet - m.pos;
                to.z = 0.0f;
            }
        }
    if (!target || dist > 7.0f)
        m.swellDir = -1;
    else if (dist < 3.0f)
        m.swellDir = 1;
    const float before = m.swell;
    m.swell = Clamp(m.swell + m.swellDir * dt / 1.5f, 0.0f, 1.0f);
    if (before == 0.0f && m.swell > 0.0f) {
        const Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
        PlaySfx(SND_CREEPER_PRIMED, &c, 1.0f, 0.5f);
    }
    if (!target)
        return false;
    m.goal = Flat(to);
    m.moving = m.swellDir < 0; // it stands still while it swells
    m.aiTimer = 0.5f;          // (when it loses him it soon wanders off)
    *speed = kTypes[m.kind].run;
    *looking = true;
    return true;
}

// Warden: blind. It hears the player walk within 16 m (not when he sneaks): every 2 s a vibration, +35 anger; it
// smells him within 6 m; a hit makes it furious (100). From 80 it roars, then goes for him: a blow takes 15 hearts,
// and from more than 3 m (up to 15, through walls) it charges a sonic boom for 1.7 s that takes 5 hearts and throws
// him back. Anger fades by 1 a second; after a minute without any it digs back into the ground. True while after him.
bool WardenBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                 const std::vector<BeingInfo>* beings) {
    Host& host = TheHost();
    const Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
    const Vec3 feet = playerPos - Vec3(0, 0, 1.0f); // (the host's player position is his middle)
    Vec3 to = feet - m.pos;
    const float dist = to.Length();
    to.z = 0.0f;
    const float flat = to.Length();
    const bool prey = gGame.gameMode == MODE_SURVIVAL && gGame.deathTime < 0.0f;
    // the host's own people are prey too: the nearest of them (it can only smell them; it cannot hear their steps)
    const BeingInfo* nearBeing = nullptr;
    float nearDist = 1e9f;
    if (beings)
        for (const BeingInfo& b : *beings) {
            const float bd = (b.pos - Vec3(0, 0, 1.0f) - m.pos).Length();
            if (bd < nearDist) {
                nearDist = bd;
                nearBeing = &b;
            }
        }

    m.listen = std::max(0.0f, m.listen - dt);
    if (prey && dist < 16.0f && m.listen <= 0.0f) {
        const Vec3 v = host.PlayerVelocity();
        const bool steps = !gGame.sneaking && (Vec3(v.x, v.y, 0.0f).Length() > 0.8f || std::fabs(v.z) > 2.0f);
        if (steps || dist < 6.0f) {
            m.anger += steps ? 35.0f : 20.0f;
            m.listen = 2.0f;
            PlaySfx(steps ? (m.anger >= 80.0f ? SND_WARDEN_LISTENING_ANGRY : SND_WARDEN_LISTENING) : SND_WARDEN_SNIFF, &c);
        }
    }
    if (nearBeing && nearDist < 6.0f && m.listen <= 0.0f) {
        m.anger += 20.0f; // it smells one of the host's people standing close
        m.listen = 2.0f;
        PlaySfx(SND_WARDEN_SNIFF, &c);
    }
    m.anger = Clamp(m.anger - dt, 0.0f, 150.0f);
    m.calm = m.anger > 0.0f ? 0.0f : m.calm + dt;
    m.heart -= dt;
    if (m.heart <= 0.0f) {
        m.heart = 2.0f - Clamp(m.anger / 80.0f, 0.0f, 1.0f) * 1.2f; // it beats faster the angrier it is
        m.pulse = 1.0f;
        if (dist < 16.0f)
            PlaySfx(SND_WARDEN_HEARTBEAT, &c, 0.8f);
    }
    m.pulse = std::max(0.0f, m.pulse - dt * 4.0f);
    m.attackCooldown -= dt;
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 2.5f);
    m.boomCooldown -= dt;

    if ((!prey && !nearBeing) || m.anger < 80.0f) {
        m.roared = false;
        m.boom = -1.0f;
        return false;
    }
    // it goes for the nearer prey: one of the host's people, or the player
    const bool huntBeing = nearBeing && (!prey || nearDist < dist);
    const Vec3 targetFeet = huntBeing ? nearBeing->pos - Vec3(0, 0, 1.0f) : feet;
    Vec3 toT = targetFeet - m.pos;
    const float distT = toT.Length();
    toT.z = 0.0f;
    const float flatT = toT.Length();
    const Vec3 dirT = flatT > 0.01f ? toT * (1.0f / flatT) : Vec3(0, 1, 0);
    m.goal = Flat(toT);
    m.aiTimer = 0.5f;
    *looking = true;
    *speed = kTypes[m.kind].run;
    if (!m.roared) {
        m.roared = true;
        m.roar = 4.2f;
        PlaySfx(SND_WARDEN_ROAR, &c, 1.5f);
    }
    if (m.roar > 0.0f) {
        m.roar -= dt;
        m.moving = false;
        return true;
    }
    if (m.boom < 0.0f && flatT > 3.0f && distT < 15.0f && m.boomCooldown <= 0.0f) {
        m.boom = 0.0f;
        PlaySfx(SND_WARDEN_SONIC_CHARGE, &c, 3.0f);
    }
    if (m.boom >= 0.0f) {
        m.moving = false;
        m.boom += dt;
        if (m.boom < 1.7f)
            return true;
        // the sonic boom: straight through anything in the way
        m.boom = -1.0f;
        m.boomCooldown = 3.3f;
        const Vec3 mouth = m.pos + Vec3(0, 0, 1.6f);
        const Vec3 line = (huntBeing ? nearBeing->pos : playerPos) - mouth;
        for (int i = 1; i <= 8; ++i)
            Puff(mouth + line * (i / 8.0f), TILE_P_GENERIC_0, 2, 0.3f, 0xFF60E8E0);
        PlaySfx(SND_WARDEN_SONIC_BOOM, &c, 3.0f);
        if (distT < 16.0f) {
            if (huntBeing) {
                host.HurtBeing(nearBeing->being, 10.0f, HURT_SHOT, nullptr);
                host.PushBeing(nearBeing->being, dirT * 12.0f + Vec3(0, 0, 3.0f));
            } else {
                if (host.PlayerVehicle() >= 0)
                    host.HurtVehicle(host.PlayerVehicle(), 10.0f);
                else
                    host.HurtPlayer(10.0f);
                const Vec3 dir = flat > 0.01f ? to * (1.0f / flat) : Vec3(0, 1, 0);
                Gust(feet - dir * 0.8f + Vec3(0, 0, 0.5f), 1.5f, 14.0f, 4.0f, true);
            }
        }
        return true;
    }
    m.moving = flatT > 1.6f;
    if (flatT < 2.2f && std::fabs(targetFeet.z - m.pos.z) < 2.5f && m.attackCooldown <= 0.0f) {
        m.attackCooldown = 0.9f;
        m.attackAnim = 1.0f;
        if (huntBeing) {
            host.HurtBeing(nearBeing->being, 30.0f, HURT_FIST, nullptr);
            host.PushBeing(nearBeing->being, dirT * 5.0f + Vec3(0, 0, 2.0f));
        } else if (host.PlayerVehicle() >= 0)
            host.HurtVehicle(host.PlayerVehicle(), 30.0f);
        else
            host.HurtPlayer(30.0f);
        PlaySfx(SND_WARDEN_ATTACK, &c);
    }
    return true;
}

// Who a hunting monster goes for: the player (in survival, alive), or the nearest of the host's own people —
// whichever is nearer, exactly as the creeper's fuse and the warden's brain pick between them. Sets m.huntBeing
// and hands back the prey's feet and middle (host positions are a man's middle: feet are 1 m lower).
// A monster does not forget its prey the moment it steps out of reach, the way Minecraft's do not: once it is
// after someone (m.huntValid) it keeps after him while he stays within half as much again of the range that
// found him, and only forgets him after kHuntMemory seconds with the prey out of that reach (or gone, or dead).
// The scan runs every tick, and a prey within 3 m is always seen at once — nobody overlooks what is under his nose.
bool PickPrey(Mob& m, float dt, const Vec3& playerPos, const std::vector<BeingInfo>* beings, float range, Vec3* feet,
              Vec3* mid, float* dist) {
    const float kHuntMemory = 10.0f; // seconds a prey stays remembered once out of reach
    // one pass of the hunt scan: the nearest prey closer than `cap`, or nobody
    auto scan = [&](float cap, int* being) {
        bool have = false;
        float best = cap;
        if (gGame.gameMode == MODE_SURVIVAL && gGame.deathTime < 0.0f) {
            const Vec3 f = playerPos - Vec3(0, 0, 1.0f);
            const float d = (f - m.pos).Length();
            if (d < best) {
                best = d;
                *feet = f;
                *mid = playerPos;
                *being = -1;
                have = true;
            }
        }
        if (beings)
            for (const BeingInfo& b : *beings) {
                const Vec3 f = b.pos - Vec3(0, 0, 1.0f);
                const float d = (f - m.pos).Length();
                if (d < best) {
                    best = d;
                    *feet = f;
                    *mid = b.pos;
                    *being = b.being;
                    have = true;
                }
            }
        if (have)
            *dist = best;
        return have;
    };
    int being = -1;
    // fresh sight: point blank first, then the full range
    if (scan(3.0f, &being) || scan(range, &being)) {
        m.huntBeing = being;
        m.huntValid = true;
        m.huntMemory = kHuntMemory;
        return true;
    }
    if (!m.huntValid)
        return false;
    // nobody in sight: is the remembered prey still within reach (half as much again of the range)?
    if (m.huntBeing < 0) {
        if (gGame.gameMode != MODE_SURVIVAL || gGame.deathTime >= 0.0f) {
            m.huntValid = false; // the player is dead (or no longer prey): drop him at once
            m.huntMemory = 0.0f;
            return false;
        }
        const Vec3 f = playerPos - Vec3(0, 0, 1.0f);
        const float d = (f - m.pos).Length();
        if (d <= range * 1.5f) {
            *feet = f;
            *mid = playerPos;
            *dist = d;
            m.huntMemory = kHuntMemory;
            return true;
        }
    } else if (beings) {
        for (const BeingInfo& b : *beings) {
            if (b.being != m.huntBeing)
                continue;
            const Vec3 f = b.pos - Vec3(0, 0, 1.0f);
            const float d = (f - m.pos).Length();
            if (d <= range * 1.5f) {
                *feet = f;
                *mid = b.pos;
                *dist = d;
                m.huntMemory = kHuntMemory;
                return true;
            }
            break;
        }
    }
    // out of reach (or gone from the host's list): the memory fades, then he is forgotten
    m.huntMemory -= dt;
    if (m.huntMemory <= 0.0f) {
        m.huntValid = false;
        m.huntMemory = 0.0f;
    }
    return false;
}

// A blow at the prey PickPrey picked: one of the host's people takes it through the bridge, the player directly.
void StrikePrey(Mob& m, const Vec3& dirFlat, float halfHearts, float push) {
    Host& host = TheHost();
    if (m.huntBeing >= 0) {
        host.HurtBeing(m.huntBeing, halfHearts, HURT_FIST, nullptr);
        host.PushBeing(m.huntBeing, dirFlat * push + Vec3(0, 0, 2.0f));
    } else if (host.PlayerVehicle() >= 0)
        host.HurtVehicle(host.PlayerVehicle(), halfHearts);
    else
        host.HurtPlayer(halfHearts);
}

// Zombie: it shambles straight at whoever is nearest and beats on him with its arms (3 half hearts a blow). It has
// no fuse and no fear; like Minecraft's it burns up in daylight (that part lives in MobsTick, where the sun is).
bool ZombieBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                 const std::vector<BeingInfo>* beings) {
    m.attackCooldown -= dt;
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 3.0f);
    Vec3 feet, mid;
    float dist = 0.0f;
    if (!PickPrey(m, dt, playerPos, beings, 35.0f, &feet, &mid, &dist)) {
        m.doorCell = Int3{ 0, 0, -1 }; // nobody to chase: any door count starts over
        m.doorHitTime = 0.0f;
        return false;
    }
    Vec3 to = feet - m.pos;
    to.z = 0.0f;
    const float flat = to.Length();
    const Vec3 dir = flat > 0.01f ? to * (1.0f / flat) : Vec3(0, 1, 0);
    m.goal = Flat(to);
    m.aiTimer = 0.5f;
    *looking = true;
    *speed = kTypes[m.kind].run;
    m.moving = flat > 1.5f;
    // a closed wooden door between it and its prey: it stops and beats on the wood. About five
    // seconds of steady hitting breaks the door through the normal break path, so both halves
    // go and the door drops as an item. Only zombies do this, and only to wooden doors. The
    // door's four meta bits are all spoken for (Doors.h), so there are no crack stages - the
    // warning is the noise and the flying chips.
    {
        const float reach = MobWidth(m) * 0.5f + 0.45f; // just past its own front, where its hands land
        Int3 doorAt{ 0, 0, -1 };
        for (int i = 0; i < 2 && doorAt.z < 0; ++i) {
            const Vec3 probe = m.pos + dir * reach + Vec3(0, 0, i == 0 ? 0.3f : 1.3f);
            const Int3 c{ FloorI(probe.x), FloorI(probe.y), FloorI(probe.z) };
            const int b = gWorld.GetBlock(c.x, c.y, c.z);
            if (IsDoorBlock(b) && !DoorIsOpen(VoxMeta(gWorld.Get(c.x, c.y, c.z))))
                doorAt = IsDoorUpper(b) ? Int3{ c.x, c.y, c.z - 1 } : c;
        }
        if (doorAt.z >= 0) {
            if (m.doorCell.x != doorAt.x || m.doorCell.y != doorAt.y || m.doorCell.z != doorAt.z) {
                m.doorCell = doorAt; // a different door: the count starts over
                m.doorHitTime = 0.0f;
            }
            m.doorHitTime += dt;
            m.moving = false;
            if (m.attackCooldown <= 0.0f) {
                m.attackCooldown = 0.85f;
                m.attackAnim = 1.0f;
                const Vec3 c((float)doorAt.x + 0.5f, (float)doorAt.y + 0.5f, (float)doorAt.z + 0.9f);
                PlaySfx(SND_HIT_WOOD, &c);
                Puff(c, BlockFaceTile(ID_OAK_DOOR, FACE_SOUTH, VoxMeta(gWorld.Get(doorAt.x, doorAt.y, doorAt.z))), 3, 0.7f);
            }
            if (m.doorHitTime >= 5.0f) {
                BreakVoxel(doorAt, true); // both halves go, and the door drops as an item
                m.doorCell = Int3{ 0, 0, -1 };
                m.doorHitTime = 0.0f;
            }
            return true;
        }
        m.doorCell = Int3{ 0, 0, -1 }; // no closed door in front of it now: the count starts over
        m.doorHitTime = 0.0f;
    }
    if (flat < 1.9f && std::fabs(feet.z - m.pos.z) < 2.2f && m.attackCooldown <= 0.0f) {
        m.attackCooldown = 1.0f; // about a blow a second, the way Minecraft's zombies hit
        m.attackAnim = 1.0f;
        StrikePrey(m, dir, 3.0f, 3.0f);
        const Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
        PlaySfx(SND_ATTACK_WEAK, &c, 0.8f);
    }
    return true;
}

// Skeleton: it keeps its distance and shoots. Between about 5 and 14 m it stands off (strafing sideways the way
// Minecraft's do), inside 5 it backs away, beyond 14 it closes in; every second and a half to two and a half it
// looses a real arrow from the core's own projectile system, with a clear line of sight.
bool SkeletonBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                   const std::vector<BeingInfo>* beings) {
    m.shoot -= dt;
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 1.5f);
    Vec3 feet, mid;
    float dist = 0.0f;
    if (!PickPrey(m, dt, playerPos, beings, 20.0f, &feet, &mid, &dist))
        return false;
    Vec3 to = feet - m.pos;
    to.z = 0.0f;
    const float flat = to.Length();
    const Vec3 dir = flat > 0.01f ? to * (1.0f / flat) : Vec3(0, 1, 0);
    *looking = true;
    *speed = kTypes[m.kind].run;
    m.aiTimer = 0.5f;
    if (flat > 14.0f) {
        m.goal = dir;
        m.moving = true;
    } else if (flat < 5.0f) {
        m.goal = dir * -1.0f;
        m.moving = true;
    } else {
        // in its band: drift sideways, changing side every few seconds
        const Vec3 side(dir.y, -dir.x, 0.0f);
        m.goal = (m.id % 2 == 0) ? side : side * -1.0f;
        m.moving = true;
        *speed = kTypes[m.kind].walk;
    }
    if (flat > 3.0f && flat < 16.0f)
        m.attackAnim = std::max(m.attackAnim, 0.7f); // the bow stays up while prey is in range, the way Minecraft's aim
    if (m.shoot <= 0.0f && flat > 3.0f && flat < 16.0f) {
        const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);
        if (!TheHost().LineBlocked(eye, mid)) {
            // the arrow leaves from the bow, half a metre in front of it on the very line it faces,
            // not out of the middle of its head
            ShootArrowAt(eye + dir * 0.5f, mid, -1, -1); // its own arrows: no host shooter behind them
            m.attackAnim = 1.0f;
            m.shoot = 1.5f + Rand01() * 1.0f;
        } else {
            m.shoot = 0.4f; // no clear shot: look again in a moment
        }
    }
    return true;
}

// Spider: the fastest of the three. It runs its prey down and bites (2 half hearts); walls do not stop it —
// MoveMob lets it climb them. In daylight Minecraft's spiders calm down unless they are already after someone;
// here they simply hunt less far by day (they notice prey at 18 m in the dark, 12 m by day).
bool SpiderBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                 const std::vector<BeingInfo>* beings) {
    m.attackCooldown -= dt;
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 3.0f);
    const float range = TheHost().Dark() ? 18.0f : 12.0f;
    Vec3 feet, mid;
    float dist = 0.0f;
    if (!PickPrey(m, dt, playerPos, beings, range, &feet, &mid, &dist))
        return false;
    Vec3 to = feet - m.pos;
    to.z = 0.0f;
    const float flat = to.Length();
    const Vec3 dir = flat > 0.01f ? to * (1.0f / flat) : Vec3(0, 1, 0);
    m.goal = Flat(to);
    m.aiTimer = 0.5f;
    *looking = true;
    *speed = kTypes[m.kind].run;
    m.moving = flat > 1.3f;
    if (flat < 1.8f && std::fabs(feet.z - m.pos.z) < 1.8f && m.attackCooldown <= 0.0f) {
        m.attackCooldown = 0.7f;
        m.attackAnim = 1.0f;
        StrikePrey(m, dir, 2.0f, 2.5f);
    }
    return true;
}

// ================================================================ the Counter-Strike soldier
// Step 1 of the Counter-Strike project: one counter-terrorist (the SEAL model), a real creature of this
// core like the monsters - not a skin and not one of the host's people. His body, his voice and his
// M4A1 are Counter-Strike 1.6's own (his texture is cut out of the game's own model file, his sounds are
// the game's own sound files); his brain is written here the way the zombie's was: from the game's own
// rules, seen from outside, since Valve's bot code was never published. The user's standard for him is
// zero-to-a-hundred faithful to the game, but much better - so the brain is deliberately a professional
// player's, far past the original bots. One line is kept: he is a strong professional, not a cheat wall.
// Every bullet still rolls its spread, a moving man at range can live, and he can be outplayed. His
// final ceiling is set by the user's own eyes in the game, which is why every number of his strength
// sits in this ONE table and nowhere else.

// The one soldier table: turn a number here and the soldier in the game changes with it.
struct SoldierTune {
    float sightRange;   // how far he sees his enemy
    float sightCos;     // the edge of his field of view, as a cosine (120 degrees in all)
    float pointBlank;   // inside this he feels the enemy even at his back
    float reaction;     // seconds of seeing before the first burst (half of it once he is alerted)
    float strafeSpeed;  // his sideways drift while he holds his angle in a fight
    float patrolRadius; // how far around his post he patrols
    int magazine;       // the M4A1's thirty rounds
    float reloadTime;   // the M4A1's reload, about three seconds
    float shotInterval; // 600 rounds a minute
    int burstMin, burstMax; // a professional's burst: three to five rounds, then a breath
    float burstPauseMin, burstPauseMax;
    float damage;     // half hearts a bullet does up close (the M4A1's 33, on this host's scale)
    float damageFar;  // the share of it left at sightRange (Counter-Strike's falloff with distance)
    float spreadSettled; // his aim error in degrees: standing settled...
    float spreadStrafe;  // ...drifting sideways...
    float spreadRun;     // ...and at a run (Counter-Strike punishes shooting on the move the same way)
    float firstShot;     // the first bullet of a burst is this much tighter (tap discipline)
    float retreatUnder;  // below this share of his health he falls back while he fights
    float searchSeconds; // how long he hunts a lost enemy before he gives him up
    float armorAbsorb;   // kevlar swallows this share of every hit while it lasts (Counter-Strike: half)
};
const SoldierTune kSoldier = {
    45.0f, 0.5f, 4.0f, // sight
    0.22f,             // reaction
    3.0f,              // strafe
    13.0f,             // patrol radius
    30, 3.0f, 0.1f,    // the M4A1
    3, 5, 0.35f, 0.6f, // bursts
    6.0f, 0.55f,       // damage and falloff
    1.1f, 2.8f, 5.5f,  // spread: settled, strafing, running
    0.6f,              // first shot of a burst
    0.30f,             // retreat under 30% health
    5.0f,              // search seconds
    0.5f,              // kevlar absorbs half
};

// turn his body towards a flat direction at a set rate (his brain uses it while he stands and scans;
// while he walks, Think turns him)
void SoldierTurn(Mob& m, const Vec3& toward, float rate, float dt) {
    Vec3 t = toward;
    t.z = 0.0f;
    if (t.Length() < 0.05f)
        return;
    const float want = std::atan2(-t.x, t.y);
    const float d = WrapAngle(want - m.yaw);
    const float step = rate * dt;
    m.yaw = WrapAngle(m.yaw + Clamp(d, -step, step));
}

// One bullet of his M4A1. Counter-Strike's rifles are hitscan - no flying bullet, the shot lands the
// moment it is fired - so the shot is a line, not a projectile: from his muzzle, through an aim point
// scattered by his spread, and the man is hit when the line passes through his body. His spread follows
// Counter-Strike's own rule (settled he is tight, moving he sprays) and the first bullet of a burst is
// the tightest, the way a professional taps.
void SoldierShoot(Mob& m, const Vec3& playerFeet, const Vec3& playerMid, float dist) {
    const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);
    const Vec3 fwd = Flat(playerFeet - m.pos);
    const Vec3 from = eye + fwd * 0.6f;
    const float hv = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
    float spread = hv < 0.6f ? kSoldier.spreadSettled : hv < 3.6f ? kSoldier.spreadStrafe : kSoldier.spreadRun;
    if (m.burstShots == m.burstSize)
        spread *= kSoldier.firstShot;
    const float err = std::tan(spread * kPi / 180.0f) * dist;
    const float a = Rand01() * 6.2831853f, rr = err * std::sqrt(Rand01());
    const Vec3 aim = playerMid + Vec3(std::cos(a) * rr, std::sin(a) * rr, (Rand01() - 0.5f) * err);
    Vec3 dir = aim - from;
    const float len = dir.Length();
    bool hit = false;
    if (len > 0.1f) {
        dir = dir * (1.0f / len);
        // the closest the line comes to the man's body (checked at shin, chest and head height)
        for (float h = 0.4f; h <= 1.7f && !hit; h += 0.65f) {
            const Vec3 p = playerFeet + Vec3(0, 0, h);
            const Vec3 rel = p - from;
            const float t = rel.Dot(dir);
            if (t < 0.0f || t > dist + 2.0f)
                continue;
            hit = (rel - dir * t).Length() < 0.42f;
        }
    }
    m.attackAnim = 1.0f;
    m.shootShow = 0.18f; // the shoot pose holds a breath past the shot, so a burst reads as firing
    PlaySfx(SND_M4A1_SHOOT, &from, 3.0f, 0.95f + Rand01() * 0.1f);
    // the shot is SEEN, not only heard: a flash at the rifle's own muzzle (the model's
    // anchor, through the same stand the draw uses) and a tracer along the bullet's
    // line - to the man it hit, or past the aim point when it missed. A model file
    // without anchors (version 1) falls back to the brain's own muzzle point.
    if (len > 0.1f) {
        Vec3 muzzle = from, butt;
        const Vec3 mfwd(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
        const Vec3 mright(mfwd.y, -mfwd.x, 0.0f);
        const Pose stand = EntityPose(m.pos, mright, Vec3(0, 0, 1), mfwd, MobScale(m));
        SoldierGunPose(stand, SP_SHOOT, 0, m.soldierPoseTime, &muzzle, &butt); // the flash sits at the model's muzzle
        Vec3 end;
        if (hit) {
            const float t = Clamp((playerMid - muzzle).Dot(dir), 0.0f, len);
            end = muzzle + dir * t;
        } else {
            end = muzzle + dir * (dist + 2.0f);
        }
        gShotFx.push_back(ShotFx{ muzzle, end, 0.0f });
        if (gShotFx.size() > 64)
            gShotFx.erase(gShotFx.begin(), gShotFx.begin() + (ptrdiff_t)(gShotFx.size() - 64));
    }
    if (hit) {
        float dmg = kSoldier.damage;
        if (dist > 10.0f) {
            const float f = Clamp((dist - 10.0f) / (kSoldier.sightRange - 10.0f), 0.0f, 1.0f);
            dmg *= 1.0f - (1.0f - kSoldier.damageFar) * f;
        }
        StrikePrey(m, fwd, dmg, 0.0f); // his prey is the player (huntBeing -1), down the usual road
    }
}

// The soldier's brain. His enemy is the other team and only the other team: in this step that is the
// player (the player fights for the terrorists; the soldier is counter). The host's people in the
// street belong to no team and he never goes for them, whatever passes - `beings` is not even read.
// He patrols his post, checks the angles at every stop, and fights the way he was tuned: see, react,
// burst, hold or close, fall back when he is hurt, hunt what he lost, and reload like a man who knows
// an empty rifle is a death sentence - on the move, never standing in the open.
// ---- his one piece of real navigation: a sight-line probe ahead of his body. He
// asks the bridge what stands in front of him (the bridge's line of sight counts the
// city's own buildings, vehicles and objects) and, when his road is blocked, walks
// around the blockage at the first open angle instead of pressing into the wall.
// What this probe cannot do is stated plainly in CS-REALMODEL-fa.md: it looks 2.4 m
// ahead at chest height and nothing more - no stairs, no low fences, no route
// planning across a street. It is a detour reflex, not a pathfinder.
Vec3 SoldierAdvance(Mob& m, const Vec3& want, float dt) {
    if (m.steerTime > 0.0f) {
        m.steerTime -= dt;
        return m.steerDir;
    }
    const Vec3 from = m.pos + Vec3(0, 0, 1.1f);
    if (!TheHost().LineBlocked(from, from + want * 2.4f))
        return want;
    static const float kTry[6] = { 0.61f, -0.61f, 1.22f, -1.22f, 1.92f, -1.92f }; // 35, 70, 110 degrees
    for (float a : kTry) {
        const float c = std::cos(a), s = std::sin(a);
        const Vec3 cand(want.x * c - want.y * s, want.x * s + want.y * c, 0.0f);
        if (!TheHost().LineBlocked(from, from + cand * 2.4f)) {
            m.steerDir = cand;
            m.steerTime = 0.6f;
            return cand;
        }
    }
    return want; // walled in on every side he can reach: he pushes on, and physics stops him
}

bool SoldierBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                  const std::vector<BeingInfo>* beings) {
    (void)beings; // the street is not his war
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 2.5f);
    m.shootShow = std::max(0.0f, m.shootShow - dt);
    const bool wasAlerted = m.alerted;
    const Vec3 playerFeet = playerPos - Vec3(0, 0, 1.0f);
    Vec3 to = playerFeet - m.pos;
    to.z = 0.0f;
    const float dist = to.Length();
    const Vec3 dir = dist > 0.01f ? to * (1.0f / dist) : Vec3(0, 1, 0);
    const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);

    // ---- seeing: a cone in front of him (and a man's instinct at point blank), a clear line, a living enemy
    bool see = false;
    if (gGame.deathTime < 0.0f && dist < kSoldier.sightRange && std::fabs(playerFeet.z - m.pos.z) < 8.0f) {
        const Vec3 fwd(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
        const float dot = fwd.x * dir.x + fwd.y * dir.y;
        if (dot > kSoldier.sightCos || dist < kSoldier.pointBlank)
            see = !TheHost().LineBlocked(eye, playerPos);
    }
    if (see) {
        m.seenTime += dt;
        m.lastSeen = playerFeet;
        m.searchTime = kSoldier.searchSeconds;
        m.alerted = true;
        m.alertFade = 0.0f;
        m.scanTime = 0.0f;
    } else {
        m.seenTime = std::max(0.0f, m.seenTime - dt * 3.0f);
        if (m.searchTime > 0.0f)
            m.searchTime -= dt;
        else if (m.alerted) {
            m.alertFade += dt;
            if (m.alertFade > 25.0f)
                m.alerted = false; // a long quiet: he settles back to a patrolman's calm
        }
    }
    const float reaction = wasAlerted ? kSoldier.reaction * 0.5f : kSoldier.reaction;
    const bool canFire = see && m.seenTime >= reaction;
    m.huntValid = see; // Think's head and body machinery reads the prey from these two
    m.huntBeing = -1;
    *looking = see;

    // ---- the rifle: reload first (it owns him until it is done), then bursts while he may fire
    if (m.reloadT > 0.0f) {
        m.reloadT -= dt;
        if (m.reloadStage == 1 && m.reloadT <= 2.0f) {
            m.reloadStage = 2;
            const Vec3 c = m.pos + Vec3(0, 0, 1.2f);
            PlaySfx(SND_M4A1_CLIPIN, &c, 1.2f);
        } else if (m.reloadStage == 2 && m.reloadT <= 0.8f) {
            m.reloadStage = 3;
            const Vec3 c = m.pos + Vec3(0, 0, 1.2f);
            PlaySfx(SND_M4A1_BOLTPULL, &c, 1.2f);
        }
        if (m.reloadT <= 0.0f) {
            m.mag = kSoldier.magazine;
            m.burstShots = 0;
            m.burstPause = 0.15f;
        }
    } else if (canFire) {
        if (m.burstPause > 0.0f) {
            m.burstPause -= dt;
        } else {
            if (m.burstShots <= 0) { // a new burst (or, with an empty magazine, the reload)
                if (m.mag <= 0) {
                    m.reloadT = kSoldier.reloadTime;
                    m.reloadStage = 1;
                    const Vec3 c = m.pos + Vec3(0, 0, 1.2f);
                    PlaySfx(SND_M4A1_CLIPOUT, &c, 1.2f);
                } else {
                    m.burstSize = m.burstShots = std::min(m.mag, kSoldier.burstMin + rand() % (kSoldier.burstMax - kSoldier.burstMin + 1));
                    m.shotTimer = 0.0f;
                }
            }
            if (m.burstShots > 0) {
                m.shotTimer -= dt;
                if (m.shotTimer <= 0.0f) {
                    m.shotTimer = kSoldier.shotInterval;
                    SoldierShoot(m, playerFeet, playerPos, dist);
                    --m.burstShots;
                    --m.mag;
                    if (m.burstShots <= 0)
                        m.burstPause = kSoldier.burstPauseMin + Rand01() * (kSoldier.burstPauseMax - kSoldier.burstPauseMin);
                }
            }
        }
        m.attackAnim = std::max(m.attackAnim, 0.7f); // the rifle stays up while the enemy is in his sight
    } else {
        m.burstPause = std::max(0.0f, m.burstPause - dt);
        m.shotTimer = 0.0f;
    }

    // ---- moving: fight, hunt, or patrol
    const MobType& t = kTypes[m.kind];
    const float maxHealth = t.health;
    m.aiTimer = 0.5f;
    if (see) {
        const bool hurt = m.health < maxHealth * kSoldier.retreatUnder;
        if (hurt && dist < 30.0f) {
            m.goal = SoldierAdvance(m, dir * -1.0f, dt); // falling back, still facing the fight
            m.moving = true;
            *speed = t.run * 0.8f;
        } else if (dist > 26.0f) {
            m.goal = SoldierAdvance(m, dir, dt);
            m.moving = true;
            *speed = t.run;
        } else if (dist < 6.0f) {
            m.goal = SoldierAdvance(m, dir * -1.0f, dt);
            m.moving = true;
            *speed = kSoldier.strafeSpeed;
        } else {
            // his band: he holds his angle and drifts across it, changing side every few seconds -
            // a standing target is a dead one, and he knows it
            m.strafeTime -= dt;
            if (m.strafeTime <= 0.0f) {
                m.strafeTime = 1.6f + Rand01() * 1.6f;
                m.strafeDir = -m.strafeDir;
            }
            const Vec3 side(dir.y, -dir.x, 0.0f);
            m.goal = side * m.strafeDir;
            m.moving = true;
            *speed = kSoldier.strafeSpeed;
        }
    } else if (m.searchTime > 0.0f) {
        Vec3 toSeen = m.lastSeen - m.pos;
        toSeen.z = 0.0f;
        if (toSeen.Length() > 1.8f) {
            m.goal = SoldierAdvance(m, Flat(toSeen), dt);
            m.moving = true;
            *speed = t.run * 0.75f; // he advances on a lost enemy like a man clearing a street: fast, not charging
            m.scanTime = 0.0f;
        } else {
            // at the last seen spot: he stops and sweeps the angles before he gives the man up
            m.moving = false;
            if (m.scanTime <= 0.0f)
                m.scanTime = 2.2f;
            m.scanTime -= dt;
            SoldierTurn(m, Vec3(std::sin(m.scanTime * 2.4f + (float)m.id), std::cos(m.scanTime * 2.4f + (float)m.id), 0.0f), 2.2f, dt);
            m.attackAnim = std::max(m.attackAnim, 0.7f);
        }
    } else {
        // patrol: six posts around his anchor; at every post he stops, rifle up, and checks the angles
        // before he walks on (the seed of corner-checking: never drift past an angle unlooked)
        if (m.patrolWait > 0.0f) {
            m.patrolWait -= dt;
            m.moving = false;
            m.attackAnim = std::max(m.attackAnim, 0.7f);
            const float a = (float)m.patrolStep * 1.0472f + (float)(m.id % 6) * 0.5f;
            SoldierTurn(m, Vec3(std::cos(a), std::sin(a), 0.0f), 1.6f, dt);
            if (m.patrolWait <= 0.0f) {
                m.patrolStep = (m.patrolStep + 1) % 6;
                m.patrolLeg = 0.0f;
            }
        } else {
            const float a = (float)m.patrolStep * 1.0472f + (float)(m.id % 6) * 0.5f;
            const Vec3 post = m.anchor + Vec3(std::cos(a), std::sin(a), 0.0f) * kSoldier.patrolRadius;
            Vec3 toPost = post - m.pos;
            toPost.z = 0.0f;
            m.patrolLeg += dt;
            if (toPost.Length() < 1.2f || m.patrolLeg > 9.0f) {
                m.patrolWait = 1.2f + Rand01() * 1.2f; // (a leg that takes too long is given up, post reached or not)
                m.moving = false;
            } else {
                m.goal = SoldierAdvance(m, Flat(toPost), dt);
                m.moving = true;
                *speed = t.walk;
            }
        }
    }

    // ---- what his body shows: the brain's state IS the animation state, the way the
    // game itself layers it (SoldierModel.h). Firing plants his feet for the burst;
    // a moving man walks or runs under the same aim layer; a still man with his
    // rifle up stands in the aim pose; the rest is the patrolman's idle.
    // the gait his pace asks for: the run at a fighting pace, the walk below it -
    // and a man drifting SIDEWAYS across his aim keeps the walk gait whatever his
    // speed: his legs step where he drifts, they do not run in place sideways.
    // (The model has no strafe gait of its own - urban.mdl carries none - so the
    // walk cycle is the honest cloth.) The shoot pose borrows this gait for its
    // moving variants, the way the game's lower body runs independently.
    int movePose = SP_IDLE;
    if (m.moving) {
        const Vec3 face = see ? dir : m.goal;
        const float along = std::fabs(m.goal.x * face.x + m.goal.y * face.y);
        movePose = (*speed > 2.6f && along >= 0.45f) ? SP_RUN : SP_WALK;
    }
    int pose = SP_IDLE;
    int variant = 0;
    if (m.reloadT > 0.0f)
        pose = SP_RELOAD;
    else if (m.burstShots > 0 || m.shootShow > 0.0f) {
        pose = SP_SHOOT;
        variant = movePose == SP_RUN ? 2 : movePose == SP_WALK ? 1 : 0; // firing keeps his gait under him
    } else if (m.moving)
        pose = movePose;
    else if (see || m.attackAnim > 0.3f)
        pose = SP_AIM;
    if (pose != m.soldierPose || variant != m.soldierVariant) {
        // the pose he leaves becomes the cross-fade's source (SoldierModel blends
        // the two); his gait phase carries over, so a stride is never restarted
        m.soldierPrevPose = m.soldierPose;
        m.soldierPrevVariant = m.soldierVariant;
        m.soldierPrevTime = m.soldierPoseTime;
        m.soldierPrevGait = m.soldierGait;
        m.soldierFade = 0.0f;
        m.soldierPose = pose;
        m.soldierVariant = variant;
        m.soldierPoseTime = 0.0f;
    } else {
        m.soldierPoseTime += dt;
    }
    if (m.soldierFade < 1.0f) {
        m.soldierFade = std::min(1.0f, m.soldierFade + dt / kSoldierFadeSeconds);
        m.soldierPrevTime += dt; // the pose he is leaving keeps playing while it fades
    }

    // ---- his gait phase: advanced by the ground he actually covers, at the
    // model's own stride length, so a foot planted on the ground stays planted.
    // Towards or away from what he faces the phase is signed (a man falling back
    // runs his cycle backwards, the way the game plays it); drifting sideways it
    // advances with the distance, unsigned.
    auto gaitPoseOf = [](int p, int v) -> int {
        if (p == SP_WALK || p == SP_RUN)
            return p;
        if (p == SP_SHOOT && v == 1)
            return SP_WALK;
        if (p == SP_SHOOT && v == 2)
            return SP_RUN;
        return -1;
    };
    const int gaitPose = gaitPoseOf(pose, variant);
    if (gaitPose >= 0) {
        const Vec3 fwdv(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
        const float alongV = m.vel.x * fwdv.x + m.vel.y * fwdv.y;
        const float hv = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
        const float sideV = std::sqrt(std::max(0.0f, hv * hv - alongV * alongV));
        const float adv = std::fabs(alongV) >= sideV ? alongV : hv;
        const float stride = SoldierStride(gaitPose);
        if (stride > 0.01f)
            m.soldierGait += adv * dt / stride;
        if (m.soldierFade < 1.0f) {
            const int prevGaitPose = gaitPoseOf(m.soldierPrevPose, m.soldierPrevVariant);
            if (prevGaitPose >= 0) {
                const float prevStride = SoldierStride(prevGaitPose);
                if (prevStride > 0.01f)
                    m.soldierPrevGait += adv * dt / prevStride;
            }
        }
    }

    // ---- his steps, on his stride, the game's own footstep sounds: one step is
    // half a gait cycle, so the sound lands when a foot does
    if (m.moving && m.onGround) {
        const float hv = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
        const float stride = gaitPose >= 0 ? SoldierStride(gaitPose) : 0.0f;
        const float stepLen = stride > 0.01f ? stride * 0.5f : 2.6f;
        m.stepAcc += hv * dt;
        if (m.stepAcc > stepLen) {
            m.stepAcc = 0.0f;
            PlaySfx(SND_SOLDIER_STEP, &m.pos, 1.3f, 0.95f + Rand01() * 0.1f);
        }
    } else {
        m.stepAcc = 0.0f;
    }
    return true; // a soldier is always on duty: patrol, hunt and fight are all his
}

void Think(Mob& m, float dt, bool playerOnFoot, const Vec3& playerPos, uint16_t playerHeld,
           const std::vector<BeingInfo>* beings) {
    const MobType& t = kTypes[m.kind];
    m.aiTimer -= dt;
    float speed = 0.0f;
    Vec3 toPlayer = playerPos - m.pos;
    toPlayer.z = 0.0f;
    const float playerDist = toPlayer.Length();
    bool looking = false;
    bool chasing = false;
    if (t.monster) {
        m.panic = 0.0f; // monsters do not run away
        if (m.kind == MOB_WARDEN)
            chasing = WardenBrain(m, dt, playerPos, &speed, &looking, beings);
        else if (m.kind == MOB_ZOMBIE)
            chasing = ZombieBrain(m, dt, playerPos, &speed, &looking, beings);
        else if (m.kind == MOB_SKELETON)
            chasing = SkeletonBrain(m, dt, playerPos, &speed, &looking, beings);
        else if (m.kind == MOB_SPIDER)
            chasing = SpiderBrain(m, dt, playerPos, &speed, &looking, beings);
        else if (m.kind == MOB_SOLDIER)
            chasing = SoldierBrain(m, dt, playerPos, &speed, &looking, beings);
        else
            chasing = CreeperFuse(m, dt, playerPos, toPlayer, &speed, &looking, beings);
    }

    if (chasing) {
        // (CreeperFuse or WardenBrain steered it)
    } else if (m.panic > 0.0f) {
        m.panic -= dt;
        if (m.aiTimer <= 0.0f) {
            m.goal = RandomDir();
            m.aiTimer = 0.7f + Rand01() * 0.8f;
        }
        m.moving = true;
        speed = t.run;
    } else if (playerOnFoot && playerDist < 10.0f && IsFood(m, playerHeld)) {
        // TemptGoal
        m.goal = Flat(toPlayer);
        m.moving = playerDist > 2.4f;
        speed = t.walk * 1.25f;
        looking = true;
    } else {
        if (m.aiTimer <= 0.0f) {
            if (Rand01() < 0.45f) {
                m.moving = true;
                m.goal = RandomDir();
                m.aiTimer = 1.5f + Rand01() * 3.5f;
            } else {
                m.moving = false;
                m.aiTimer = 2.0f + Rand01() * 5.0f;
            }
        }
        speed = t.walk;
        looking = !m.moving && playerDist < 7.0f;
    }
    if (m.baby > 0.0f)
        speed *= 1.1f;

    // where the prey this hunter is after actually stands: the player, or one of the host's people it picked.
    // A hunter's eyes belong on its prey; until now the head always turned to the player, whoever it was after.
    Vec3 preyPos = playerPos;
    bool hasPrey = false;
    if (chasing && m.huntValid) {
        hasPrey = m.huntBeing < 0;
        if (m.huntBeing >= 0 && beings)
            for (const BeingInfo& b : *beings)
                if (b.being == m.huntBeing) {
                    preyPos = b.pos;
                    hasPrey = true;
                    break;
                }
    }
    // A skeleton fights the way Minecraft's do: the whole body faces its prey while it sidesteps, backs off
    // or closes in. Its bow is drawn in the body's frame (McModel) and its head rides on the body, so facing
    // the prey is what puts the look, the aim and the arrow on one line. Before this, the body faced where it
    // walked (sideways, while strafing), the head looked at the player, and the arrow flew at the prey: three
    // different ways at once, which is exactly what the user saw. The soldier fights the same way - a man
    // with a rifle keeps his body on his enemy while his feet drift - so he shares the rule.
    const bool aiming = hasPrey && (m.kind == MOB_SKELETON || m.kind == MOB_SOLDIER);

    // turn the body towards where it walks (a skeleton with prey: towards the prey, fast, the way it aims)
    if (aiming) {
        Vec3 to = preyPos - m.pos;
        to.z = 0.0f;
        if (to.Length() > 0.05f) {
            float want = std::atan2(-to.x, to.y);
            float d = WrapAngle(want - m.yaw);
            float maxTurn = 12.0f * dt;
            m.yaw = WrapAngle(m.yaw + Clamp(d, -maxTurn, maxTurn));
        }
    } else if (m.moving) {
        float want = std::atan2(-m.goal.x, m.goal.y);
        float d = WrapAngle(want - m.yaw);
        float maxTurn = (chasing && t.monster ? 10.0f : 7.0f) * dt; // a hunter turns sharper than a grazer
        m.yaw = WrapAngle(m.yaw + Clamp(d, -maxTurn, maxTurn));
    }
    Vec3 fwd(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
    // A hunting skeleton walks where its brain says (sideways, back, in) while its body keeps facing the
    // prey: its feet follow the goal, not the nose. Everyone else walks where he faces, as before.
    Vec3 target = Vec3(0, 0, 0);
    if (m.moving)
        target = aiming ? m.goal * speed : fwd * speed;
    // a hunter also answers quicker: the shared soft start (9 a second) made the chase feel like wading
    float k = Clamp(dt * (m.onGround ? (chasing && t.monster ? 14.0f : 9.0f) : 1.2f), 0.0f, 1.0f);
    m.vel.x += (target.x - m.vel.x) * k;
    m.vel.y += (target.y - m.vel.y) * k;

    // head: look at the player now and then (a hunter: at its prey, so the eyes agree with the body)
    float wantYaw = 0.0f, wantPitch = 0.0f;
    Vec3 toLook = hasPrey ? preyPos - m.pos : toPlayer;
    toLook.z = 0.0f;
    const float lookDist = toLook.Length();
    if (looking && lookDist > 0.3f) {
        Vec3 dir = toLook * (1.0f / lookDist);
        Vec3 right(fwd.y, -fwd.x, 0.0f);
        wantYaw = Clamp(std::atan2(dir.x * right.x + dir.y * right.y, dir.x * fwd.x + dir.y * fwd.y), -1.1f, 1.1f);
        float dz = ((hasPrey ? preyPos.z : playerPos.z) + 0.6f) - (m.pos.z + MobHeight(m) * 0.9f);
        wantPitch = Clamp(-std::atan2(dz, lookDist), -0.6f, 0.6f);
    }
    float hk = Clamp(dt * 6.0f, 0.0f, 1.0f);
    m.headYaw += (wantYaw - m.headYaw) * hk;
    m.headPitch += (wantPitch - m.headPitch) * hk;
}

void Kill(Mob& m) {
    if (m.death >= 0.0f)
        return;
    m.death = 0.0f;
    m.health = 0.0f;
    if (m.kind == MOB_SOLDIER)
        m.soldierDeath = (int)(Rand01() * 2.99f); // one of the game's three falls, his own
    m.moving = false;
    Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
    PlaySfx(kTypes[m.kind].death, &c, 1.0f, m.baby > 0.0f ? 1.5f : 0.9f + Rand01() * 0.2f);
}

// a spot on the host's ground 30 to 75 m from the player
bool SpawnSpot(const Vec3& playerPos, Vec3* ground) {
    float a = Rand01() * 6.2831853f, dist = 30.0f + Rand01() * 45.0f;
    Vec3 from(playerPos.x + std::cos(a) * dist, playerPos.y + std::sin(a) * dist, playerPos.z + 30.0f);
    return TheHost().SpawnGround(from, ground);
}

void TrySpawn(const Vec3& playerPos) {
    if (!TheHost().Outdoors())
        return;
    // at night the monsters come out, one at a time: creepers, zombies, skeletons and spiders
    if (gRules.monsters && TheHost().Dark()) {
        int monsters = 0;
        for (auto& m : gMobs)
            monsters += kTypes[m.kind].monster;
        Vec3 ground;
        if (monsters < gRules.maxMonsters && Rand01() < 0.5f) {
            if (SpawnSpot(playerPos, &ground)) {
                const float r = Rand01();
                const int kind = r < 0.4f    ? MOB_CREEPER
                                 : r < 0.65f ? MOB_ZOMBIE
                                 : r < 0.85f ? MOB_SKELETON
                                             : MOB_SPIDER;
                SpawnMob(kind, ground, false);
            }
            return;
        }
    }
    if (!gRules.animals)
        return;
    int wild = 0;
    for (auto& m : gMobs)
        if (!m.persistent && !kTypes[m.kind].monster)
            ++wild;
    if (wild >= gRules.maxAnimals)
        return;
    float a = Rand01() * 6.2831853f, dist = 30.0f + Rand01() * 45.0f;
    Vec3 from(playerPos.x + std::cos(a) * dist, playerPos.y + std::sin(a) * dist, playerPos.z + 30.0f);
    Vec3 ground;
    if (!TheHost().SpawnGround(from, &ground))
        return;
    float r = Rand01();
    int kind = r < 0.3f ? MOB_COW : (r < 0.55f ? MOB_SHEEP : (r < 0.78f ? MOB_PIG : MOB_CHICKEN));
    int pack = 2 + rand() % 3;
    for (int i = 0; i < pack && wild < gRules.maxAnimals; ++i) {
        Vec3 p = ground;
        if (i > 0) {
            Vec3 f(ground.x + (Rand01() - 0.5f) * 8.0f, ground.y + (Rand01() - 0.5f) * 8.0f, ground.z + 8.0f);
            if (!TheHost().SpawnGround(f, &p))
                continue;
        }
        if (SpawnMob(kind, p, false) >= 0)
            ++wild;
    }
}
} // namespace

uint32_t MobIdAt(int index) { return index >= 0 && index < (int)gMobs.size() ? gMobs[index].id : 0; }

int MobIndexById(uint32_t id) {
    if (!id)
        return -1;
    for (size_t i = 0; i < gMobs.size(); ++i)
        if (gMobs[i].id == id)
            return (int)i;
    return -1;
}

bool MobRide(uint32_t id, const Vec3& velocity, Vec3* seat, float* yaw) {
    int i = MobIndexById(id);
    if (i < 0 || gMobs[i].death >= 0.0f)
        return false;
    Mob& m = gMobs[i];
    m.rideVel = velocity;
    *seat = m.pos + Vec3(0, 0, MobHeight(m) + 0.25f);
    *yaw = m.yaw;
    return true;
}

void MobsClear() {
    gMobs.clear();
    gSpawnTimer = 3.0f;
    gReinforceCalls.clear();
}

int SpawnMob(int kind, const Vec3& feet, bool persistent, bool baby) {
    if (kind < 0 || kind >= MOB_KIND_COUNT || gMobs.size() >= 96)
        return -1;
    if (kind == MOB_SOLDIER && !SoldierModelReady()) {
        // The user's law for this soldier: Counter-Strike's real model or nothing -
        // no box stand-in wearing his name. The loader already said why in the log.
        Log("summon soldier refused: cs_soldier.bin is not loaded, and a fake soldier is not an option");
        return -1;
    }
    Mob m;
    m.kind = kind;
    m.pos = feet;
    m.yaw = Rand01() * 6.2831853f - kPi;
    m.health = kTypes[kind].health;
    m.aiTimer = Rand01() * 3.0f;
    m.sayTimer = 3.0f + Rand01() * 15.0f;
    m.eggTimer = 90.0f + Rand01() * 120.0f;
    m.persistent = persistent;
    m.groundZ = feet.z;
    m.baby = baby ? 90.0f : 0.0f;
    if (kind == MOB_SOLDIER) {
        // posted where he was summoned: counter-terrorist, kevlar on, a full magazine in the rifle
        m.team = TEAM_COUNTER;
        m.armor = 200.0f; // Counter-Strike's 100 kevlar points, in half hearts like his health
        m.anchor = feet;
        m.mag = kSoldier.magazine;
    }
    m.id = gNextMobId++;
    gMobs.push_back(m);
    return (int)gMobs.size() - 1;
}

Vec3 MobCentre(int index) {
    if (index < 0 || index >= (int)gMobs.size())
        return Vec3(0, 0, 0);
    const Mob& m = gMobs[index];
    return m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
}

MobHit MobsRaycast(const Vec3& o, const Vec3& dir, float maxDist) {
    MobHit best;
    best.dist = maxDist;
    for (size_t i = 0; i < gMobs.size(); ++i) {
        const Mob& m = gMobs[i];
        if (m.death >= 0.0f)
            continue;
        const float r = MobWidth(m) * 0.5f + 0.05f;
        const float lo[3] = { m.pos.x - r, m.pos.y - r, m.pos.z };
        const float hi[3] = { m.pos.x + r, m.pos.y + r, m.pos.z + MobHeight(m) };
        const float ro[3] = { o.x, o.y, o.z }, rd[3] = { dir.x, dir.y, dir.z };
        float t0 = 0.0f, t1 = best.dist;
        bool miss = false;
        for (int a = 0; a < 3 && !miss; ++a) {
            if (std::fabs(rd[a]) < 1e-6f) {
                miss = ro[a] < lo[a] || ro[a] > hi[a];
                continue;
            }
            float ta = (lo[a] - ro[a]) / rd[a], tb = (hi[a] - ro[a]) / rd[a];
            if (ta > tb)
                std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            miss = t0 > t1;
        }
        if (miss)
            continue;
        best.index = (int)i;
        best.dist = t0;
        best.point = o + dir * t0;
    }
    return best;
}

void MobHurt(int index, float halfHearts, const Vec3& from, float knock) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    if (m.death >= 0.0f || m.hurt > 0.25f)
        return; // dead, or still invulnerable from the last hit
    float dmg = halfHearts;
    if (m.kind == MOB_SOLDIER) {
        // Counter-Strike's kevlar: while it lasts it swallows half of every hit, and what is left reaches
        // the man. A professional in armour is also barely shoved by a blow. And being shot at tells him
        // where his enemy is: he is alerted, and he hunts the shot's origin.
        knock *= 0.25f;
        if (m.armor > 0.0f) {
            const float absorbed = std::min(m.armor, dmg * kSoldier.armorAbsorb);
            m.armor -= absorbed;
            dmg -= absorbed;
        }
        m.alerted = true;
        m.alertFade = 0.0f;
        m.lastSeen = from;
        m.searchTime = std::max(m.searchTime, kSoldier.searchSeconds);
    }
    m.health -= dmg;
    m.hurt = 0.5f;
    m.panic = kTypes[m.kind].monster ? 0.0f : 4.0f + Rand01() * 2.0f;
    m.aiTimer = 0.0f;
    m.love = 0.0f;
    if (m.kind == MOB_WARDEN) {
        m.anger = std::max(m.anger, 100.0f); // furious, and nothing knocks it back
        knock = 0.0f;
    }
    Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
    if (knock > 0.0f) {
        Vec3 away = Flat(m.pos - from);
        m.vel.x = m.vel.x * 0.5f + away.x * 7.0f * knock;
        m.vel.y = m.vel.y * 0.5f + away.y * 7.0f * knock;
        m.vel.z = std::min(8.0f, m.vel.z * 0.5f + 6.0f);
        m.onGround = false;
        m.goal = away;
    }
    if (m.health <= 0.0f)
        Kill(m);
    else
        PlaySfx(kTypes[m.kind].hurt, &c, 1.0f, m.baby > 0.0f ? 1.5f : 0.9f + Rand01() * 0.2f);
    // a zombie that lives through the blow and is after someone may call for help, the way Minecraft's do
    // (8% a landed hit, Normal-style). The call is only noted here: spawning inside MobHurt could move gMobs
    // under a caller that still holds a Mob&, so the spawn itself happens at the top of MobsTick.
    if (m.kind == MOB_ZOMBIE && m.health > 0.0f && m.huntValid && gRules.monsters && gReinforceCalls.size() < 4 &&
        Rand01() < 0.08f)
        gReinforceCalls.push_back(m.pos);
}

void MobPush(int index, const Vec3& velocity) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    m.vel += velocity;
    if (velocity.z > 0.5f)
        m.onGround = false;
    if (m.death < 0.0f) {
        m.panic = std::max(m.panic, 3.0f);
        m.aiTimer = 0.0f;
    }
}

void MobsRadial(const Vec3& at, float radius, float halfHearts, float push) {
    for (size_t i = 0; i < gMobs.size(); ++i) {
        Mob& m = gMobs[i];
        Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
        Vec3 d = c - at;
        float dist = d.Length();
        if (dist >= radius)
            continue;
        float f = 1.0f - dist / radius;
        if (halfHearts > 0.0f) {
            m.hurt = 0.0f; // explosions ignore the invulnerability window
            MobHurt((int)i, halfHearts * f, at, 0.0f);
        }
        Vec3 dir = dist > 0.05f ? d * (1.0f / dist) : Vec3(0, 0, 1);
        dir.z = std::max(dir.z, 0.0f) + 0.5f;
        MobPush((int)i, dir * (push * f));
    }
}

bool MobInteract(int index, bool playerOnFoot) {
    if (index < 0 || index >= (int)gMobs.size())
        return false;
    Mob& m = gMobs[index];
    ItemStack& held = gInv.Held();
    if (m.death >= 0.0f)
        return false;
    const int special = held.Empty() ? 0 : Item(held.id).special;
    Vec3 c = m.pos + Vec3(0, 0, MobHeight(m) * 0.6f);
    const bool survival = gGame.gameMode == MODE_SURVIVAL;
    if (special == SP_SADDLE && m.kind == MOB_PIG && !m.saddled && m.baby <= 0.0f) {
        m.saddled = true;
        m.persistent = true;
        if (survival && --held.count == 0)
            held.Clear();
        PlaySfx(SND_SADDLE, &c);
        StartSwing();
        return true;
    }
    if (m.saddled && !IsFood(m, held.id) && special != SP_SHEARS && special != SP_BUCKET && special != SP_WAND &&
        !gGame.ridingMob && playerOnFoot) {
        gGame.ridingMob = m.id;
        PlaySfx(SND_SADDLE, &c, 0.5f);
        return true;
    }
    if (held.Empty())
        return false;
    const ItemDef& d = Item(held.id);
    if (d.special == SP_SHEARS && m.kind == MOB_SHEEP && !m.sheared && m.baby <= 0.0f) {
        m.sheared = true;
        m.woolTimer = 60.0f + Rand01() * 60.0f;
        int n = 1 + rand() % 3;
        for (int i = 0; i < n; ++i) {
            ItemStack s;
            s.id = ID_WHITE_WOOL;
            s.count = 1;
            SpawnDrop(c, s, Vec3((Rand01() - 0.5f) * 3.0f, (Rand01() - 0.5f) * 3.0f, 3.0f + Rand01() * 2.0f), 0.5f);
        }
        PlaySfx(SND_SHEEP_SHEAR, &c);
        DamageHeldItem(1);
        StartSwing();
        return true;
    }
    if (d.special == SP_BUCKET && m.kind == MOB_COW && m.baby <= 0.0f) {
        ItemStack milk;
        milk.id = ID_MILK_BUCKET;
        milk.count = 1;
        if (!survival) {
            if (gInv.CountOf(ID_MILK_BUCKET) == 0)
                gInv.Add(milk);
        } else if (held.count <= 1) {
            held = milk;
        } else {
            --held.count;
            if (gInv.Add(milk) > 0)
                DropStackAtPlayer(milk, false);
        }
        PlaySfx(SND_COW_MILK, &c);
        StartSwing();
        gWorld.dirty = true;
        return true;
    }
    if (IsFood(m, held.id) && m.baby <= 0.0f && m.love <= 0.0f && m.breedCooldown <= 0.0f) {
        m.love = 30.0f;
        if (survival && --held.count == 0)
            held.Clear();
        Puff(c + Vec3(0, 0, 0.4f), TILE_P_HEART, 7, 0.8f);
        PlaySfx(SND_EAT, &c, 0.6f);
        StartSwing();
        return true;
    }
    return false;
}

void MobsSpawnTick(float dt, const Vec3& playerPos) {
    gSpawnTimer -= dt;
    if (gSpawnTimer <= 0.0f) {
        gSpawnTimer = 1.5f;
        TrySpawn(playerPos);
    }
}

void MobsTick(float dt, const Vec3& playerPos, uint16_t playerHeld, bool playerOnFoot,
              const std::vector<BeingInfo>* beings) {
    // a shot's flash and tracer live for a blink and no longer; they age out here
    for (size_t i = 0; i < gShotFx.size();) {
        gShotFx[i].age += dt;
        if (gShotFx[i].age >= kShotFxSeconds)
            gShotFx.erase(gShotFx.begin() + (ptrdiff_t)i);
        else
            ++i;
    }

    // zombie reinforcements called for since the last tick are answered here, before any Mob& is held: one new
    // zombie on real ground 10 to 20 m from the one that called, through the same SpawnMob as every other spawn.
    // The ceiling is sane: never more monsters than twice the night-spawn limit (and SpawnMob's own cap besides).
    if (!gReinforceCalls.empty()) {
        std::vector<Vec3> calls;
        calls.swap(gReinforceCalls);
        for (const Vec3& at : calls) {
            int monsters = 0;
            for (const Mob& o : gMobs)
                monsters += kTypes[o.kind].monster;
            if (monsters >= gRules.maxMonsters * 2)
                continue;
            for (int tries = 0; tries < 8; ++tries) {
                const float a = Rand01() * 6.2831853f, d = 10.0f + Rand01() * 10.0f;
                Vec3 from(at.x + std::cos(a) * d, at.y + std::sin(a) * d, at.z + 30.0f);
                Vec3 ground;
                if (TheHost().SpawnGround(from, &ground) && SpawnMob(MOB_ZOMBIE, ground, false) >= 0)
                    break;
            }
        }
    }
    for (size_t i = 0; i < gMobs.size();) {
        Mob& m = gMobs[i];
        const MobType& t = kTypes[m.kind];
        bool remove = false;
        m.hurt = std::max(0.0f, m.hurt - dt);
        m.hitByCar = std::max(0.0f, m.hitByCar - dt);
        m.breedCooldown = std::max(0.0f, m.breedCooldown - dt);
        if (m.baby > 0.0f)
            m.baby = std::max(0.0f, m.baby - dt);
        Vec3 centre = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
        const float distToPlayer = (m.pos - playerPos).Length();

        if (m.death >= 0.0f) {
            m.death += dt;
            m.vel.x *= 1.0f - Clamp(dt * 4.0f, 0.0f, 1.0f);
            m.vel.y *= 1.0f - Clamp(dt * 4.0f, 0.0f, 1.0f);
            if (m.death >= 1.0f) {
                Puff(centre, TILE_P_GENERIC_0, 14, MobWidth(m), 0xFFFFFFFF, true);
                DropLoot(m);
                remove = true;
            }
        } else {
            // zombies and skeletons burn in the sun, the way Minecraft's do: out in the open in daylight the fire
            // is kept alight (the burn code in MoveMob hurts them once a second while the timer runs down)
            if ((m.kind == MOB_ZOMBIE || m.kind == MOB_SKELETON) && !TheHost().Dark() && TheHost().Outdoors() &&
                m.burn < 0.5f)
                m.burn = 2.5f;
            if (m.id != 0 && m.id == gGame.ridingMob) {
                Vec3 want = m.rideVel;
                float k = Clamp(dt * (m.onGround ? 8.0f : 1.0f), 0.0f, 1.0f);
                m.vel.x += (want.x - m.vel.x) * k;
                m.vel.y += (want.y - m.vel.y) * k;
                m.moving = want.Length() > 0.2f;
                if (m.moving) {
                    float target = std::atan2(-want.x, want.y);
                    float d = WrapAngle(target - m.yaw);
                    m.yaw = WrapAngle(m.yaw + Clamp(d, -6.0f * dt, 6.0f * dt));
                }
                m.headYaw *= 0.9f;
                m.panic = 0.0f;
            } else {
                Think(m, dt, playerOnFoot, playerPos, playerHeld, beings);
            }
            if (m.swell >= 1.0f) {
                // the creeper blows up (radius 3, as a creeper does) and is gone
                const Vec3 at = m.pos + Vec3(0, 0, MobHeight(m) * 0.5f);
                m.death = 1e9f; // (its own blast does not kill it a second time)
                if ((playerPos - at).Length() < 7.0f)
                    NoteDamage(STR_DEATH_EXPLOSION);
                ExplodeAt(at, 3.0f, true, BLAST_TNT);
                remove = true;
            }

            if (m.kind == MOB_WARDEN && m.calm >= 60.0f) {
                // a minute with nobody to be angry at: it digs back into the ground
                Puff(m.pos + Vec3(0, 0, 0.3f), TILE_P_GENERIC_0, 20, MobWidth(m), 0xFF203038, true);
                PlaySfx(SND_WARDEN_DIG, &centre);
                remove = true;
            }

            m.sayTimer -= dt;
            if (m.sayTimer <= 0.0f && (!t.monster || (m.kind == MOB_WARDEN && m.anger < 80.0f))) {
                m.sayTimer = 8.0f + Rand01() * 18.0f;
                if (distToPlayer < 32.0f)
                    PlaySfx(t.say, &centre, 1.0f, m.baby > 0.0f ? 1.5f : 0.9f + Rand01() * 0.2f);
            }
            if (m.kind == MOB_CHICKEN && m.baby <= 0.0f) {
                m.eggTimer -= dt;
                if (m.eggTimer <= 0.0f) {
                    m.eggTimer = 120.0f + Rand01() * 180.0f;
                    ItemStack s;
                    s.id = ID_EGG;
                    s.count = 1;
                    SpawnDrop(centre, s, Vec3(0, 0, 1.0f), 0.5f);
                    PlaySfx(SND_CHICKEN_EGG, &centre);
                }
            }
            if (m.sheared) {
                m.woolTimer -= dt;
                if (m.woolTimer <= 0.0f)
                    m.sheared = false;
            }
            if (m.love > 0.0f) {
                m.love -= dt;
                if (rand() % 20 == 0)
                    Puff(centre + Vec3(0, 0, MobHeight(m) * 0.5f), TILE_P_HEART, 1, 0.6f);
                for (size_t j = 0; j < gMobs.size(); ++j) {
                    Mob& o = gMobs[j];
                    if (j == i || o.kind != m.kind || o.love <= 0.0f || o.death >= 0.0f)
                        continue;
                    if ((o.pos - m.pos).Length() > 6.0f)
                        continue;
                    m.love = o.love = 0.0f;
                    m.breedCooldown = o.breedCooldown = 60.0f;
                    Vec3 at = (m.pos + o.pos) * 0.5f;
                    const int kind = m.kind;
                    const bool persistent = m.persistent || o.persistent;
                    SpawnXp(at + Vec3(0, 0, 0.5f), 1 + rand() % 7);
                    SpawnMob(kind, at, persistent, true); // may move gMobs: do not touch m / o after this
                    Puff(at + Vec3(0, 0, 0.8f), TILE_P_HEART, 7, 0.8f);
                    break;
                }
            }
        }
        // (gMobs may have been reallocated by SpawnMob above)
        Mob& mm = gMobs[i];

        // gravity; chickens flutter down
        mm.vel.z -= 32.0f * dt;
        if (mm.kind == MOB_CHICKEN && mm.vel.z < 0.0f && mm.death < 0.0f)
            mm.vel.z *= std::pow(0.6f, dt * 20.0f);
        if (mm.vel.z < -60.0f)
            mm.vel.z = -60.0f;
        MoveMob(mm, dt);

        // animation
        float hs = std::sqrt(mm.vel.x * mm.vel.x + mm.vel.y * mm.vel.y);
        float amount = mm.death >= 0.0f ? 0.0f : Clamp(hs / 20.0f * 4.0f / MobScale(mm), 0.0f, 1.0f);
        mm.limbAmount += (amount - mm.limbAmount) * Clamp(dt * 8.0f, 0.0f, 1.0f);
        mm.limbSwing += mm.limbAmount * 20.0f * dt * (mm.baby > 0.0f ? 1.5f : 1.0f);
        if (mm.kind == MOB_CHICKEN) {
            const float ticks = dt * 20.0f;
            mm.flapSpeed = Clamp(mm.flapSpeed + (mm.onGround ? -1.0f : 4.0f) * 0.3f * ticks, 0.0f, 1.0f);
            if (!mm.onGround && mm.flapping < 1.0f)
                mm.flapping = 1.0f;
            mm.flapping *= std::pow(0.9f, ticks);
            mm.flap += mm.flapping * 2.0f * ticks;
        }

        const float limit = mm.id == gGame.ridingMob ? 1e9f : mm.persistent ? 320.0f : 130.0f;
        if (distToPlayer > limit || mm.pos.z < -120.0f)
            remove = true;
        if (remove) {
            gMobs[i] = gMobs.back();
            gMobs.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace mc
