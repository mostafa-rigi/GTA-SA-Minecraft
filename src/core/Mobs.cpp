#include "Mobs.h"
#include "NavGraph.h"

#include <chrono>
#include <cstdlib>

#include "Audio.h"
#include "BossTest.h"
#include "BlockRules.h"
#include "Combat.h"
#include "CsRules.h"
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
    // the Counter-Strike soldier: a man, not a monster of Minecraft's - 1.8 m tall. As the BOSS his
    // health is 4000 half hearts (2000 CS hit points - the magazine arithmetic is in CS-BOSS-fa.md)
    // with 1200 more of kevlar on top (the `armor` field, set at spawn). His `say` is never played on
    // the ambient timer (monsters have none); his brain plays his footsteps itself, on his stride. His
    // walk is his patrol pace, his run is CJ's own sprint; the whole of his difficulty lives in the
    // two tune tables (kSoldier, kBoss). (The pool was 2400 + 800 until the smart pack: the user
    // ordered more health, and the structure - kevlar absorbing half, no headshot channel - is kept.)
    { 0.6f, 1.8f, 4000.0f, 2.4f, 5.5f, SND_SOLDIER_STEP, SND_SOLDIER_HURT, SND_SOLDIER_DEATH, { 0, 0, 0 }, true },
};

uint32_t gNextMobId = 1;
float gSpawnTimer = 3.0f;
std::vector<Vec3> gReinforceCalls; // where hurt zombies have called for help (answered at the top of MobsTick)
} // namespace


// ------------------------------------------------------------------ movement

std::vector<Mob> gMobs;
std::vector<ShotFx> gShotFx;

// ---- boss diagnostics (the diagnostic build): the phase timings of the tick
// in flight, read by the long-frame logger at the bottom of MobsTick. The
// soldier brain's own share is timed around its call in Think; the voice
// count is lines actually spoken (BossSay), the one voice number the core
// can count honestly - the host's playback cost is its own.
static float sBrainMs = 0.0f;
static int sVoiceLines = 0;

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
    SpawnXp(at, kTypes[m.kind].monster ? 5 : 1 + McsaRand() % 3);
    auto drop = [&](uint16_t id, int lo, int hi) {
        int n = lo + (hi > lo ? McsaRand() % (hi - lo + 1) : 0);
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
// =====================================================================
// THE BOSS - the solo Counter-Strike soldier, finished (workstreams B-I, K, L).
// One SEAL, rebuilt to the standard the user set: a microscope-eyed, sharp-eared
// professional with the whole map in his head, who talks first and fights like
// the game's own best - and who the street itself can fight (workstream K: his
// perception body in the bridge takes the gangs' fire for him).
//
// Every number of his body, senses and craft lives in the two tables below
// (kSoldier kept from step 1 for the weapon's character, kBoss for the rest) -
// nowhere else. His final ceiling is set by the user's own eyes in the game.

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
    float damage;     // half hearts a bullet does up close (the M4A1's 33, on this host's scale) -
                      // since the rulebook pour this is the SCALE ANCHOR of CsRules (33 CS = 6 hh)
                      // and the vehicle-scaling reference; a bullet's price itself is CsRules' work
    float spreadSettled; // his aim error in degrees: standing settled...
    float spreadStrafe;  // ...drifting sideways...
    float spreadRun;     // ...and at a run (Counter-Strike punishes shooting on the move the same way)
    float firstShot;     // the first bullet of a burst is this much tighter (tap discipline)
    float retreatUnder;  // below this share of his health he falls back while he fights
    float searchSeconds; // how long he hunts a lost enemy before he gives him up
};
const SoldierTune kSoldier = {
    60.0f, 0.5f, 4.0f, // sight (the boss's range; his cone/meter live in kBoss)
    0.15f,             // reaction: the boss's post-detection trigger discipline
    3.0f,              // strafe
    13.0f,             // patrol radius
    30, 3.07f, 0.09f,  // the M4A1: CS's own reload (3.07 s) and cycle (0.09 s, ~667 rpm)
    3, 5, 0.35f, 0.6f, // bursts
    6.0f,              // the anchor (above)
    1.1f, 2.8f, 5.5f,  // spread: settled, strafing, running
    0.6f,              // first shot of a burst
    0.30f,             // retreat under 30% health
    5.0f,              // search seconds
};

// The boss table: body, senses, hunt, combat craft, talk-first, voice.
struct BossTune {
    // B - the body (see CS-BOSS-fa.md for the magazine arithmetic)
    float health;      // half hearts: 4000 = two thousand CS hit points (smart pack; was 2400)
    float armor;       // half hearts of kevlar pool: 1200 (was 800)
    float ramCapShare; // a full-speed car hit can never take more than this share of his pool
    // C - the senses
    float coneHalfDeg;   // vision cone half-angle (150 degrees in all)
    float centreHalfDeg; // the fast centre of the cone (120 in all)
    float pitchUpDeg;    // he sees up to +70 degrees of elevation...
    float pitchDownDeg;  // ...and down to -60
    float nightRangeMul; // night: vision range x0.75, hearing x1.3 - net the same man
    float nightHearMul;
    float fillClose;   // detection fill rate at arm's length, full body, cone centre (per second)
    float fillFar;     // ...and at maximum range
    float fillCentreMul; // cone-centre fill bonus
    float fillEdgeMul;   // cone-edge fill
    float fillMoveMul;   // a moving target fills faster
    float fillAimMul;    // being AIMED at fills at this flat rate on top (native peds react to aim)
    float fillDecay;     // the meter drains this fast when nothing feeds it
    float glanceAt;      // at this fill he turns to look (suspicion)
    float hearGunshot;   // hearing radii, metres (direction + alert only; walls muffle by LOS)
    float hearExplosion;
    float hearSprint;
    float hearRun;       // a jog: Counter-Strike's running footstep, honestly audible
    float hearWalk;      // ZERO by CS's own law: shift-walking and creeping make no
                         // footstep sound at all - a slow man is a silent man
    float hearWeapon;    // a bowstring / crossbow at this range
    float hearVehicle;   // an engine at this range primes him; faster = louder
    // D - mobility
    float jumpClear;   // he jumps obstacles up to this height (fences, low walls, car hoods)
    float jumpVel;     // the arc's launch speed under the world's gravity
    float chaseSpeed;  // his sprint when closing, chasing or hunting: faster than CJ's own 5.5 -
                       // the gait phase advances by ground covered, so the run cycle simply turns
                       // faster (about 2.2 cycles a second) and his feet still meet the street;
                       // the accepted cost is a hurried-looking sprint, never foot-skate
    // D2 - combat navigation on the city graph (the smart pack)
    int repathBudget;    // the hard A* expansion budget of one combat repath (measured: the longest
                         // city chase route in Los Santos expands ~4100; this leaves headroom and
                         // caps one repath at a few milliseconds even on the user's Core 2 Duo)
    float repathMinT;    // seconds between combat repaths at the closest (the throttle)
    float repathGoalMove; // a repath is owed when the steered-to point has moved this far, metres
    float stuckSeconds;  // no real progress for this long while a move is wanted: stall break + repath
    // E - the hunt
    float huntSeconds;   // a hunt (intercept + search) lasts this long before he stands down to suspicious
    float searchRadius;  // search points gather within this of the last seen spot
    // F - combat craft
    float crouchRange;   // at this range or more he crouches to shoot (stance spread below)
    float crouchSpread;  // crouched spread multiplier
    float peekMin, peekMax; // seconds exposed while peek-firing from cover
    int pistolMag;       // the USP's fifteen
    float pistolDamage;  // half hearts per USP bullet up close: the USP's TRUE price since the
                         // rulebook pour (34 CS on the anchor scale) - it was a deliberate 3.4
                         // before; BossShoot prices through CsRules, this is the vehicle reference
    float pistolInterval; // his cadence on the pistol: a brain's pacing, not a weapon cap
    float pistolRange;   // inside this, an empty rifle becomes the pistol instead of a reload
    float nadeMin, nadeMax; // the HE grenade's throwing window (target held in cover)
    float nadeFuse;
    float nadeRadius;
    float nadeDamage;    // the HE's heart, in CS points (96 unarmoured by the measured guide);
                         // CsRules prices the curve from it, through the core channel
    float nadeHeldTime;  // the target must sit in cover this long before a grenade is worth throwing
    float suppWindow;    // suppressive fire covers a corner this long after the target slips behind it
    float woundedShare;  // below this health share he fights more carefully (longer cover, shorter peeks)
    float settleSeconds; // after stopping, this long before his spread is the settled one (stop-shoot)
    float vehDamage;     // half hearts of vehicle damage for a plain body hit of his rifle on a car
                         // (the bridge pays x4 onto the game's 1000-point vehicle health: 36 a hit,
                         // a native M4 bullet's own weight against a car)
    float vehEngineMul;  // an engine-bay hit multiplies the vehicle damage by this
    float vehWheelMul;   // a wheel-band hit pays this share as body damage - and bursts the tyre
    // G - talk first
    float commandWindow; // seconds he holds his command before judging compliance
    float commandRange;  // he approaches a compliant man to this distance
    float primedSeconds; // after a stand-down his detection runs primed (x2) this long
    // I - presence
    float idleVoiceMin, idleVoiceMax; // an idle radio line every 45-90 s on post
    float returnRange;   // idle and farther than this from Grove: he routes home
    // H - the warm trail (the fix round after the 2026-11-07 eye verdict)
    float warmSeconds;   // a firefight stays warm this long after the last exchange
    float warmArea;      // ...and the hunt may not stand down while the prey is inside this radius
    // the engagement law + damage feedback (the user's order, 2026-11-07)
    float aimAggroSeconds; // a weapon pointed at him this long makes the player his enemy
    float staggerHit;      // a single hit (post-armour half hearts) at or above this staggers him
    float staggerSeconds;  // ...for this long: no fire, barely any movement
    float hurtVoiceHit;    // a single hit at or above this always earns a hurt line,
                           // whatever the voice throttle says
};
const BossTune kBoss = {
    4000.0f, 1200.0f, 0.35f,                   // body (smart pack: a deeper pool, same structure)
    75.0f, 60.0f, 70.0f, 60.0f,                // cone and pitch
    0.75f, 1.3f,                               // night trading
    7.0f, 2.4f, 1.5f, 0.7f, 1.35f, 3.5f, 0.25f, 0.45f, // detection fill
    70.0f, 90.0f, 16.0f, 13.0f, 0.0f, 12.0f, 30.0f, // hearing (walk is silent: CS's law)
    1.1f, 8.6f, 6.4f,                          // jump + the chase sprint (smart pack)
    6000, 1.0f, 4.0f, 1.5f,                    // combat navigation: A* budget, repath throttle, goal move, stall
    45.0f, 12.0f,                              // hunt
    25.0f, 0.55f, 0.6f, 1.0f,                  // crouch + peek
    15, 6.18f, 0.28f, 8.0f,                    // the USP at its true price (34 CS on the anchor)
    8.0f, 30.0f, 1.8f, 5.5f, 96.0f, 2.5f, 2.5f, 0.35f, // the grenade (CS heart: 96) + suppression + wounded
    0.12f, 9.0f, 2.0f, 0.5f,                   // the settle + vehicle damage (smart pack)
    1.6f, 4.0f, 60.0f,                         // talk first
    45.0f, 90.0f, 150.0f,                      // presence
    60.0f, 175.0f,                             // the warm trail: a minute of memory, a wide hunt area
    0.45f, 150.0f, 0.35f, 100.0f,              // the engagement law + damage feedback
};

// Talk-first (workstream G) belongs to the future GUARD step - a posted
// soldier at his checkpoint, under orders. The free-roam boss lives under
// the engagement law the user ordered on 2026-11-07: no warnings, no
// commands, nothing to do with the player until the player provokes him.
// The command machinery stays in the tree for the guard step, behind this
// switch, OFF - there must be no live path that shouts orders at a calm man.
constexpr bool kGuardTalkEnabled = false;



// ------------------------------------------------------------------ his voice
// Lines per state, chosen here - never twice the same in a row, and about one
// time in five he says nothing at all on a line that is not critical (a real
// man does not narrate himself). The events are appended sound events; a state
// with no files yet maps to -1 and he stays silent rather than faking a line.
struct BossVoice {
    int ev[3];   // up to three line events for the state
    int count;
    bool critical;
};
enum BossSayState {
    SAY_COMMAND, SAY_SPOTTED, SAY_LOST, SAY_SEARCH, SAY_RELOAD,
    SAY_GRENADE, SAY_STANDDOWN, SAY_IDLE, SAY_COUNT
};
const BossVoice kBossVoice[SAY_COUNT] = {
    { { SND_BOSS_CMD1, SND_BOSS_CMD2, SND_BOSS_CMD3 }, 3, true },       // command
    { { SND_BOSS_SPOT1, SND_BOSS_SPOT2, SND_BOSS_SPOT3 }, 3, true },    // spotted
    { { SND_BOSS_LOST1, SND_BOSS_LOST2, -1 }, 2, false },               // lost him
    { { SND_BOSS_SRCH1, SND_BOSS_SRCH2, -1 }, 2, false },               // searching
    { { SND_BOSS_RLD1, -1, -1 }, 1, false },                            // reloading
    { { SND_BOSS_GREN1, -1, -1 }, 1, true },                            // grenade out
    { { SND_BOSS_DOWN1, -1, -1 }, 1, false },                           // stand down
    { { SND_BOSS_IDLE1, SND_BOSS_IDLE2, -1 }, 2, false },               // idle radio
};

void BossSay(Mob& m, int state, float cooldown) {
    if (state < 0 || state >= SAY_COUNT || m.voiceCd > 0.0f)
        return;
    const BossVoice& v = kBossVoice[state];
    if (v.count <= 0)
        return;
    if (!v.critical && Rand01() < 0.2f) { // the random silence
        m.voiceCd = cooldown;
        return;
    }
    int pick = (int)(Rand01() * (float)v.count);
    if (v.count > 1 && v.ev[pick] == m.lastVoiceEv)
        pick = (pick + 1) % v.count; // never the same line twice in a row
    if (v.ev[pick] < 0)
        return;
    m.lastVoiceEv = v.ev[pick];
    m.voiceCd = cooldown;
    ++sVoiceLines; // diag: a line actually spoken this tick (long-frame logger)
    const Vec3 c = m.pos + Vec3(0, 0, 1.5f);
    PlaySfx((SoundEvent)v.ev[pick], &c, 1.6f);
}

// turn a yaw towards a flat direction at a set rate (his aim and his legs turn
// on their own clocks - see the pose block at the brain's end)
float TurnYawToward(float yaw, const Vec3& toward, float rate, float dt) {
    Vec3 t = toward;
    t.z = 0.0f;
    if (t.Length() < 0.05f)
        return yaw;
    const float want = std::atan2(-t.x, t.y);
    const float d = WrapAngle(want - yaw);
    const float step = rate * dt;
    return WrapAngle(yaw + Clamp(d, -step, step));
}
void SoldierTurn(Mob& m, const Vec3& toward, float rate, float dt) {
    m.yaw = TurnYawToward(m.yaw, toward, rate, dt);
}

// ------------------------------------------------------------------ targets
// Who the boss is fighting this tick, described once for the senses, the craft
// and the rifle: the player on foot, the player in his car, or one of the
// street's own people who attacked him (workstream K1).
struct BossTarget {
    bool valid = false;
    int being = -1;          // -1: the player; >= 0: a host being
    Vec3 feet{ 0, 0, 0 };    // his ground position
    Vec3 mid{ 0, 0, 0 };     // his chest (the aim point on foot)
    float dist = 0.0f;
    bool inVehicle = false;  // the player, driving
    Vec3 vehCentre{ 0, 0, 0 }, vehFwd{ 0, 1, 0 }, vehSeat{ 0, 0, 0 }, vehVel{ 0, 0, 0 };
};

// Damage down the core's OWN channel, the one no cheat code stands in front of:
// the bridge builds the player's hurt directly out of this call (a generated
// damage event on the player ped, never a weapon fired AT him - the GTA weapon
// path is where BAGUVIX lives). Beings take a real shot attribution instead.
void BossStrike(Mob& m, const BossTarget& t, const Vec3& dir, float halfHearts) {
    if (t.being >= 0) {
        ShotOwner by;
        by.hostile = true;
        by.shooter = -3; // the boss's mark: not the player, nobody else in particular
        TheHost().HurtBeing(t.being, halfHearts, HURT_SHOT, &by);
        TheHost().PushBeing(t.being, dir * 1.2f);
    } else if (t.inVehicle) {
        TheHost().HurtPlayer(halfHearts); // through the windshield: the core channel again
    } else {
        TheHost().HurtPlayer(halfHearts);
    }
}

// (The zone pricing for a round that lands on a vehicle - BossVehicleZone -
// is defined at mc:: scope below, next to the boss's other shared state: the
// tests pin its numbers, so it cannot live in this file-local section.)

// One bullet of his rifle (or, with the pistol flag, of the USP). Counter-Strike's
// rifles are hitscan: the shot is a line from his muzzle through an aim point
// scattered by his spread and walked by his recoil, and it lands where the
// line first meets the world - or, through a thin surface, on the far side
// of it, at Counter-Strike's own price (the rulebook pour: CsRules).
//
// THE SMART PACK'S HIT RESOLUTION (the workstream-L repair): the shipped build
// never traced this line at all. It tested three body points by hand and, only
// when the target was the player AND driving, one coarse closest-approach box
// around his car - so a round through a parked car, a shielding bike, or the
// player's own car beside him touched nothing, and the box's rare hits paid
// ~14 of the game's 1000 vehicle health, far under the smoke line: vehicle
// damage that existed in the tests and never in the game. Now every round is
// traced through the host's own world (Host::ShotTrace, the same door the
// player's arrows have always used) and the FIRST thing on the line decides:
// a vehicle in front of the target takes the round itself - his target's own
// car included, zoned by BossVehicleZone - a wall stops it, and only a line
// that reaches the body strikes the man (a driver still through his glass).
void BossShoot(Mob& m, const BossTarget& t, bool pistol, const Vec3* aimOverride = nullptr, bool noDamage = false) {
    const CsWeapon& weap = CsWeaponAt(pistol ? CSW_USP : CSW_M4A1); // his two, at their true rules
    const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);
    const Vec3 fwd = Flat(t.feet - m.pos);
    const Vec3 from = eye + fwd * 0.6f;
    const float dist = t.dist;
    const float hv = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
    // Counter-Strike's stop-shoot rule: a man who has only just planted his
    // feet has not settled yet - his first fraction of a second shoots at the
    // strafe grade, and only a held stop earns the settled spread
    float effHv = hv;
    if (hv < 0.6f && m.settleT < kBoss.settleSeconds)
        effHv = 0.6f;
    float spread = effHv < 0.6f ? kSoldier.spreadSettled : effHv < 3.6f ? kSoldier.spreadStrafe : kSoldier.spreadRun;
    if (m.crouch)
        spread *= kBoss.crouchSpread;
    if (!pistol && m.burstShots == m.burstSize)
        spread *= kSoldier.firstShot;
    if (pistol)
        spread *= 1.6f; // the USP is a sidearm, not a rifle
    // Counter-Strike's recoil rides on top of the spread: the pattern says
    // where this shot of the spray walks (the rifles' climb, then the seeded
    // sway; the USP's straight vertical line), and he - a professional -
    // pulls against most of it, so a tenth of the climb and under half the
    // sway survive his compensation. The index is the spray's own: it climbs
    // while he fires and the brain recovers it while he rests; the pistol's
    // spray is counted apart from the rifle's, as the game's accuracy is.
    if (pistol != m.sprayPistol) {
        m.sprayPistol = pistol;
        m.sprayIndex = 0.0f;
    }
    const CsRecoil rec = CsRecoilPattern(pistol ? CSW_USP : CSW_M4A1, (int)m.sprayIndex, (uint32_t)m.spraySeed);
    m.sprayIndex += 1.0f;
    m.sprayIdle = 0.0f;
    const Vec3 aimPoint = aimOverride ? *aimOverride : (t.inVehicle ? t.vehSeat : t.mid);
    const float err = std::tan(spread * kPi / 180.0f) * dist;
    const float a = Rand01() * 6.2831853f, rr = err * std::sqrt(Rand01());
    const Vec3 rightF(fwd.y, -fwd.x, 0.0f);
    const Vec3 patOff = rightF * (std::tan(rec.yawDeg * 0.45f * kPi / 180.0f) * dist) +
                        Vec3(0, 0, std::tan(rec.pitchDeg * 0.12f * kPi / 180.0f) * dist);
    const Vec3 aim = aimPoint + Vec3(std::cos(a) * rr, std::sin(a) * rr, (Rand01() - 0.5f) * err) + patOff;
    Vec3 dir = aim - from;
    const float len = dir.Length();
    bool hit = false;       // the line reaches the target's body
    float tBody = 1e9f;     // ...at this distance along the line
    int zone = CSZ_CHEST;   // the zone the line crosses the body at (a driver: his chest, by the seat)
    HostHit tr;             // the host's own trace of the same line
    bool haveTrace = false;
    if (len > 0.1f) {
        dir = dir * (1.0f / len);
        if (t.inVehicle) {
            // the driver: one chest point behind glass - a line within a torso's
            // width of it wounds him (the glass keeps a seventh of the bullet)
            const Vec3 rel = t.vehSeat - from;
            const float tt = rel.Dot(dir);
            if (tt > 0.0f && tt < dist + 3.0f && (rel - dir * tt).Length() < 0.45f) {
                hit = true;
                tBody = tt;
            }
        } else {
            for (float h = 0.4f; h <= 1.7f && !hit; h += 0.65f) {
                const Vec3 p = t.feet + Vec3(0, 0, h);
                const Vec3 rel = p - from;
                const float tt = rel.Dot(dir);
                if (tt < 0.0f || tt > dist + 2.0f)
                    continue;
                if ((rel - dir * tt).Length() < 0.42f) {
                    hit = true;
                    tBody = tt;
                }
            }
        }
        ShotOwner by;
        by.hostile = true;
        by.shooter = -3; // his mark: the bridge skips nobody for it, the line is honest
        tr = TheHost().ShotTrace(from, dir, dist + 3.0f, by);
        haveTrace = true;
        if (hit && !t.inVehicle)
            zone = CsZoneFromHeight(CsHitHeight(from, dir, t.feet, tBody));
    }
    // ---- resolution: what the round actually meets first, surface by
    // surface. The smart pack's law stands - the first thing on the line
    // decides - and Counter-Strike's signature rides on it now: a round that
    // meets a THIN surface does not die there. It pays the surface's price
    // out of its damage (CsPenetrate, FireBullets3's shape) and flies on,
    // up to the weapon's penetration count; the trace is re-asked past each
    // surface it passes. A thick wall still stops the round cold, a vehicle
    // in the line still takes it whole, and a man the trace itself struck is
    // a hit, not a wall. Suppressive fire never buys passage: it stops at
    // the first surface, as it always did - its whole job is the noise.
    const int targetVeh = t.inVehicle ? TheHost().PlayerVehicle() : -1;
    int resVeh = -1;      // a vehicle takes this round
    Vec3 resPoint;        // ...at this point on it
    bool resWall = false; // the round dies on the world before any body
    Vec3 wallPoint;       // ...on this surface
    float penMul = 1.0f;  // the damage share left after the surfaces it passed
    if (haveTrace) {
        HostHit seg = tr;
        float segBase = 0.0f; // how far along the line this segment starts
        int surfaces = 0;
        while (true) {
            const float segAbs = segBase + seg.dist;
            if (seg.vehicle >= 0 && (!hit || (seg.vehicle != targetVeh && segAbs < tBody - 0.05f))) {
                resVeh = seg.vehicle; // a vehicle shields the man, or is simply
                resPoint = seg.point; // in the line: it takes his round. (The
                // target's own car never shields its driver - his glass is
                // priced in the damage - but a round that misses the man
                // still lands on his car.)
                hit = false;
                break;
            }
            const bool wallHere = seg.hit && seg.vehicle < 0 &&
                segAbs < (hit ? tBody - 0.3f : dist + 2.0f) &&
                !(seg.being >= 0 && std::fabs(seg.beingDist - seg.dist) < 0.5f);
            if (!wallHere)
                break; // the line is clear to the body (or to nothing)
            if (noDamage || surfaces >= weap.penSurfaces) {
                resWall = true;
                wallPoint = seg.point;
                hit = false;
                break;
            }
            const CsPen pen = CsPenetrate(weap, seg.surface, seg.depth);
            if (!pen.passes) {
                resWall = true;
                wallPoint = seg.point;
                hit = false;
                break;
            }
            penMul *= pen.dmgMul;
            ++surfaces;
            const float step = pen.thicknessM + 0.06f;
            const float remain = (dist + 3.0f) - (segAbs + step);
            if (remain <= 0.05f) {
                resWall = true;
                wallPoint = seg.point;
                hit = false;
                break;
            }
            ShotOwner by;
            by.hostile = true;
            by.shooter = -3;
            seg = TheHost().ShotTrace(seg.point + dir * step, dir, remain, by);
            segBase = segAbs + step;
        }
    }
    m.attackAnim = 1.0f;
    m.shootShow = 0.18f; // the shoot pose holds a breath past the shot, so a burst reads as firing
    PlaySfx(pistol ? SND_BOSS_USP : SND_M4A1_SHOOT, &from, 3.0f, 0.95f + Rand01() * 0.1f);
    if (len > 0.1f) {
        Vec3 muzzle = from, butt;
        const Vec3 mfwd(-std::sin(m.yaw), std::cos(m.yaw), 0.0f);
        const Vec3 mright(mfwd.y, -mfwd.x, 0.0f);
        const Pose stand = EntityPose(m.pos, mright, Vec3(0, 0, 1), mfwd, MobScale(m));
        SoldierDrawState st = m.draw;
        st.upCategory = SP_SHOOT;
        SoldierGunPose(stand, st, &muzzle, &butt); // the flash sits at the model's muzzle
        Vec3 end;
        if (hit) {
            const float tt = Clamp((aimPoint - muzzle).Dot(dir), 0.0f, len);
            end = muzzle + dir * tt;
        } else if (resVeh >= 0 || resWall) {
            end = resVeh >= 0 ? resPoint : wallPoint; // the tracer ends where the round ended
        } else {
            end = muzzle + dir * (dist + 2.0f);
        }
        if (!gBossTest.fxOff) { // bisect (/bosstest fx): the shot still happens, its flash and tracer do not
            gShotFx.push_back(ShotFx{ muzzle, end, 0.0f });
            if (gShotFx.size() > 64)
                gShotFx.erase(gShotFx.begin(), gShotFx.begin() + (ptrdiff_t)(gShotFx.size() - 64));
        }
    }
    if (noDamage)
        return; // suppressive fire: noise and tracers on a corner, never damage - the standing label
    if (hit) {
        // Counter-Strike prices the bullet now, whole (CsRules): the weapon's
        // base damage, the range modifier over the true distance, the zone
        // the line crossed the body at - and, for the player, kevlar where
        // the game's own vest is actually on him: the armoured zones pay the
        // ratio's half-share to his health and wear the vest by the same law
        // (this channel bypasses the game's armour, so the core keeps the
        // vest's books itself, through the host). A street being wears
        // nothing. A driver takes the chest price through his windshield -
        // the glass keeps its tax, as it always has on this host.
        float dmg;
        if (t.being >= 0) {
            dmg = CsShotHh(weap, dist, zone, false, 0.0f, nullptr) * penMul;
            BossStrike(m, t, fwd, dmg);
        } else {
            const float armourPts = TheHost().PlayerArmor();
            float wear = 0.0f;
            dmg = CsShotHh(weap, dist, zone, armourPts > 0.5f, armourPts * 0.2f, &wear);
            if (t.inVehicle) {
                dmg *= 0.85f; // the windshield's tax
                wear *= 0.85f;
            }
            dmg *= penMul;
            wear *= penMul;
            BossStrike(m, t, fwd, dmg);
            if (wear > 0.0f)
                TheHost().WearPlayerArmor(wear);
        }
    } else if (resVeh >= 0) {
        // his fire genuinely works the vehicle over: zoned damage through the
        // bridge on whichever vehicle the line met - the player's car, a
        // traffic car in his line, a bike used as a shield; a wheel-band hit
        // bursts the tyre nearest where it landed
        Vec3 centre, vfwd;
        bool have = false;
        if (resVeh == targetVeh) {
            centre = t.vehCentre;
            vfwd = t.vehFwd;
            have = true;
        } else {
            have = TheHost().VehiclePose(resVeh, &centre, &vfwd);
        }
        const float base = kBoss.vehDamage * ((pistol ? kBoss.pistolDamage : kSoldier.damage) / kSoldier.damage) * penMul;
        if (!have) {
            TheHost().HurtVehicle(resVeh, base); // no frame to zone by: the plain body price
        } else {
            const BossVehZone z = BossVehicleZone(resPoint, centre, vfwd, base);
            TheHost().HurtVehicle(resVeh, z.dmg);
            if (z.tyre)
                TheHost().VehicleShotEffect(resVeh, resPoint, 1);
        }
    }
}

// ---- his street navigation: the sight-line probe he already had (2.4 m ahead
// at chest height, detour at the first open angle), kept as the last-metres and
// off-graph reflex. Routes over the map (NavGraph) steer him between probes.
Vec3 SoldierAdvance(Mob& m, const Vec3& want, float dt) {
    if (m.steerTime > 0.0f) {
        m.steerTime -= dt;
        return m.steerDir;
    }
    const Vec3 from = m.pos + Vec3(0, 0, 1.1f);
    if (!TheHost().LineBlocked(from, from + want * 2.4f)) {
        m.steerSide = 0.0f; // the way ahead is clear: the old obstacle is done with
        return want;
    }
    static const float kTry[6] = { 0.61f, -0.61f, 1.22f, -1.22f, 1.92f, -1.92f }; // 35, 70, 110 degrees
    // a man going around a building commits to a side: while this obstacle
    // lasts, the side he first found open is tried first at every re-probe.
    // Re-picking from scratch used to slide him along the face one way, then
    // back the other - hugging walls for half a minute and rounding nothing.
    float order[6];
    int nTry = 0;
    if (m.steerSide != 0.0f) {
        for (float a : kTry)
            if ((a > 0.0f) == (m.steerSide > 0.0f))
                order[nTry++] = a;
        for (float a : kTry)
            if ((a > 0.0f) != (m.steerSide > 0.0f))
                order[nTry++] = a;
    } else {
        for (float a : kTry)
            order[nTry++] = a;
    }
    for (int i = 0; i < nTry; ++i) {
        const float a = order[i];
        const float c = std::cos(a), s = std::sin(a);
        const Vec3 cand(want.x * c - want.y * s, want.x * s + want.y * c, 0.0f);
        if (!TheHost().LineBlocked(from, from + cand * 2.4f)) {
            m.steerDir = cand;
            m.steerSide = a > 0.0f ? 1.0f : -1.0f;
            // a steep detour means a long obstacle (a wall, a building): commit
            // to it, or he hugs the face re-probing every half second and crawls
            m.steerTime = std::fabs(a) > 1.0f ? 1.1f : 0.6f;
            return cand;
        }
    }
    return want; // walled in on every side he can reach: he pushes on, and physics stops him
}

// ------------------------------------------------------------------ senses
// How much of a target his eyes honestly take in: a fraction from sampling the
// body at shin, chest and head - a man fully in the open is 1, a head-and-
// shoulders peek over a wall is a third, and a third still fills the meter,
// only slower. The cone is three-dimensional (roofs and jetpacks are in it),
// the range is the tune's, and night trades range for hearing (kBoss).
float BossVisibleFraction(Mob& m, const Vec3& feet, bool inVehicle, const Vec3& seat, int vehRef, bool* inCentre) {
    const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);
    const Vec3 mid = inVehicle ? seat : feet + Vec3(0, 0, 0.95f);
    Vec3 to = mid - eye;
    const float dist3 = to.Length();
    float range = kSoldier.sightRange;
    if (TheHost().Dark())
        range *= kBoss.nightRangeMul;
    if (inCentre)
        *inCentre = false;
    if (dist3 > range || dist3 < 0.01f)
        return 0.0f;
    const float horiz = std::sqrt(to.x * to.x + to.y * to.y);
    const Vec3 fwd(-std::sin(m.aimYaw), std::cos(m.aimYaw), 0.0f);
    const float dot = horiz > 0.01f ? (fwd.x * to.x + fwd.y * to.y) / horiz : 1.0f;
    const float angH = std::acos(Clamp(dot, -1.0f, 1.0f)) * 57.29578f;
    const float pitch = std::atan2(to.z, std::max(0.01f, horiz)) * 57.29578f;
    const bool pointBlank = dist3 < kSoldier.pointBlank;
    if (!pointBlank && (angH > kBoss.coneHalfDeg || pitch > kBoss.pitchUpDeg || pitch < -kBoss.pitchDownDeg))
        return 0.0f;
    if (inCentre)
        *inCentre = angH <= kBoss.centreHalfDeg;
    if (inVehicle)
        // a driver is a partial target by nature - and he is judged through
        // his own glass, not against his own bodywork: the plain line query
        // counts the target's car as a wall standing on the line, so a seated
        // man went invisible the moment he got in (the eye verdict of
        // 2026-11-07). This line runs with his vehicle ignored, the shot
        // trace's own trick.
        return TheHost().LineBlockedSkipVeh(eye, seat, vehRef) ? 0.0f : 0.8f;
    int clear = 0;
    const float hs[3] = { 0.25f, 0.95f, 1.62f };
    for (float h : hs)
        if (!TheHost().LineBlocked(eye, feet + Vec3(0, 0, h)))
            ++clear;
    return clear / 3.0f;
}


// The last stretch of any directed walk: aim his feet at one point through
// the probe (detours, side commitment), jumping what a man can jump. Shared
// by the calm walker and the combat navigator - the probe's whole job in the
// smart pack is micro-obstacles: cars, low walls, steps.
bool BossWalkToward(Mob& m, const Vec3& goal, float speedWanted, float dt) {
    Vec3 to = goal - m.pos;
    to.z = 0.0f;
    if (to.Length() < 0.05f) {
        m.moving = false;
        return false;
    }
    const Vec3 dir = Flat(to);
    // the jump: the road ahead is blocked low and open at head height - a fence,
    // a low wall, a car. A man jumps those; he does not detour around them.
    if (m.onGround && m.jumpCd <= 0.0f) {
        const Vec3 shin = m.pos + Vec3(0, 0, 0.35f);
        const Vec3 head = m.pos + Vec3(0, 0, 1.35f);
        if (TheHost().LineBlocked(shin, shin + dir * 1.7f) && !TheHost().LineBlocked(head, head + dir * 1.7f)) {
            m.vel.z = kBoss.jumpVel;
            m.onGround = false;
            m.airT = 0.0f;
            m.jumpCd = 0.9f;
        }
    }
    m.goal = SoldierAdvance(m, dir, dt);
    m.moving = true;
    m.moveSpeed = speedWanted;
    return true;
}

// Walk or run him to a destination: over the map when a route is worth having
// (recomputed when the destination moves or the route runs out), by probe for
// the last metres and off-graph ground, jumping what a man can jump. Sets his
// goal and speed through the usual fields and returns whether he is moving.
// This is the CALM walker (patrol, return, the talk-first approach): its
// repath policy is the loose one. Combat and the hunt use BossChaseTo below.
bool BossGoTo(Mob& m, const Vec3& dest, float speedWanted, float dt, float arriveDist) {
    if (m.staggerT > 0.0f)
        speedWanted *= 0.3f; // a staggered man stumbles, he does not travel
    Vec3 toDest = dest - m.pos;
    toDest.z = 0.0f;
    const float distDest = toDest.Length();
    if (distDest < arriveDist) {
        m.moving = false;
        return false;
    }
    Vec3 goal = dest;
    if (NavReady() && distDest > 6.0f) {
        m.routeT -= dt;
        if (m.route.empty() || m.routeIdx >= (int)m.route.size() || m.routeT <= 0.0f ||
            (m.routeGoal - dest).LengthSq() > 9.0f) {
            // the calm walker's route is budget-capped like the combat one:
            // the unbounded search could eat a whole frame on a far goal
            // (the spawn tick's 111 ms), and a route the budget cannot find
            // is simply retried in a second while the probe walks - the
            // budget covers every measured Los Santos route anyway
            if (NavRouteBudget(m.pos, dest, &m.route, kBoss.repathBudget, -1)) {
                m.routeIdx = 0;
                m.routeGoal = dest;
                m.routeT = 2.5f;
            } else {
                m.route.clear();
                m.routeT = 1.0f; // no route: probe-walk, and ask again in a second
            }
        }
        while (m.routeIdx < (int)m.route.size()) {
            Vec3 toWp = m.route[m.routeIdx] - m.pos;
            toWp.z = 0.0f;
            if (toWp.Length() < 2.2f)
                ++m.routeIdx;
            else {
                goal = m.route[m.routeIdx];
                break;
            }
        }
    }
    return BossWalkToward(m, goal, speedWanted, dt);
}

// ------------------------------------------------------------------ combat navigation (the smart pack)
// The shipped combat movement steered to the target's FEET through the calm
// walker: the route was rebuilt whenever the feet had moved 3 m - several
// times a second against a fleeing man - and every rebuild re-snapped its
// first waypoint to the graph node nearest the boss, which is as often the
// node BEHIND him as the one ahead, while at range the burst-pause rule
// planted him half of every second. The session of 2026-11-06 22:15 is the
// fossil: five seconds, 1.7 m travelled, the player gone from 27 to 54 m.
// This controller is the combat answer, and the hunt uses it too (one mover,
// no seam):
//   - it steers to an INTERCEPT point (the caller leads the target's tracked
//     velocity), not to live feet;
//   - repaths are throttled: at most one a second, and only when owed - the
//     point moved more than repathGoalMove, the route ran out, or a stall
//     broke - and every repath runs under the hard A* budget;
//   - a fresh route is pruned of leading waypoints that lie BEHIND him or
//     farther from the goal than he already is: the snap-back step is gone;
//   - waypoints are also passed when they fall behind his line to the goal,
//     so a slightly stale route never turns him around;
//   - a progress watchdog watches the ground he actually covers: no 1.2 m of
//     it in stuckSeconds while a move is wanted is a stall - the route is
//     dropped, a repath is forced at once, and if he stalls again at the
//     same spot the pocket's nearest node is banned from that repath.
// The probe keeps only micro-obstacles, through BossWalkToward.
bool BossChaseTo(Mob& m, const Vec3& dest, float speedWanted, float dt, float arriveDist) {
    if (m.staggerT > 0.0f)
        speedWanted *= 0.3f; // a staggered man stumbles, he does not travel
    else if (m.health < kBoss.health * 0.5f)
        speedWanted *= 0.92f; // decline, visible before the wounded threshold:
                              // a man under half health runs a step slower
    m.chaseDest = dest;
    Vec3 toDest = dest - m.pos;
    toDest.z = 0.0f;
    const float distDest = toDest.Length();
    if (distDest < arriveDist) {
        m.moving = false;
        m.navProgT = 0.0f;
        m.navProgPos = m.pos;
        return false;
    }
    m.repathT = std::max(0.0f, m.repathT - dt);
    bool moving = false;
    if (!NavReady() || distDest < 7.0f) {
        // off-graph ground or the last metres: the probe walks it alone
        // (its own detour commitment is the mechanism here; there is no
        // route to drop and no node to ban)
        moving = BossWalkToward(m, dest, speedWanted, dt);
    } else {
        const bool routeDone = m.route.empty() || m.routeIdx >= (int)m.route.size();
        const bool goalMoved = (m.routeGoal - dest).LengthSq() > kBoss.repathGoalMove * kBoss.repathGoalMove;
        bool stalled = false;
        if ((m.pos - m.navProgPos).Length() > 1.2f) {
            m.navProgPos = m.pos;
            m.navProgT = 0.0f;
            m.navStuckCount = 0; // real ground covered: the pocket is forgiven
        } else {
            m.navProgT += dt;
            stalled = m.navProgT > kBoss.stuckSeconds;
        }
        if ((routeDone || goalMoved || stalled) && (m.repathT <= 0.0f || routeDone || stalled)) {
            if (stalled) {
                // the stall break: count it against this spot, drop the route
                // that led into it, and let the probe carry him this tick
                const bool samePocket = (m.pos - m.navStuckPos).LengthSq() < 4.0f;
                m.navStuckCount = samePocket ? m.navStuckCount + 1 : 1;
                m.navStuckPos = m.pos;
                m.route.clear();
                m.routeIdx = 0;
                m.navProgPos = m.pos;
                m.navProgT = 0.0f;
            }
            const int avoid = m.navStuckCount >= 2 ? NavNearest(m.navStuckPos, 30.0f) : -1;
            std::vector<Vec3> fresh;
            if (NavRouteBudget(m.pos, dest, &fresh, kBoss.repathBudget, avoid)) {
                m.route = std::move(fresh);
                m.routeGoal = dest;
                m.routeIdx = 0;
                // prune the snap-back: leading waypoints behind him, or no
                // closer to the goal than he already stands, are already past
                const Vec3 toGoal = Flat(dest - m.pos);
                while (m.routeIdx < (int)m.route.size()) {
                    const Vec3 wp = m.route[m.routeIdx];
                    Vec3 toWp = wp - m.pos;
                    toWp.z = 0.0f;
                    const float wpGoal = (wp - dest).LengthSq();
                    if (toWp.Length() < 8.0f && toWp.Dot(toGoal) < 0.0f && wpGoal > distDest * distDest)
                        ++m.routeIdx;
                    else
                        break;
                }
            } else {
                m.route.clear(); // no route inside the budget: probe this stretch
                m.routeIdx = 0;
            }
            m.repathT = kBoss.repathMinT;
        }
        Vec3 goal = dest; // the off-graph tail: the last metres are walked direct
        bool onRoute = false;
        while (m.routeIdx < (int)m.route.size()) {
            const Vec3 wp = m.route[m.routeIdx];
            Vec3 toWp = wp - m.pos;
            toWp.z = 0.0f;
            const float d = toWp.Length();
            const Vec3 toGoal = Flat(dest - m.pos);
            if (d < 2.2f || (d < 5.0f && toWp.Dot(toGoal) < 0.0f)) {
                ++m.routeIdx; // reached, or fallen behind his line to the goal
                continue;
            }
            goal = wp;
            onRoute = true;
            break;
        }
        moving = BossWalkToward(m, goal, speedWanted, dt);
        if (!onRoute)
            m.routeIdx = (int)m.route.size(); // tail-walking: the route is spent
    }
    return moving;
}

// ------------------------------------------------------------------ the brain
// States: 0 calm (patrol/return), 1 command (talk first), 2 combat, 3 hunt,
// 4 suspicious (primed). The senses run in every state; the states decide what
// he does about them.
enum BossState { BS_CALM = 0, BS_COMMAND = 1, BS_COMBAT = 2, BS_HUNT = 3, BS_SUSPICIOUS = 4 };

static const char* BossStateName(int s) {
    switch (s) {
    case BS_COMMAND: return "command";
    case BS_COMBAT: return "combat";
    case BS_HUNT: return "hunt";
    case BS_SUSPICIOUS: return "suspicious";
    default: return "calm";
    }
}

// The point a hunt runs to: the last seen spot, led by the prey's tracked
// velocity for as long as the run itself will take (plus the time he has
// already been lost) - capped at six seconds of travel, so a stale velocity
// cannot throw the point across the city. The combat-to-hunt seam and every
// hunt renewal share this one point, so a hunt never restarts colder than
// the chase it continues. (The shipped lead was a flat 2.5 s off the live
// distance: against a man who kept running after breaking the line, the
// boss arrived at where the prey had been a breath after losing him -
// and then searched a twelve-metre disc around that stale spot.)
static Vec3 BossHuntDest(const Mob& m) {
    Vec3 vel = m.preyVel;
    if (std::sqrt(vel.x * vel.x + vel.y * vel.y) < 0.5f)
        vel = m.exitDir * 3.0f; // no tracked velocity: a man leaving at a jog, the way he was heading
    const float toSeen = (m.lastSeen - m.pos).Length();
    const float lead = Clamp(m.lostT + toSeen / kBoss.chaseSpeed, 0.0f, 6.0f);
    return m.lastSeen + vel * lead;
}

bool SoldierBrain(Mob& m, float dt, const Vec3& playerPos, float* speed, bool* looking,
                  const std::vector<BeingInfo>* beings) {
    const MobType& t = kTypes[m.kind];
    m.attackAnim = std::max(0.0f, m.attackAnim - dt * 2.5f);
    m.shootShow = std::max(0.0f, m.shootShow - dt);
    m.voiceCd = std::max(0.0f, m.voiceCd - dt);
    m.hurtSayT = std::max(0.0f, m.hurtSayT - dt);
    m.flinchT = std::max(0.0f, m.flinchT - dt);
    m.staggerT = std::max(0.0f, m.staggerT - dt);
    m.attackerFresh = std::max(0.0f, m.attackerFresh - dt);
    m.playerHostileT = std::max(0.0f, m.playerHostileT - dt);
    m.primedT = std::max(0.0f, m.primedT - dt);
    m.nadeCd = std::max(0.0f, m.nadeCd - dt);
    m.jumpCd = std::max(0.0f, m.jumpCd - dt);
    m.dodgeT = std::max(0.0f, m.dodgeT - dt);
    m.coverT = std::max(0.0f, m.coverT - dt);
    m.heardT = std::max(0.0f, m.heardT - dt);
    m.nadeShowT = std::max(0.0f, m.nadeShowT - dt);
    m.loudT = std::min(99.0f, m.loudT + dt);
    m.firePlantT = std::max(0.0f, m.firePlantT - dt);
    m.fallbackBanT = std::max(0.0f, m.fallbackBanT - dt);
    m.fightWarmT = std::max(0.0f, m.fightWarmT - dt);
    // Counter-Strike's recoil recovery: while the trigger rests, the spray
    // index walks back down (fast, the rifles' own habit) and a spray that
    // fully recovers is over - the next one sways on a fresh seed, the way
    // 1.6's semi-random pattern starts anew
    m.sprayIdle += dt;
    if (m.sprayIdle > 0.30f && m.sprayIndex > 0.0f) {
        m.sprayIndex = std::max(0.0f, m.sprayIndex - dt * 14.0f);
        if (m.sprayIndex <= 0.0f)
            m.spraySeed = 1 + (int)(Rand01() * 2000000000.0f);
    }
    m.reposT = std::max(0.0f, m.reposT - dt);
    // the settle clock: how long his feet have honestly been still - fire
    // control prices his spread off it (Counter-Strike's stop-shoot recovery)
    {
        const float hvNow = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
        m.settleT = hvNow < 0.6f ? m.settleT + dt : 0.0f;
    }
    m.moveSpeed = 0.0f;
    if (!m.onGround)
        m.airT += dt;
    if (m.alerted && m.bossState == BS_CALM && m.fightStarted)
        m.bossState = BS_SUSPICIOUS; // a hurt or a noise leaves a man who has
                                     // fought primed, not calm - but under the
                                     // engagement law an unprovoked stranger's
                                     // sight or sound alone never promotes him

    const bool playerAlive = gGame.deathTime < 0.0f;
    // The kill ends the fight - the law of this world: death resets only the
    // current pressure, the system itself never switches off. The moment the
    // player dies, a command, fight or hunt in progress stands down: he drops
    // to primed suspicion and walks back to his Grove anchor to watch, instead
    // of what the first build did - holding a live lock on a corpse and
    // replanning a cross-city A* route to the hospital bed every 2.5 s for the
    // whole 45 s hunt (a train of hitches on the respawn on a 2 GB Core 2 Duo),
    // until the distance sweep erased him at 320 m anyway and the "posted
    // boss" was simply gone when the player walked back.
    const bool wasAlive = m.sawPlayerAlive;
    if (wasAlive && !playerAlive)
        // diag marker: the death moment itself, so the session log can be
        // lined up with the hitch the eye reports at exactly this edge
        Log("boss diag: player death edge (boss in state %d)", m.bossState);
    if (!wasAlive && playerAlive)
        Log("boss diag: player respawn edge - alive again; the boss is on post, primed");
    if (wasAlive && !playerAlive) {
        // the engagement law: death pays EVERYTHING off - the respawned man
        // is a stranger again until he provokes him anew.
        m.bossState = BS_CALM;
        m.stateWhy = "the player died: the pressure resets, the slate is clean";
        m.fightStarted = false;
        m.aggro = false;
        m.aimAggroT = 0.0f;
        m.playerHostileT = 0.0f;
        m.detect = 0.0f;
        m.primedT = 60.0f; // a veteran's sharpness only: it can speed a NEW
                           // fight's detection, it cannot start one
        m.cmdPhase = 0;
        m.cmdT = 0.0f;
        m.huntT = 0.0f;
        m.huntPhase = 0;
        m.searchPts.clear();
        m.searchIdx = 0;
        m.searchTime = 0.0f;
        m.route.clear();
        m.routeIdx = 0;
        m.routeT = 0.0f;
        m.repathT = 0.0f;
        m.navProgT = 0.0f;
        m.navStuckCount = 0;
        m.reposT = 0.0f;
        m.firePlantT = 0.0f;
        m.hasFallback = false;
        m.tgtLockBeing = -2;
        m.tgtLockT = 0.0f;
        m.lostT = 0.0f;
        m.targetVisT = 0.0f;
        m.fightWarmT = 0.0f; // death pays the fight off in full: no warmth survives it
    }
    m.sawPlayerAlive = playerAlive;
    const int playerVeh = TheHost().PlayerVehicle();
    const bool inVeh = playerAlive && playerVeh >= 0;
    Vec3 vehCentre, vehFwd, vehSeat, vehVel;
    const bool vehPose = inVeh && TheHost().PlayerVehiclePose(&vehCentre, &vehFwd, &vehSeat, &vehVel);
    const Vec3 playerFeet = inVeh && vehPose ? vehCentre : playerPos - Vec3(0, 0, 1.0f);
    const Vec3 playerVel = TheHost().PlayerVelocity();
    const float playerSpeed = std::sqrt(playerVel.x * playerVel.x + playerVel.y * playerVel.y);

    // ---------------- the senses, for the player
    bool inCentre = false;
    float frac = 0.0f;
    if (playerAlive)
        frac = BossVisibleFraction(m, playerFeet, inVeh && vehPose, vehSeat, playerVeh, &inCentre);
    const bool seeNow = frac > 0.0f;
    const Vec3 eye = m.pos + Vec3(0, 0, MobHeight(m) * 0.85f);
    Vec3 toPlayer = playerFeet - m.pos;
    toPlayer.z = 0.0f;
    const float distPlayer = toPlayer.Length();
    // being aimed at fills the meter fast, the way the game's own men react
    bool aimedAt = false;
    if (playerAlive && distPlayer < kSoldier.sightRange) {
        const Vec3 fwd(-std::sin(m.aimYaw), std::cos(m.aimYaw), 0.0f);
        const float dot = distPlayer > 0.01f ? (fwd.x * toPlayer.x + fwd.y * toPlayer.y) / distPlayer : 1.0f;
        if (dot > -0.09f) // inside a wide 190-degree... no: inside the cone's shoulders (95 deg half)
            aimedAt = TheHost().PlayerAimingAt(m.pos + Vec3(0, 0, 1.2f), 1.3f);
    }
    // ---------------- the engagement law: being aimed at is a provocation
    // A weapon pointed at him - from any side, at any range the game's own
    // aiming flag can see - held for a breath (a passing crosshair sweep
    // does not count) makes the player his enemy. Checked directly here,
    // not through the facing-gated aimedAt above: the provocation is the
    // player's act, not the boss's field of view.
    if (!m.aggro && playerAlive && distPlayer < 120.0f &&
        TheHost().PlayerAimingAt(m.pos + Vec3(0, 0, 1.2f), 1.3f)) {
        m.aimAggroT += dt;
        if (m.aimAggroT >= kBoss.aimAggroSeconds) {
            m.aggro = true;
            m.fightStarted = true;
            m.negotiated = true;
            m.detect = 1.0f;
            m.playerHostileT = 6.0f;
            m.fightWarmT = kBoss.warmSeconds;
            m.lastSeen = playerFeet;
            m.exitDir = Flat(playerVel);
            if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) {
                m.bossState = BS_COMBAT;
                m.stateWhy = "a weapon held on him: the player is an enemy now";
                m.seenTime = 0.0f;
                BossSay(m, SAY_SPOTTED, 2.0f);
            }
        }
    } else {
        m.aimAggroT = std::max(0.0f, m.aimAggroT - dt * 2.0f);
    }
    const float primed = m.primedT > 0.0f ? 2.0f : 1.0f;
    if (seeNow) {
        float rate = kBoss.fillFar + (kBoss.fillClose - kBoss.fillFar) *
                     (1.0f - Clamp(distPlayer / kSoldier.sightRange, 0.0f, 1.0f));
        rate *= inCentre ? kBoss.fillCentreMul : kBoss.fillEdgeMul;
        if (playerSpeed > 2.5f)
            rate *= kBoss.fillMoveMul;
        m.detect = std::min(1.5f, m.detect + frac * rate * primed * dt);
    } else if (!aimedAt) {
        m.detect = std::max(0.0f, m.detect - kBoss.fillDecay * dt);
    }
    if (aimedAt) // a gun pointed at him is fast news on top of everything else
        m.detect = std::min(1.5f, m.detect + kBoss.fillAimMul * primed * dt);
    if (!m.aggro)
        m.detect = 0.0f; // the engagement law: an unprovoked player is scenery -
                         // his eyes may track the man, his meter never fills
    const bool detected = m.detect >= 1.0f;
    // tracking while visible: last seen, his velocity, the hiding-spot memory
    if (seeNow) {
        const Vec3 newSeen = playerFeet;
        if (m.targetVisT > 0.0f && dt > 0.0f) {
            const Vec3 v = (newSeen - m.lastSeen) * (1.0f / dt);
            m.preyVel = m.preyVel * 0.6f + v * 0.4f;
        }
        if (std::sqrt(m.preyVel.x * m.preyVel.x + m.preyVel.y * m.preyVel.y) > 1.0f)
            m.exitDir = Flat(m.preyVel); // the way he is leaving, kept current while he is seen
        m.lastSeen = newSeen;
        m.targetVisT = std::min(10.0f, m.targetVisT + dt);
        m.lostT = 0.0f;
        m.searchTime = kSoldier.searchSeconds;
        m.alerted = true;
        m.alertFade = 0.0f;
    } else {
        if (m.targetVisT > 0.3f && m.bossState >= BS_COMBAT) {
            NoteHidingSpot(m.lastSeen); // he broke the line cleanly: remember the spot
            // ...and if this break is at a NEW place, far from the last one,
            // the man is repositioning from corner to corner: the grenade's
            // clock is allowed to run short for him while the move is fresh
            if ((m.lastSeen - m.reposAt).LengthSq() > 25.0f) {
                m.reposAt = m.lastSeen;
                m.reposT = 6.0f;
            }
        }
        m.targetVisT = 0.0f;
        // a man holding good cover over a hidden target waits a beat before he
        // pushes - and that beat is exactly the grenade's window (2.5 s), so
        // the hunt timer runs slow while he holds cover in grenade range
        const float lastSeenDist = (m.lastSeen - m.pos).Length();
        const bool holdingForNade = m.bossState == BS_COMBAT && lastSeenDist > kBoss.nadeMin && lastSeenDist < kBoss.nadeMax;
        m.lostT += dt * (holdingForNade ? 0.25f : 1.0f);
        m.searchTime = std::max(0.0f, m.searchTime - dt);
    }
    // ---------------- hearing
    float hearMul = TheHost().Dark() ? kBoss.nightHearMul : 1.0f;
    bool heardSomething = false;
    for (const Stimulus& s : gStimuli) {
        Vec3 d = s.at - m.pos;
        d.z = 0.0f;
        const float dd = d.Length();
        float r = s.radius * hearMul;
        if (TheHost().LineBlocked(eye, s.at))
            r *= 0.45f; // walls muffle
        if (dd < r) {
            heardSomething = true;
            if (s.radius >= 40.0f)
                m.loudT = 0.0f; // a shot or a blast, just now: a fight may already be on
            m.heardDir = Flat(d);
            m.heardT = 2.5f;
            m.alerted = true;
            m.alertFade = 0.0f;
            if (m.bossState == BS_CALM && m.fightStarted) {
                // a noise makes a man who has already fought look up; a man
                // at peace does not go investigating strangers (the law)
                m.bossState = BS_SUSPICIOUS;
                m.detect = std::max(m.detect, 0.3f);
            }
            if ((m.bossState == BS_COMBAT || m.bossState == BS_HUNT) && s.radius >= 40.0f) {
                // a shot or a blast while he hunts: he checks where it came from
                m.lastSeen = s.at;
                m.searchTime = std::max(m.searchTime, 3.0f);
                if (m.bossState == BS_HUNT && m.huntPhase == 0) {
                    // fresher news than the point he is running to: re-project
                    // the intercept on it now, not on arrival at the old one
                    m.huntDest = BossHuntDest(m);
                    m.route.clear();
                }
            }
        }
    }
    // his own ears on the player's movement and engine, no stimulus list needed
    if (playerAlive && m.bossState != BS_COMBAT) {
        float noiseR = 0.0f;
        if (inVeh) {
            const float sp = vehPose ? std::sqrt(vehVel.x * vehVel.x + vehVel.y * vehVel.y) : playerSpeed;
            if (sp > 2.0f)
                noiseR = kBoss.hearVehicle * (0.7f + std::min(1.0f, sp / 14.0f));
        } else if (playerSpeed > 4.2f)
            noiseR = kBoss.hearSprint;
        else if (playerSpeed > 2.2f)
            noiseR = kBoss.hearRun;
        else if (playerSpeed > 1.2f)
            noiseR = kBoss.hearWalk; // zero: CS's law - a creeping man makes no sound
        if (noiseR > 0.0f) {
            float r = noiseR * hearMul;
            const bool muffled = inVeh ? TheHost().LineBlockedSkipVeh(eye, playerPos, playerVeh)
                                       : TheHost().LineBlocked(eye, playerPos);
            if (muffled)
                r *= 0.45f; // walls muffle an engine - the driver's own car does not (the eyes' law)
            if (distPlayer < r) {
                heardSomething = true;
                m.heardDir = Flat(toPlayer);
                m.heardT = std::max(m.heardT, 1.2f);
                m.alerted = true;
                m.alertFade = 0.0f;
                if (m.fightStarted) {
                    m.detect = std::max(m.detect, 0.3f);
                    if (m.bossState == BS_CALM)
                        m.bossState = BS_SUSPICIOUS;
                }
            }
        }
    }
    // ---------------- being attacked by the street (K1) refreshes his target
    const BeingInfo* attackerInfo = nullptr;
    if (m.attackerBeing >= 0 && beings) {
        for (const BeingInfo& b : *beings)
            if (b.being == m.attackerBeing) {
                attackerInfo = &b;
                break;
            }
        if (!attackerInfo && m.attackerFresh <= 0.0f)
            m.attackerBeing = -1;
    }
    // ---------------- state entries from the senses
    if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) {
        if (m.attackerFresh > 0.0f && m.attackerBeing >= 0) {
            m.bossState = BS_COMBAT;
            m.stateWhy = "the street attacked him";
            m.negotiated = true; // a street fight is not a negotiation
            m.fightStarted = true;
            BossSay(m, SAY_SPOTTED, 2.0f);
        } else if (detected && playerAlive) {
            const bool peaceful = m.playerHostileT <= 0.0f && !aimedAt && playerSpeed < 4.2f;
            // workstream G - SUPERSEDED for the free-roam boss by the user's
            // order of 2026-11-07: no warnings, no commands, nothing to do
            // with the player at all until provoked. Commands survive only
            // behind kGuardTalkEnabled (off) for the future checkpoint step,
            // whose posted guard is a different context with its own orders.
            const bool guardContext = kGuardTalkEnabled && !m.fightStarted && m.loudT > 3.0f;
            if (peaceful && !m.negotiated && guardContext) {
                m.bossState = BS_COMMAND;
                m.stateWhy = "a guard contact: he talks first";
                m.cmdPhase = 0;
                m.cmdT = kBoss.commandWindow;
                m.holdT = 0.0f;
                m.negotiated = true;
                BossSay(m, SAY_COMMAND, 2.0f);
            } else if (peaceful && m.negotiated && m.primedT > 0.0f && m.bossState == BS_SUSPICIOUS) {
                // the talk already happened and he is still primed from it: a
                // peaceful man is watched at aim, not fought and not re-ordered
                m.aimGoal = playerPos;
                m.aimRate = 8.0f;
            } else {
                m.bossState = BS_COMBAT;
                m.stateWhy = "spotted: the fight is on";
                m.fightStarted = true;
                m.seenTime = 0.0f;
                BossSay(m, SAY_SPOTTED, 2.0f);
            }
        }
    }
    if (m.bossState == BS_SUSPICIOUS && m.primedT <= 0.0f && m.detect <= 0.05f && !heardSomething) {
        m.bossState = BS_CALM;
        m.stateWhy = "all quiet: back to a calm watch";
        m.negotiated = false; // a fresh encounter, a fresh chance to talk
        m.fightStarted = false;
        m.alerted = false;
    }
    // ---------------- pick the fighting target (player vs street attacker)
    // The choice runs on a lock (the smart pack): re-evaluated every third of
    // a second at the slowest, so two flickering provocations - the player
    // lowering his gun for a frame, a gang banger's freshness blinking - can
    // never flap his aim between two men ten times a second. A locked man
    // who leaves the street is dropped the same tick: the lock is trigger
    // discipline, not blindness.
    BossTarget tgt;
    const bool playerFighting = m.playerHostileT > 0.0f;
    if (m.bossState == BS_COMBAT || m.bossState == BS_HUNT) {
        const int wantBeing = (attackerInfo && m.attackerFresh > 0.0f && !playerFighting) ? m.attackerBeing : -1;
        m.tgtLockT -= dt;
        if (m.tgtLockBeing == -2 || m.tgtLockT <= 0.0f) {
            m.tgtLockBeing = wantBeing;
            m.tgtLockT = 0.35f;
        } else if (m.tgtLockBeing >= 0 && (!attackerInfo || m.attackerBeing != m.tgtLockBeing)) {
            m.tgtLockBeing = wantBeing; // the man he held is gone: re-choose now
            m.tgtLockT = 0.35f;
        }
        const bool useBeing = m.tgtLockBeing >= 0 && attackerInfo && m.attackerBeing == m.tgtLockBeing &&
                              m.attackerFresh > 0.0f;
        if (useBeing) {
            tgt.valid = true;
            tgt.being = m.attackerBeing;
            tgt.feet = attackerInfo->pos - Vec3(0, 0, 1.0f);
            tgt.mid = attackerInfo->pos;
            Vec3 d = tgt.feet - m.pos;
            d.z = 0.0f;
            tgt.dist = d.Length();
        } else if (playerAlive) {
            tgt.valid = true;
            tgt.being = -1;
            tgt.feet = playerFeet;
            tgt.mid = inVeh && vehPose ? vehSeat : playerPos;
            tgt.dist = distPlayer;
            tgt.inVehicle = inVeh && vehPose;
            tgt.vehCentre = vehCentre;
            tgt.vehFwd = vehFwd;
            tgt.vehSeat = vehSeat;
            tgt.vehVel = vehVel;
        }
    }
    const bool tgtVisible = tgt.valid && (tgt.being >= 0
        ? !TheHost().LineBlocked(eye, tgt.mid + Vec3(0, 0, 0.2f)) && tgt.dist < kSoldier.sightRange
        : seeNow);
    if (tgt.being >= 0 && tgtVisible) {
        m.lastSeen = tgt.feet;
        m.lostT = 0.0f;
    }
    m.huntValid = tgt.valid && (m.bossState == BS_COMBAT || m.bossState == BS_HUNT);
    m.huntBeing = tgt.valid ? tgt.being : -1;
    *looking = tgtVisible || m.bossState == BS_COMMAND;
    m.aiTimer = 0.5f;

    // ---------------- the reload machinery ticks in every state (the rifle
    // does not pause because he is walking); firing lives in combat below.
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
            m.pistolOut = false;
            m.burstShots = 0;
            m.burstPause = 0.15f;
        }
    }
    auto startReload = [&]() {
        m.reloadT = kSoldier.reloadTime;
        m.reloadStage = 1;
        const Vec3 c = m.pos + Vec3(0, 0, 1.2f);
        PlaySfx(SND_M4A1_CLIPOUT, &c, 1.2f);
        BossSay(m, SAY_RELOAD, 4.0f);
    };
    // ---------------- dodge: an incoming projectile or a vehicle's charge
    // (the charge itself arrives through MobVehicleThreat, which sets dodgeT)
    if (m.dodgeT <= 0.0f && (m.bossState == BS_COMBAT || m.bossState == BS_HUNT)) {
        for (const Projectile& pr : gProjectiles) {
            if (pr.hostile || pr.stuck)
                continue;
            const float pv = pr.vel.Length();
            if (pv < 5.0f)
                continue;
            const Vec3 rel = m.pos + Vec3(0, 0, 1.0f) - pr.pos;
            const Vec3 vh = pr.vel * (1.0f / pv);
            const float tca = rel.Dot(vh);
            if (tca < 0.0f || tca / pv > 0.55f)
                continue;
            const Vec3 closest = rel - vh * tca;
            if (closest.Length() < 1.3f) {
                Vec3 side(vh.y, -vh.x, 0.0f);
                if (side.Dot(closest) > 0.0f)
                    side = side * -1.0f;
                m.dodgeDir = side;
                m.dodgeT = 0.34f;
                break;
            }
        }
    }
    const bool dodging = m.dodgeT > 0.0f;
    if (dodging) {
        m.goal = m.dodgeDir;
        m.moving = true;
        m.moveSpeed = kBoss.chaseSpeed;
    }

    // ================================================================ COMBAT
    if (m.bossState == BS_COMBAT) {
        m.seenTime = tgtVisible ? m.seenTime + dt : std::max(0.0f, m.seenTime - dt * 2.0f);
        // a live firing solution on the player - or the player fighting him
        // back - keeps the encounter warm. This memory is what the hunt's
        // stand-down answers to: senses alone call a quiet man at ninety
        // metres "cold" seconds after a firefight, and the law is that he is
        // hunted, not forgotten, while the fight is still warm.
        if (tgt.valid && tgt.being < 0 && (tgtVisible || m.playerHostileT > 0.0f))
            m.fightWarmT = kBoss.warmSeconds;
        const float healthShare = m.health / kBoss.health;
        const bool wounded = healthShare < kBoss.woundedShare;
        // cover: re-judged twice a second - a point is cover when the threat's
        // line to it is blocked and a half-step sideways from it can shoot
        bool wantCover = false, inCoverNow = false, peekFire = false;
        if (tgt.valid && !dodging) {
            const bool exposed = !TheHost().LineBlocked(tgt.mid, eye);
            wantCover = exposed && (m.reloadT > 0.0f || wounded || m.burstPause > 0.05f) && tgt.dist < 34.0f;
            if (wantCover && m.coverT <= 0.0f) {
                m.coverT = 0.5f;
                bool found = false;
                for (float r : { 2.2f, 3.4f }) {
                    for (int k = 0; k < 12 && !found; ++k) {
                        const float an = (float)k * 0.5236f;
                        const Vec3 cand = m.pos + Vec3(std::cos(an) * r, std::sin(an) * r, 0.0f);
                        float gz = 0.0f;
                        if (!TheHost().GroundBelow(cand + Vec3(0, 0, 30.0f), 40.0f, &gz) ||
                            std::fabs(gz - m.pos.z) > 1.3f)
                            continue;
                        const Vec3 cp = Vec3(cand.x, cand.y, gz);
                        if (!TheHost().LineBlocked(tgt.mid, cp + Vec3(0, 0, 1.2f)))
                            continue; // not hidden: not cover
                        const Vec3 sidev(tgt.mid.y - cp.y, -(tgt.mid.x - cp.x), 0.0f);
                        const Vec3 side = Flat(sidev);
                        for (float sgn : { 1.0f, -1.0f }) {
                            const Vec3 peek = cp + side * (0.85f * sgn);
                            if (!TheHost().LineBlocked(peek + Vec3(0, 0, 1.2f), tgt.mid)) {
                                m.coverPoint = cp;
                                m.peekPoint = peek;
                                m.hasCover = true;
                                found = true;
                                break;
                            }
                        }
                    }
                    if (found)
                        break;
                }
                if (!found)
                    m.hasCover = false;
            }
            if (wantCover && m.hasCover) {
                inCoverNow = (m.pos - m.coverPoint).LengthSq() < 1.2f;
                if (m.peekPhase) {
                    if (!BossChaseTo(m, m.peekPoint, kSoldier.strafeSpeed, dt, 0.45f))
                        m.peekT -= dt;
                    peekFire = true;
                    if (m.peekT <= 0.0f) {
                        m.peekPhase = false;
                        m.hideT = wounded ? 1.7f : 0.8f + Rand01() * 0.9f;
                    }
                } else {
                    BossChaseTo(m, m.coverPoint, kBoss.chaseSpeed * 0.8f, dt, 0.45f);
                    m.hideT -= dt;
                    if (m.hideT <= 0.0f && inCoverNow) {
                        m.peekPhase = true;
                        // no two peeks from the same hand's breadth: a man who
                        // leans out of cover varies where he leans (the point
                        // is re-judged twice a second anyway; this is the
                        // human wobble on top of it)
                        m.peekPoint = m.peekPoint + Vec3((Rand01() - 0.5f) * 0.3f, (Rand01() - 0.5f) * 0.3f, 0.0f);
                        m.peekT = (kBoss.peekMin + Rand01() * (kBoss.peekMax - kBoss.peekMin)) *
                                  (wounded ? 0.6f : 1.0f);
                    }
                }
            }
        }
        // open-field movement when cover is not the play. The smart pack
        // steers all of it through the combat navigator (BossChaseTo): the
        // advance runs to an INTERCEPT point - the target's tracked velocity
        // led by range, the hunt's own arithmetic, so combat and hunt read as
        // one pursuit - and a wounded man's retreat is a routed walk to a
        // fallback, never a blind backward shove into whatever stands behind
        // him. While the target is not visible the intercept is built from
        // lastSeen alone: steering to a hidden man's live feet would be wall
        // knowledge, and this is also what hands the chase to the hunt with
        // no seam - the hunt starts from the very point he is running to.
        if (!dodging && tgt.valid && !(wantCover && m.hasCover)) {
            const Vec3 dir = Flat(tgt.feet - m.pos);
            const float seenDist = tgtVisible ? tgt.dist : (m.lastSeen - m.pos).Length();
            const float lead = Clamp(seenDist / 6.0f, 0.0f, 2.0f);
            const Vec3 chasePoint = tgt.being >= 0
                                        ? (tgtVisible ? tgt.feet : m.lastSeen)
                                        : (tgtVisible ? tgt.feet : m.lastSeen) + m.preyVel * lead;
            if (wounded && (tgt.dist < 30.0f || m.hasFallback)) {
                // the fallback is chosen ONCE and kept until he stands on it:
                // his cover point when he has one, else a point a street's
                // length behind him, snapped onto the graph when the map
                // offers a node near it - routed, not wandered. Keeping it
                // matters: the combat-to-hunt flip is gated on hasFallback,
                // so losing sight mid-retreat cannot turn him around into
                // the threat's street halfway - he finishes the fall-back,
                // THEN the hunt may have him. A fallback his own stuck
                // watchdog condemns three times is abandoned for a while.
                if (m.hasFallback && m.navStuckCount >= 3) {
                    m.hasFallback = false;
                    m.fallbackBanT = 6.0f;
                }
                if (!m.hasFallback && m.fallbackBanT <= 0.0f) {
                    if (m.hasCover) {
                        m.fallbackPoint = m.coverPoint;
                    } else {
                        Vec3 fb = m.pos - dir * 14.0f;
                        fb.z = m.pos.z;
                        if (NavReady()) {
                            const int n = NavNearest(fb, 25.0f);
                            Vec3 np;
                            if (n >= 0 && NavNodePos(n, &np))
                                fb = np;
                        }
                        m.fallbackPoint = fb;
                    }
                    m.hasFallback = true;
                }
                if (!m.hasFallback ||
                    !BossChaseTo(m, m.fallbackPoint, kBoss.chaseSpeed * 0.8f, dt, 1.2f)) {
                    // arrived at the fallback (or it was abandoned): he holds
                    // the ground and drifts sideways like a man covering his
                    // own retreat, never a statue; the hunt gate opens here
                    m.hasFallback = false;
                    m.goal = Vec3(dir.y, -dir.x, 0.0f) * m.strafeDir;
                    m.moving = true;
                    m.moveSpeed = kSoldier.strafeSpeed;
                }
            } else if (tgt.dist > 13.0f) {
                if (((wounded && tgtVisible) ||
                     (tgt.dist >= kBoss.crouchRange && (m.firePlantT > 0.0f || m.burstShots > 0))) &&
                    m.groundBad == 0) {
                    // planted: a wounded man holds his long ground, and a sound
                    // one plants only while a burst is actually leaving (fire
                    // control sets firePlantT when a burst opens at range) -
                    // the pause between bursts is movement again; planting for
                    // the whole pause was half of the 22:15 stall. And never
                    // planted on ground his own sampler just distrusted: a man
                    // does not stop to shoot while the street drops away
                    // under him (the ground-hold's three samples would run
                    // out and take him down the hole he was crossing)
                    m.goal = Vec3(0, 0, 0);
                    m.moving = false;
                    m.moveSpeed = 0.0f;
                } else {
                    BossChaseTo(m, chasePoint, kBoss.chaseSpeed, dt, 11.0f);
                }
            } else if (tgt.dist < 6.0f) {
                m.goal = SoldierAdvance(m, dir * -1.0f, dt);
                m.moving = true;
                m.moveSpeed = kSoldier.strafeSpeed;
            } else {
                m.strafeTime -= dt;
                if (m.strafeTime <= 0.0f) {
                    m.strafeTime = 1.6f + Rand01() * 1.6f;
                    m.strafeDir = -m.strafeDir;
                }
                const Vec3 side(dir.y, -dir.x, 0.0f);
                m.goal = side * m.strafeDir;
                m.moving = true;
                m.moveSpeed = kSoldier.strafeSpeed;
            }
        }
        // ---- fire control
        m.crouch = tgt.valid && tgt.dist >= kBoss.crouchRange && !m.moving;
        const bool mayFire = tgt.valid && m.seenTime >= kSoldier.reaction &&
                             (tgtVisible || (tgt.being < 0 && m.lostT < kBoss.suppWindow));
        if (m.reloadT <= 0.0f && mayFire && (!wantCover || !m.hasCover || peekFire)) {
            if (m.burstPause > 0.0f) {
                m.burstPause -= dt;
            } else {
                if (m.burstShots <= 0) {
                    if (!m.pistolOut && m.mag <= 0) {
                        if (tgt.dist < kBoss.pistolRange && m.pistolMag > 0) {
                            m.pistolOut = true; // the USP, not a reload, at arm's length
                        } else {
                            // the rules say reload in cover or at range; when
                            // neither offers, he reloads on the move - the
                            // movement code never leaves him standing for it
                            startReload();
                        }
                    }
                    if (m.burstShots <= 0 && m.reloadT <= 0.0f) {
                        const int avail = m.pistolOut ? m.pistolMag : m.mag;
                        if (avail > 0) {
                            // burst discipline by range, the way the game itself
                            // is played: committed sprays up close, the classic
                            // three-to-four in the middle, single taps at range
                            int bMin, bMax;
                            if (tgt.dist < 12.0f) { bMin = 4; bMax = 5; }
                            else if (tgt.dist <= 30.0f) { bMin = 3; bMax = 4; }
                            else { bMin = 1; bMax = 2; }
                            m.burstSize = m.burstShots = std::min(avail, bMin + McsaRand() % (bMax - bMin + 1));
                            m.shotTimer = 0.0f;
                            if (tgt.dist >= kBoss.crouchRange)
                                m.firePlantT = (float)m.burstShots *
                                                   (m.pistolOut ? kBoss.pistolInterval : kSoldier.shotInterval) +
                                               0.3f; // planted for the burst and a breath - not for the pause after it
                        } else if (m.pistolOut && m.pistolMag <= 0) {
                            m.pistolOut = false;
                            startReload();
                        }
                    }
                }
                if (m.burstShots > 0 && m.staggerT <= 0.0f) {
                    // a staggered man does not fire: the burst waits out the
                    // rock-back, then resumes where it left off
                    m.shotTimer -= dt;
                    if (m.shotTimer <= 0.0f) {
                        m.shotTimer = m.pistolOut ? kBoss.pistolInterval : kSoldier.shotInterval;
                        if (tgtVisible)
                            BossShoot(m, tgt, m.pistolOut);
                        else { // suppressive fire on the corner he vanished behind:
                               // real noise and real tracers at the corner, and it
                               // can only hurt a man who steps back into the line
                            const Vec3 corner = m.lastSeen + Vec3(0, 0, 1.0f);
                            BossShoot(m, tgt, m.pistolOut, &corner, true);
                        }
                        --m.burstShots;
                        if (m.pistolOut)
                            --m.pistolMag;
                        else
                            --m.mag;
                        if (m.burstShots <= 0)
                            m.burstPause = (kSoldier.burstPauseMin + Rand01() * (kSoldier.burstPauseMax - kSoldier.burstPauseMin)) *
                                           (wounded ? 1.3f : 1.0f) * (tgt.dist > 30.0f ? 1.25f : 1.0f);
                    }
                }
            }
        } else {
            m.burstPause = std::max(0.0f, m.burstPause - dt);
            m.shotTimer = 0.0f;
        }
        // ---- the grenade: a target dug in behind cover gets flushed out. The
        // clock is the TARGET's time unreachable - leaky, because his own
        // peek-fire glimpses do not un-dig the man (coverHeldT is that clock)
        if (tgt.valid && tgt.being < 0 && !tgtVisible && m.lostT > 0.2f)
            m.coverHeldT += dt;
        else
            m.coverHeldT = std::max(0.0f, m.coverHeldT - dt * 0.5f);
        // ...and a man who keeps REPOSITIONING - seen a breath ago at a new
        // corner, gone again - is flushed on half the clock while the move is
        // fresh (reposT, set where his line last broke at a new place): a
        // professional does not wait for a mover to settle into his new cover
        const bool flushDue = m.coverHeldT > kBoss.nadeHeldTime ||
                              (m.reposT > 0.0f && m.coverHeldT > kBoss.nadeHeldTime * 0.5f);
        if (tgt.valid && tgt.being < 0 && flushDue && m.nades > 0 && m.nadeCd <= 0.0f) {
            Vec3 d = m.lastSeen - m.pos;
            const float dd = std::sqrt(d.x * d.x + d.y * d.y);
            if (dd > kBoss.nadeMin && dd < kBoss.nadeMax) {
                const float T = Clamp(dd / 11.0f, 0.7f, 1.6f);
                BossNade n;
                n.pos = m.pos + Vec3(0, 0, 1.6f);
                n.vel = Vec3(d.x / T, d.y / T, (d.z + 16.0f * T * T) / T);
                n.fuse = kBoss.nadeFuse;
                gBossNades.push_back(n);
                m.nadeShowT = 0.6f; // diag: the pose log's weapon field reads HE while this runs
                --m.nades;
                m.nadeCd = 7.0f;
                m.coverHeldT = 0.0f;
                m.reposT = 0.0f; // one reposition buys one early grenade, not two
                BossSay(m, SAY_GRENADE, 3.0f);
            }
        }
        // ---- losing him starts the hunt (the player only; a street attacker
        // who runs off is let go - the street is not his war until it shoots).
        // One gate: a wounded man mid-retreat finishes his fall-back first -
        // the hunt takes him from his fallback, never halfway, turned around.
        if (tgt.valid && tgt.being < 0 && !tgtVisible && m.lostT > 1.2f && gGame.deathTime < 0.0f &&
            !(wounded && m.hasFallback)) {
            m.bossState = BS_HUNT;
            m.stateWhy = "lost him: the hunt takes the chase";
            m.huntPhase = 0;
            m.huntT = 0.0f;
            m.huntDest = BossHuntDest(m);
            m.route.clear();
            BossSay(m, SAY_LOST, 3.0f);
        }
        if (tgt.being >= 0 && m.attackerFresh <= 0.0f && !attackerInfo) {
            // the street fight is over: back to watching for the real enemy
            m.bossState = BS_SUSPICIOUS;
            m.stateWhy = "the street fight is over";
            m.primedT = std::max(m.primedT, 20.0f);
            m.hasCover = false;
        }
        if (!playerAlive && tgt.being < 0) {
            m.bossState = BS_SUSPICIOUS;
            m.primedT = std::max(m.primedT, 30.0f);
            m.detect = 0.3f;
        }
    }

    // ================================================================ COMMAND
    if (m.bossState == BS_COMMAND) {
        m.aimGoal = playerPos;
        m.aimRate = 8.0f;
        bool violation = m.playerHostileT > 0.0f || playerSpeed > 4.2f;
        if (aimedAt) {
            m.aimViolT += dt;
            if (m.aimViolT > 0.35f)
                violation = true;
        } else {
            m.aimViolT = 0.0f;
        }
        if (violation) {
            m.bossState = BS_COMBAT;
            m.stateWhy = "he did not obey the order";
            m.fightStarted = true;
            m.seenTime = 0.0f;
            BossSay(m, SAY_SPOTTED, 2.0f);
        } else if (m.cmdPhase == 0) {
            // the window: a man who stands still and keeps his gun down is heard out
            if (playerSpeed < 0.9f) {
                m.cmdT -= dt;
                if (m.cmdT <= 0.0f)
                    m.cmdPhase = 1;
            } else if (distPlayer > 16.0f) {
                // walking away, slowly, is not war: he lets him go, primed
                m.bossState = BS_SUSPICIOUS;
                m.stateWhy = "he walked off inside the window: let go, primed";
                m.primedT = std::max(m.primedT, 30.0f);
                m.detect = 0.3f;
            }
        } else if (m.cmdPhase == 1) {
            if (!BossGoTo(m, playerFeet, t.walk, dt, kBoss.commandRange)) {
                m.cmdPhase = 2;
                m.holdT = 2.0f;
            }
            m.aimGoal = playerPos;
        } else {
            m.holdT -= dt;
            if (m.holdT <= 0.0f) {
                // stood down: he lowers the rifle but not his guard - and this
                // encounter has had its one talk (no re-commanding a man who
                // already obeyed; the primed watch above is what he gets)
                m.bossState = BS_SUSPICIOUS;
                m.stateWhy = "he obeyed: stood down";
                m.negotiated = true;
                m.primedT = kBoss.primedSeconds;
                m.detect = 0.35f;
                m.alerted = true;
                BossSay(m, SAY_STANDDOWN, 3.0f);
            }
        }
        if (!playerAlive) {
            m.bossState = BS_CALM;
            m.detect = 0.0f;
        }
    }

    // ================================================================ HUNT
    if (m.bossState == BS_HUNT) {
        m.huntT += dt;
        // a fresh clue is EVIDENCE: something heard just now, or a loud
        // sound inside the last seconds. (The search clock is not evidence -
        // it is the search's own budget and reads positive at nearly every
        // exhaustion; counting it renewed the hunt forever on nothing.) The
        // relentless rule (the user's own order: he does not give up a man
        // who is still making noise in his city) - while clues keep coming,
        // the hunt renews instead of dying on its 45-second clock; it stands
        // down only when the trail is honestly cold, or on the player's
        // death (handled at the top).
        const bool freshClue = m.heardT > 0.0f || m.loudT < 10.0f;
        // the warm trail: a firefight inside the last minute (fightWarmT,
        // stamped by the exchange itself) with the prey still inside the
        // wider hunt area. Senses alone call this trail cold - a quiet man
        // past gunshot-hearing range makes no clue at all - and standing
        // down on that verdict is exactly what walked him away from a live
        // fight into a slow patrol on 2026-11-07. A hunt now stands down
        // only when the trail is cold BOTH ways: no clue AND no warmth.
        const bool warmTrail = m.fightWarmT > 0.0f && playerAlive && distPlayer < kBoss.warmArea;
        auto renewHunt = [&]() {
            m.huntPhase = 0;
            m.huntT = 0.0f;
            m.huntDest = BossHuntDest(m);
            m.route.clear();
            m.searchPts.clear();
            m.searchIdx = 0;
        };
        if (seeNow && m.detect >= 0.85f) {
            m.bossState = BS_COMBAT;
            m.stateWhy = "found him again";
            m.seenTime = 0.0f;
            BossSay(m, SAY_SPOTTED, 2.0f);
        } else if (m.huntT > kBoss.huntSeconds) {
            if (freshClue || warmTrail) {
                renewHunt();
            } else {
                m.bossState = BS_SUSPICIOUS;
                m.stateWhy = "the trail is honestly cold";
                m.primedT = std::max(m.primedT, 30.0f);
                m.detect = std::min(m.detect, 0.3f);
            }
        } else if (m.huntPhase == 0) {
            m.aimGoal = m.huntDest + Vec3(0, 0, 1.2f);
            m.aimRate = 6.0f;
            if (!BossChaseTo(m, m.huntDest, kBoss.chaseSpeed, dt, 1.8f)) {
                // arrived: gather the search - and the search follows the
                // EXIT, not the disc. A man who broke the line at speed is
                // not within twelve metres of where he was seen; he is down
                // the line he was running. So: first the line itself, swept
                // forward in strides (snapped onto the street's nodes where
                // the map offers), then his remembered hiding spots - the
                // ones down the exit line before the ones behind him - then
                // the street's corners weighted toward the exit side of the
                // disc over mere nearness, then the nearest named place.
                m.searchPts.clear();
                const Vec3 exitD = m.exitDir;
                for (float step : { 1.5f, 3.0f, 4.5f }) {
                    Vec3 pt = m.lastSeen + exitD * (kBoss.searchRadius * step);
                    if (NavReady()) {
                        const int n = NavNearest(pt, 15.0f);
                        Vec3 np;
                        if (n >= 0 && NavNodePos(n, &np))
                            pt = np;
                    }
                    m.searchPts.push_back(pt);
                }
                std::vector<std::pair<float, Vec3>> spots;
                for (const Vec3& s : gHidingSpots) {
                    const Vec3 rel = s - m.lastSeen;
                    if (rel.LengthSq() < kBoss.searchRadius * kBoss.searchRadius * 4.0f)
                        spots.push_back({ -(rel.x * exitD.x + rel.y * exitD.y), s });
                }
                std::sort(spots.begin(), spots.end(),
                          [](const std::pair<float, Vec3>& a, const std::pair<float, Vec3>& b) { return a.first < b.first; });
                for (const auto& sp : spots)
                    if (m.searchPts.size() < 12)
                        m.searchPts.push_back(sp.second);
                if (NavReady()) {
                    std::vector<std::pair<float, Vec3>> cand;
                    for (int i = 0; i < NavNodeCount(); ++i) {
                        Vec3 np;
                        if (NavNodePos(i, &np)) {
                            const float d2 = (np - m.lastSeen).LengthSq();
                            if (d2 < kBoss.searchRadius * kBoss.searchRadius) {
                                const float along = (np.x - m.lastSeen.x) * exitD.x + (np.y - m.lastSeen.y) * exitD.y;
                                cand.push_back({ d2 - along * kBoss.searchRadius, np });
                            }
                        }
                    }
                    std::sort(cand.begin(), cand.end(),
                              [](const std::pair<float, Vec3>& a, const std::pair<float, Vec3>& b) { return a.first < b.first; });
                    for (size_t i = 0; i < cand.size() && m.searchPts.size() < 12; ++i)
                        m.searchPts.push_back(cand[i].second);
                }
                const int poi = NavNearestPoi(m.lastSeen);
                if (poi >= 0 && (NavPoiAt(poi).at - m.lastSeen).LengthSq() < 900.0f)
                    m.searchPts.push_back(NavPoiAt(poi).at);
                m.searchIdx = 0;
                m.scanT = 0.0f;
                m.huntPhase = 1;
                BossSay(m, SAY_SEARCH, 3.0f);
            }
        } else {
            if (m.searchIdx >= (int)m.searchPts.size()) {
                if (freshClue) {
                    renewHunt(); // the points are worked but the trail is warm: back to the intercept
                } else if (warmTrail) {
                    // the sweep found nothing and the senses hear nothing -
                    // but the fight is a minute old at most, the prey is
                    // still in the area, and the ground just searched is
                    // ground he is NOT on. The search base marches down the
                    // exit line and the hunt goes on; it does not become a
                    // patrol, and it does not re-search the same disc.
                    m.lastSeen = m.lastSeen + m.exitDir * (kBoss.searchRadius * 2.0f);
                    renewHunt();
                } else {
                    m.bossState = BS_SUSPICIOUS;
                    m.stateWhy = "the trail is honestly cold";
                    m.primedT = std::max(m.primedT, 30.0f);
                    m.detect = std::min(m.detect, 0.3f);
                }
            } else {
                const Vec3 pt = m.searchPts[m.searchIdx];
                if (!BossChaseTo(m, pt, kBoss.chaseSpeed * 0.85f, dt, 1.4f)) {
                    // at a corner he slices it: the rifle sweeps the angle he
                    // could not see on the way in, before he trusts the spot
                    m.scanT += dt;
                    const float base = std::atan2(-(pt.x - m.pos.x), pt.y - m.pos.y);
                    const float sweep = base + std::sin(m.scanT * 2.2f) * 0.9f;
                    m.aimGoal = m.pos + Vec3(-std::sin(sweep), std::cos(sweep), 0.0f) * 10.0f + Vec3(0, 0, 1.2f);
                    m.aimRate = 7.0f;
                    if (m.scanT > 1.6f) {
                        ++m.searchIdx;
                        m.scanT = 0.0f;
                    }
                } else {
                    m.aimGoal = pt + Vec3(0, 0, 1.2f);
                    m.aimRate = 6.0f;
                }
            }
        }
    }

    // ================================================================ the warm guard
    // No road into a quiet state may be walked while a firefight is warm and
    // its prey is still in the area - not the street fight's wind-down, not
    // a command's aftermath, not anything. The hunt resumes at the door of
    // the quiet states themselves; this is the belt over the hunt's own
    // stand-down gate. (The death edge clears the warmth, so a respawned
    // man is never re-hunted for a fight he already paid for.)
    if ((m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) && m.fightStarted && m.fightWarmT > 0.0f &&
        playerAlive && distPlayer < kBoss.warmArea) {
        m.bossState = BS_HUNT;
        m.stateWhy = "a warm trail does not patrol: the hunt resumes";
        m.huntPhase = 0;
        m.huntT = 0.0f;
        m.huntDest = BossHuntDest(m);
        m.route.clear();
    }

    // ================================================================ CALM / SUSPICIOUS
    if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) {
        const bool suspicious = m.bossState == BS_SUSPICIOUS;
        if ((int)m.patrolPts.size() < 6) {
            // his posts stand on Grove Street's own graph nodes when the map is
            // loaded - the street he guards, in the street's own geometry. One
            // post a tick, not all six on his first frame: each snap is a full
            // scan of the node list, and six of them inside the spawn frame
            // were part of its measured 111 ms
            const int k = (int)m.patrolPts.size();
            const float a = (float)k * 1.0472f + (float)(m.id % 6) * 0.5f;
            Vec3 post = m.anchor + Vec3(std::cos(a), std::sin(a), 0.0f) * kSoldier.patrolRadius;
            if (NavReady()) {
                const int n = NavNearest(post, 10.0f);
                Vec3 np;
                if (n >= 0 && NavNodePos(n, &np))
                    post = np;
            }
            m.patrolPts.push_back(post);
        }
        Vec3 toAnchor = m.anchor - m.pos;
        toAnchor.z = 0.0f;
        if (!suspicious && toAnchor.Length() > kBoss.returnRange) {
            BossGoTo(m, m.anchor, t.run, dt, 3.0f); // home is Grove; he routes back
            m.aimGoal = m.anchor + Vec3(0, 0, 1.2f);
            m.aimRate = 4.0f;
        } else if (m.patrolWait > 0.0f) {
            m.patrolWait -= dt;
            m.moving = false;
            // head scans on post: his eyes keep working the street
            m.scanClock += dt;
            const float sweep = m.legYaw + std::sin(m.scanClock * (suspicious ? 1.3f : 0.8f)) * 0.85f;
            m.aimGoal = m.pos + Vec3(-std::sin(sweep), std::cos(sweep), 0.0f) * 10.0f + Vec3(0, 0, 1.4f);
            m.aimRate = 3.0f;
            m.idleVoiceT -= dt;
            if (m.idleVoiceT <= 0.0f) {
                m.idleVoiceT = kBoss.idleVoiceMin + Rand01() * (kBoss.idleVoiceMax - kBoss.idleVoiceMin);
                BossSay(m, SAY_IDLE, 5.0f);
            }
            if (m.patrolWait <= 0.0f) {
                m.patrolStep = (m.patrolStep + 1) % 6;
                m.patrolLeg = 0.0f;
            }
        } else {
            const Vec3 post = m.patrolPts[m.patrolStep % (int)m.patrolPts.size()];
            m.patrolLeg += dt;
            if (!BossGoTo(m, post, t.walk, dt, 1.2f) || m.patrolLeg > 12.0f) {
                m.patrolWait = 1.4f + Rand01() * 1.6f;
                m.moving = false;
            }
            m.aimGoal = post + Vec3(0, 0, 1.4f);
            m.aimRate = 3.0f;
        }
        if (m.heardT > 0.0f) {
            // a noise in the street: his head goes to it first
            m.aimGoal = m.pos + m.heardDir * 10.0f + Vec3(0, 0, 1.2f);
            m.aimRate = 6.0f;
        } else if (m.detect >= kBoss.glanceAt && playerAlive) {
            m.aimGoal = playerPos; // something half-seen: he looks, properly
            m.aimRate = 6.0f;
        }
    }

    // crouch is a shooting stance, never a way of travelling: the field is
    // written inside combat only, and it is cleared here, at the one door
    // every non-combat tick passes, so the crouch planted for a last burst
    // at range cannot ride a hunt or a patrol as a crouched torso - the
    // slow creep of 2026-11-07 wore exactly that, combat's last value,
    // never cleared by anyone.
    if (m.bossState != BS_COMBAT)
        m.crouch = false;

    // ================================================================ his body
    // The aim: in a fight his eyes are on the target at a professional's turn
    // rate; elsewhere the states above left their wish in aimGoal.
    {
        Vec3 aimAt = m.aimGoal;
        float aimRate = m.aimRate;
        if (m.bossState == BS_COMBAT) {
            aimAt = tgt.valid ? tgt.mid : m.lastSeen + Vec3(0, 0, 1.2f);
            aimRate = 12.0f;
        }
        m.aimYaw = TurnYawToward(m.aimYaw, aimAt - m.pos, aimRate, dt);
    }
    // The legs: they face where he is actually going, eased; the torso reaches
    // the aim through the spine (SoldierModel). Standing still with the aim far
    // off his legs, he step-turns like a man instead of pivoting like a statue.
    const float velH = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
    bool stepping = false;
    if (m.moving && velH > 0.4f) {
        const Vec3 mv = Flat(Vec3(m.vel.x, m.vel.y, 0.0f));
        float desired = std::atan2(-mv.x, mv.y);
        if (std::fabs(WrapAngle(m.aimYaw - desired)) > 2.27f)
            desired = m.aimYaw; // running straight away from his aim: the back gait carries it
        m.legYaw = TurnYawToward(m.legYaw, Vec3(-std::sin(desired), std::cos(desired), 0.0f), 6.0f, dt);
    } else if (std::fabs(WrapAngle(m.aimYaw - m.legYaw)) > 0.3f) {
        m.legYaw = TurnYawToward(m.legYaw, Vec3(-std::sin(m.aimYaw), std::cos(m.aimYaw), 0.0f), 3.5f, dt);
        stepping = true;
    }
    m.yaw = m.legYaw;
    // the aim pitch row, from the target's elevation (his rifle must visibly
    // pitch to a man on a roof or below him - the baked grid rows do it)
    if (tgt.valid) {
        const float dz = tgt.mid.z - eye.z;
        const float pitch = std::atan2(dz, std::max(1.0f, tgt.dist));
        m.pitchIdx = pitch > 0.21f ? 1 : pitch < -0.21f ? 2 : 0;
    }
    // ---- compose the draw state (workstream M's two layers)
    SoldierDrawState& d = m.draw;
    int legCat = SP_IDLE, legVar = 0, upCat = SP_IDLE, upVar = 0;
    if (!m.onGround && m.airT > 0.02f) {
        legCat = SP_JUMP;
    } else if (m.crouch && m.bossState == BS_COMBAT) {
        legCat = SP_CROUCH;
    } else if (m.moving || stepping) {
        const bool running = m.moveSpeed > 2.6f && !stepping;
        if (running) {
            legCat = SP_RUN;
            const Vec3 mv = velH > 0.4f ? Flat(Vec3(m.vel.x, m.vel.y, 0.0f)) : m.goal;
            const Vec3 lf(-std::sin(m.legYaw), std::cos(m.legYaw), 0.0f);
            const float along = mv.x * lf.x + mv.y * lf.y;
            const float cross = lf.x * mv.y - lf.y * mv.x;
            const float res = std::acos(Clamp(along, -1.0f, 1.0f));
            if (res < 0.87f)
                legVar = 0;
            else if (res > 2.27f)
                legVar = 3; // back
            else
                legVar = cross > 0.0f ? 1 : 2; // strafe left / right of his facing (the model's own gaits, in its own naming)
        } else {
            legCat = SP_WALK;
        }
    }
    if (m.flinchT > 0.0f) {
        upCat = SP_FLINCH;
        upVar = m.hurtHigh ? 1 : 0;
    } else if (m.reloadT > 0.0f) {
        upCat = SP_RELOAD;
        upVar = m.crouch ? 1 : 0;
    } else if (m.burstShots > 0 || m.shootShow > 0.0f) {
        upCat = SP_SHOOT;
        upVar = m.pitchIdx + (m.crouch ? 3 : 0);
    } else if (m.bossState != BS_CALM || m.alerted) {
        upCat = SP_AIM;
        upVar = m.pitchIdx + (m.crouch ? 3 : 0);
    }
    // his clocks: the upper pose runs on seconds, the gaits on the ground covered
    if (upCat == SP_SHOOT)
        m.shootClock += dt;
    else
        m.shootClock = 0.0f;
    const float upTime = upCat == SP_RELOAD ? (kSoldier.reloadTime - m.reloadT)
                         : upCat == SP_SHOOT ? m.shootClock
                         : upCat == SP_FLINCH ? (0.30f - m.flinchT)
                                              : d.upTime + dt;
    const float legTime = legCat == SP_JUMP ? m.airT : d.legTime + dt;
    if (legCat != d.legCategory || legVar != d.legVariant || upCat != d.upCategory || upVar != d.upVariant) {
        // the pose he leaves is snapshotted composed, layers and all, and the
        // draw blends out of it - no transition ever snaps
        SoldierLocal prev[64];
        if (SoldierComposeLocals(d, prev, 64)) {
            for (int i = 0; i < 64; ++i)
                d.prevLocals[i] = prev[i];
            d.prevBones = SoldierBoneCount();
            d.hasPrev = d.prevBones > 0;
            d.fade = 0.0f;
        }
    }
    d.legCategory = legCat;
    d.legVariant = legVar;
    d.upCategory = upCat;
    d.upVariant = upVar;
    d.upTime = upTime;
    d.legTime = legTime;
    d.aimYaw = m.aimYaw;
    d.legYaw = m.legYaw;
    d.fullCategory = -1;
    if (d.fade < 1.0f)
        d.fade = std::min(1.0f, d.fade + dt / kSoldierFadeSeconds);
    // ---- the pose ring log (diagnostic build): every pose-state change and
    // at least every 100 ms, the pose he is being drawn in, so the session
    // log can be laid next to what the eye saw, window by window. When the
    // vertex watchdog stays silent through visible garbage, this log plus
    // that silence is the proof the mesh itself is clean.
    {
        m.poseLogT += dt;
        const bool poseChanged = legCat != m.poseLogLeg || legVar != m.poseLogLegVar || upCat != m.poseLogUp ||
                                 upVar != m.poseLogUpVar;
        if (poseChanged || m.poseLogT >= 0.1f) {
            const char* weap = m.nadeShowT > 0.0f ? "HE" : m.pistolOut ? "USP" : "M4";
            const float twist = WrapAngle(m.aimYaw - m.legYaw);
            if (poseChanged && m.poseLogLeg >= 0)
                Log("boss pose: leg %d/%d up %d/%d (from leg %d/%d up %d/%d) fade %.2f twist %.2f row %d "
                    "crouch %d weap %s pos (%.1f %.1f %.1f) pdist %.1f",
                    legCat, legVar, upCat, upVar, m.poseLogLeg, m.poseLogLegVar, m.poseLogUp, m.poseLogUpVar,
                    d.fade, twist, m.pitchIdx, m.crouch ? 1 : 0, weap, m.pos.x, m.pos.y, m.pos.z, distPlayer);
            else
                Log("boss pose: leg %d/%d up %d/%d fade %.2f twist %.2f row %d crouch %d weap %s "
                    "pos (%.1f %.1f %.1f) pdist %.1f",
                    legCat, legVar, upCat, upVar, d.fade, twist, m.pitchIdx, m.crouch ? 1 : 0, weap, m.pos.x,
                    m.pos.y, m.pos.z, distPlayer);
            m.poseLogLeg = legCat;
            m.poseLogLegVar = legVar;
            m.poseLogUp = upCat;
            m.poseLogUpVar = upVar;
            m.poseLogT = 0.0f;
        }
    }
    // gait phase from the ground he actually covers, at the current gait's own
    // stride; in a step-turn the cycle advances as if shuffling on the spot
    if (legCat == SP_WALK || legCat == SP_RUN) {
        const float stride = SoldierGaitStride(legCat, legVar);
        if (stride > 0.01f) {
            if (stepping)
                d.gait += dt * 1.1f / stride;
            else {
                const Vec3 fwdv(-std::sin(m.legYaw), std::cos(m.legYaw), 0.0f);
                const float alongV = m.vel.x * fwdv.x + m.vel.y * fwdv.y;
                const float hv = std::sqrt(m.vel.x * m.vel.x + m.vel.y * m.vel.y);
                const float sideV = std::sqrt(std::max(0.0f, hv * hv - alongV * alongV));
                const float adv = std::fabs(alongV) >= sideV ? alongV : hv;
                d.gait += adv * dt / stride;
            }
        }
    }
    // footsteps on his stride, the game's own sounds
    if ((m.moving || stepping) && m.onGround) {
        const float stride = SoldierGaitStride(legCat, legVar);
        const float stepLen = stride > 0.01f ? stride * 0.5f : 2.6f;
        m.stepAcc += velH * dt + (stepping ? dt * 1.1f : 0.0f);
        if (m.stepAcc > stepLen) {
            m.stepAcc = 0.0f;
            PlaySfx(SND_SOLDIER_STEP, &m.pos, 1.3f, 0.95f + Rand01() * 0.1f);
        }
    } else {
        m.stepAcc = 0.0f;
    }
    // ---- the transition record (permanent): every state change with the
    // reason its gate recorded and the facts around it, so the next eye
    // report can be convicted from the log alone - the pose ring proved
    // this instrumentation pays for itself the first night it flew, and the
    // creep of 2026-11-07 was only diagnosable as far as it went because
    // the ring lacked exactly this line.
    if (m.bossState != m.stateLogFrom) {
        if (m.stateLogFrom >= 0)
            Log("boss state: %s -> %s | why %s | seen %d lost %.1f hunt %.0f/%.0f warm %.0f loud %.0f hp %.2f pdist %.0f",
                BossStateName(m.stateLogFrom), BossStateName(m.bossState),
                m.stateWhy ? m.stateWhy : "-", tgtVisible ? 1 : 0, m.lostT, m.huntT, (float)m.huntPhase,
                m.fightWarmT, m.loudT, (double)(m.health / kBoss.health), distPlayer);
        m.stateLogFrom = m.bossState;
        m.stateWhy = nullptr;
    }
    *speed = m.moveSpeed;
    return true; // a soldier is always on duty: patrol, talk, hunt and fight are all his
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
        else if (m.kind == MOB_SOLDIER) {
            const auto brainT0 = std::chrono::steady_clock::now();
            chasing = SoldierBrain(m, dt, playerPos, &speed, &looking, beings);
            sBrainMs += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - brainT0).count();
        }
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
    // different ways at once, which is exactly what the user saw. The soldier shared this rule in step 1,
    // but the BOSS owns his own facing now: his brain turns his LEGS to his movement and his TORSO to his
    // aim (the two-layer draw, SoldierModel), so the generic body-turn must not touch him - and his feet
    // follow his goal, not his nose, like the skeleton's.
    const bool aiming = hasPrey && m.kind == MOB_SKELETON;

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
        target = (aiming || m.kind == MOB_SOLDIER) ? m.goal * speed : fwd * speed;
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
    if (m.kind == MOB_SOLDIER) {
        m.soldierDeath = (int)(Rand01() * 2.99f); // one of the game's three falls, his own
        // he falls out of the pose he died in, not out of a snap: the composed
        // pose is snapshotted for the cross-fade, then the death takes over
        SoldierLocal prev[64];
        if (SoldierComposeLocals(m.draw, prev, 64)) {
            for (int i = 0; i < 64; ++i)
                m.draw.prevLocals[i] = prev[i];
            m.draw.prevBones = SoldierBoneCount();
            m.draw.hasPrev = m.draw.prevBones > 0;
            m.draw.fade = 0.0f;
        }
        // diag: the death is a pose-state change too - the ring log names it
        Log("boss pose: DEATH var %d (fading from leg %d/%d up %d/%d)", m.soldierDeath, m.draw.legCategory,
            m.draw.legVariant, m.draw.upCategory, m.draw.upVariant);
        m.draw.fullCategory = SP_DEATH;
        m.draw.fullVariant = m.soldierDeath;
        m.draw.fullTime = 0.0f;
    }
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
    int pack = 2 + McsaRand() % 3;
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

// ------------------------------------------------------------------
// BossVehicleZone (declared in Mobs.h, pinned by tests): one of his rounds
// has landed on a vehicle - the zone it struck, priced from the vehicle's
// own frame. Below the body's centre line is the wheel band (the burst comes
// with it), the front of the frame is the engine bay, everything else is
// honest bodywork. The bridge pays the number onto the game's own 1000-point
// vehicle health (x4 in GtaHurtVehicle), so a body hit is 36 points: smoke
// inside a burst or two of deliberate fire, fire inside four - the stages
// the eye can grade.
BossVehZone BossVehicleZone(const Vec3& point, const Vec3& centre, const Vec3& fwd, float baseDmg) {
    BossVehZone z;
    z.dmg = baseDmg;
    const Vec3 d = point - centre;
    const Vec3 fh = Flat(fwd);
    const float along = d.x * fh.x + d.y * fh.y; // > 0: the nose half of the frame
    if (d.z < 0.0f) {
        z.dmg = baseDmg * kBoss.vehWheelMul;
        z.tyre = true;
    } else if (along > 0.85f) {
        z.dmg = baseDmg * kBoss.vehEngineMul;
    }
    return z;
}

// ------------------------------------------------------------------
// The boss's shared world state, at mc:: scope: the stimulus list his
// ears read, his live grenades, and the session's hiding-spot memory.
// (Defined here, outside the brain's anonymous namespace, because
// Combat, Entities, MobsTick and MobsClear all reach them.)
// ------------------------------------------------------------------ stimuli
// Loud facts the whole core can hear: a weapon loosed, an explosion. Combat and
// Entities report them here; every soldier's hearing reads the same list, which
// MobsTick ages. A stimulus never says WHO - direction and alert only, the way
// a real ear works.
std::vector<Stimulus> gStimuli;

void NotePlayerShot(const Vec3& at, float radius) {
    if (gStimuli.size() < 32)
        gStimuli.push_back(Stimulus{ at, radius, 0.5f });
}
void NoteExplosion(const Vec3& at) { NotePlayerShot(at, kBoss.hearExplosion); }

// ------------------------------------------------------------------ grenades
// His HE grenades, flown honestly: an arc, gravity, a fuse, then the bridge's
// own explosion for the eye and the core channel for the hurt (the same split
// the creeper lives by). Two a fight - a professional carries two.
std::vector<BossNade> gBossNades;

void BossNadeBlastPlayer(const Vec3& at, const Vec3& playerPos); // below
void BossNadesTick(float dt, const Vec3& playerPos) {
    for (size_t i = 0; i < gBossNades.size();) {
        BossNade& n = gBossNades[i];
        n.fuse -= dt;
        n.vel.z -= 32.0f * dt; // the world's gravity, the one MoveMob falls by
        n.pos = n.pos + n.vel * dt;
        // the ground is real to it: the arc is shorter than the fuse, so an
        // unclamped grenade fell THROUGH the street and detonated under it -
        // an invisible blast with its heart sunk below the pavement (the eye
        // verdict's "no effect"). It lands, rests where it fell, and goes
        // off on the surface like the object it is.
        float gz = 0.0f;
        if (TheHost().GroundBelow(n.pos + Vec3(0, 0, 0.3f), 1.2f, &gz) && n.pos.z < gz) {
            n.pos.z = gz;
            n.vel = Vec3(0, 0, 0);
        }
        if (n.fuse <= 0.0f) {
            const Vec3 at = n.pos;
            TheHost().Explosion(at, BLAST_SMALL);
            PlaySfx(SND_EXPLODE, &at, 3.0f);
            // other mobs take the radial as usual; the player's share lands on
            // the core's own channel with a falloff (BAGUVIX cannot stand in
            // front of this either) - BossNadeBlastPlayer, just below
            MobsRadial(at, kBoss.nadeRadius, 4.0f, 8.0f);
            BossNadeBlastPlayer(at, playerPos);
            gBossNades.erase(gBossNades.begin() + (ptrdiff_t)i);
        } else {
            ++i;
        }
    }
}

void BossNadeBlastPlayer(const Vec3& at, const Vec3& playerPos) {
    // Counter-Strike's own HE curve (CsHeHh): the measured heart of 96,
    // falling linearly to nothing at the radius' edge, halved by armour -
    // and priced through walls, as 1.6 prices it: a man behind a door is
    // not safe from a grenade, only farther from its heart. The game's own
    // vest, when worn, takes its wear through the host.
    const float d = (playerPos - at).Length();
    const float armourPts = TheHost().PlayerArmor();
    float wear = 0.0f;
    const float hh = CsHeHh(d, kBoss.nadeRadius, kBoss.nadeDamage, armourPts > 0.5f, armourPts * 0.2f, &wear);
    if (hh > 0.0f) {
        TheHost().HurtPlayer(hh);
        if (wear > 0.0f)
            TheHost().WearPlayerArmor(wear);
    }
}

// ---- his street navigation
// One session's worth of the player's hiding spots: where his line of sight
// broke successfully. The hunt checks these first next time. Session-only -
// the world save has no honest slot for them, and a new session is a new man
// to hunt. (Cleared in MobsClear.)
std::vector<Vec3> gHidingSpots;
void NoteHidingSpot(const Vec3& at) {
    for (const Vec3& s : gHidingSpots)
        if ((s - at).LengthSq() < 9.0f)
            return;
    if (gHidingSpots.size() < 12)
        gHidingSpots.push_back(at);
}

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
    // the boss's session memory dies with the session (his hiding spots are
    // session-only by design), and no stimulus or live grenade outlives it
    gHidingSpots.clear();
    gStimuli.clear();
    gBossNades.clear();
}

int SpawnMob(int kind, const Vec3& feet, bool persistent, bool baby) {
    if (kind < 0 || kind >= MOB_KIND_COUNT || gMobs.size() >= 96)
        return -1;
    // CleanSA: San Andreas has no Minecraft mobs in it - every spawn path in the
    // core (natural spawns, eggs, breeding, zombie reinforcements) funnels
    // through this one door, and only the soldier may pass.
    if (gRules.cleanSA && kind != MOB_SOLDIER)
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
        // posted where he was summoned: counter-terrorist, the boss's kevlar on,
        // a full magazine in the rifle, two grenades, a loaded USP, his aim and
        // his legs both on the way he was summoned facing
        m.team = TEAM_COUNTER;
        m.armor = kBoss.armor;
        m.spraySeed = 1 + (int)(Rand01() * 2000000000.0f); // his first spray sways his own way
        m.anchor = feet;
        m.mag = kSoldier.magazine;
        m.pistolMag = kBoss.pistolMag;
        m.nades = 2;
        m.aimYaw = m.yaw;
        m.legYaw = m.yaw;
        m.draw.aimYaw = m.yaw;
        m.draw.legYaw = m.yaw;
        m.bossState = BS_CALM;
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
        // Counter-Strike's kevlar, at the game's own law (CsArmorSplit): the health share of a hit
        // is the damage times the armour ratio over two - for the rounds that reach him here the
        // ratio is 1.0, the generic one, since the player's weapons are not CS weapons with ratios
        // of their own - and the vest wears at (damage - health) x 0.5, half the rate the old flat
        // law drained it, spilling to health as it dies. There is no hit-zone anywhere in this
        // road - a hit is a hit, head or body alike, and no multiplier exists for it to take (the
        // boss's no-headshot law is structural, the user's standing design choice, not a CS rule).
        // A professional in armour is also barely shoved by a blow. And being hurt tells him where his
        // enemy is: he is alerted, his detection is full, and he hunts the blow's origin.
        knock *= 0.25f;
        if (m.armor > 0.0f) {
            float wear = 0.0f;
            const float health = CsArmorSplit(dmg, 1.0f, m.armor, &wear);
            m.armor -= wear;
            dmg = health;
        }
        m.alerted = true;
        m.alertFade = 0.0f;
        m.lastSeen = from;
        m.searchTime = std::max(m.searchTime, kSoldier.searchSeconds);
        m.detect = 1.0f;
        m.hurtHigh = from.z > m.pos.z + 1.4f;
        // ---- damage feedback, scaled by the blow (the user's eye verdict:
        // 4,000 half hearts with a fixed chirp read as invincibility). A
        // light hit earns the brief throttled flinch he always had; a heavy
        // one staggers him - a visible rock-back where he cannot fire and
        // barely moves - and a meaningful hit ALWAYS earns a hurt line,
        // whatever the voice throttle says.
        if (m.flinchT <= 0.0f)
            m.flinchT = 0.30f; // a brief upper-body flinch, throttled (workstream M)
        if (dmg >= kBoss.staggerHit) {
            m.flinchT = std::max(m.flinchT, 0.45f);
            m.staggerT = std::max(m.staggerT, kBoss.staggerSeconds);
        }
        if (dmg >= kBoss.hurtVoiceHit) {
            // guaranteed: a hit this size is always heard - neither the
            // hurt throttle nor the voice cooldown may swallow it
            const Vec3 c = m.pos + Vec3(0, 0, 1.3f);
            PlaySfx(SND_BOSS_HURT, &c, 1.6f);
            m.hurtSayT = std::max(m.hurtSayT, 1.2f);
        } else if (m.hurtSayT <= 0.0f) {
            // decline made audible before the wounded threshold: below 70%
            // health his hurt throttle shortens - he reacts to lighter hits
            // too, and the player can hear the fight going out of him
            m.hurtSayT = (m.health - dmg < kBoss.health * 0.7f) ? 0.25f : 0.6f;
            if (m.proxyHit)
                BossSay(m, SAY_SPOTTED, 1.5f); // shot by the street: he calls it
            else {
                const Vec3 c = m.pos + Vec3(0, 0, 1.3f);
                PlaySfx(SND_BOSS_HURT, &c, 1.4f);
            }
        }
        m.fightStarted = true; // he has been hurt: no command phase after this
        if (m.proxyHit) {
            // the street hurt him through his perception body: the attacker is a
            // legitimate target now (K1) - and a talk-first window, if one was
            // open with the player, is not burnt by somebody else's war
            if (m.proxyAttacker >= 0) {
                m.attackerBeing = m.proxyAttacker;
                m.attackerFresh = 6.0f;
            } else if (m.proxyAttacker == -2) {
                // no gang hand on the hurt and the player right there: the shot
                // was his (his projectiles meet the proxy; no task names them)
                m.aggro = true; // the law: damage dealt makes the player an enemy
                m.playerHostileT = 5.0f;
                m.fightWarmT = kBoss.warmSeconds; // a firefight with the player: the trail is warm
                m.negotiated = true;
                if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS || m.bossState == BS_COMMAND) {
                    m.bossState = BS_COMBAT;
                    m.stateWhy = "shot through his perception body: the fight is on";
                }
            }
            m.proxyHit = false;
            m.proxyAttacker = -1;
        } else {
            // hurt by the player (his blows, his arrows, his blasts land here):
            // the player is the one fighting him, and any talk is over
            m.aggro = true; // the law: damage dealt makes the player an enemy
            m.playerHostileT = 5.0f;
            m.fightWarmT = kBoss.warmSeconds; // a firefight with the player: the trail is warm
            m.negotiated = true;
            if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS || m.bossState == BS_COMMAND) {
                m.bossState = BS_COMBAT;
                m.stateWhy = "hurt by the player: the fight is on";
            }
        }
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

// ------------------------------------------------------------------
// The boss's doors in from the bridge (workstreams K and L). All of them are
// soldier-only; every other kind ignores them.

void MobProvoked(int index, int being, const Vec3& from) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    if (m.kind != MOB_SOLDIER || m.death >= 0.0f || being < 0)
        return;
    // somebody in the street raised a hand to him: a legitimate target (K1).
    // He does not need to be hurt first - a ped drawing on him is enough.
    m.attackerBeing = being;
    m.attackerFresh = 6.0f;
    m.fightStarted = true;
    m.alerted = true;
    m.alertFade = 0.0f;
    m.lastSeen = from;
    if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) {
        m.bossState = BS_COMBAT;
        m.negotiated = true;
        m.seenTime = 0.0f;
    }
}

void MobProxyDamage(int index, float halfHearts, const Vec3& from, int attackerBeing) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    if (m.kind != MOB_SOLDIER || m.death >= 0.0f || halfHearts <= 0.0f)
        return;
    // One relayed event can never take a boss-killing share: the game's own
    // headshots land on the proxy too, and his law says a head hit is a body
    // hit. The cap is a tenth of his pool - ten solid events minimum to die by
    // the street, and ordinary gang fire arrives far below it.
    const float capped = std::min(halfHearts, kBoss.health * 0.10f);
    m.proxyHit = true;
    m.proxyAttacker = attackerBeing;
    const float hurtBefore = m.hurt;
    m.hurt = 0.0f; // the proxy bleeds at the game's rate, not at MobHurt's window
    MobHurt(index, capped, from, 0.0f);
    if (m.death < 0.0f)
        m.hurt = std::min(m.hurt, std::max(hurtBefore, 0.2f));
}

void MobVehicleThreat(int index, const Vec3& vehPos, const Vec3& vehVel, bool driverIsPlayer) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    if (m.kind != MOB_SOLDIER || m.death >= 0.0f)
        return;
    if (driverIsPlayer) {
        // the law's third provocation, read at intent: the bridge only calls
        // this when the player's vehicle is fast, close, and on a line that
        // runs through his body - driving AT him. A dodge with no consequence
        // was the eye verdict's complaint ("I run at him and he does nothing
        // to me"): the attempt itself makes the driver his enemy, hit or miss.
        m.aggro = true;
        m.fightStarted = true;
        m.negotiated = true;
        m.detect = 1.0f;
        m.playerHostileT = std::max(m.playerHostileT, 6.0f);
        m.fightWarmT = kBoss.warmSeconds;
        m.lastSeen = vehPos;
        if (m.bossState == BS_CALM || m.bossState == BS_SUSPICIOUS) {
            m.bossState = BS_COMBAT;
            m.stateWhy = "the player drove straight at him: an attempted ram";
            m.seenTime = 0.0f;
            BossSay(m, SAY_SPOTTED, 2.0f);
        }
    }
    if (m.dodgeT > 0.0f || !m.onGround)
        return;
    // a fast lateral step, to whichever side has room (probed honestly)
    const Vec3 vh = Flat(vehVel);
    Vec3 side(vh.y, -vh.x, 0.0f);
    const Vec3 chest = m.pos + Vec3(0, 0, 1.1f);
    const bool leftOpen = !TheHost().LineBlocked(chest, chest + side * 2.2f);
    const bool rightOpen = !TheHost().LineBlocked(chest, chest - side * 2.2f);
    if (!leftOpen && !rightOpen)
        return; // walled in: he braces instead (the hit itself is capped)
    if (!leftOpen)
        side = side * -1.0f;
    m.dodgeDir = side;
    m.dodgeT = 0.38f;
    m.vel.x += side.x * 4.5f;
    m.vel.y += side.y * 4.5f;
}

void MobVehicleHit(int index, float speed, const Vec3& from, bool driverIsPlayer) {
    if (index < 0 || index >= (int)gMobs.size())
        return;
    Mob& m = gMobs[index];
    if (m.kind != MOB_SOLDIER || m.death >= 0.0f)
        return;
    // a car is the heaviest blow the street owns - and it still cannot one-shot
    // him: the tune caps it at a share of his whole pool, whatever the speed
    const float raw = 30.0f + speed * 55.0f;
    const float capped = std::min(raw, kBoss.health * kBoss.ramCapShare);
    m.proxyHit = false;
    const float hurtBefore = m.hurt;
    m.hurt = 0.0f;
    MobHurt(index, capped, from, 0.6f);
    if (m.death < 0.0f) {
        m.hurt = std::min(m.hurt, std::max(hurtBefore, 0.2f));
        m.ramT = 0.9f; // a breath on the ground, then he is up and fighting
        m.detect = 1.0f;
        if (driverIsPlayer) {
            m.aggro = true; // the law: ramming him makes the player an enemy
            m.fightStarted = true;
            m.fightWarmT = kBoss.warmSeconds;
            m.playerHostileT = 6.0f;
            m.negotiated = true;
            m.bossState = BS_COMBAT;
            m.stateWhy = "rammed by the player's vehicle";
            m.seenTime = kSoldier.reaction; // the driver is known, completely, at once
        }
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
        int n = 1 + McsaRand() % 3;
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
    // CleanSA has no natural spawns at all (the bridge does not call this in
    // that mode either; this is the core's own belt to the bridge's braces).
    if (gRules.cleanSA)
        return;
    gSpawnTimer -= dt;
    if (gSpawnTimer <= 0.0f) {
        gSpawnTimer = 1.5f;
        TrySpawn(playerPos);
    }
}

void MobsTick(float dt, const Vec3& playerPos, uint16_t playerHeld, bool playerOnFoot,
              const std::vector<BeingInfo>* beings) {
    // ---- the long-frame logger (diagnostic build): while a boss is active,
    // any frame stretched past 50 ms - by the game's own dt or by this tick's
    // measured cost - is named with the phase timings that could be measured
    // cheaply, so the hitch at the player's death stops being a rumour and
    // becomes a line with numbers. The bridge's two phases (proxy sync, mob
    // render) are reported back through BossTest and describe the frame that
    // just rendered; the core's own phases are this tick's.
    const auto diagT0 = std::chrono::steady_clock::now();
    sBrainMs = 0.0f;
    sVoiceLines = 0;
    bool bossActive = false;
    for (const Mob& m : gMobs)
        if (m.kind == MOB_SOLDIER && m.death < 0.0f) {
            bossActive = true;
            break;
        }
    // the boss's world ticks: loud facts age out, his thrown grenades fly
    for (size_t i = 0; i < gStimuli.size();) {
        gStimuli[i].ttl -= dt;
        if (gStimuli[i].ttl <= 0.0f)
            gStimuli.erase(gStimuli.begin() + (ptrdiff_t)i);
        else
            ++i;
    }
    BossNadesTick(dt, playerPos);
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
    // In CleanSA the queue is dropped unread: no zombie exists to call, and the
    // SpawnMob gate would refuse the answer anyway.
    if (!gReinforceCalls.empty() && !gRules.cleanSA) {
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
                if (McsaRand() % 20 == 0)
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
                    SpawnXp(at + Vec3(0, 0, 0.5f), 1 + McsaRand() % 7);
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

        // the boss is a posted man, not a wandering spawn: the distance sweep
        // never erases him (his own anchor logic walks him home, and his
        // perception body's teardown rides on his death alone) - the first
        // build lost him at the 320 m persistent limit the moment his player
        // respawned across the city
        const float limit = mm.id == gGame.ridingMob || mm.kind == MOB_SOLDIER ? 1e9f : mm.persistent ? 320.0f : 130.0f;
        if (distToPlayer > limit || mm.pos.z < -120.0f)
            remove = true;
        if (remove) {
            gMobs[i] = gMobs.back();
            gMobs.pop_back();
        } else {
            ++i;
        }
    }
    if (bossActive) {
        const float tickMs =
            std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - diagT0).count();
        if (dt * 1000.0f > 50.0f || tickMs > 50.0f)
            Log("LONG FRAME: game dt %.1f ms | core mobs %.1f ms (soldier brain %.1f ms, voice lines %d) | "
                "bridge last frame: proxy %.1f ms, mobs render %.1f ms",
                dt * 1000.0f, tickMs, sBrainMs, sVoiceLines, BossTestProxyMs(), BossTestRenderMs());
    }
}

} // namespace mc
