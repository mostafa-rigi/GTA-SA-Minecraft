// Offline checks of the pure game logic (recipes, cooking, tables, movement, blocks, loose things, arm's length, survival, controls, game state, world file). Not part of the mod.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>

#include "Audio.h"
#include "BlockRules.h"
#include "BossTest.h"
#include "Controls.h"
#include "Entities.h"
#include "GameState.h"
#include "Host.h"
#include "Interact.h"
#include "Inventory.h"
#include "BlockMesh.h"
#include "Combat.h"
#include "CsRules.h"
#include "Fishing.h"
#include "Hands.h"
#include "Hud.h"
#include "Items.h"
#include "McModel.h"
#include "Mobs.h"
#include "Cop.h"
#include "CopTest.h"
#include "CopCarModel.h"
#include "Atlas.h"
#include "NavGraph.h"
#include "SoldierModel.h"
#include "Screens.h"
#include "Shapes.h"
#include "Beds.h"
#include "Doors.h"
#include "Chat.h"
#include "Commands.h"
#include "Villagers.h"
#include "Particles.h"
#include "PlayerAnim.h"
#include "Renderers.h"
#include "Physics.h"
#include "Save.h"
#include "Survival.h"
#include "World.h"

using namespace mc;

// the sounds the core asked for, written down instead of played
struct Played {
    SoundEvent ev;
    bool placed; // at a position in the world (not at the listener)
    Vec3 pos;
    float volume, pitch;
};
static std::vector<Played> gPlayed;

static int Heard(SoundEvent ev) {
    int n = 0;
    for (const Played& p : gPlayed)
        n += p.ev == ev;
    return n;
}

// What the core expects from the game it runs in (see Core.h, World.h, Audio.h): here, next to nothing.
namespace mc {
void PlaySfx(SoundEvent ev, const Vec3* pos, float volume, float pitch) {
    gPlayed.push_back({ ev, pos != nullptr, pos ? *pos : Vec3(), volume, pitch });
}
void SetLoopSfx(SoundEvent, bool, float) {}
struct ChunkMesh {};
Chunk::Chunk() = default;
Chunk::~Chunk() = default;
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
}
} // namespace mc

static int gFail = 0;
#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            gFail++; \
            printf("FAIL: "); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static ItemStack Craft(int w, int h, std::initializer_list<uint16_t> cells) {
    ItemStack grid[9];
    int i = 0;
    for (uint16_t c : cells) {
        if (c) {
            grid[i].id = c;
            grid[i].count = 1;
        }
        ++i;
    }
    return MatchRecipe(grid, w, h);
}

static void Expect(const char* what, const ItemStack& r, uint16_t id, int count) {
    CHECK(r.id == id && r.count == count, "%s -> got %s x%d, want %s x%d", what, r.id ? Item(r.id).key : "nothing",
          r.count, Item(id).key, count);
}

// ---------------------------------------------------------------- movement (Physics.h)
// A host with no map of its own: only blocks, plus what a test puts in (a thin box).
struct TestMap : MoveWorld {
    std::vector<Aabb> extra;   // host boxes, each inside one cell
    void CellBoxes(int x, int y, int z, std::vector<Aabb>& out) override {
        for (const Aabb& b : extra)
            if (FloorI(b.x0) == x && FloorI(b.y0) == y && FloorI(b.z0) == z)
                out.push_back(b);
    }
};

static const float kFloor = 10.0f; // top of the test floor

// three blocks of `block` (water, lava, or air to drain it) on the whole test floor
static void Pool(int block) {
    for (int x = -8; x <= 40; ++x)
        for (int y = -8; y <= 8; ++y)
            for (int z = 10; z <= 12; ++z)
                gWorld.SetRaw(x, y, z, MakeVox(block));
}

// a body standing on the floor at (x, y)
static Body Standing(float x, float y) {
    Body b;
    b.Place(Vec3(x, y, kFloor + 1.0f), Vec3(), true);
    return b;
}

static float HorizSpeed(const Body& b) { return std::sqrt(b.vel.x * b.vel.x + b.vel.y * b.vel.y); }

// jumps once and returns how high the feet got above the floor
static float JumpHeight(TestMap& map, float dt, bool sprint) {
    Body b = Standing(0.5f, 0.5f);
    WalkInput in;
    in.look = Vec3(1, 0, 0);
    in.sprint = sprint;
    in.fwd = sprint ? 1.0f : 0.0f;
    WalkEvents ev;
    for (int i = 0; i < 10; ++i)
        WalkStep(b, in, dt, map, ev);
    in.jump = true;
    WalkStep(b, in, dt, map, ev);
    CHECK(ev.jumped && !b.onGround, "jump starts (dt %.4f)", dt);
    in.jump = false;
    float top = b.pos.z;
    for (int i = 0; i < (int)(3.0f / dt) && !b.onGround; ++i) {
        WalkStep(b, in, dt, map, ev);
        top = std::max(top, b.pos.z);
    }
    CHECK(b.onGround && std::fabs(b.pos.z - (kFloor + 1.0f)) < 1e-3f, "jump lands on the floor (z %.3f)", b.pos.z);
    return top - (kFloor + 1.0f);
}

static void TestPhysics() {
    const float dt = 1.0f / 60.0f;
    gWorld.Clear();
    for (int x = -8; x <= 40; ++x)
        for (int y = -8; y <= 8; ++y)
            gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
    TestMap map;
    WalkEvents ev;

    // box clipping
    {
        std::vector<Aabb> boxes = { { 2, 0, 0, 3, 1, 1 } };
        const Aabb p = { 0.0f, 0.2f, 0.0f, 0.6f, 0.8f, 1.8f };
        CHECK(std::fabs(ClipX(p, 5.0f, boxes) - 1.4f) < 1e-5f, "ClipX stops at the box (%.3f)", ClipX(p, 5.0f, boxes));
        CHECK(ClipX(p, -5.0f, boxes) == -5.0f, "ClipX away from the box is free");
        CHECK(ClipY(p, 5.0f, boxes) == 5.0f, "ClipY beside the box is free");
        const Aabb above = { 2.2f, 0.2f, 3.0f, 2.8f, 0.8f, 4.8f };
        CHECK(std::fabs(ClipZ(above, -9.0f, boxes) + 2.0f) < 1e-5f, "ClipZ lands on the box");
        const Aabb body = BodyBox(Vec3(5.0f, 6.0f, 11.0f), kBodyHeight);
        CHECK(std::fabs(body.z0 - 10.0f) < 1e-5f && std::fabs(body.z1 - 11.8f) < 1e-5f && std::fabs(body.x1 - body.x0 - 0.6f) < 1e-5f,
              "body box is 0.6 wide, 1.8 tall, feet 1 m under the position");
    }

    // footsteps: one every 1.6 m at the feet, quiet when sneaking
    {
        const SoundEvent step = (SoundEvent)(SND_STEP_STONE + Block(ID_STONE).sound);
        for (int sneak = 0; sneak < 2; ++sneak) {
            Body b = Standing(0.5f, 0.5f);
            WalkInput in;
            in.look = Vec3(1, 0, 0);
            in.fwd = 1.0f;
            in.sneak = sneak != 0;
            gPlayed.clear();
            for (int i = 0; i < 360; ++i)
                WalkStep(b, in, dt, map, ev);
            const float steps = (b.pos.x - 0.5f) / 1.6f;
            CHECK(std::fabs(Heard(step) - steps) <= 1.0f && Heard(step) == (int)gPlayed.size() && Heard(step) > 0,
                  "%d footsteps on %.1f m", Heard(step), b.pos.x - 0.5f);
            bool right = true;
            for (const Played& p : gPlayed)
                right = right && p.placed && std::fabs(p.pos.z - kFloor) < 1e-3f && p.volume == (sneak ? 0.08f : 0.18f);
            CHECK(right, "footsteps sound at the feet, %s", sneak ? "quietly when sneaking" : "normally when walking");
        }
    }

    // standing still stays put
    {
        Body b = Standing(0.5f, 0.5f);
        b.onGround = false;
        WalkInput in;
        in.look = Vec3(1, 0, 0);
        for (int i = 0; i < 60; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(b.onGround && std::fabs(b.pos.z - 11.0f) < 1e-3f && std::fabs(b.pos.x - 0.5f) < 1e-4f, "stands on the floor (z %.4f)",
              b.pos.z);
    }

    // Minecraft's speeds: walking 4.317, sprinting 5.612, sneaking 1.295 blocks per second
    {
        const struct { const char* name; bool sprint, sneak, item; float want; } kCases[] = {
            { "walk", false, false, false, kWalkSpeed },
            { "sprint", true, false, false, kSprintSpeed },
            { "sneak", false, true, false, kSneakSpeed },
            { "walk while eating", false, false, true, kWalkSpeed * 0.2f },
        };
        for (const auto& c : kCases) {
            Body b = Standing(0.5f, 0.5f);
            WalkInput in;
            in.look = Vec3(1, 0, -0.4f); // looking down a bit must not slow the walk
            in.fwd = 1.0f;
            in.sprint = c.sprint;
            in.sneak = c.sneak;
            in.usingItem = c.item;
            for (int i = 0; i < 120; ++i)
                WalkStep(b, in, dt, map, ev);
            const float x0 = b.pos.x;
            for (int i = 0; i < 60; ++i)
                WalkStep(b, in, dt, map, ev);
            CHECK(std::fabs(HorizSpeed(b) - c.want) < 0.01f, "%s speed %.3f, want %.3f", c.name, HorizSpeed(b), c.want);
            CHECK(std::fabs((b.pos.x - x0) - c.want) < 0.02f, "%s covers %.3f m in a second, want %.3f", c.name, b.pos.x - x0,
                  c.want);
            CHECK(b.onGround && std::fabs(b.pos.y - 0.5f) < 1e-3f, "%s goes straight on the ground", c.name);
        }
        // diagonal input is no faster
        Body b = Standing(0.5f, 0.5f);
        WalkInput in;
        in.look = Vec3(1, 0, 0);
        in.fwd = in.strafe = 1.0f;
        for (int i = 0; i < 120; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(std::fabs(HorizSpeed(b) - kWalkSpeed) < 0.01f, "diagonal speed %.3f", HorizSpeed(b));
    }

    // the jump: 1.25 blocks, whatever the frame rate
    {
        const float h60 = JumpHeight(map, 1.0f / 60.0f, false), h30 = JumpHeight(map, 1.0f / 30.0f, false),
                    h144 = JumpHeight(map, 1.0f / 144.0f, false);
        printf("jump height: %.3f (60 fps), %.3f (30 fps), %.3f (144 fps)\n", h60, h30, h144);
        CHECK(std::fabs(h60 - 1.25f) < 0.02f, "jump height %.3f, want 1.25", h60);
        CHECK(std::fabs(h30 - 1.25f) < 0.03f && std::fabs(h144 - 1.25f) < 0.02f, "the jump is as high at any frame rate (%.3f / %.3f)", h30,
              h144);
        CHECK(h60 > 1.0f && h30 > 1.0f && h144 > 1.0f, "a jump clears one block");
        CHECK(std::fabs(JumpHeight(map, 1.0f / 60.0f, true) - h60) < 1e-3f, "sprint jumps are as high");
    }

    // steps: half a block is walked up, a full block stops the walk
    {
        map.extra.push_back({ 5.0f, 0.0f, kFloor, 6.0f, 1.0f, kFloor + 0.5f });
        gWorld.SetRaw(12, 0, 10, MakeVox(ID_STONE));
        Body b = Standing(3.5f, 0.5f);
        WalkInput in;
        in.look = Vec3(1, 0, 0);
        in.fwd = 1.0f;
        bool onSlab = false;
        for (int i = 0; i < 60 * 6; ++i) {
            WalkStep(b, in, dt, map, ev);
            if (b.pos.x > 5.4f && b.pos.x < 5.6f)
                onSlab = onSlab || (std::fabs(b.pos.z - (kFloor + 1.5f)) < 1e-3f && b.onGround);
        }
        CHECK(onSlab, "walks up a half block");
        CHECK(std::fabs(b.pos.x - (12.0f - kBodyHalfWidth)) < 1e-3f && b.vel.x == 0.0f, "a full block stops the walk (x %.3f)",
              b.pos.x);
        CHECK(b.onGround && std::fabs(b.pos.z - 11.0f) < 1e-3f, "back on the floor after the half block (z %.3f)", b.pos.z);
        // sprinting into it ends the sprint
        in.sprint = true;
        b = Standing(10.5f, 0.5f);
        bool stopped = false;
        for (int i = 0; i < 120; ++i) {
            WalkStep(b, in, dt, map, ev);
            stopped = stopped || ev.sprintStopped;
        }
        CHECK(stopped, "running into a wall stops the sprint");
        // and a jump gets over it
        in.sprint = false;
        b = Standing(10.5f, 0.5f);
        for (int i = 0; i < 60 * 3; ++i) {
            in.jump = b.pos.x > 11.0f && b.pos.x < 12.5f;
            WalkStep(b, in, dt, map, ev);
        }
        CHECK(b.pos.x > 13.5f, "jumps over a full block (x %.3f)", b.pos.x);
        map.extra.clear();
        gWorld.SetRaw(12, 0, 10, MakeVox(ID_AIR));
    }

    // edges: sneaking never walks off, walking does and the fall hurts by its height
    {
        Body b = Standing(38.5f, 0.5f);
        WalkInput in;
        in.look = Vec3(1, 0, 0);
        in.fwd = 1.0f;
        in.sneak = true;
        for (int i = 0; i < 60 * 8; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(b.onGround && b.pos.x > 41.0f && b.pos.x < 41.0f + kBodyHalfWidth + 1e-3f, "sneaking stops at the edge (x %.3f)",
              b.pos.x);

        for (int x = 41; x <= 60; ++x) // ground 10 blocks below the floor
            for (int y = -2; y <= 2; ++y)
                gWorld.SetRaw(x, y, -1, MakeVox(ID_STONE));
        in.sneak = false;
        in.mortal = true;
        bool landed = false;
        float damage = 0.0f, height = 0.0f;
        gPlayed.clear();
        for (int i = 0; i < 60 * 6 && !landed; ++i) {
            WalkStep(b, in, dt, map, ev);
            if (ev.landed) {
                landed = true;
                damage = ev.fallDamage;
                height = ev.fallHeight;
            }
        }
        CHECK(landed && std::fabs(b.pos.z - 1.0f) < 1e-3f, "falls off the edge to the ground below (z %.3f)", b.pos.z);
        CHECK(std::fabs(height - 10.0f) < 0.01f && std::fabs(damage - 7.0f) < 0.01f, "10 block fall: %.2f high, %.2f damage, want 7",
              height, damage);
        CHECK(Heard(SND_FALL_BIG) == 1 && Heard(SND_FALL_SMALL) == 0, "a long fall ends with the big thud");

        // three blocks do not hurt, creative never does, a wind charge flight neither
        const struct { const char* name; float fall; bool mortal, charge; float want; } kFalls[] = {
            { "3 blocks", 3.0f, true, false, 0.0f },   { "4 blocks", 4.0f, true, false, 1.0f },
            { "23 blocks", 23.0f, true, false, 20.0f }, { "creative", 23.0f, false, false, 0.0f },
            { "wind charge", 23.0f, true, true, 0.0f },
        };
        for (const auto& c : kFalls) {
            Body f;
            f.Place(Vec3(45.5f, 0.5f, 1.0f + c.fall), Vec3(), false);
            f.noFallDamage = c.charge;
            WalkInput still;
            still.look = Vec3(1, 0, 0);
            still.mortal = c.mortal;
            float got = -1.0f;
            gPlayed.clear();
            for (int i = 0; i < 60 * 6 && got < 0.0f; ++i) {
                WalkStep(f, still, dt, map, ev);
                if (ev.landed)
                    got = ev.fallDamage;
            }
            CHECK(std::fabs(got - c.want) < 0.01f, "fall of %s: damage %.2f, want %.2f", c.name, got, c.want);
            CHECK(!f.noFallDamage, "fall of %s: the free landing is used up", c.name);
            if (c.fall > 3.5f) // (exactly three blocks is on the edge)
                CHECK(Heard(SND_FALL_BIG) == (c.want > 0.0f && c.fall > 7.0f) && Heard(SND_FALL_SMALL) == (c.want > 0.0f && c.fall <= 7.0f),
                      "fall of %s: the thud fits the fall", c.name);
        }
    }

    // water: slow, no fall damage, Space swims up, lava burns twice a second
    {
        Pool(ID_WATER);
        Body b = Standing(0.5f, 0.5f);
        WalkInput in;
        in.look = Vec3(1, 0, 0);
        in.fwd = 1.0f;
        in.mortal = true;
        for (int i = 0; i < 180; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(ev.inFluid && ev.fluid == ID_WATER, "in the water");
        CHECK(std::fabs(HorizSpeed(b) - 2.0f) < 0.01f, "swimming speed %.3f, want 2", HorizSpeed(b));
        in.fwd = 0.0f;
        in.jump = true;
        for (int i = 0; i < 180; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(b.pos.z - 1.0f > kFloor + 1.0f, "Space swims up (feet at %.3f)", b.pos.z - 1.0f);

        Body f;
        f.Place(Vec3(20.5f, 0.5f, kFloor + 30.0f), Vec3(), false);
        in.jump = false;
        bool hurt = false;
        gPlayed.clear();
        for (int i = 0; i < 60 * 8; ++i) {
            WalkStep(f, in, dt, map, ev);
            hurt = hurt || ev.fallDamage > 0.0f;
        }
        CHECK(Heard(SND_SPLASH) == 1 && Heard(SND_FALL_BIG) == 0 && !hurt && f.onGround,
              "a fall into water splashes and does not hurt");

        Pool(ID_LAVA);
        b = Standing(0.5f, 0.5f);
        for (int i = 0; i < 180; ++i)
            WalkStep(b, in, dt, map, ev);
        CHECK(ev.fluid == ID_LAVA && ev.inFluid, "in lava (it burns: Survival BurnTick)");
        Pool(ID_AIR);
    }

    // sprinting is latched by the key and ends when the player stops
    {
        bool latch = false;
        CHECK(!UpdateSprintLatch(latch, true, false, false), "no sprint without the key");
        CHECK(UpdateSprintLatch(latch, true, true, false), "the key starts the sprint");
        CHECK(UpdateSprintLatch(latch, true, false, false), "the sprint lasts after the key is released");
        CHECK(!UpdateSprintLatch(latch, false, false, false), "stopping ends the sprint");
        CHECK(!UpdateSprintLatch(latch, false, true, false), "no sprint standing still");
        CHECK(!UpdateSprintLatch(latch, true, true, true), "no sprint when sneaking or hungry");
    }

    // creative flight
    {
        Vec3 v;
        for (int i = 0; i < 180; ++i)
            FlySteer(v, Vec3(0, 1, 0), 1.0f, 0.0f, false, false, false, dt);
        CHECK(std::fabs(v.y - kFlySpeed) < 0.01f && std::fabs(v.x) < 1e-4f && std::fabs(v.z) < 1e-4f, "flying speed %.3f", v.y);
        for (int i = 0; i < 180; ++i)
            FlySteer(v, Vec3(0, 1, 0), 1.0f, 0.0f, true, true, false, dt);
        CHECK(std::fabs(v.y - kFlySprintSpeed) < 0.01f && std::fabs(v.z - kFlyClimbSpeed) < 0.01f, "fast flying %.3f, climbing %.3f",
              v.y, v.z);

        for (int z = 10; z <= 16; ++z)
            for (int y = -2; y <= 2; ++y)
                gWorld.SetRaw(20, y, z, MakeVox(ID_STONE));
        Vec3 pos(15.5f, 0.5f, 13.0f);
        v = Vec3(kFlySprintSpeed, 0, 0);
        float impact = 0.0f;
        bool ground = false;
        for (int i = 0; i < 60; ++i)
            ground = FlyStep(pos, v, dt, false, map, &impact) || ground;
        CHECK(std::fabs(pos.x - (20.0f - kBodyHalfWidth)) < 1e-3f && v.x == 0.0f, "flying into a wall stops there (x %.3f)", pos.x);
        CHECK(std::fabs(impact - kFlySprintSpeed) < 0.01f && !ground, "the wall was hit at %.2f m/s", impact);
        v = Vec3(0, 0, -kFlyClimbSpeed);
        for (int i = 0; i < 60 && !ground; ++i)
            ground = FlyStep(pos, v, dt, false, map, nullptr);
        CHECK(ground && std::fabs(pos.z - 11.0f) < 1e-3f, "flying down lands on the floor (z %.3f)", pos.z);
        for (int z = 10; z <= 16; ++z)
            for (int y = -2; y <= 2; ++y)
                gWorld.SetRaw(20, y, z, MakeVox(ID_AIR));
    }

    // elytra: a dive speeds up, pulling up trades speed for height, a rocket pushes
    {
        Vec3 v;
        float boost = 0.0f, acc = 0.0f;
        const Vec3 down(0.7071f, 0, -0.7071f), level(1, 0, 0), up(0.866f, 0, 0.5f);
        CHECK(ElytraAdvance(v, down, boost, acc, 0.125f) == 2 && std::fabs(acc - 0.5f) < 1e-4f, "elytra runs 20 ticks a second");
        for (int i = 0; i < 60; ++i)
            ElytraTick(v, down, boost);
        const float dive = std::sqrt(v.x * v.x + v.y * v.y), sink = v.z;
        printf("elytra: 3 s dive at 45 degrees -> %.1f m/s forward, %.1f m/s down\n", dive, -sink);
        CHECK(dive > 15.0f && sink < -10.0f, "a dive speeds up (%.1f forward, %.1f down)", dive, sink);
        for (int i = 0; i < 40; ++i)
            ElytraTick(v, up, boost);
        CHECK(v.z > 0.0f, "pulling up after a dive climbs (%.1f up)", v.z);
        v = Vec3(10, 0, 0);
        for (int i = 0; i < 100; ++i)
            ElytraTick(v, level, boost);
        CHECK(v.z < 0.0f && v.z > -4.0f && v.x > 0.0f, "level gliding sinks slowly (%.2f)", v.z);
        boost = 1.0f;
        int ticks = 0;
        while (boost > 0.0f && ticks < 100) {
            ElytraTick(v, level, boost);
            ++ticks;
        }
        CHECK(ticks >= 20 && ticks <= 21 && v.x > 30.0f, "a rocket pushes for a second (%d ticks, %.1f m/s)", ticks, v.x);
        CHECK(GlideImpactDamage(30.0f, 0.0f) == 12.0f && GlideImpactDamage(5.0f, 0.0f) < 0.0f, "flying into a wall hurts by speed");
    }
    gWorld.Clear();
}

static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// ---------------------------------------------------------------- blocks that do things (BlockRules.h)
// The game the tests run in: no map of its own, rain and a player when a test says so; it counts what it is asked
// to do.
struct TestHost : Host {
    bool rain = false;
    bool hasPlayer = false;
    Vec3 player;
    int placed = 0, explosions = 0, doused = 0, thingsBroken = 0;
    bool outdoors = true;
    bool dark = false;
    bool herdGround = false; // animals may appear anywhere, at z = 10
    float sea = -1000.0f;    // its water stands this high everywhere
    bool soil = false;     // its ground is everywhere, and it is earth
    bool blocking = false; // somebody stands in `blocked`
    Int3 blocked;
    bool wall = false;  // a wall segment stands on the ground for the soldier's probe
    Vec3 wallC;         // its centre
    Vec3 wallDir;       // unit vector along it
    float wallHalf = 0; // half its length
    bool ownCarBlocks = false; // the player's own car body stands on lines to his seat
    bool LineBlocked(const Vec3& a, const Vec3& b) override {
        if (ownCarBlocks && vehPoseOn) {
            // the player's own car body stands on any line with exactly one
            // end at his seat: its shell is between an outside eye and the
            // driver (the real game's answer - and the one that made a
            // seated man invisible to the boss's senses on 2026-11-07)
            const bool aSeat = (a - vehSeat).Length() < 1.3f, bSeat = (b - vehSeat).Length() < 1.3f;
            if (aSeat != bSeat)
                return true;
        }
        if (!wall)
            return false;
        const Vec3 w0 = wallC - wallDir * wallHalf, w1 = wallC + wallDir * wallHalf;
        auto cross = [](const Vec3& p, const Vec3& q, const Vec3& r) {
            return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
        };
        const float d1 = cross(w0, w1, a), d2 = cross(w0, w1, b), d3 = cross(a, b, w0), d4 = cross(a, b, w1);
        const bool seg = ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f));
        return seg && std::min(a.z, b.z) < wallC.z + 3.0f && std::max(a.z, b.z) > wallC.z;
    }
    bool LineBlockedSkipVeh(const Vec3& a, const Vec3& b, int veh) override {
        if (veh >= 0 && veh == vehicle) {
            const bool keep = ownCarBlocks;
            ownCarBlocks = false; // his own car is simply not on this line
            const bool r = LineBlocked(a, b);
            ownCarBlocks = keep;
            return r;
        }
        return LineBlocked(a, b);
    }
    bool thing = false;    // something of its own is in front of the player, two metres away
    bool Raining() override { return rain; }
    bool Outdoors() override { return outdoors; }
    bool Dark() override { return dark; }
    bool ownGround = false; // the dug ground's cells are the host's, half as bright
    bool OwnGround(const Int3&, float* depthShade) override {
        *depthShade = 0.5f;
        return ownGround;
    }
    bool SpawnGround(const Vec3& from, Vec3* ground) override {
        if (herdGround)
            *ground = Vec3(from.x, from.y, 10.0f);
        return herdGround;
    }
    // a scripted floor of the host's own (the bridge's collision ground, in the
    // real game): off by default, so every existing test sees the host it always
    // saw. With it on, the ground is groundZVal everywhere except a strip of x,
    // where it is groundStripZ - or nothing at all, when stripFound is false (a
    // probe that slips through a gap in the collision).
    int groundMode = 0;
    float groundZVal = 10.0f, groundStripZ = -30.0f;
    float stripX0 = 1e9f, stripX1 = -1e9f;
    bool stripFound = true;
    bool GroundBelow(const Vec3& from, float, float* z) override {
        if (groundMode == 0)
            return false;
        if (from.x >= stripX0 && from.x <= stripX1) {
            if (!stripFound)
                return false;
            *z = groundStripZ;
        } else {
            *z = groundZVal;
        }
        return true;
    }
    bool WaterLevel(const Vec3&, float* level) override {
        if (sea > -900.0f)
            *level = sea;
        return sea > -900.0f;
    }
    bool Supports(const Int3&) override { return soil; }
    bool SoilBelow(const Vec3& from, bool, float* z) override {
        if (soil && z)
            *z = from.z - 2.0f;
        return soil;
    }
    bool CellBlocked(const Int3& c) override { return blocking && c == blocked; }
    bool Pick(const PickRay& ray, const VoxelHit&, Target& out) override {
        if (!thing)
            return false;
        out.valid = true;
        out.pos = out.key = Int3{ 7, 7, 7 };
        out.point = ray.origin + ray.dir * 2.0f;
        out.normal = Vec3(0, 0, 1);
        out.virtualBlock = ID_DIRT;
        out.hardness = 0.5f;
        return true;
    }
    void BreakTarget() override { ++thingsBroken; }
    void Douse(const Vec3&, float) override { ++doused; }
    bool PlayerPos(Vec3* pos) override {
        if (hasPlayer)
            *pos = player;
        return hasPlayer;
    }
    void FluidPlaced(const Int3&) override { ++placed; }
    void Explosion(const Vec3&, int kind) override {
        ++explosions;
        lastBlast = kind;
    }

    // ---- fighting: its people are balls, and everything beyond x = carX is one of its vehicles (number 77)
    struct Person {
        Vec3 centre;
        float r = 0.5f;
        float hurt = 0.0f;
        int how = -1;
        bool byHostile = false;
        Vec3 push;
    };
    enum { kPlayerNumber = 1000, kCarNumber = 77 };
    std::vector<Person> people;
    float carX = 1e9f, carHurt = 0.0f;
    Vec3 velocity;
    bool onGround = true, falling = false, storm = false, vehiclesOk = true, gustPlayerToo = false;
    int vehicle = -1;     // the one the player sits in
    int shotVehicle = -2; // what the last shot was said to leave from
    int gusts = 0, moves = 0, itemAttacks = 0, itemUses = 0, lastBlast = -1, lastPlaced = -1, lastShooter = -1;
    float playerHurt = 0.0f, boost = 0.0f;
    std::vector<float> fires; // seconds of every fire it was asked for

    HostHit Trace(const Vec3& from, const Vec3& dir, float len, bool playerToo) {
        HostHit h;
        if (dir.x > 1e-6f && from.x < carX && from.x + dir.x * len >= carX) {
            h.hit = true;
            h.dist = (carX - from.x) / dir.x;
            h.point = from + dir * h.dist;
            h.vehicle = kCarNumber;
        }
        if (vehBoxOn) {
            // the scripted vehicle body: a ray/slab test in the vehicle's own
            // frame (along its forward, across, up), the way the game's own
            // collision would answer
            const Vec3 f = vehBoxFwd;
            const Vec3 r(f.y, -f.x, 0.0f);
            const Vec3 oc = from - vehBoxCentre;
            const float o[3] = { oc.Dot(f), oc.Dot(r), oc.z };
            const float d[3] = { dir.Dot(f), dir.Dot(r), dir.z };
            const float hh[3] = { vehBoxHalf.x, vehBoxHalf.y, vehBoxHalf.z };
            float t0 = 0.0f, t1 = len;
            bool ok = true;
            for (int i = 0; i < 3 && ok; ++i) {
                if (std::fabs(d[i]) < 1e-8f) {
                    ok = o[i] >= -hh[i] && o[i] <= hh[i];
                } else {
                    float ta = (-hh[i] - o[i]) / d[i], tb = (hh[i] - o[i]) / d[i];
                    if (ta > tb)
                        std::swap(ta, tb);
                    t0 = std::max(t0, ta);
                    t1 = std::min(t1, tb);
                    ok = t0 <= t1;
                }
            }
            if (ok && t0 > 0.0f && t0 < h.dist) {
                h.hit = true;
                h.dist = t0;
                h.point = from + dir * t0;
                h.vehicle = vehBoxNum;
            }
        }
        auto ball = [&](const Vec3& c, float r, int number) {
            const Vec3 oc = c - from;
            const float along = oc.Dot(dir), off2 = oc.Dot(oc) - along * along;
            if (along < 0.0f || off2 > r * r)
                return;
            const float t = std::max(0.0f, along - std::sqrt(r * r - off2));
            if (t < len && t < h.dist && t < h.beingDist) {
                h.being = number;
                h.beingDist = t;
                h.beingPoint = from + dir * t;
            }
        };
        for (size_t i = 0; i < people.size(); ++i)
            ball(people[i].centre, people[i].r, (int)i);
        if (playerToo && hasPlayer)
            ball(player, 0.5f, kPlayerNumber);
        return h;
    }
    Vec3 PlayerVelocity() override { return velocity; }
    bool PlayerOnGround() override { return onGround; }
    bool PlayerFalling() override { return falling; }
    int PlayerVehicle() override { return vehicle; }
    // the momentum channel's receipt (the 5x update): every throw the
    // core ordered lands here first - the staged sims grade the live
    // path off these fields (which vehicle, what dv, what spin), and
    // the coptest fake below adds the throw's motion to its decoys
    int kickVehCount = 0, kickPedCount = 0;
    int lastKickVeh = -1, lastKickPed = -2;
    Vec3 lastKickDv, lastKickPedDv;
    float lastKickSpin = 0.0f;
    void KickVehicle(int veh, const Vec3& dv, float yawRate) override {
        ++kickVehCount;
        lastKickVeh = veh;
        lastKickDv = dv;
        lastKickSpin = yawRate;
    }
    void KickPed(int ped, const Vec3& dv) override {
        ++kickPedCount;
        lastKickPed = ped;
        lastKickPedDv = dv;
    }
    // the boss's vehicle senses (workstream L): a scripted car to sit in
    bool vehPoseOn = false;
    Vec3 vehCentre, vehFwd{ 0, 1, 0 }, vehSeat, vehVel;
    bool PlayerVehiclePose(Vec3* centre, Vec3* forward, Vec3* driverChest, Vec3* velocity) override {
        if (!vehPoseOn)
            return false;
        *centre = vehCentre;
        *forward = vehFwd;
        *driverChest = vehSeat;
        *velocity = vehVel;
        return true;
    }
    // ---- a scripted vehicle body for traced shots (the smart pack): an
    // oriented box ShotTrace can actually hit, where the carX plane above was
    // a convenience of the arrow days. Its host number is vehBoxNum.
    bool vehBoxOn = false;
    int vehBoxNum = 78;
    Vec3 vehBoxCentre, vehBoxFwd{ 0, 1, 0 }, vehBoxHalf{ 2.3f, 1.0f, 0.85f };
    std::map<int, float> vehHurt;    // vehicle damage taken, per host number
    std::map<int, int> vehTyrePops;  // tyres burst, per host number
    // a fully scripted trace answer, for pinning the boss's shot resolution
    // itself (the geometry belongs to the bridge; the decision is the core's)
    bool traceScriptOn = false;
    HostHit traceScript;
    // a scripted answer for exactly the FIRST trace after arming (the rest
    // fall through to the natural trace): a surface standing in one shot's
    // line, for the penetration proofs. traceCalls is reset when arming.
    bool traceFirstOn = false;
    HostHit traceFirst;
    int traceCalls = 0;
    // the player's vest, for the kevlar proofs (the bridge's own scale:
    // PlayerArmor reads points, WearPlayerArmor takes half hearts)
    float armorPoints = 0.0f, armorWorn = 0.0f;
    float PlayerArmor() override { return armorPoints; }
    void WearPlayerArmor(float halfHearts) override {
        armorWorn += halfHearts;
        armorPoints = std::max(0.0f, armorPoints - halfHearts * 5.0f);
    }
    int tyrePops = 0;
    Vec3 lastTyrePoint;
    void VehicleShotEffect(int veh, const Vec3& hitPoint, int effect) override {
        if (effect == 1) {
            ++tyrePops;
            ++vehTyrePops[veh];
            lastTyrePoint = hitPoint;
        }
    }
    bool VehiclePose(int veh, Vec3* centre, Vec3* forward) override {
        if (vehBoxOn && veh == vehBoxNum) {
            *centre = vehBoxCentre;
            *forward = vehBoxFwd;
            return true;
        }
        if (vehPoseOn && veh == vehicle) {
            *centre = vehCentre;
            *forward = vehFwd;
            return true;
        }
        return false;
    }
    bool aimingAtHim = false;
    bool PlayerAimingAt(const Vec3&, float) override { return aimingAtHim; }
    void MovePlayer(const Vec3& to) override {
        player = to;
        ++moves;
    }
    void HurtPlayer(float halfHearts) override { playerHurt += halfHearts; }
    float AimDistance(const Vec3& origin, const Vec3& dir, float reach, float nothing) override {
        const HostHit h = Trace(origin, dir, reach, false);
        const float dist = std::min(h.hit ? h.dist : 1e9f, h.beingDist);
        return dist < 1e8f ? dist : nothing;
    }
    HostHit BlowTrace(const Vec3& origin, const Vec3& dir, float reach) override { return Trace(origin, dir, reach, false); }
    HostHit ShotTrace(const Vec3& from, const Vec3& dir, float len, const ShotOwner& by) override {
        shotVehicle = by.vehicle;
        ++traceCalls;
        if (traceFirstOn && traceCalls == 1)
            return traceFirst;
        if (traceScriptOn)
            return traceScript;
        return Trace(from, dir, len, by.hostile);
    }
    void HurtBeing(int being, float halfHearts, int how, const ShotOwner* by) override {
        if (being == kPlayerNumber) {
            playerHurt += halfHearts;
            lastShooter = by ? by->shooter : -1;
            return;
        }
        Person& p = people[being];
        p.hurt += halfHearts;
        p.how = how;
        p.byHostile = by && by->hostile;
    }
    void PushBeing(int being, const Vec3& v) override {
        if (being != kPlayerNumber)
            people[being].push += v;
    }
    // the fishing hook: their vehicle tells where on it the hook sits
    int flung = 0;
    Vec3 flingVel;
    HostHit HookTrace(const Vec3& from, const Vec3& dir, float len) override {
        HostHit h = Trace(from, dir, len, false);
        if (h.vehicle >= 0)
            h.local = Vec3(0.0f, 0.0f, h.point.z - 10.0f);
        return h;
    }
    bool HookPoint(const HostHit& on, Vec3* at) override {
        if (on.being >= 0 && on.being < (int)people.size()) {
            *at = people[on.being].centre + Vec3(0, 0, 0.3f);
            return true;
        }
        if (on.vehicle == kCarNumber && carX < 1e8f) {
            *at = Vec3(carX, 0.5f, 10.0f) + on.local;
            return true;
        }
        return false;
    }
    int talks = 0;
    HostHit BeingTrace(const Vec3& origin, const Vec3& dir, float reach) override {
        HostHit h = Trace(origin, dir, reach, false);
        h.hit = false;
        h.vehicle = -1;
        return h;
    }
    bool UseOnBeing(int) override {
        ++talks;
        return true;
    }
    bool Fling(const HostHit& what, const Vec3& v) override {
        Vec3 at;
        if (!HookPoint(what, &at))
            return false;
        ++flung;
        flingVel = v;
        return true;
    }
    void HurtVehicle(int number, float halfHearts) override {
        vehHurt[number] += halfHearts;
        if (number == kCarNumber)
            carHurt += halfHearts;
    }
    void Gust(const Vec3&, float, float, float, bool playerToo) override {
        ++gusts;
        gustPlayerToo = playerToo;
    }
    void Ignite(const Vec3&, float seconds, int) override { fires.push_back(seconds); }
    bool Thunderstorm() override { return storm; }
    int weather = -1, wanted = -1;
    bool killed = false;
    void SetWeather(int kind) override { weather = kind; }
    void SetWanted(int level) override { wanted = level; }
    void KillPlayer() override { killed = true; }
    float hours = 12.0f, clockSet = -1.0f;
    bool clockNextDay = false;
    float ClockHours() override { return hours; }
    void SetClock(float h, bool nextDay) override {
        clockSet = h;
        clockNextDay = nextDay;
    }
    bool PlaceVehicle(int kind, const Vec3&, float) override {
        if (vehiclesOk)
            lastPlaced = kind;
        return vehiclesOk;
    }
    void BoostVehicle(float seconds) override { boost = seconds; }
    bool ItemAttack(int special) override {
        ++itemAttacks;
        return special == SP_WAND;
    }
    bool ItemUse(int special) override {
        ++itemUses;
        return special == SP_WAND;
    }
};

// how many of an item lie on the ground
static int CountDrops(uint16_t id) {
    int n = 0;
    for (const DropEntity& d : gDrops)
        if (d.stack.id == id)
            n += d.stack.count;
    return n;
}

static float gBlockClock = 0.0f;

// lets the blocks live for a while
static void RunBlocks(float seconds) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt) {
        gBlockClock += dt;
        BlocksTick(dt, gBlockClock);
    }
}

// an empty world with a stone floor (top at z = 10) around the origin
static void BlockStage() {
    gWorld.Clear();
    BlockRulesClear();
    gDrops.clear();
    gXpOrbs.clear();
    gPrimedTnt.clear();
    gParticles.clear();
    for (int x = -12; x <= 12; ++x)
        for (int y = -12; y <= 12; ++y)
            gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
}

static int CountBlocks(int block, int r = 12, int z0 = 9, int z1 = 30) {
    int n = 0;
    for (int z = z0; z <= z1; ++z)
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                n += gWorld.GetBlock(x, y, z) == block;
    return n;
}

static void TestBlockRules() {
    TestHost host;
    SetHost(&host);
    McsaSrand(2024);
    // the sound of a block being put down, by what it is made of
    auto placeSound = [](int block) { return (SoundEvent)(SND_PLACE_STONE + Block(block).sound); };

    // a bucket of water on flat ground: seven blocks out, a step lower each block
    {
        BlockStage();
        CHECK(PlaceFluid(ID_WATER, { 0, 0, 10 }) && host.placed == 1, "a bucket of water goes into the air");
        CHECK(!PlaceFluid(ID_WATER, { 0, 0, 9 }), "but not into stone");
        RunBlocks(8.0f);
        bool levels = true;
        for (int d = 1; d <= 7; ++d) {
            const Voxel v = gWorld.Get(d, 0, 10);
            levels = levels && VoxBlock(v) == ID_WATER && (VoxMeta(v) & META_FLUID_LEVEL) == d;
        }
        CHECK(levels, "water gets a step lower with every block from the source");
        CHECK(gWorld.GetBlock(8, 0, 10) == ID_AIR && gWorld.GetBlock(0, -8, 10) == ID_AIR, "and stops after seven blocks");
        CHECK(VoxMeta(gWorld.Get(3, 4, 10)) == 7 && gWorld.GetBlock(4, 4, 10) == ID_AIR, "it spreads as a diamond");
        CHECK(CountBlocks(ID_WATER) == 113, "113 cells of water (%d)", CountBlocks(ID_WATER));

        // how deep it is, where it is, where it flows
        CHECK(FluidAt(Vec3(0.5f, 0.5f, 10.5f)) == ID_WATER && FluidAt(Vec3(0.5f, 0.5f, 10.95f)) == ID_AIR,
              "a source fills 8/9 of its cell");
        CHECK(FluidAt(Vec3(6.5f, 0.5f, 10.1f)) == ID_WATER && FluidAt(Vec3(6.5f, 0.5f, 10.4f)) == ID_AIR, "thin water is thin");
        CHECK(Near(FluidOwnHeight(gWorld.Get(0, 0, 10)), 8.0f / 9.0f) && Near(FluidOwnHeight(gWorld.Get(7, 0, 10)), 1.0f / 9.0f),
              "fluid heights");
        const Vec3 flow = FluidFlowAt({ 3, 0, 10 });
        CHECK(Near(flow.x, 1.0f) && Near(flow.y, 0.0f) && flow.z == 0.0f, "water flows away from the source (%.2f, %.2f)", flow.x,
              flow.y);
        CHECK(FluidFlowAt({ 5, 5, 20 }).Length() == 0.0f, "no fluid, no flow");
        int fluid = 0;
        CHECK(FluidDepth(Vec3(0.5f, 0.5f, 10.0f), 1.8f, &fluid) == 0.5f && fluid == ID_WATER, "a player stands half in it");
        CHECK(FluidDepth(Vec3(0.5f, 0.5f, 12.0f), 1.8f, &fluid) == 0.0f && fluid == ID_AIR, "and not at all above it");

        // the bucket takes the source back; the rest dries up
        int taken = 0;
        CHECK(!TakeFluid({ 3, 0, 10 }, &taken) && TakeFluid({ 0, 0, 10 }, &taken) && taken == ID_WATER,
              "only a source fills a bucket");
        RunBlocks(12.0f);
        CHECK(CountBlocks(ID_WATER) == 0, "without its source the water dries up (%d cells left)", CountBlocks(ID_WATER));
    }

    // water falls, and two sources make a third
    {
        BlockStage();
        PlaceFluid(ID_WATER, { 0, 0, 14 });
        RunBlocks(6.0f);
        bool column = true;
        for (int z = 10; z <= 13; ++z) {
            const Voxel v = gWorld.Get(0, 0, z);
            column = column && VoxBlock(v) == ID_WATER && (VoxMeta(v) & META_FLUID_FALLING);
        }
        CHECK(column && gWorld.GetBlock(1, 0, 14) == ID_AIR, "water falls straight down before it spreads");
        CHECK(VoxBlock(gWorld.Get(1, 0, 10)) == ID_WATER && (VoxMeta(gWorld.Get(1, 0, 10)) & 0xF) == 1, "and spreads at the bottom");
        CHECK(FluidAt(Vec3(0.5f, 0.5f, 12.99f)) == ID_WATER, "a falling column is full");

        BlockStage();
        PlaceFluid(ID_WATER, { 0, 0, 10 });
        PlaceFluid(ID_WATER, { 2, 0, 10 });
        RunBlocks(3.0f);
        CHECK(gWorld.Get(1, 0, 10) == MakeVox(ID_WATER, 0), "two sources make a third between them");
    }

    // lava is slow, short, and hardens where it meets water
    {
        BlockStage();
        PlaceFluid(ID_LAVA, { 0, 0, 10 });
        RunBlocks(1.0f);
        CHECK(gWorld.GetBlock(1, 0, 10) == ID_AIR, "lava takes its time");
        RunBlocks(14.0f);
        CHECK(VoxMeta(gWorld.Get(1, 0, 10)) == 2 && VoxMeta(gWorld.Get(3, 0, 10)) == 6 && gWorld.GetBlock(4, 0, 10) == ID_AIR,
              "lava reaches three blocks");
        gPlayed.clear();
        gParticles.clear();
        PlaceFluid(ID_WATER, { 0, 0, 11 });
        RunBlocks(6.0f);
        CHECK(gWorld.GetBlock(0, 0, 10) == ID_OBSIDIAN, "water on a lava source makes obsidian");
        CHECK(Heard(SND_LAVA_EXTINGUISH) > 0 && (int)gParticles.size() == 8 * Heard(SND_LAVA_EXTINGUISH),
              "with a hiss and a puff of smoke each time (%d)", Heard(SND_LAVA_EXTINGUISH));
        CHECK(CountBlocks(ID_LAVA) == 0 && CountBlocks(ID_COBBLESTONE) + CountBlocks(ID_STONE) > 0,
              "and flowing lava turns to stone (%d lava left)", CountBlocks(ID_LAVA));
    }

    // sand and gravel fall; plants and fire need something under them
    {
        BlockStage();
        gPlayed.clear();
        gWorld.Set(0, 0, 15, MakeVox(ID_SAND));
        gWorld.Set(0, 0, 16, MakeVox(ID_GRAVEL));
        gWorld.Set(3, 0, 15, MakeVox(ID_STONE));
        RunBlocks(0.05f);
        CHECK(gWorld.GetBlock(0, 0, 15) == ID_AIR && FallingBlocks().size() >= 1, "sand lets go");
        RunBlocks(3.0f);
        CHECK(gWorld.GetBlock(0, 0, 10) == ID_SAND && gWorld.GetBlock(0, 0, 11) == ID_GRAVEL && FallingBlocks().empty(),
              "sand and gravel land on top of each other");
        CHECK(gPlayed.size() == 2 && Heard(placeSound(ID_SAND)) >= 1 && Heard(placeSound(ID_GRAVEL)) >= 1 && gPlayed[0].placed &&
                  std::fabs(gPlayed[0].pos.z - 10.5f) < 1e-3f,
              "each with the sound of what it is made of (%d sounds)", (int)gPlayed.size());
        CHECK(gWorld.GetBlock(3, 0, 15) == ID_STONE, "stone stays in the air");
        gWorld.Set(0, 0, 9, MakeVox(ID_AIR)); // the floor under the pile goes
        gWorld.SetRaw(0, 0, 3, MakeVox(ID_STONE));
        RunBlocks(3.0f);
        CHECK(gWorld.GetBlock(0, 0, 4) == ID_SAND && gWorld.GetBlock(0, 0, 5) == ID_GRAVEL && gWorld.GetBlock(0, 0, 10) == ID_AIR,
              "the pile falls when the ground under it goes");

        gDrops.clear();
        gWorld.Set(5, 5, 10, MakeVox(ID_POPPY));
        RunBlocks(0.5f);
        CHECK(gWorld.GetBlock(5, 5, 10) == ID_POPPY, "a flower stands on the ground");
        gWorld.Set(5, 5, 9, MakeVox(ID_AIR));
        RunBlocks(0.5f);
        CHECK(gWorld.GetBlock(5, 5, 10) == ID_AIR && gDrops.size() == 1 && CountDrops(ID_POPPY) == 1,
              "and drops when the ground goes");
        gWorld.Set(6, 6, 10, MakeVox(ID_POPPY));
        PlaceFluid(ID_WATER, { 6, 5, 10 });
        RunBlocks(2.0f);
        CHECK(VoxBlock(gWorld.Get(6, 6, 10)) == ID_WATER && CountDrops(ID_POPPY) == 2, "water washes flowers away");

        // sand that lands where a fire burns has no place: it drops as an item
        BlockStage();
        gPlayed.clear();
        CHECK(PlaceFire({ 0, 0, 10 }) && FireAt(Vec3(0.5f, 0.5f, 10.5f)) && !FireAt(Vec3(1.5f, 0.5f, 10.5f)), "fire on the ground");
        gWorld.Set(0, 0, 13, MakeVox(ID_SAND));
        RunBlocks(0.9f);
        CHECK(CountDrops(ID_SAND) == 1 && Heard(placeSound(ID_SAND)) == 0 && CountBlocks(ID_SAND) == 0,
              "sand falling into a fire drops as an item (%d on the ground)", CountDrops(ID_SAND));
    }

    // fire: needs ground or fuel, burns wood, lights TNT, dies in the rain
    {
        BlockStage();
        CHECK(!PlaceFire({ 0, 0, 15 }), "no fire in thin air");
        CHECK(!PlaceFire({ 0, 0, 9 }), "no fire inside a block");
        gWorld.SetRaw(0, 0, 16, MakeVox(ID_OAK_PLANKS));
        CHECK(PlaceFire({ 0, 0, 15 }), "fire next to wood");
        CHECK(FireIgnite(ID_OAK_PLANKS) > 0 && FireBurn(ID_OAK_PLANKS) > 0 && FireBurn(ID_TNT) > 0 && FireIgnite(ID_STONE) == 0,
              "what burns");

        // bare stone: the fire dies down by itself
        BlockStage();
        PlaceFire({ 0, 0, 10 });
        RunBlocks(300.0f);
        CHECK(CountBlocks(ID_FIRE) == 0, "fire on bare stone dies down");

        // a wooden hut burns
        BlockStage();
        for (int x = -2; x <= 2; ++x)
            for (int y = -2; y <= 2; ++y)
                for (int z = 10; z <= 13; ++z)
                    if (std::abs(x) == 2 || std::abs(y) == 2 || z == 13)
                        gWorld.SetRaw(x, y, z, MakeVox(ID_OAK_PLANKS));
        const int wood = CountBlocks(ID_OAK_PLANKS);
        CHECK(PlaceFire({ 0, 1, 10 }) || PlaceFire({ 1, 1, 10 }), "fire in the hut");
        gWorld.SetRaw(1, 1, 10, MakeVox(ID_TNT));
        PlaceFire({ 1, 0, 10 });
        gPrimedTnt.clear();
        gPlayed.clear();
        RunBlocks(600.0f);
        CHECK(CountBlocks(ID_OAK_PLANKS) < wood / 2, "the hut burns down (%d of %d planks left)", CountBlocks(ID_OAK_PLANKS), wood);
        CHECK(gPrimedTnt.size() == 1 && Heard(SND_FUSE) == 1 && CountBlocks(ID_TNT) == 0, "and the fire lights the TNT");

        // rain
        BlockStage();
        for (int x = -3; x <= 3; ++x)
            for (int y = -3; y <= 3; ++y)
                gWorld.SetRaw(x, y, 10, MakeVox(ID_OAK_PLANKS));
        for (int x = -3; x <= 3; ++x)
            PlaceFire({ x, 0, 11 });
        host.rain = true;
        RunBlocks(40.0f);
        host.rain = false;
        CHECK(CountBlocks(ID_FIRE) == 0 && CountBlocks(ID_OAK_PLANKS) > 30, "rain puts the fire out (%d fires left)", CountBlocks(ID_FIRE));

        // lava lights what burns around it
        BlockStage();
        for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
                gWorld.SetRaw(x, y, 12, MakeVox(ID_OAK_PLANKS));
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_LAVA));
        RunBlocks(60.0f);
        CHECK(CountBlocks(ID_FIRE) > 0 || CountBlocks(ID_OAK_PLANKS) < 9, "lava sets the wood above it on fire");

        // water puts fire out
        BlockStage();
        gPlayed.clear();
        PlaceFire({ 1, 0, 10 });
        PlaceFluid(ID_WATER, { 0, 0, 10 });
        RunBlocks(1.0f);
        CHECK(VoxBlock(gWorld.Get(1, 0, 10)) == ID_WATER && Heard(SND_FIRE_EXTINGUISH) == 1, "water puts fire out, with a hiss");
    }

    // saplings grow into trees of their kind
    {
        BlockStage();
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_GRASS_BLOCK));
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_OAK_SAPLING));
        CHECK(gWorld.ticking.count({ 0, 0, 10 }) == 1, "a sapling waits for its random tick");
        gWorld.SetRaw(0, 0, 13, MakeVox(ID_STONE));
        CHECK(!GrowSapling({ 0, 0, 10 }) && gWorld.GetBlock(0, 0, 10) == ID_OAK_SAPLING, "no room, no tree");
        gWorld.SetRaw(0, 0, 13, MakeVox(ID_AIR));
        gPlayed.clear();
        CHECK(GrowSapling({ 0, 0, 10 }) && gParticles.size() == 15 && Heard(SND_PLACE_GRASS) == 1, "an oak grows, with sparkles");
        const int logs = CountBlocks(ID_OAK_LOG), leaves = CountBlocks(ID_OAK_LEAVES);
        CHECK(logs >= 4 && logs <= 6 && leaves > 30 && gWorld.GetBlock(0, 0, 10) == ID_OAK_LOG, "oak: %d logs, %d leaves", logs, leaves);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_DIRT && gWorld.ticking.count({ 0, 0, 10 }) == 0, "the grass under it becomes dirt");
        CHECK(!GrowSapling({ 0, 0, 10 }), "a log is no sapling");

        BlockStage();
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_SPRUCE_SAPLING));
        CHECK(GrowSapling({ 0, 0, 10 }) && CountBlocks(ID_SPRUCE_LOG) >= 6 && CountBlocks(ID_SPRUCE_LEAVES) > 10 &&
                  CountBlocks(ID_OAK_LOG) == 0,
              "a spruce grows from a spruce sapling");

        // left alone, a sapling grows by itself (one chance in fifty a second)
        BlockStage();
        gPlayed.clear();
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_BIRCH_SAPLING));
        RunBlocks(600.0f);
        CHECK(Heard(SND_PLACE_GRASS) == 1 && CountBlocks(ID_BIRCH_LOG) >= 5, "a sapling grows by itself in time");

        CHECK(IsSoil(ID_GRASS_BLOCK) && IsSoil(ID_DIRT) && !IsSoil(ID_STONE) && !IsSoil(ID_SAND), "soil");
        for (int i = 0; i < 50; ++i)
            CHECK(IsPlantBlock(RandomFlower()), "flowers are plants");
        CHECK(PlantCellOnGround(10.0f) == 10 && PlantCellOnGround(9.8f) == 10 && PlantCellOnGround(9.7f) == 9,
              "a plant on uneven ground sits in the cell its foot is in");
    }

    SetHost(nullptr);
    BlockStage();
    gWorld.Clear();
}

// ---------------------------------------------------------------- survival (Survival.h)
static void ResetSurvival() {
    gSurvival = Survival();
    gInv = PlayerInventory();
}

static ItemStack Stack(uint16_t id, int count = 1) {
    ItemStack s;
    s.id = id;
    s.count = (uint8_t)count;
    return s;
}

static void TestSurvival() {
    const float kFull = 100.0f; // the host's full health here: one Minecraft point is 5
    Survival& s = gSurvival;

    for (int i = 0; i < 1000; ++i) {
        const float r = Rand01();
        CHECK(r >= 0.0f && r < 1.0f, "Rand01 out of range: %f", r);
    }

    // experience: 7 points for the first level, more for each one after
    {
        ResetSurvival();
        gPlayed.clear();
        CHECK(XpToNextLevel(0) == 7 && XpToNextLevel(14) == 35 && XpToNextLevel(15) == 37 && XpToNextLevel(30) == 112 &&
                  XpToNextLevel(31) == 121,
              "experience needed per level");
        CHECK(!AddXp(7) && s.xpLevel == 1 && s.xpProgress == 0.0f && s.xpTotal == 7, "7 points make level 1 (level %d)", s.xpLevel);
        CHECK(!AddXp(0) && !AddXp(-5) && s.xpTotal == 7, "no points, no experience");
        CHECK(Heard(SND_LEVELUP) == 0, "no fanfare for level 1");
        CHECK(AddXp(9 + 11 + 13 + 15) && s.xpLevel == 5 && Heard(SND_LEVELUP) == 1, "every fifth level is announced (level %d)",
              s.xpLevel);
        CHECK(!AddXp(3) && s.xpLevel == 5 && Near(s.xpProgress, 3.0f / 17.0f), "progress towards level 6: %.3f", s.xpProgress);
        CHECK(s.xpTotal == 7 + 48 + 3, "total experience %d", s.xpTotal);
        int sum = 0, orbs = 0;
        for (int left = 100; left > 0; ++orbs) {
            const int v = XpOrbSize(left);
            left -= v;
            sum += v;
        }
        CHECK(sum == 100 && orbs == 4, "100 points split into 73 + 17 + 7 + 3 (%d orbs)", orbs);
        CHECK(XpOrbSize(5000) == 2477 && XpOrbSize(2) == 1 && XpOrbSize(1) == 1, "orb sizes");
        for (int i = 0; i < 300; ++i) {
            const int coal = XpForBlock(ID_COAL_ORE), diamond = XpForBlock(ID_DIAMOND_ORE);
            CHECK(coal >= 0 && coal <= 2 && diamond >= 3 && diamond <= 7, "ore experience: coal %d, diamond %d", coal, diamond);
        }
        CHECK(XpForBlock(ID_STONE) == 0 && XpForBlock(ID_IRON_ORE) == 0, "only some ores give experience");
    }

    // hunger: exhaustion eats saturation first, then food
    {
        ResetSurvival();
        float h = kFull;
        SurvivalEvents ev;
        s.exhaustion = 4.5f;
        HungerTick(0.1f, h, kFull, ev);
        CHECK(s.saturation == 4.0f && s.food == 20.0f && Near(s.exhaustion, 0.5f), "exhaustion costs saturation first");
        s.saturation = 0.0f;
        s.exhaustion = 8.0f;
        HungerTick(0.1f, h, kFull, ev);
        CHECK(s.food == 18.0f && Near(s.exhaustion, 0.0f), "then food (%.1f)", s.food);

        // a full stomach heals one point every four seconds, and that costs hunger
        ResetSurvival();
        h = 50.0f;
        int checks = 0;
        for (int i = 0; i < 45; ++i)
            checks += HungerTick(0.1f, h, kFull, ev);
        CHECK(checks == 1 && h == 55.0f && s.saturation == 4.0f && Near(s.exhaustion, 2.0f), "healing by food: health %.1f", h);
        // nothing to heal: no cost
        ResetSurvival();
        h = kFull;
        for (int i = 0; i < 100; ++i)
            HungerTick(0.1f, h, kFull, ev);
        CHECK(h == kFull && s.exhaustion == 0.0f && s.saturation == 5.0f, "full health costs no hunger");
        // not full enough: no healing
        ResetSurvival();
        s.food = 17.0f;
        h = 50.0f;
        for (int i = 0; i < 100; ++i)
            HungerTick(0.1f, h, kFull, ev);
        CHECK(h == 50.0f, "17 food does not heal");

        // an empty stomach starves, but not to death
        ResetSurvival();
        s.food = s.saturation = 0.0f;
        h = 30.0f;
        int starved = 0;
        gPlayed.clear();
        for (int i = 0; i < 400; ++i) {
            SurvivalEvents e;
            HungerTick(0.1f, h, kFull, e);
            starved += e.starved;
        }
        CHECK(h == 10.0f && starved == 4 && Heard(SND_HURT) == 4, "starving stops at a tenth of full health (%.1f after %d hits)", h,
              starved);

        s.food = 3.0f;
        h = 40.0f;
        CreativeTick(h, kFull);
        float dead = 0.0f;
        CreativeTick(dead, kFull);
        CHECK(s.food == 20.0f && h == kFull && dead == 0.0f, "creative: fed and healthy, but the dead stay dead");
        s.food = 6.0f;
        const bool hungry = TooHungryToSprint();
        s.food = 7.0f;
        CHECK(hungry && !TooHungryToSprint(), "no sprinting at 6 food or less");
        s.food = 3.0f;
        s.saturation = 0.0f;
        s.exhaustion = 2.0f;
        s.xpLevel = 7;
        s.Respawn();
        CHECK(s.food == 20.0f && s.saturation == 5.0f && s.xpLevel == 7, "respawn fills the stomach");
        s.NewGame();
        CHECK(s.xpLevel == 0 && s.exhaustion == 0.0f, "a new game starts without experience");
    }

    // status effects
    {
        ResetSurvival();
        AddEffect(EFFECT_POISON, 10.0f, 0);
        AddEffect(EFFECT_POISON, 5.0f, 0);
        CHECK(s.effects[EFFECT_POISON].time == 10.0f, "the same strength keeps the longer time");
        AddEffect(EFFECT_POISON, 3.0f, 1);
        CHECK(s.effects[EFFECT_POISON].time == 3.0f && s.effects[EFFECT_POISON].amp == 1, "a stronger effect replaces a weaker one");
        AddEffect(EFFECT_POISON, 60.0f, 0);
        CHECK(s.effects[EFFECT_POISON].amp == 1, "a weaker effect does not");
        AddEffect(EFFECT_HUNGER, 0.0f, 0);
        AddEffect(-1, 5.0f, 0);
        AddEffect(EFFECT_COUNT, 5.0f, 0);
        CHECK(!HasEffect(EFFECT_HUNGER), "no time, no effect");
        AddEffect(EFFECT_ABSORPTION, 120.0f, 0);
        CHECK(s.absorption == 4.0f, "absorption I: two yellow hearts");
        AddEffect(EFFECT_ABSORPTION, 120.0f, 3);
        CHECK(s.absorption == 16.0f, "absorption IV: eight yellow hearts");
        RemoveEffect(EFFECT_ABSORPTION);
        CHECK(s.absorption == 0.0f && !HasEffect(EFFECT_ABSORPTION) && HasEffect(EFFECT_POISON), "removing one effect");
        ClearEffects();
        CHECK(!HasEffect(EFFECT_POISON), "clearing all effects");

        // regeneration II: a point every 1.25 s
        float h = 50.0f;
        SurvivalEvents ev;
        AddEffect(EFFECT_REGENERATION, 5.0f, 1);
        for (int i = 0; i < 120; ++i)
            EffectsTick(0.05f, h, kFull, ev);
        CHECK(h >= 65.0f && h <= 70.0f && !HasEffect(EFFECT_REGENERATION) && ev.expired == (1u << EFFECT_REGENERATION),
              "regeneration II for 5 s: health %.1f", h);
        h = 99.0f;
        AddEffect(EFFECT_REGENERATION, 5.0f, 1);
        for (int i = 0; i < 120; ++i)
            EffectsTick(0.05f, h, kFull, ev);
        CHECK(h == kFull, "regeneration stops at full health (%.1f)", h);

        // poison hurts past the armour, but never kills
        ev = SurvivalEvents();
        h = 12.0f;
        AddEffect(EFFECT_POISON, 30.0f, 0);
        for (int i = 0; i < 400; ++i)
            EffectsTick(0.05f, h, kFull, ev);
        CHECK(h == 7.0f && ev.directDamage == 5.0f && HasEffect(EFFECT_POISON), "poison leaves the last heart (health %.1f)", h);
        ClearEffects();

        // hunger tires: 0.1 a second per level
        AddEffect(EFFECT_HUNGER, 30.0f, 2);
        s.exhaustion = 0.0f;
        for (int i = 0; i < 50; ++i)
            EffectsTick(0.1f, h, kFull, ev);
        CHECK(Near(s.exhaustion, 1.5f, 1e-3f), "hunger III for 5 s: exhaustion %.3f", s.exhaustion);

        // resistance takes a fifth off a hit, the yellow hearts take it first
        ResetSurvival();
        CHECK(AbsorbDamage(50.0f, kFull) == 50.0f && AbsorbDamage(0.0f, kFull) == 0.0f && AbsorbDamage(-3.0f, kFull) == 0.0f,
              "no effects, the whole hit");
        AddEffect(EFFECT_RESISTANCE, 60.0f, 0);
        CHECK(Near(AbsorbDamage(50.0f, kFull), 40.0f), "resistance I: %.2f of 50", AbsorbDamage(50.0f, kFull));
        ResetSurvival();
        AddEffect(EFFECT_ABSORPTION, 60.0f, 0);
        CHECK(AbsorbDamage(5.0f, kFull) == 0.0f && Near(s.absorption, 3.0f), "a small hit only costs a yellow half heart");
        CHECK(Near(AbsorbDamage(30.0f, kFull), 15.0f) && s.absorption == 0.0f && !HasEffect(EFFECT_ABSORPTION),
              "a big hit uses the yellow hearts up");
    }

    // air: fifteen seconds of it, then a heart a second
    {
        ResetSurvival();
        float h = kFull;
        SurvivalEvents ev;
        for (int i = 0; i < 150; ++i)
            BreathTick(0.1f, true, true, h, kFull, ev);
        CHECK(!ev.drowned && h == kFull && Near(s.air, 0.0f, 0.01f), "15 s under water: out of air (%.1f)", s.air);
        for (int i = 0; i < 10; ++i)
            BreathTick(0.1f, true, true, h, kFull, ev);
        CHECK(ev.drowned && h == 90.0f && ev.directDamage == 10.0f, "then drowning: health %.1f", h);
        for (int i = 0; i < 10; ++i)
            BreathTick(0.1f, true, true, h, kFull, ev);
        CHECK(h == 80.0f, "a hit every second (%.1f)", h);
        BreathTick(1.0f, false, true, h, kFull, ev);
        CHECK(Near(s.air, 80.0f, 0.01f), "air comes back above water (%.1f)", s.air);
        for (int i = 0; i < 5; ++i)
            BreathTick(1.0f, false, true, h, kFull, ev);
        CHECK(s.air == kMaxAir, "up to ten bubbles");
        s.air = 100.0f;
        BreathTick(0.1f, true, false, h, kFull, ev);
        CHECK(s.air == kMaxAir, "creative needs no air");
        s.air = -19.0f;
        h = 5.0f;
        BreathTick(0.1f, true, true, h, kFull, ev);
        CHECK(h == 0.0f, "drowning kills");
    }

    // armour: 4% a point, at most 80%; hits wear it out
    {
        ResetSurvival();
        CHECK(ArmorPoints() == 0 && ArmorBlock() == 0.0f, "no armour");
        gInv.armor[ARMOR_HEAD] = Stack(ID_IRON_HELMET);
        gInv.armor[ARMOR_CHEST] = Stack(ID_IRON_CHESTPLATE);
        gInv.armor[ARMOR_LEGS] = Stack(ID_IRON_LEGGINGS);
        gInv.armor[ARMOR_FEET] = Stack(ID_IRON_BOOTS);
        CHECK(ArmorPoints() == 15 && Near(ArmorBlock(), 0.6f), "iron armour: %d points", ArmorPoints());
        CHECK(WearArmor(3) == 0 && gInv.armor[ARMOR_HEAD].damage == 3 && gInv.armor[ARMOR_FEET].damage == 3, "a hit wears every piece");
        gPlayed.clear();
        const int broke = WearArmor(Item(ID_IRON_BOOTS).durability - 3);
        CHECK(broke >= 1 && gInv.armor[ARMOR_FEET].Empty() && Heard(SND_TOOL_BREAK) == broke, "worn out pieces break (%d did)", broke);
        gInv.armor[ARMOR_HEAD] = Stack(ID_DIAMOND_HELMET);
        gInv.armor[ARMOR_CHEST] = Stack(ID_DIAMOND_CHESTPLATE);
        gInv.armor[ARMOR_LEGS] = Stack(ID_DIAMOND_LEGGINGS);
        gInv.armor[ARMOR_FEET] = Stack(ID_DIAMOND_BOOTS);
        CHECK(ArmorPoints() == 20 && Near(ArmorBlock(), 0.8f), "diamond armour: %d points", ArmorPoints());
        gInv = PlayerInventory();
        gInv.armor[ARMOR_CHEST] = Stack(ID_ELYTRA);
        CHECK(WearArmor(30000) == 0 && gInv.armor[ARMOR_CHEST].damage == 0, "hits do not wear the elytra");
    }

    // totem of undying
    {
        ResetSurvival();
        gInv.offhand = Stack(ID_TOTEM_OF_UNDYING);
        float h = 20.0f;
        CHECK(!UseTotem(h, kFull) && !gInv.offhand.Empty(), "the totem waits for the last moment");
        h = 10.0f;
        AddEffect(EFFECT_POISON, 30.0f, 0);
        gPlayed.clear();
        CHECK(UseTotem(h, kFull) && h == 50.0f && gInv.offhand.Empty() && Heard(SND_TOTEM) == 1, "the totem saves: health %.1f", h);
        CHECK(!HasEffect(EFFECT_POISON) && HasEffect(EFFECT_REGENERATION) && HasEffect(EFFECT_FIRE_RESISTANCE) && s.absorption == 8.0f,
              "and swaps the effects");
        h = 10.0f;
        CHECK(!UseTotem(h, kFull) && h == 10.0f, "once");
        gInv.Held() = Stack(ID_TOTEM_OF_UNDYING);
        gInv.offhand = Stack(ID_TOTEM_OF_UNDYING);
        CHECK(UseTotem(h, kFull) && gInv.Held().Empty() && !gInv.offhand.Empty(), "the main hand's totem goes first");
    }

    // eating and drinking
    {
        ResetSurvival();
        const ItemDef& bread = Item(ID_BREAD);
        CHECK(bread.food == 5 && Item(ID_MILK_BUCKET).special == SP_MILK, "bread is worth %d", bread.food);
        s.food = 10.0f;
        CHECK(CanEat(ID_BREAD, true) && !CanEat(ID_BREAD, false) && !CanEat(ID_STONE, true), "food is for the hungry survivor");
        s.food = 20.0f;
        CHECK(!CanEat(ID_BREAD, true) && CanEat(ID_GOLDEN_APPLE, true) && CanEat(ID_GOLDEN_APPLE, false) && CanEat(ID_MILK_BUCKET, false),
              "golden apples and milk always go down");

        s.food = 10.0f;
        s.saturation = 0.0f;
        gInv.Held() = Stack(ID_BREAD, 2);
        FinishEating(true);
        CHECK(s.food == 15.0f && Near(s.saturation, std::min(15.0f, bread.saturation)) && gInv.Held().count == 1,
              "bread: food %.1f, saturation %.1f", s.food, s.saturation);
        FinishEating(true);
        CHECK(s.food == 20.0f && gInv.Held().Empty() && s.saturation <= s.food, "the last one empties the hand");
        gInv.Held() = Stack(ID_COOKED_BEEF);
        FinishEating(true);
        CHECK(s.food == 20.0f && s.saturation <= 20.0f, "the stomach holds 20");

        gInv.Held() = Stack(ID_GOLDEN_APPLE);
        FinishEating(false);
        CHECK(gInv.Held().count == 1 && HasEffect(EFFECT_REGENERATION) && s.absorption == 4.0f, "golden apple (creative keeps it)");
        gInv.Held() = Stack(ID_ENCHANTED_GOLDEN_APPLE);
        FinishEating(true);
        CHECK(gInv.Held().Empty() && s.absorption == 16.0f && HasEffect(EFFECT_RESISTANCE) && HasEffect(EFFECT_FIRE_RESISTANCE),
              "enchanted golden apple");
        gInv.Held() = Stack(ID_MILK_BUCKET);
        FinishEating(true);
        CHECK(!HasEffect(EFFECT_RESISTANCE) && s.absorption == 0.0f && gInv.Held().id == ID_BUCKET && gInv.Held().count == 1,
              "milk clears the effects and leaves the bucket");
        AddEffect(EFFECT_POISON, 30.0f, 0);
        AddEffect(EFFECT_HUNGER, 30.0f, 0);
        gInv.Held() = Stack(ID_HONEY_BOTTLE);
        FinishEating(true);
        CHECK(!HasEffect(EFFECT_POISON) && HasEffect(EFFECT_HUNGER), "honey cures poison only");
        gInv.Held() = Stack(ID_SPIDER_EYE);
        FinishEating(true);
        CHECK(HasEffect(EFFECT_POISON), "spider eyes poison");

        // eating takes 1.6 s of holding "use", with chewing on the way and a burp at the end
        ResetSurvival();
        gPlayed.clear();
        s.food = 10.0f;
        gInv.Held() = Stack(ID_BREAD, 2);
        EatResult r;
        int meals = 0;
        for (int i = 0; i < 30; ++i) { // 1.5 s
            r = EatingTick(0.05f, true, true);
            meals += r.finished != 0;
        }
        CHECK(r.eating && meals == 0 && s.food == 10.0f && Heard(SND_EAT) >= 6 && Heard(SND_EAT) <= 8 && Heard(SND_BURP) == 0,
              "chewing for 1.5 s (%d bites heard)", Heard(SND_EAT));
        r = EatingTick(0.05f, false, true);
        CHECK(!r.eating && s.eatTimer == 0.0f, "letting go starts over");
        int eaten = 0;
        for (int i = 0; i < 40; ++i) { // 2 s
            r = EatingTick(0.05f, true, true);
            if (r.finished) {
                eaten = r.finished;
                ++meals;
            }
        }
        CHECK(meals == 1 && eaten == ID_BREAD && s.food == 15.0f && gInv.Held().count == 1 && Heard(SND_BURP) == 1,
              "one bread eaten after 1.6 s (food %.1f)", s.food);
        gInv.Held() = Stack(ID_STONE);
        CHECK(!EatingTick(0.05f, true, true).eating, "stone is not food");
        gInv.Held().Clear();
        CHECK(!EatingTick(0.05f, true, true).eating, "nor is an empty hand");
        gInv.Held() = Stack(ID_MILK_BUCKET);
        eaten = 0;
        for (int i = 0; i < 34 && !eaten; ++i)
            eaten = EatingTick(0.05f, true, false).finished;
        CHECK(eaten == ID_MILK_BUCKET && Heard(SND_DRINK) >= 5 && Heard(SND_BURP) == 2, "milk is drunk, not chewed");

        McsaSrand(12345);
        int sick = 0;
        for (int i = 0; i < 2000; ++i) {
            ClearEffects();
            gInv.Held() = Stack(ID_ROTTEN_FLESH);
            FinishEating(true);
            sick += HasEffect(EFFECT_HUNGER);
        }
        CHECK(sick > 1500 && sick < 1700, "rotten flesh makes hungry 4 times in 5 (%d of 2000)", sick);
    }
    ResetSurvival();
}

// ---------------------------------------------------------------- loose things (Entities.h, Particles.h, Host.h)
// lets the items, the TNT, the particles and the blocks live for a while
static void RunEntities(float seconds, const Vec3* collector = nullptr, float reach = 1.6f) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt) {
        DropsTick(dt, collector, reach);
        TntTick(dt);
        ParticlesTick(dt);
        gBlockClock += dt;
        BlocksTick(dt, gBlockClock);
    }
}

static void TestEntities() {
    TestHost host;
    SetHost(&host);
    McsaSrand(77);
    const Vec3 still(0, 0, 0.01f); // (an item spawned without any speed hops off in a random direction)

    // the ground under a point
    {
        BlockStage();
        float z = 0.0f;
        CHECK(GroundBelow(Vec3(0.5f, 0.5f, 13.0f), 5.0f, &z) && z == 10.0f, "the floor is the ground (%.2f)", z);
        CHECK(!GroundBelow(Vec3(0.5f, 0.5f, 13.0f), 2.0f, &z), "but not when it is further down than asked for");
        CHECK(!GroundBelow(Vec3(30.5f, 0.5f, 13.0f), 40.0f, &z), "no floor, no ground");
        gWorld.SetRaw(0, 0, 11, MakeVox(ID_STONE));
        CHECK(GroundBelow(Vec3(0.5f, 0.5f, 13.0f), 5.0f, &z) && z == 12.0f, "a block on the floor is higher ground");
    }

    // items on the ground
    {
        BlockStage();
        ResetSurvival();
        gPlayed.clear();
        SpawnDrop(Vec3(0.5f, 0.5f, 14.0f), Stack(ID_DIAMOND, 3), still);
        RunEntities(3.0f);
        CHECK(gDrops.size() == 1 && Near(gDrops[0].pos.z, 10.0f, 1e-3f) && gDrops[0].stack.count == 3,
              "an item falls to the floor and stays there");
        const Vec3 away(8.5f, 0.5f, 11.0f), close(1.0f, 0.5f, 11.0f);
        RunEntities(1.0f, &away);
        CHECK(gDrops.size() == 1 && gInv.CountOf(ID_DIAMOND) == 0, "too far away to pick up");
        RunEntities(0.2f, &close);
        CHECK(gDrops.empty() && gInv.CountOf(ID_DIAMOND) == 3 && Heard(SND_PICKUP) == 1, "walking up to it picks it up");

        SpawnDrop(Vec3(1.0f, 0.5f, 10.2f), Stack(ID_STICK, 1), still, 1.5f);
        RunEntities(1.0f, &close);
        CHECK(gDrops.size() == 1, "an item that was just thrown waits before it can be picked up");
        RunEntities(1.0f, &close);
        CHECK(gDrops.empty() && gInv.CountOf(ID_STICK) == 1, "then it can");

        for (auto& slot : gInv.slots)
            slot = Stack(ID_STONE, 64);
        SpawnDrop(Vec3(1.0f, 0.5f, 10.2f), Stack(ID_DIRT, 5), still, 0.0f);
        RunEntities(1.0f, &close);
        CHECK(gDrops.size() == 1 && gDrops[0].stack.count == 5, "a full inventory leaves it lying");
        gInv.slots[7] = Stack(ID_DIRT, 62);
        RunEntities(0.2f, &close);
        CHECK(gDrops.size() == 1 && gDrops[0].stack.count == 3 && gInv.slots[7].count == 64, "what fits is taken, the rest stays");
        gDrops.clear();
        ResetSurvival();

        SpawnDrop(Vec3(0.5f, 0.5f, 10.2f), Stack(ID_COAL, 10), still);
        SpawnDrop(Vec3(0.9f, 0.5f, 10.2f), Stack(ID_COAL, 20), still);
        SpawnDrop(Vec3(0.7f, 0.5f, 10.2f), Stack(ID_STICK, 1), still);
        RunEntities(1.5f);
        CHECK(gDrops.size() == 2 && CountDrops(ID_COAL) == 30 && CountDrops(ID_STICK) == 1, "equal items lying together become one stack");

        gDrops.clear();
        gPlayed.clear();
        gWorld.SetRaw(3, 3, 10, MakeVox(ID_LAVA));
        SpawnDrop(Vec3(3.5f, 3.5f, 12.0f), Stack(ID_DIAMOND, 1), still);
        RunEntities(2.0f);
        CHECK(gDrops.empty() && Heard(SND_LAVA_EXTINGUISH) == 1, "lava burns items");
        for (int wz = 10; wz <= 12; ++wz)
            gWorld.SetRaw(-3, -3, wz, MakeVox(ID_WATER));
        SpawnDrop(Vec3(-2.5f, -2.5f, 10.1f), Stack(ID_DIAMOND, 1), still);
        RunEntities(3.0f);
        CHECK(gDrops.size() == 1 && gDrops[0].pos.z > 12.0f, "items float up in water (%.2f)", gDrops.empty() ? 0.0f : gDrops[0].pos.z);

        gDrops.clear();
        SpawnDrop(Vec3(0.5f, 0.5f, 10.2f), Stack(ID_DIRT, 1), still);
        gDrops[0].age = 299.5f;
        RunEntities(1.0f);
        CHECK(gDrops.empty(), "items vanish after five minutes");

        // what blocks and containers leave behind
        SpawnBlockDrops(ID_STONE, Vec3(0.5f, 0.5f, 10.5f));
        CHECK(CountDrops(ID_COBBLESTONE) == 1 && CountDrops(ID_STONE) == 0, "stone leaves cobblestone");
        gDrops.clear();
        SpawnBlockDrops(ID_OAK_LOG, Vec3(0.5f, 0.5f, 10.5f));
        CHECK(CountDrops(ID_OAK_LOG) == 1, "a log leaves itself");
        gDrops.clear();
        SpawnDropItem(Vec3(0.5f, 0.5f, 10.5f), ID_DIRT, 150);
        CHECK(gDrops.size() == 3 && CountDrops(ID_DIRT) == 150, "a big pile is split into stacks");
        gDrops.clear();
        gWorld.SetRaw(5, 5, 10, MakeVox(ID_CHEST));
        gWorld.chests[Int3{ 5, 5, 10 }].slots[3] = Stack(ID_DIAMOND, 7);
        DropContainerContents({ 5, 5, 10 });
        CHECK(CountDrops(ID_DIAMOND) == 7 && gWorld.chests.count(Int3{ 5, 5, 10 }) == 0, "a chest that goes spills what is in it");

        // throwing something out of the hand
        gDrops.clear();
        gGame = GameState();
        gGame.lookDir = Vec3(1, 0, 0);
        DropStackAtPlayer(Stack(ID_STICK, 2), true);
        CHECK(gDrops.empty(), "nobody there, nothing thrown");
        host.hasPlayer = true;
        host.player = Vec3(0.5f, 0.5f, 11.0f);
        DropStackAtPlayer(Stack(ID_STICK, 2), true);
        CHECK(gDrops.size() == 1 && Near(gDrops[0].pos.x, 1.1f) && Near(gDrops[0].vel.x, 4.0f) && gDrops[0].pickupDelay == 1.5f,
              "an item is thrown the way the player looks");
    }

    // TNT and what an explosion does
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        gPlayed.clear();
        host.explosions = 0;
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_TNT));
        gWorld.SetRaw(1, 0, 10, MakeVox(ID_STONE));
        gWorld.SetRaw(-1, 0, 10, MakeVox(ID_OBSIDIAN));
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_TNT));
        IgniteTnt({ 0, 0, 10 }, 1.0f);
        CHECK(gWorld.GetBlock(0, 0, 10) == ID_AIR && gPrimedTnt.size() == 1 && Heard(SND_FUSE) == 1, "lit TNT becomes a primed one");
        RunEntities(0.5f);
        CHECK(gPrimedTnt.size() == 1 && gWorld.GetBlock(1, 0, 10) == ID_STONE && Heard(SND_EXPLODE) == 0, "the fuse burns");
        RunEntities(0.6f);
        CHECK(Heard(SND_EXPLODE) == 1 && host.explosions == 1, "then it blows up");
        CHECK(gWorld.GetBlock(1, 0, 10) == ID_AIR && gWorld.GetBlock(0, 0, 9) == ID_AIR && gWorld.GetBlock(-1, 0, 10) == ID_OBSIDIAN,
              "stone and the floor go, obsidian stays");
        CHECK(gWorld.GetBlock(2, 0, 10) == ID_AIR && gPrimedTnt.size() == 1 && Heard(SND_FUSE) == 2, "TNT next to it is lit and thrown");
        CHECK(!gParticles.empty(), "bits fly");
        RunEntities(4.0f);
        CHECK(gPrimedTnt.empty() && Heard(SND_EXPLODE) == 2 && host.explosions == 2, "and blows up in turn");

        // survival: some of the broken blocks lie around as items; creative: none
        for (int mode = 0; mode < 2; ++mode) {
            BlockStage();
            gGame.gameMode = mode == 0 ? MODE_SURVIVAL : MODE_CREATIVE;
            const int before = CountBlocks(ID_STONE);
            ExplodeAt(Vec3(0.5f, 0.5f, 9.5f), 4.0f, false);
            const int broken = before - CountBlocks(ID_STONE);
            if (mode == 0)
                CHECK(broken > 20 && CountDrops(ID_COBBLESTONE) > broken / 10 && CountDrops(ID_COBBLESTONE) < broken * 6 / 10,
                      "about a third of the %d broken blocks drop (%d did)", broken, CountDrops(ID_COBBLESTONE));
            else
                CHECK(broken > 20 && gDrops.empty(), "no drops from explosions in creative");
        }
        gGame.gameMode = MODE_SURVIVAL;
        CHECK(host.explosions == 2 && Heard(SND_EXPLODE) == 2, "a blast that is not ours has no fire ball of ours");

        BlockStage();
        gRules.explosionsBreakBlocks = false;
        ExplodeAt(Vec3(0.5f, 0.5f, 9.5f), 4.0f, false);
        CHECK(CountBlocks(ID_STONE) == 625, "the game rule keeps the blocks whole");
        gRules.explosionsBreakBlocks = true;

        SpawnDrop(Vec3(2.5f, 0.5f, 10.0f), Stack(ID_STICK, 1), still);
        PushLooseThings(Vec3(0.5f, 0.5f, 10.0f), 6.0f, 14.0f);
        CHECK(gDrops[0].vel.x > 5.0f && gDrops[0].vel.z > 1.0f && std::fabs(gDrops[0].vel.y) < 1e-3f, "a blast throws loose things away");

        // the chest the player is looking into blows up: the screen closes, the contents lie around
        BlockStage();
        gPlayed.clear();
        gGame.viewW = 800.0f;
        gGame.viewH = 600.0f;
        gWorld.SetRaw(1, 0, 10, MakeVox(ID_CHEST));
        OpenScreen(SCREEN_CHEST, { 1, 0, 10 });
        CHECK(gGame.screen == SCREEN_CHEST && gGame.openPos == (Int3{ 1, 0, 10 }) && gGame.cursorX == 400.0f && gGame.cursorY == 300.0f,
              "a chest opens with the cursor in the middle");
        CHECK(gWorld.chests.count(Int3{ 1, 0, 10 }) == 1 && Heard(SND_CHEST_OPEN) == 1, "and creaks");
        gWorld.chests[Int3{ 1, 0, 10 }].slots[0] = Stack(ID_DIAMOND, 2);
        ExplodeAt(Vec3(0.5f, 0.5f, 10.5f), 3.0f, false);
        CHECK(gGame.screen == SCREEN_NONE && Heard(SND_CHEST_CLOSE) == 1 && gWorld.GetBlock(1, 0, 10) == ID_AIR &&
                  CountDrops(ID_DIAMOND) == 2,
              "a chest that blows up closes its screen and spills");
    }

    // closing a screen: what was left on the crafting grid and on the cursor comes back
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        OpenScreen(SCREEN_CRAFTING, { 0, 0, 10 });
        gInv.craft3[4] = Stack(ID_STICK, 3);
        gInv.craft[0] = Stack(ID_COAL, 1);
        gInv.cursor = Stack(ID_DIAMOND, 5);
        CloseScreen();
        CHECK(gGame.screen == SCREEN_NONE && gInv.CountOf(ID_STICK) == 3 && gInv.CountOf(ID_COAL) == 1 && gInv.CountOf(ID_DIAMOND) == 5 &&
                  gInv.cursor.Empty() && gInv.craft3[4].Empty() && gInv.craft[0].Empty() && gDrops.empty(),
              "the crafting grid and the cursor are emptied into the inventory");
        for (auto& slot : gInv.slots)
            slot = Stack(ID_STONE, 64);
        gInv.cursor = Stack(ID_DIAMOND, 5);
        CloseScreen();
        CHECK(gInv.cursor.Empty() && CountDrops(ID_DIAMOND) == 5, "or at the player's feet when it is full");
        OpenScreen(SCREEN_FURNACE, { 2, 2, 10 });
        CHECK(gWorld.furnaces.count(Int3{ 2, 2, 10 }) == 1, "a furnace gets its inside when it is first opened");
        CloseScreen();
    }

    // particles
    {
        gParticles.clear();
        SpawnBreakParticles({ 0, 0, 10 }, ID_STONE);
        bool inside = gParticles.size() == 16;
        for (const Particle& p : gParticles)
            inside = inside && p.pos.x > 0.0f && p.pos.x < 1.0f && p.pos.y > 0.0f && p.pos.y < 1.0f && p.pos.z > 10.0f && p.pos.z < 11.0f;
        CHECK(inside, "a broken block leaves 16 bits where it was");
        const float z0 = gParticles[0].pos.z;
        ParticlesTick(0.05f);
        CHECK(gParticles.size() == 16 && gParticles[0].pos.z != z0, "they fly");
        for (int i = 0; i < 30; ++i)
            ParticlesTick(0.05f);
        CHECK(gParticles.empty(), "and are gone after a second");
        for (int i = 0; i < 2000; ++i)
            SpawnParticle(Particle());
        CHECK(gParticles.size() == 1500, "never more than 1500 at once");
    }

    SetHost(nullptr);
    BlockStage();
    gWorld.Clear();
    ResetSurvival();
    gGame = GameState();
}

// ---------------------------------------------------------------- arm's length (Interact.h)
// the player's eyes are at `eye` and look along `dir`
static void LookFrom(const Vec3& eye, const Vec3& dir) {
    gGame.eyePos = gGame.rayOrigin = eye;
    gGame.lookDir = dir;
    UpdateTarget();
}

// holds the attack button for a while
static void Mine(float seconds, bool held = true) {
    const float dt = 0.05f;
    const int ticks = (int)(seconds / dt + 0.5f);
    for (int i = 0; i < ticks; ++i) {
        InteractTick(dt);
        MineTick(dt, held);
    }
}

static void TestInteract() {
    TestHost host;
    SetHost(&host);
    McsaSrand(99);
    const Vec3 eye(0.5f, 0.5f, 11.62f), down(0, 0, -1);

    // how long a block takes, and whether the tool gets its drops
    {
        bool harvest = true;
        CHECK(Near(BreakSeconds(ID_STONE, ItemStack(), &harvest), 7.5f) && !harvest, "stone by hand: 7.5 s, and nothing to show for it");
        CHECK(Near(BreakSeconds(ID_STONE, Stack(ID_WOODEN_PICKAXE), &harvest), 1.125f, 1e-3f) && harvest, "stone with a wooden pickaxe: %.3f s",
              BreakSeconds(ID_STONE, Stack(ID_WOODEN_PICKAXE), nullptr));
        CHECK(Near(BreakSeconds(ID_STONE, Stack(ID_DIAMOND_PICKAXE), nullptr), 0.28125f, 1e-3f), "a diamond pickaxe is four times as fast");
        CHECK(Near(BreakSeconds(ID_STONE, Stack(ID_WOODEN_SHOVEL), &harvest), 7.5f) && !harvest, "the wrong tool is no better than a hand");
        BreakSeconds(ID_DIAMOND_ORE, Stack(ID_STONE_PICKAXE), &harvest);
        const bool withStone = harvest;
        BreakSeconds(ID_DIAMOND_ORE, Stack(ID_IRON_PICKAXE), &harvest);
        CHECK(!withStone && harvest, "diamond ore wants an iron pickaxe");
        CHECK(BreakSeconds(ID_BEDROCK, Stack(ID_DIAMOND_PICKAXE), &harvest) > 1e8f && !harvest, "bedrock does not break");
        CHECK(BreakSeconds(ID_POPPY, ItemStack(), &harvest) == 0.0f && harvest, "a flower breaks at once");
        CHECK(Near(BreakSecondsFor(ID_STONE, 4.0f, Stack(ID_WOODEN_PICKAXE), nullptr), 3.0f, 1e-3f), "a thing of the host's has its own hardness");
    }

    // what the player looks at
    {
        BlockStage();
        gGame = GameState();
        LookFrom(eye, down);
        CHECK(gTarget.valid && gTarget.voxel && gTarget.pos == (Int3{ 0, 0, 9 }) && gTarget.key == gTarget.pos && gTarget.face == FACE_TOP &&
                  Near(gTarget.point.z, 10.0f) && gTarget.normal.z == 1.0f,
              "looking down: the floor block, from above");
        LookFrom(Vec3(0.5f, 0.5f, 15.2f), down);
        CHECK(!gTarget.valid, "survival does not reach 5.2 blocks");
        gGame.gameMode = MODE_CREATIVE;
        LookFrom(Vec3(0.5f, 0.5f, 15.2f), down);
        CHECK(gTarget.valid, "creative does");
        gGame.gameMode = MODE_SURVIVAL;
        LookFrom(eye, Vec3(0, 0, 1));
        CHECK(!gTarget.valid, "the sky is no target");
        LookFrom(eye, Vec3());
        CHECK(!gTarget.valid, "no direction, no target");
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_STONE));
        LookFrom(Vec3(0.5f, 0.5f, 10.5f), Vec3(1, 0, 0));
        CHECK(gTarget.valid && gTarget.pos == (Int3{ 3, 0, 10 }) && gTarget.face == FACE_WEST && gTarget.normal.x == -1.0f &&
                  Near(gTarget.point.x, 3.0f),
              "looking east: the west face of the block in the way");
        host.thing = true;
        LookFrom(Vec3(0.5f, 0.5f, 10.5f), Vec3(1, 0, 0));
        CHECK(gTarget.valid && !gTarget.voxel && gTarget.virtualBlock == ID_DIRT && Near(gTarget.point.x, 2.5f), "the host's thing in front of it");
        host.thing = false;
    }

    // mining
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        gPlayed.clear();
        gInv.Held() = Stack(ID_WOODEN_PICKAXE);
        LookFrom(eye, down);
        Mine(1.0f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_STONE && MiningProgress() > 0.8f && MiningProgress() < 0.95f, "after a second the stone is cracked (%.2f)",
              MiningProgress());
        CHECK(Heard(HitSound(ID_STONE)) >= 3 && Heard(HitSound(ID_STONE)) <= 4 && gGame.swing >= 0.0f, "with a knock four times a second (%d)",
              Heard(HitSound(ID_STONE)));
        Mine(0.05f, false);
        CHECK(MiningProgress() == 0.0f, "letting go starts over");
        Mine(1.15f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_AIR && CountDrops(ID_COBBLESTONE) == 1 && Heard(DigSound(ID_STONE)) == 1 && !gParticles.empty(),
              "1.125 s with a wooden pickaxe: it breaks and drops");
        CHECK(gInv.Held().damage == 1 && Near(gSurvival.exhaustion, kExhaustMine) && MiningProgress() == 0.0f, "the pickaxe wears, the miner tires");
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_STONE));
        LookFrom(eye, down);
        Mine(0.2f);
        CHECK(MiningProgress() == 0.0f, "a short pause before the next block");
        Mine(0.2f);
        CHECK(MiningProgress() > 0.0f, "then on");

        gInv.Held().Clear();
        gDrops.clear();
        Mine(0.05f, false); // (the cracks made with the pickaxe would count)
        Mine(7.0f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_STONE, "by hand it takes long");
        Mine(1.0f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_AIR && gDrops.empty(), "and leaves nothing");

        // ores give experience, to the right tool only
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_DIAMOND_ORE));
        gInv.Held() = Stack(ID_IRON_PICKAXE);
        BreakVoxel({ 0, 0, 9 }, true);
        int xp = 0;
        for (const XpOrb& o : gXpOrbs)
            xp += o.value;
        CHECK(CountDrops(ID_DIAMOND) == 1 && xp >= 3 && xp <= 7, "diamond ore: a diamond and %d experience", xp);
        gDrops.clear();
        gXpOrbs.clear();
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_DIAMOND_ORE));
        gInv.Held() = Stack(ID_STONE_PICKAXE);
        BreakVoxel({ 0, 0, 9 }, true);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_AIR && gDrops.empty() && gXpOrbs.empty(), "with a stone pickaxe: nothing");
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_STONE));
        gPlayed.clear();
        gParticles.clear();
        CHECK(PlaceFire({ 0, 0, 10 }), "a fire");
        BreakVoxel({ 0, 0, 10 }, true);
        CHECK(gWorld.GetBlock(0, 0, 10) == ID_AIR && Heard(SND_FIRE_EXTINGUISH) == 1 && gParticles.empty(), "a fire is punched out");

        // creative: at once, without drops, one block every quarter second, and never with a sword
        gGame.gameMode = MODE_CREATIVE;
        gInv.Held().Clear();
        gDrops.clear();
        LookFrom(eye, down);
        Mine(0.3f, false);
        Mine(0.05f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_AIR && gDrops.empty() && MiningProgress() == 0.0f, "creative breaks at once and takes nothing");
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_STONE));
        Mine(0.05f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_STONE, "but not two blocks in the same quarter second");
        gInv.Held() = Stack(ID_DIAMOND_SWORD);
        Mine(1.0f);
        CHECK(gWorld.GetBlock(0, 0, 9) == ID_STONE, "and a sword in creative breaks nothing");
        gGame.gameMode = MODE_SURVIVAL;

        // a thing of the host's: mined by its own hardness, broken by the host
        gInv.Held().Clear();
        host.thing = true;
        host.thingsBroken = 0;
        LookFrom(eye, down);
        Mine(0.3f, false);
        Mine(0.6f);
        CHECK(host.thingsBroken == 0 && MiningProgress() > 0.5f, "the host's dirt is being dug (%.2f)", MiningProgress());
        Mine(0.3f);
        CHECK(host.thingsBroken == 1, "and broken by the host after 0.75 s");
        host.thing = false;
    }

    // placing
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        gPlayed.clear();
        gInv.Held() = Stack(ID_STONE, 2);
        LookFrom(eye, down);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(0, 0, 10) == ID_STONE && gInv.Held().count == 1 && Heard(PlaceSound(ID_STONE)) == 1 && gGame.swing == 0.0f,
              "a block is put on the face looked at");
        CHECK(!PlaceReady(), "the next one has to wait a moment");
        Mine(0.3f, false);
        CHECK(PlaceReady(), "a quarter of a second");
        gGame.gameMode = MODE_CREATIVE;
        LookFrom(Vec3(4.5f, 4.5f, 11.62f), down);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(4, 4, 10) == ID_STONE && gInv.Held().count == 1, "creative keeps the block");
        gGame.gameMode = MODE_SURVIVAL;

        gInv.Held() = Stack(ID_FURNACE);
        LookFrom(Vec3(5.5f, 5.5f, 11.62f), Vec3(0.6f, 0.0f, -0.8f));
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(6, 5, 10) == ID_FURNACE && VoxMeta(gWorld.Get(6, 5, 10)) == 3 && gWorld.furnaces.count(Int3{ 6, 5, 10 }) == 1 &&
                  gInv.Held().Empty(),
              "a furnace faces the way the player looks, and has an inside");
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_STONE));
        gInv.Held() = Stack(ID_OAK_LOG);
        LookFrom(Vec3(0.5f, 0.5f, 10.5f), Vec3(1, 0, 0));
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(2, 0, 10) == ID_OAK_LOG && VoxMeta(gWorld.Get(2, 0, 10)) == 1, "a log lies along the face it is put on");

        gInv.Held() = Stack(ID_STONE);
        host.blocking = true;
        host.blocked = Int3{ -3, -3, 10 };
        LookFrom(Vec3(-2.5f, -2.5f, 11.62f), down);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(-3, -3, 10) == ID_AIR && gInv.Held().count == 1, "no block where somebody stands");
        host.blocking = false;
        gWorld.SetRaw(-3, -3, 10, MakeVox(ID_WATER));
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(-3, -3, 10) == ID_STONE, "water makes room for a block");

        gInv.Held() = Stack(ID_POPPY);
        LookFrom(Vec3(-6.5f, -6.5f, 11.62f), down);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(-7, -7, 10) == ID_AIR, "no flower on bare stone");
        gWorld.SetRaw(-7, -7, 9, MakeVox(ID_GRASS_BLOCK));
        LookFrom(Vec3(-6.5f, -6.5f, 11.62f), down);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(-7, -7, 10) == ID_POPPY, "but on grass");
        gInv.Held() = Stack(ID_WATER_BUCKET);
        PlaceHeldBlock();
        CHECK(gInv.Held().id == ID_WATER_BUCKET, "a bucket is not a block");
    }

    // planting and bone meal
    {
        BlockStage();
        gWorld.SetRaw(0, 0, 9, MakeVox(ID_GRASS_BLOCK));
        gWorld.SetRaw(1, 0, 9, MakeVox(ID_SAND));
        CHECK(CanPlantAt(ID_POPPY, { 0, 0, 10 }, false) && !CanPlantAt(ID_POPPY, { 2, 0, 10 }, false) && CanPlantAt(ID_POPPY, { 2, 0, 10 }, true),
              "flowers want soil (creative plants on anything solid)");
        CHECK(CanPlantAt(ID_DEAD_BUSH, { 1, 0, 10 }, false) && !CanPlantAt(ID_POPPY, { 1, 0, 10 }, false) &&
                  CanPlantAt(ID_RED_MUSHROOM, { 2, 0, 10 }, false),
              "dead bushes grow on sand, mushrooms anywhere");
        CHECK(!CanPlantAt(ID_POPPY, { 0, 0, 9 }, true) && !CanPlantAt(ID_POPPY, { 0, 0, 15 }, true), "not into a block, not in the air");
        host.soil = true;
        CHECK(CanPlantAt(ID_POPPY, { 0, 0, 15 }, false), "the host's earth is soil too");
        host.soil = false;

        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_GRASS_BLOCK));
        gParticles.clear();
        CHECK(ApplyBoneMeal(true, { 0, 0, 9 }, Vec3(0.5f, 0.5f, 10.0f), Vec3(0, 0, 1), false), "bone meal works on a grass block");
        int plants = 0;
        for (int x = -4; x <= 4; ++x)
            for (int y = -4; y <= 4; ++y)
                plants += IsPlantBlock(gWorld.GetBlock(x, y, 10));
        CHECK(plants >= 3 && plants <= 10 && !gParticles.empty(), "grass and flowers come up around it (%d)", plants);
        CHECK(!ApplyBoneMeal(true, { 8, 8, 9 }, Vec3(8.5f, 8.5f, 10.0f), Vec3(0, 0, 1), false), "not on stone");
        CHECK(!ApplyBoneMeal(true, { 0, 0, 9 }, Vec3(0.0f, 0.5f, 9.5f), Vec3(-1, 0, 0), false), "nor on the side of a grass block");
        gWorld.SetRaw(6, 6, 10, MakeVox(ID_OAK_SAPLING));
        CHECK(ApplyBoneMeal(true, { 6, 6, 10 }, Vec3(6.5f, 6.5f, 10.5f), Vec3(0, 0, 1), false), "and on a sapling");
    }

    // buckets, bone meal and armour in the hand
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        gPlayed.clear();
        host.doused = 0;
        gInv.Held() = Stack(ID_WATER_BUCKET);
        LookFrom(Vec3(8.5f, 8.5f, 11.62f), down);
        CHECK(UseWorldItem() && gWorld.GetBlock(8, 8, 10) == ID_WATER && gInv.Held().id == ID_BUCKET && host.doused == 1 &&
                  Heard(SND_BUCKET_EMPTY) == 1,
              "a bucket of water is emptied onto the block looked at");
        CHECK(UseWorldItem() && gWorld.GetBlock(8, 8, 10) == ID_AIR && gInv.Held().id == ID_WATER_BUCKET && Heard(SND_BUCKET_FILL) == 1,
              "and scooped up again");
        gGame.gameMode = MODE_CREATIVE;
        CHECK(UseWorldItem() && gWorld.GetBlock(8, 8, 10) == ID_WATER && gInv.Held().id == ID_WATER_BUCKET, "creative keeps the water");
        gGame.gameMode = MODE_SURVIVAL;
        gInv.Held() = Stack(ID_BUCKET, 3);
        CHECK(UseWorldItem() && gInv.Held().id == ID_BUCKET && gInv.Held().count == 2 && gInv.CountOf(ID_WATER_BUCKET) == 1,
              "from a stack of buckets one is filled");
        gInv.Held() = Stack(ID_BUCKET);
        LookFrom(Vec3(8.5f, 8.5f, 11.62f), Vec3(0, 0, 1));
        CHECK(!UseWorldItem(), "nothing to scoop from the sky");

        gWorld.SetRaw(0, 0, 10, MakeVox(ID_OAK_SAPLING));
        gInv.Held() = Stack(ID_BONE_MEAL, 3);
        LookFrom(eye, down);
        CHECK(gTarget.valid && gTarget.pos == (Int3{ 0, 0, 10 }) && UseWorldItem() && gInv.Held().count == 2 && Heard(SND_BONE_MEAL) == 1,
              "bone meal on a sapling is used up");

        gInv.Held() = Stack(ID_IRON_HELMET);
        CHECK(UseWorldItem() && gInv.armor[ARMOR_HEAD].id == ID_IRON_HELMET && gInv.Held().Empty() && Heard(SND_EQUIP_IRON) == 1,
              "a use puts a helmet on");
        gInv.Held() = Stack(ID_DIAMOND_HELMET);
        CHECK(!UseWorldItem() && gInv.Held().id == ID_DIAMOND_HELMET, "not a second one on top");
        gInv.Held() = Stack(ID_STICK);
        CHECK(!UseWorldItem(), "a stick does nothing");

        CHECK(HasRightClickUse(Stack(ID_STONE)) && HasRightClickUse(Stack(ID_BREAD)) && HasRightClickUse(Stack(ID_BOW)) &&
                  HasRightClickUse(Stack(ID_IRON_HELMET)),
              "blocks, food, bows and armour are used");
        CHECK(!HasRightClickUse(ItemStack()) && !HasRightClickUse(Stack(ID_DIAMOND_SWORD)) && !HasRightClickUse(Stack(ID_ARROW)) &&
                  !HasRightClickUse(Stack(ID_TOTEM_OF_UNDYING)),
              "swords, arrows and the totem are not (the off hand gets the use)");
    }

    // containers
    {
        BlockStage();
        gGame = GameState();
        CHECK(IsContainer(ID_CHEST) && IsContainer(ID_FURNACE) && IsContainer(ID_CRAFTING_TABLE) && !IsContainer(ID_STONE), "containers");
        LookFrom(eye, down);
        CHECK(!TargetHasUse(), "the floor is none");
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_CRAFTING_TABLE));
        LookFrom(Vec3(0.5f, 0.5f, 12.62f), down);
        CHECK(TargetHasUse(), "a crafting table is");
        UseTargetBlock();
        CHECK(gGame.screen == SCREEN_CRAFTING && gGame.openPos == (Int3{ 0, 0, 10 }), "and opens its screen");
        CloseScreen();
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_CHEST));
        UseTargetBlock();
        CHECK(gGame.screen == SCREEN_CHEST, "a chest opens the chest screen");
        CloseScreen();
        gWorld.SetRaw(0, 0, 10, MakeVox(ID_FURNACE));
        UseTargetBlock();
        CHECK(gGame.screen == SCREEN_FURNACE, "a furnace the furnace screen");
        CloseScreen();
    }

    // the hands: hotbar, off hand, throwing, picking
    {
        BlockStage();
        ResetSurvival();
        gGame = GameState();
        gControls = Controls();
        gPlayed.clear();
        gInv.slots[0] = Stack(ID_STONE, 5);
        gInv.slots[2] = Stack(ID_DIRT);
        gGame.attackTimer = 5.0f;
        HotbarTick();
        CHECK(gInv.selected == 0 && gGame.selectedNameTimer == 0.0f && gGame.attackTimer == 5.0f, "no key, no change");
        gControls.Set(ACT_HOTBAR_3, true, true);
        HotbarTick();
        CHECK(gInv.selected == 2 && gGame.selectedNameTimer == 2.0f && gGame.attackTimer == 0.0f, "3 picks the third slot; the attack recharges");
        gControls = Controls();
        gControls.scrollDown = true;
        HotbarTick();
        CHECK(gInv.selected == 3, "the wheel goes to the next slot");
        gControls.scrollDown = false;
        gControls.scrollUp = true;
        for (int i = 0; i < 4; ++i)
            HotbarTick();
        CHECK(gInv.selected == 8, "and around the end");
        gControls = Controls();

        gInv.selected = 0;
        gInv.offhand = Stack(ID_TOTEM_OF_UNDYING);
        SwapHands();
        CHECK(gInv.Held().id == ID_TOTEM_OF_UNDYING && gInv.offhand.id == ID_STONE && gInv.offhand.count == 5 && Heard(SND_EQUIP_GENERIC) == 1,
              "the hands swap what they hold");
        SwapHands();

        host.hasPlayer = true;
        host.player = Vec3(0.5f, 0.5f, 11.0f);
        gGame.lookDir = Vec3(1, 0, 0);
        DropHeldItem(false);
        CHECK(gInv.Held().count == 4 && CountDrops(ID_STONE) == 1 && gGame.swing == 0.0f, "one item is thrown");
        DropHeldItem(true);
        CHECK(gInv.Held().Empty() && CountDrops(ID_STONE) == 5, "or the whole stack");
        DropHeldItem(true);
        CHECK(CountDrops(ID_STONE) == 5, "an empty hand throws nothing");

        LookFrom(eye, down);
        PickBlock();
        CHECK(gInv.Held().Empty(), "survival cannot pick blocks");
        gGame.gameMode = MODE_CREATIVE;
        LookFrom(eye, down);
        PickBlock();
        CHECK(gInv.Held().id == ID_STONE && gInv.Held().count == 1, "creative takes the block looked at into the hand");
        gGame.gameMode = MODE_SURVIVAL;
    }

    // experience orbs
    {
        BlockStage();
        ResetSurvival();
        gPlayed.clear();
        SpawnXp(Vec3(0.5f, 0.5f, 11.0f), 100);
        CHECK(gXpOrbs.size() == 4, "100 experience are four orbs (%d)", (int)gXpOrbs.size());
        const Vec3 away(0.5f, 0.5f, 60.0f);
        for (int i = 0; i < 60; ++i)
            XpTick(0.05f, &away);
        bool resting = gXpOrbs.size() == 4;
        for (const XpOrb& o : gXpOrbs)
            resting = resting && o.pos.z > 10.0f && o.pos.z < 10.6f;
        CHECK(resting && gSurvival.xpTotal == 0, "they fall and bounce on the floor");
        const Vec3 here(0.5f, 0.5f, 11.0f);
        for (int i = 0; i < 100 && !gXpOrbs.empty(); ++i)
            XpTick(0.05f, &here);
        CHECK(gXpOrbs.empty() && gSurvival.xpTotal == 100 && Heard(SND_XP_ORB) == 4, "and fly to the player who comes near (%d left)",
              (int)gXpOrbs.size());
        SpawnXp(Vec3(0.5f, 0.5f, 11.0f), 3);
        gXpOrbs[0].age = 299.99f;
        XpTick(0.05f, nullptr);
        CHECK(gXpOrbs.empty(), "an orb nobody takes is gone after five minutes");
    }

    SetHost(nullptr);
    BlockStage();
    gWorld.Clear();
    ResetSurvival();
    gGame = GameState();
    gControls = Controls();
}

// ---------------------------------------------------------------- the world file (Save.h)
// lets the animals live for a while; the player stands at `player`
static void RunMobs(float seconds, const Vec3& player, uint16_t held = 0, bool onFoot = true) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt)
        MobsTick(dt, player, held, onFoot);
}

// the block stage with a wall around it (animals wander), no animals, a fresh player
static void MobStage() {
    BlockStage();
    for (int i = -12; i <= 12; ++i)
        for (int z = 10; z <= 11; ++z) {
            gWorld.SetRaw(i, -12, z, MakeVox(ID_STONE));
            gWorld.SetRaw(i, 12, z, MakeVox(ID_STONE));
            gWorld.SetRaw(-12, i, z, MakeVox(ID_STONE));
            gWorld.SetRaw(12, i, z, MakeVox(ID_STONE));
        }
    MobsClear();
    ResetSurvival();
    gGame = GameState();
    gRules = GameRules();
    gPlayed.clear();
}

static int CountParticles(uint16_t tile) {
    int n = 0;
    for (const Particle& p : gParticles)
        if (p.tile == tile)
            ++n;
    return n;
}

// the three new monsters of feature 2: a zombie, a skeleton and a spider, each with its own brain
static void RunMobsB(float seconds, const Vec3& player, const std::vector<BeingInfo>* beings) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt)
        MobsTick(dt, player, 0, true, beings);
}

// The engagement law's own ignition (the user's order, 2026-11-07), used by
// the fight sims written under that law: the player holds his gun on the
// boss for a breath, the way the real provocation works (0.45 s of aim;
// 0.7 s here leaves margin). Under the TRACKER LAW (later the same day)
// fights also open by sight alone - the sims keep using this helper where
// the fight must be a PROVOKED one (its aggro bookkeeping is what they
// price), and it remains the provocations' own ignition either way.
static void ProvokeByAim(TestHost& host, const Vec3& player, const std::vector<BeingInfo>* beings) {
    host.aimingAtHim = true;
    RunMobsB(0.7f, player, beings);
    host.aimingAtHim = false;
}

// a long open floor with no pen walls, for the tests of how far the new monsters see and how long they remember
static void WideStage() {
    BlockStage();
    for (int x = -95; x <= 95; ++x)
        for (int y = -14; y <= 14; ++y)
            gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
    MobsClear();
    ResetSurvival();
    gGame = GameState();
    gRules = GameRules();
    gPlayed.clear();
}

static void TestNewMobs() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const Vec3 player(0.5f, 0.5f, 11.0f); // his middle; his feet stand at z = 10

    // what they are
    {
        MobStage();
        host.dark = true;
        const int z = SpawnMob(MOB_ZOMBIE, Vec3(3.5f, 0.5f, 10.0f), true);
        const int s = SpawnMob(MOB_SKELETON, Vec3(-3.5f, 0.5f, 10.0f), true);
        const int sp = SpawnMob(MOB_SPIDER, Vec3(0.5f, 4.5f, 10.0f), true);
        CHECK(gMobs[z].health == 40.0f && IsMonster(MOB_ZOMBIE) && Near(MobHeight(gMobs[z]), 1.95f), "a zombie has forty half hearts");
        CHECK(gMobs[s].health == 40.0f && IsMonster(MOB_SKELETON), "a skeleton has forty half hearts");
        CHECK(gMobs[sp].health == 32.0f && IsMonster(MOB_SPIDER) && Near(MobWidth(gMobs[sp]), 1.4f), "a spider has thirty-two half hearts and is 1.4 wide");
        CHECK(Item(ID_ZOMBIE_SPAWN_EGG).special == SP_EGG_ZOMBIE && Item(ID_SKELETON_SPAWN_EGG).special == SP_EGG_SKELETON &&
                  Item(ID_SPIDER_SPAWN_EGG).special == SP_EGG_SPIDER,
              "each of them has its own spawn egg");
    }
    // the zombie walks at the player and beats on him
    {
        MobStage();
        host.dark = true;
        host.playerHurt = 0.0f;
        SpawnMob(MOB_ZOMBIE, Vec3(6.5f, 0.5f, 10.0f), true);
        RunMobsB(4.0f, player, nullptr);
        CHECK(gMobs.size() == 1 && gMobs[0].pos.x < 3.0f, "the zombie walks at the player");
        RunMobsB(4.0f, player, nullptr);
        CHECK(host.playerHurt >= 3.0f, "and beats on him, three half hearts a blow");
    }
    // and at one of the host's own people, down the same road as step 1
    {
        MobStage();
        host.dark = true;
        host.people.clear();
        TestHost::Person p;
        p.centre = Vec3(-4.5f, 0.5f, 11.0f);
        host.people.push_back(p);
        const std::vector<BeingInfo> beings = { BeingInfo{ 0, p.centre } };
        SpawnMob(MOB_ZOMBIE, Vec3(0.5f, 0.5f, 10.0f), true);
        RunMobsB(6.0f, Vec3(0.5f, 11.5f, 11.0f), &beings);
        CHECK(host.people[0].hurt >= 3.0f, "the zombie beats on the host's people too");
    }
    // the skeleton keeps its distance and looses real arrows
    {
        MobStage();
        host.dark = true;
        gProjectiles.clear();
        SpawnMob(MOB_SKELETON, Vec3(8.5f, 0.5f, 10.0f), true);
        RunMobsB(3.0f, player, nullptr);
        bool arrow = false;
        for (const Projectile& pr : gProjectiles)
            if (pr.type == PJ_ARROW && pr.hostile)
                arrow = true;
        CHECK(arrow, "the skeleton looses real hostile arrows");
        CHECK(gMobs.size() == 1 && gMobs[0].pos.x > 5.0f, "and keeps its distance instead of charging in");
    }
    // the spider runs its prey down and bites
    {
        MobStage();
        host.dark = true;
        host.playerHurt = 0.0f;
        SpawnMob(MOB_SPIDER, Vec3(9.5f, 0.5f, 10.0f), true);
        RunMobsB(3.0f, player, nullptr);
        CHECK(gMobs.size() == 1 && gMobs[0].pos.x < 4.0f, "the spider closes in fast");
        RunMobsB(3.0f, player, nullptr);
        CHECK(host.playerHurt >= 2.0f, "and bites, two half hearts a bite");
    }
    // daylight burns a zombie up
    {
        MobStage();
        host.dark = false;
        host.outdoors = true;
        SpawnMob(MOB_ZOMBIE, Vec3(3.5f, 0.5f, 10.0f), true);
        RunMobsB(4.0f, Vec3(63.5f, 0.5f, 11.0f), nullptr); // the player is 60 m away, beyond even its long sight: it just stands there and burns
        CHECK(gMobs.size() == 1 && gMobs[0].health < 40.0f, "a zombie burns in daylight");
        CHECK(gMobs[0].burn > 0.0f, "and is on fire while it does");
    }
    // the zombie sees as far as Minecraft's do: a man 30 m off is already prey (it used to notice at 16 m)
    {
        WideStage();
        host.dark = true;
        SpawnMob(MOB_ZOMBIE, Vec3(30.5f, 0.5f, 10.0f), true);
        RunMobsB(2.0f, player, nullptr);
        CHECK(gMobs.size() == 1 && gMobs[0].huntValid && gMobs[0].pos.x < 28.5f, "a zombie spots the player 30 m away and comes for him");
    }
    // and it does not give him up the moment he steps back: out of sight, still inside its stubborn reach
    {
        WideStage();
        host.dark = true;
        SpawnMob(MOB_ZOMBIE, Vec3(20.5f, 0.5f, 10.0f), true);
        RunMobsB(1.0f, player, nullptr);
        const float x1 = gMobs[0].pos.x;
        RunMobsB(2.0f, Vec3(63.5f, 0.5f, 11.0f), nullptr); // 45 m off: beyond the 35 it sees, inside the 52.5 it remembers
        CHECK(gMobs.size() == 1 && gMobs[0].huntValid && gMobs[0].pos.x > x1 + 2.0f, "a zombie keeps coming for a prey that stepped out of sight");
        RunMobsB(11.0f, Vec3(200.5f, 0.5f, 11.0f), nullptr); // hopelessly far: once the memory fades it forgets him
        CHECK(gMobs.size() == 1 && !gMobs[0].huntValid, "and forgets him once he is long out of reach");
    }
    // point blank is never overlooked: one tick at 2.5 m and it is already after him
    {
        WideStage();
        host.dark = true;
        SpawnMob(MOB_ZOMBIE, Vec3(3.0f, 0.5f, 10.0f), true);
        RunMobsB(0.05f, player, nullptr);
        CHECK(gMobs.size() == 1 && gMobs[0].huntValid, "a zombie notices a man standing at its nose at once");
    }
    // a hurt zombie that is after someone calls for help, and help comes (8% a landed hit: 80 light hits make it near-certain)
    {
        WideStage();
        host.dark = true;
        host.herdGround = true; // the host offers ground anywhere at z = 10
        const int z = SpawnMob(MOB_ZOMBIE, Vec3(5.5f, 0.5f, 10.0f), true);
        const uint32_t zid = MobIdAt(z);
        RunMobsB(0.5f, player, nullptr);
        CHECK(gMobs.size() == 1 && gMobs[0].huntValid, "the zombie is after the player before the hitting starts");
        for (int i = 0; i < 80; ++i) {
            const int idx = MobIndexById(zid);
            if (idx < 0)
                break;
            MobHurt(idx, 0.4f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
            RunMobsB(0.3f, player, nullptr);
        }
        int zombies = 0;
        for (const Mob& m : gMobs)
            zombies += m.kind == MOB_ZOMBIE;
        CHECK(MobIndexById(zid) >= 0 && gMobs[MobIndexById(zid)].health > 0.0f, "the beaten zombie lives through it");
        CHECK(zombies >= 2, "and its call brought at least one more zombie (%d zombies in all)", zombies);
        host.herdGround = false;
    }
    // a chasing zombie really covers ground at its (raised) run speed once it is up to speed: over the last
    // half second of a settled chase it must gain more than the old 2.3 m/s could ever give (1.15 m)
    {
        WideStage();
        host.dark = true;
        SpawnMob(MOB_ZOMBIE, Vec3(20.5f, 0.5f, 10.0f), true);
        RunMobsB(2.0f, player, nullptr);
        const float x1 = gMobs[0].pos.x;
        RunMobsB(0.5f, player, nullptr);
        CHECK(gMobs.size() == 1 && x1 - gMobs[0].pos.x > 1.3f,
              "a settled zombie chase runs faster than Minecraft's dry 2.3 m/s (gained %.2f m in half a second)", x1 - gMobs[0].pos.x);
    }
    // the skeleton faces its prey while it strafes: body (and so the bow) on the player, feet sideways,
    // distance kept - look, aim and arrow on one line, the way Minecraft's skeletons fight
    {
        MobStage();
        host.dark = true;
        SpawnMob(MOB_SKELETON, Vec3(8.5f, 0.5f, 10.0f), true);
        RunMobsB(1.5f, player, nullptr);
        const Mob& s = gMobs[0];
        const Vec3 fwd(-std::sin(s.yaw), std::cos(s.yaw), 0.0f);
        const float dot = fwd.x * -1.0f + fwd.y * 0.0f; // the player stands straight down -x from where it spawned
        CHECK(gMobs.size() == 1 && dot > 0.85f, "the strafing skeleton's body faces its prey (dot %.2f)", dot);
        CHECK(std::fabs(s.pos.y - 0.5f) > 0.5f, "while its feet really drift sideways (y %.2f)", s.pos.y);
        CHECK(s.pos.x > 6.0f, "and it keeps its distance instead of charging in (x %.2f)", s.pos.x);
    }
}

// the Counter-Strike soldier of the CS project, step 1: one counter-terrorist with the game's own
// M4A1 and a professional's brain, tuned very hard (his whole difficulty sits in kSoldier in Mobs.cpp)
// a soldier's brain reads his aim and his legs, not the raw yaw field: face all of them
static void FaceMob(int s, float yaw) {
    gMobs[s].yaw = yaw;
    gMobs[s].aimYaw = yaw;
    gMobs[s].legYaw = yaw;
    gMobs[s].draw.aimYaw = yaw;
    gMobs[s].draw.legYaw = yaw;
}

static void TestSoldier() {
    TestHost host;
    SetHost(&host);
    McsaSrand(4242);
    const Vec3 player(0.5f, 0.5f, 11.0f); // his middle; his feet stand at z = 10

    // what he is: Counter-Strike's numbers on this host's scale, posted to the counter team
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        CHECK(s >= 0 && gMobs[s].health == 4000.0f && gMobs[s].armor == 1200.0f, "the boss: 4000 health and 1200 kevlar in half hearts - a full magazine of solid hits cannot kill him, about five can");
        CHECK(gMobs[s].team == TEAM_COUNTER && IsMonster(MOB_SOLDIER), "he fights for the counter-terrorists");
        CHECK(gMobs[s].mag == 30 && Near(MobHeight(gMobs[s]), 1.8f), "a full M4A1 magazine and a man's height");
        CHECK(TEAM_TERRORIST != TEAM_COUNTER && TEAM_NONE != TEAM_COUNTER, "the two sides of the war are kept apart");
    }
    // Counter-Strike's kevlar law, exact: half the blow reaches health, and
    // the vest wears at (damage - health) x 0.5 - half the old flat rate.
    // A vest that dies inside a hit spills the uncovered rest to health.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        MobHurt(s, 10.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        CHECK(Near(gMobs[s].health, 3995.0f) && Near(gMobs[s].armor, 1197.5f), "CS kevlar: half to health, a quarter's wear (health %.1f, armour %.1f)", gMobs[s].health, gMobs[s].armor);
        gMobs[s].hurt = 0.0f;
        gMobs[s].armor = 1.0f; // a breath of vest left: it covers 2 of the blow, the other 8 land whole
        MobHurt(s, 10.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        CHECK(Near(gMobs[s].health, 3987.0f) && Near(gMobs[s].armor, 0.0f), "a dying vest spills the rest (health %.1f, armour %.1f)", gMobs[s].health, gMobs[s].armor);
        gMobs[s].hurt = 0.0f;
        MobHurt(s, 10.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        CHECK(Near(gMobs[s].health, 3977.0f), "with the kevlar gone the whole blow lands (health %.1f)", gMobs[s].health);
        CHECK(gMobs[s].alerted && gMobs[s].bossState == 2, "and being shot at puts him in combat at once (state %d)", gMobs[s].bossState);
    }
    // the street is not his war: a man of the host's people at his nose is never his target
    {
        WideStage();
        host.people.clear();
        TestHost::Person p;
        p.centre = Vec3(6.5f, 0.5f, 11.0f);
        host.people.push_back(p);
        const std::vector<BeingInfo> beings = { BeingInfo{ 0, p.centre } };
        const int s = SpawnMob(MOB_SOLDIER, Vec3(10.5f, 0.5f, 10.0f), true);
        FaceMob(s, kPi / 2.0f); // facing the man in the street
        RunMobsB(3.0f, Vec3(90.5f, 0.5f, 11.0f), &beings); // the player is 80 m off
        CHECK(host.people[0].hurt == 0.0f, "a soldier never shoots the people of the street");
        CHECK(host.playerHurt == 0.0f && !gMobs[s].alerted, "and a far-off player is not engaged either");
        // LAW UPDATE (the tracker law, 2026-11-07): the patrol pin is gone -
        // a live player 80 m off is not scenery any more, he is a destination.
        // In 3 s the boss has left his post and marched most of 20 m toward
        // the man (chase pace, minus his start-up), still far outside any
        // fight: nobody hurt, nobody alerted, no shot - he is simply COMING.
        CHECK(gMobs[s].pos.x > 10.5f + 8.0f, "under the tracker law he leaves his post and comes for the far-off player (x %.1f)", gMobs[s].pos.x);
        host.people.clear();
    }
    // his eyes are a cone, not a sphere: a man behind him is not seen (he stands at a patrol stop)
    // LAW NOTE (the tracker law): this block prices the cone's pure geometry
    // on a POSTED man. A tracking boss turns toward the feed and walks at
    // the man, so the fixed bearing this geometry needs exists only under
    // the provocation law - the block runs with the tracker flipped off and
    // restores it after. The cone itself is priced under the tracker too,
    // by TestBoss's peek/open detection ratios.
    {
        gTrackerAlways = false;
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(20.5f, 0.5f, 10.0f), true);
        FaceMob(s, -kPi / 2.0f); // facing +x; the player stands 15 m behind him, at -x
        gMobs[s].patrolWait = 5.0f;
        RunMobsB(0.3f, Vec3(5.5f, 0.5f, 11.0f), nullptr);
        CHECK(gMobs[s].detect == 0.0f && !gMobs[s].alerted && host.playerHurt == 0.0f, "a man at his back, 15 m off, is not seen - the point-blank bypass is 4 m, not a sphere of eyes");
        gTrackerAlways = true;
    }
    // in front of him, once the player has provoked him, it takes only a
    // moment: he reacts, and his bursts land. (Under the engagement law of
    // 2026-11-07 a bare sighting starts nothing - that is proven in
    // TestBossAggroLaw; every fight in these older sims now opens with the
    // law's own ignition, ProvokeByAim.)
    {
        WideStage();
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(14.5f, 0.5f, 10.0f), true);
        FaceMob(s, kPi / 2.0f); // facing the player, 14 m off
        ProvokeByAim(host, player, nullptr); // the law's ignition (2026-11-07):
                                             // a sighting alone starts nothing now
        RunMobsB(0.3f, player, nullptr);
        CHECK(gMobs[s].bossState == 2 && gMobs[s].alerted, "a provoked fight is on at once");
        RunMobsB(2.5f, player, nullptr);
        CHECK(host.playerHurt > 0.0f, "and his rifle lands hits (%.1f half hearts)", host.playerHurt);
        const int used = 30 - gMobs[s].mag;
        CHECK(used >= 3 && used <= 18, "fired in professional bursts, not a hose (%d rounds in the first seconds; a hose would be over 20)", used);
    }
    // an empty magazine means a reload: three seconds of no fire, then a full rifle again
    {
        WideStage();
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(12.5f, 0.5f, 10.0f), true);
        FaceMob(s, kPi / 2.0f);
        ProvokeByAim(host, player, nullptr);
        RunMobsB(1.0f, player, nullptr); // he is engaged by now
        gMobs[s].mag = 1;
        gMobs[s].burstShots = 0;
        gMobs[s].burstPause = 0.0f;
        RunMobsB(0.7f, player, nullptr);
        CHECK(gMobs[s].mag == 0 && gMobs[s].reloadT > 0.0f, "the last round goes, and the reload begins");
        const float hurtAtReload = host.playerHurt;
        RunMobsB(2.0f, player, nullptr);
        CHECK(host.playerHurt == hurtAtReload, "no shot is fired while he reloads");
        RunMobsB(2.5f, player, nullptr);
        CHECK(gMobs[s].mag >= 20, "and the rifle is full again after it (mag %d)", gMobs[s].mag);
    }
    // badly hurt, he does not stand and die: he falls back while he fights
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(30.5f, 0.5f, 10.0f), true);
        FaceMob(s, kPi / 2.0f); // facing the player, 12 m west of him
        gMobs[s].health = 500.0f; // a fifth of his pool: under the retreat line
        const Vec3 pl(18.5f, 0.5f, 11.0f);
        ProvokeByAim(host, pl, nullptr);
        const float d0 = (Vec3(18.5f, 0.5f, 10.0f) - gMobs[s].pos).Length();
        RunMobsB(2.5f, pl, nullptr);
        const float d1 = (Vec3(18.5f, 0.5f, 10.0f) - gMobs[s].pos).Length();
        CHECK(d1 > d0 + 1.5f, "a hurt soldier falls back (%.1f m -> %.1f m)", d0, d1);
    }
}

static void TestMobs() {
    TestHost host;
    SetHost(&host);
    McsaSrand(4242);
    const Vec3 player(0.5f, 0.5f, 10.0f);

    // what they are
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), true);
        CHECK(cow == 0 && gMobs.size() == 1 && gMobs[0].health == 10.0f && gMobs[0].kind == MOB_COW, "a cow has ten half hearts");
        CHECK(MobIdAt(cow) != 0 && MobIndexById(MobIdAt(cow)) == cow && MobIndexById(0) == -1 && MobIdAt(5) == 0, "and a number of its own");
        CHECK(Near(MobCentre(cow).z, 10.7f) && Near(MobWidth(gMobs[cow]), 0.9f), "it is 0.9 wide and 1.4 high");
        CHECK(SpawnMob(MOB_KIND_COUNT, player, true) == -1 && SpawnMob(-1, player, true) == -1 && gMobs.size() == 1, "no such animal");
        const int chick = SpawnMob(MOB_CHICKEN, Vec3(-3.5f, 0.5f, 10.0f), true, true);
        CHECK(gMobs[chick].health == 4.0f && MobScale(gMobs[chick]) == 0.5f && Near(MobHeight(gMobs[chick]), 0.35f), "a chick is half a chicken");
        CHECK(MobIdAt(chick) != MobIdAt(cow), "everybody has another number");

        // they wander about and make themselves heard
        const Vec3 start = gMobs[cow].pos;
        float furthest = 0.0f;
        for (int i = 0; i < 600; ++i) {
            MobsTick(0.05f, player, 0, true);
            furthest = std::max(furthest, (gMobs[cow].pos - start).Length());
        }
        CHECK(gMobs.size() == 2 && furthest > 1.0f, "the cow wanders (%.1f m)", furthest);
        CHECK(gMobs[cow].onGround && Near(gMobs[cow].pos.z, 10.0f, 0.01f) && gMobs[chick].onGround, "on the floor");
        CHECK(Heard(SND_COW_SAY) >= 1 && Heard(SND_CHICKEN_SAY) >= 1, "it moos now and then");
        CHECK(gMobs[cow].limbSwing > 0.0f, "and its legs moved");
    }

    // hits
    {
        MobStage();
        const int pig = SpawnMob(MOB_PIG, Vec3(3.5f, 0.5f, 10.0f), true);
        RunMobs(0.1f, player);
        gPlayed.clear();
        MobHurt(pig, 3.0f, player, 1.0f);
        const Mob& m = gMobs[pig];
        CHECK(m.health == 7.0f && Heard(SND_PIG_HURT) == 1 && m.hurt > 0.0f, "a hit hurts");
        CHECK(m.vel.x > 6.0f && m.vel.z > 5.0f && !m.onGround && m.panic > 0.0f, "and throws it back");
        MobHurt(pig, 3.0f, player, 1.0f);
        CHECK(m.health == 7.0f && Heard(SND_PIG_HURT) == 1, "the next hit has to wait a moment");
        RunMobs(0.3f, player);
        MobHurt(pig, 3.0f, player, 0.0f);
        CHECK(m.health == 4.0f, "then it counts again");
        RunMobs(1.0f, player);
        CHECK(m.moving && m.panic > 0.0f && m.pos.x > 4.0f, "it runs about in panic");
        RunMobs(7.0f, player);
        CHECK(m.panic <= 0.0f && m.onGround, "and calms down");
        MobHurt(7, 3.0f, player, 1.0f); // (no such animal: nothing happens)
        MobPush(pig, Vec3(0, 0, 5.0f));
        CHECK(m.vel.z == 5.0f && !m.onGround && m.panic >= 3.0f, "a push lifts it off its feet");
    }

    // death and what it leaves
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), true);
        const Vec3 eye(0.5f, 0.5f, 11.0f);
        CHECK(MobsRaycast(eye, Vec3(1, 0, 0), 5.0f).index == cow, "the cow can be aimed at");
        MobHurt(cow, 20.0f, player, 0.0f);
        CHECK(gMobs[cow].death >= 0.0f && Heard(SND_COW_DEATH) == 1 && Heard(SND_COW_HURT) == 0, "a hard hit kills");
        CHECK(MobsRaycast(eye, Vec3(1, 0, 0), 5.0f).index == -1, "the dead are not in the way");
        Vec3 seat;
        float yaw = 0.0f;
        CHECK(!MobRide(MobIdAt(cow), Vec3(1, 0, 0), &seat, &yaw), "nor to be ridden");
        RunMobs(0.5f, player);
        CHECK(gMobs.size() == 1 && gDrops.empty(), "it lies there for a second");
        RunMobs(0.7f, player);
        int xp = 0;
        for (const XpOrb& o : gXpOrbs)
            xp += o.value;
        CHECK(gMobs.empty() && CountDrops(ID_BEEF) >= 1 && CountDrops(ID_BEEF) <= 3 && CountDrops(ID_LEATHER) <= 2, "then it leaves beef (%d)",
              CountDrops(ID_BEEF));
        CHECK(xp >= 1 && xp <= 3 && CountParticles(TILE_P_GENERIC_0) > 0, "experience (%d) and a puff of smoke", xp);

        // burning ones leave cooked meat, babies nothing, sheared sheep no wool
        MobStage();
        const int pig = SpawnMob(MOB_PIG, Vec3(3.5f, 0.5f, 10.0f), true);
        gMobs[pig].burn = 5.0f;
        MobHurt(pig, 20.0f, player, 0.0f);
        RunMobs(1.2f, player);
        CHECK(CountDrops(ID_COOKED_PORKCHOP) >= 1 && CountDrops(ID_PORKCHOP) == 0, "a pig that died on fire leaves cooked pork");
        MobStage();
        const int lamb = SpawnMob(MOB_SHEEP, Vec3(3.5f, 0.5f, 10.0f), true, true);
        MobHurt(lamb, 20.0f, player, 0.0f);
        RunMobs(1.2f, player);
        CHECK(gMobs.empty() && gDrops.empty() && gXpOrbs.empty(), "a lamb leaves nothing");
        MobStage();
        const int bare = SpawnMob(MOB_SHEEP, Vec3(3.5f, 0.5f, 10.0f), true);
        gMobs[bare].sheared = true;
        gMobs[bare].woolTimer = 100.0f;
        const uint32_t woolly = MobIdAt(SpawnMob(MOB_SHEEP, Vec3(-3.5f, 0.5f, 10.0f), true));
        MobHurt(bare, 20.0f, player, 0.0f);
        RunMobs(1.2f, player);
        CHECK(CountDrops(ID_WHITE_WOOL) == 0 && CountDrops(ID_MUTTON) >= 1, "a sheared sheep leaves mutton only");
        MobHurt(MobIndexById(woolly), 20.0f, player, 0.0f); // (its index changed when the other one went)
        RunMobs(1.2f, player);
        CHECK(gMobs.empty() && CountDrops(ID_WHITE_WOOL) == 1, "a woolly one its wool too");
    }

    // falling, burning, swimming
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 18.0f), true);
        const int hen = SpawnMob(MOB_CHICKEN, Vec3(-3.5f, 0.5f, 18.0f), true);
        RunMobs(1.0f, player);
        CHECK(gMobs[cow].onGround && gMobs[cow].health > 4.7f && gMobs[cow].health < 5.3f, "a cow that falls 8 m loses 5 (%.2f left)",
              gMobs[cow].health);
        CHECK(!gMobs[hen].onGround && gMobs[hen].pos.z > 14.0f, "a chicken flutters down slowly");
        RunMobs(4.0f, player);
        CHECK(gMobs[hen].onGround && gMobs[hen].health == 4.0f, "and lands unhurt");

        MobStage();
        const int burning = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), true);
        gMobs[burning].burn = 3.0f;
        RunMobs(4.0f, player);
        CHECK(gMobs[burning].health >= 6.9f && gMobs[burning].health <= 8.1f && gMobs[burning].burn <= 0.0f, "fire takes one a second (%.1f left)",
              gMobs[burning].health);
        CHECK(CountParticles(TILE_P_FLAME) > 0, "with flames all over");
        gMobs[burning].burn = 30.0f;
        host.sea = 12.0f;
        RunMobs(3.0f, player);
        CHECK(gMobs[burning].burn == 0.0f, "water puts it out");
        CHECK(Near(gMobs[burning].pos.z, 12.0f - 1.4f * 0.55f, 0.15f), "and it swims with the head out (%.2f)", gMobs[burning].pos.z);
        host.sea = -1000.0f;
    }

    // food: they follow it, and it makes them breed
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(8.5f, 0.5f, 10.0f), true);
        auto standStill = [&] {
            gMobs[cow].moving = false;
            gMobs[cow].aiTimer = 100.0f;
            gMobs[cow].sayTimer = 100.0f;
        };
        standStill();
        RunMobs(6.0f, player, ID_CARROT);
        CHECK(Near(gMobs[cow].pos.x, 8.5f, 0.05f), "a cow does not care for carrots");
        RunMobs(6.0f, player, ID_WHEAT, false);
        CHECK(Near(gMobs[cow].pos.x, 8.5f, 0.05f), "nor for wheat held out of a car");
        RunMobs(8.0f, player, ID_WHEAT);
        const float dist = (gMobs[cow].pos - player).Length();
        CHECK(dist > 1.5f && dist < 2.7f, "but it comes for wheat, and stops in front of the player (%.2f)", dist);

        MobStage();
        const int a = SpawnMob(MOB_SHEEP, Vec3(2.5f, 0.5f, 10.0f), false);
        const int b = SpawnMob(MOB_SHEEP, Vec3(4.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_CARROT, 2);
        CHECK(!MobInteract(a, true) && gInv.Held().count == 2, "sheep do not eat carrots");
        gInv.Held() = Stack(ID_WHEAT, 2);
        CHECK(MobInteract(a, true) && gMobs[a].love > 0.0f && gInv.Held().count == 1 && Heard(SND_EAT) == 1, "wheat puts a sheep in the mood");
        CHECK(CountParticles(TILE_P_HEART) > 0, "(hearts)");
        CHECK(!MobInteract(a, true) && gInv.Held().count == 1, "once is enough");
        CHECK(MobInteract(b, true) && gInv.Held().Empty(), "the other one eats the last wheat");
        RunMobs(0.1f, player);
        CHECK(gMobs.size() == 3 && gMobs[2].kind == MOB_SHEEP && gMobs[2].baby > 0.0f && gMobs[2].persistent, "a lamb is born");
        CHECK(gMobs[a].love == 0.0f && gMobs[a].breedCooldown > 0.0f && !gXpOrbs.empty(), "the parents rest, the player gets experience");
        gInv.Held() = Stack(ID_WHEAT, 2);
        CHECK(!MobInteract(a, true) && !MobInteract(2, true) && gInv.Held().count == 2, "no more for the parents, and the lamb is too young");
        gMobs[2].baby = 0.2f;
        RunMobs(0.3f, player);
        CHECK(MobScale(gMobs[2]) == 1.0f, "it grows up");
        gGame.gameMode = MODE_CREATIVE;
        CHECK(MobInteract(2, true) && gInv.Held().count == 2, "in creative the food is not used up");
        gMobs[2].love = 20.0f;
        MobHurt(2, 1.0f, player, 0.0f);
        CHECK(gMobs[2].love == 0.0f, "a hit ends the mood");
    }

    // shears, bucket, saddle, eggs
    {
        MobStage();
        const int sheep = SpawnMob(MOB_SHEEP, Vec3(2.5f, 0.5f, 10.0f), true);
        const int cow = SpawnMob(MOB_COW, Vec3(4.5f, 0.5f, 10.0f), true);
        const int pig = SpawnMob(MOB_PIG, Vec3(-8.5f, 0.5f, 10.0f), false);
        for (Mob& m : gMobs) {
            m.aiTimer = 100.0f; // (they stand still)
            m.sayTimer = 100.0f;
        }
        gInv.Held() = Stack(ID_SHEARS);
        CHECK(!MobInteract(cow, true), "cows have no wool");
        CHECK(MobInteract(sheep, true) && gMobs[sheep].sheared && Heard(SND_SHEEP_SHEAR) == 1, "shears take the wool off a sheep");
        CHECK(CountDrops(ID_WHITE_WOOL) >= 1 && CountDrops(ID_WHITE_WOOL) <= 3 && gInv.Held().damage == 1, "one to three pieces, and the shears wear");
        CHECK(!MobInteract(sheep, true), "nothing left to shear");
        gMobs[sheep].woolTimer = 0.2f;
        RunMobs(0.3f, player);
        CHECK(!gMobs[sheep].sheared, "the wool grows back");

        gInv.Held() = Stack(ID_BUCKET);
        CHECK(!MobInteract(sheep, true), "sheep give no milk");
        CHECK(MobInteract(cow, true) && gInv.Held().id == ID_MILK_BUCKET && Heard(SND_COW_MILK) == 1, "a bucket becomes a bucket of milk");
        gInv.Held() = Stack(ID_BUCKET, 3);
        CHECK(MobInteract(cow, true) && gInv.Held().id == ID_BUCKET && gInv.Held().count == 2 && gInv.CountOf(ID_MILK_BUCKET) == 1,
              "from a stack of buckets one is filled");

        gInv.Held() = ItemStack();
        CHECK(!MobInteract(pig, true) && gGame.ridingMob == 0, "a pig without a saddle is not ridden");
        gInv.Held() = Stack(ID_SADDLE);
        CHECK(!MobInteract(cow, true), "a saddle is for pigs");
        CHECK(MobInteract(pig, true) && gMobs[pig].saddled && gMobs[pig].persistent && gInv.Held().Empty() && Heard(SND_SADDLE) == 1,
              "the saddle goes on the pig, which stays for good");
        CHECK(!MobInteract(pig, false) && gGame.ridingMob == 0, "nobody gets on from a car");
        CHECK(MobInteract(pig, true) && gGame.ridingMob == MobIdAt(pig), "on foot the player gets on");
        Vec3 seat = gMobs[pig].pos;
        float yaw = 0.0f;
        bool there = true;
        for (int i = 0; i < 60; ++i) {
            there = there && MobRide(gGame.ridingMob, Vec3(4.0f, 0, 0), &seat, &yaw);
            MobsTick(0.05f, seat, 0, true);
        }
        CHECK(there && gMobs[pig].pos.x > -0.5f && Near(gMobs[pig].pos.y, 0.5f, 0.05f), "it walks where it is steered (x %.1f)", gMobs[pig].pos.x);
        CHECK(Near(yaw, -kPi * 0.5f, 0.1f) && Near(seat.z, 10.0f + 0.9f + 0.25f, 0.05f), "facing that way, the seat on its back");
        MobsTick(0.05f, Vec3(900.0f, 0.5f, 10.0f), 0, true);
        CHECK(gMobs.size() == 1 && gMobs[0].id == gGame.ridingMob, "the pig the player rides never goes, wherever the host thinks he is");
        gGame.ridingMob = 0;

        MobStage();
        const int layer = SpawnMob(MOB_CHICKEN, Vec3(-3.5f, 3.5f, 10.0f), true);
        gMobs[layer].eggTimer = 0.2f;
        RunMobs(0.3f, player);
        CHECK(CountDrops(ID_EGG) == 1 && Heard(SND_CHICKEN_EGG) == 1 && gMobs[layer].eggTimer >= 120.0f, "a hen lays an egg now and then");
    }

    // aiming at them
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), true);
        const int pig = SpawnMob(MOB_PIG, Vec3(6.5f, 0.5f, 10.0f), true);
        const Vec3 eye(0.5f, 0.5f, 10.5f);
        MobHit h = MobsRaycast(eye, Vec3(1, 0, 0), 10.0f);
        CHECK(h.index == cow && Near(h.dist, 2.5f, 1e-3f) && Near(h.point.x, 3.0f, 1e-3f), "the nearer one is hit, at its side (%.2f)", h.dist);
        CHECK(MobsRaycast(eye, Vec3(1, 0, 0), 2.0f).index == -1, "not from too far");
        CHECK(MobsRaycast(eye, Vec3(0, 0, 1), 10.0f).index == -1 && MobsRaycast(eye, Vec3(-1, 0, 0), 10.0f).index == -1, "nor looking elsewhere");
        h = MobsRaycast(Vec3(0.5f, 0.5f, 11.2f), Vec3(1, 0, 0), 10.0f);
        CHECK(h.index == cow, "over the pig's back there is still the cow");
        h = MobsRaycast(Vec3(9.5f, 0.5f, 10.5f), Vec3(-1, 0, 0), 10.0f);
        CHECK(h.index == pig, "from the other side the pig comes first");
    }

    // explosions
    {
        MobStage();
        const int cow = SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), true);
        const int far = SpawnMob(MOB_COW, Vec3(10.5f, 0.5f, 10.0f), true);
        const Vec3 at(0.5f, 0.5f, 10.7f);
        MobsRadial(at, 10.0f, 10.0f, 10.0f);
        CHECK(Near(gMobs[cow].health, 3.0f, 0.01f) && gMobs[far].health == 10.0f, "a blast hurts less the further away (%.2f)", gMobs[cow].health);
        CHECK(Near(gMobs[cow].vel.x, 7.0f, 0.01f) && Near(gMobs[cow].vel.z, 3.5f, 0.01f) && gMobs[far].vel.x == 0.0f, "and throws them away from it");
        MobsRadial(at, 10.0f, 10.0f, 10.0f);
        CHECK(gMobs[cow].death >= 0.0f, "it does not wait for the last hit to wear off");

        MobStage();
        gRules.explosionsBreakBlocks = false;
        const int near = SpawnMob(MOB_PIG, Vec3(2.5f, 0.5f, 10.0f), true);
        const int away = SpawnMob(MOB_PIG, Vec3(10.5f, 0.5f, 10.0f), true);
        ExplodeAt(Vec3(0.5f, 0.5f, 10.5f), 4.0f, true, BLAST_TNT);
        CHECK(gMobs[near].death >= 0.0f && gMobs[near].vel.x > 3.0f && gMobs[away].health == 10.0f, "TNT kills the pig next to it");
    }

    // herds appear, and go again
    {
        MobStage();
        host.herdGround = true;
        gRules.maxAnimals = 6;
        for (int i = 0; i < 200; ++i)
            MobsSpawnTick(0.5f, player);
        bool wild = true, ring = true;
        for (const Mob& m : gMobs) {
            wild = wild && !m.persistent && m.baby <= 0.0f;
            const float d = (Vec3(m.pos.x, m.pos.y, player.z) - player).Length();
            ring = ring && d > 24.0f && d < 81.0f && m.pos.z == 10.0f;
        }
        CHECK(gMobs.size() == 6 && wild, "herds appear until there are enough (%d)", (int)gMobs.size());
        CHECK(ring, "30 to 75 m from the player, on the ground the host showed");
        for (Mob& m : gMobs)
            m.persistent = true;
        for (int i = 0; i < 200; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.size() == 12, "the player's own animals do not count (%d)", (int)gMobs.size());

        MobsClear();
        gRules.animals = false;
        for (int i = 0; i < 50; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.empty(), "none when the rules say so");
        gRules.animals = true;
        host.outdoors = false;
        for (int i = 0; i < 50; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.empty(), "none indoors");
        host.outdoors = true;
        host.herdGround = false;
        for (int i = 0; i < 50; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.empty(), "none where the host has no ground for them");

        MobStage();
        SpawnMob(MOB_COW, Vec3(3.5f, 0.5f, 10.0f), false);
        SpawnMob(MOB_PIG, Vec3(-3.5f, 0.5f, 10.0f), true);
        RunMobs(0.1f, Vec3(200.0f, 0.5f, 10.0f));
        CHECK(gMobs.size() == 1 && gMobs[0].kind == MOB_PIG, "wild animals go when the player is far away");
        RunMobs(0.1f, Vec3(400.0f, 0.5f, 10.0f));
        CHECK(gMobs.empty(), "the player's own a good deal later");
        SpawnMob(MOB_COW, Vec3(40.5f, 0.5f, 10.0f), true); // (no floor there)
        RunMobs(6.0f, Vec3(40.5f, 0.5f, 10.0f));
        CHECK(gMobs.empty(), "what falls out of the world is gone");
    }
    MobsClear();
    gRules = GameRules();
}

// the animals' stage with a fresh host and a player at its middle who looks east, in first person
static void CombatStage(TestHost& host) {
    MobStage();
    CombatClear();
    host = TestHost();
    host.hasPlayer = true;
    host.player = Vec3(0.5f, 0.5f, 11.0f);
    gGame.eyePos = gGame.rayOrigin = Vec3(0.5f, 0.5f, 10.9f);
    gGame.lookDir = Vec3(1, 0, 0);
    gGame.cameraMode = CAM_FIRST;
    gGame.attackTimer = 10.0f;
    gTarget = Target();
}

static void RunShots(float seconds) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt)
        ProjectilesTick(dt);
}

// pulls the held bow (crossbow, trident) for so long and lets go
static void PullAndRelease(float seconds) {
    ChargedItemsTick(seconds, true, true);
    ChargedItemsTick(0.05f, false, false);
}

static uint16_t ItemWith(int special) {
    for (int id = FIRST_ITEM; id < ITEM_END; ++id)
        if (Item((uint16_t)id).special == special)
            return (uint16_t)id;
    return 0;
}

// the player looks at the top of the floor block (3, 0, 9)
static void TargetFloor() {
    gTarget = Target();
    gTarget.valid = gTarget.voxel = true;
    gTarget.pos = Int3{ 3, 0, 9 };
    gTarget.point = Vec3(3.5f, 0.5f, 10.0f);
    gTarget.normal = Vec3(0, 0, 1);
}

static int XpOnTheGround() {
    int xp = 0;
    for (const XpOrb& o : gXpOrbs)
        xp += o.value;
    return xp;
}

static void TestCombat() {
    TestHost host;
    SetHost(&host);
    McsaSrand(2468);

    // the bow
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 2);
        CHECK(ChargedItemsTick(0.5f, true, true) && gGame.bowDraw == 0.5f && gProjectiles.empty(), "holding the button draws the bow");
        ChargedItemsTick(0.6f, true, false);
        CHECK(!ChargedItemsTick(0.05f, false, false) && gProjectiles.size() == 1 && gGame.bowDraw < 0.0f, "letting go shoots");
        const Projectile a = gProjectiles[0];
        CHECK(a.type == PJ_ARROW && a.crit && a.pickup && !a.hostile && Near(a.vel.x, 60.0f, 0.01f) && Near(a.pos.x, 1.1f),
              "fully drawn: a critical arrow at 60 m/s (%.1f)", a.vel.x);
        CHECK(gInv.CountOf(ID_ARROW) == 1 && gInv.Held().damage == 1 && Heard(SND_BOW_SHOOT) == 1, "an arrow is used up and the bow wears");
        CombatClear();
        PullAndRelease(0.2f);
        CHECK(gProjectiles.size() == 1 && !gProjectiles[0].crit && Near(gProjectiles[0].vel.x, 60.0f * 0.44f / 3.0f, 0.01f),
              "a short pull makes a slow arrow");
        CombatClear();
        gInv.slots[1] = Stack(ID_ARROW, 1);
        PullAndRelease(0.1f);
        CHECK(gProjectiles.empty() && gInv.CountOf(ID_ARROW) == 1, "barely pulled: no shot");
        gInv.slots[1] = ItemStack();
        CHECK(!ChargedItemsTick(0.5f, true, true) && gGame.bowDraw < 0.0f, "no arrows, no drawing");
        gGame.gameMode = MODE_CREATIVE;
        PullAndRelease(1.2f);
        CHECK(gProjectiles.size() == 1 && !gProjectiles[0].pickup, "creative needs no arrows (and leaves none to pick up)");
    }

    // an arrow's flight
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 1);
        PullAndRelease(1.2f);
        gPlayed.clear();
        RunShots(0.1f);
        CHECK(gProjectiles.size() == 1 && !gProjectiles[0].stuck && gProjectiles[0].pos.x > 5.0f && gProjectiles[0].vel.z < 0.0f,
              "it flies and begins to fall");
        RunShots(0.5f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].stuck && Near(gProjectiles[0].pos.x, 11.95f, 0.02f) && Heard(SND_ARROW_HIT) == 1,
              "and sticks in the wall");
        const float unaimed = gProjectiles[0].pos.z;
        CHECK(unaimed < 10.8f && unaimed > 10.3f, "a little lower than it was shot (%.2f)", unaimed);
        RunShots(5.0f);
        CHECK(gProjectiles.size() == 1 && gInv.CountOf(ID_ARROW) == 0, "there it stays");
        host.player = gProjectiles[0].pos;
        RunShots(0.05f);
        CHECK(gProjectiles.empty() && gInv.CountOf(ID_ARROW) == 1 && Heard(SND_PICKUP) == 1, "until the player comes and takes it");

        // flying (or in third person, or at the wheel) the shot is aimed at the spot under the crosshair
        CombatStage(host);
        gGame.flying = true;
        host.velocity = Vec3(0, 6.0f, 0);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 1);
        PullAndRelease(1.2f);
        RunShots(0.6f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].stuck && Near(gProjectiles[0].pos.y, 0.5f, 0.25f) &&
                  Near(gProjectiles[0].pos.z, 10.9f, 0.25f) && gProjectiles[0].pos.z > unaimed + 0.1f,
              "flying sideways, the arrow still lands under the crosshair (y %.2f, z %.2f)", gProjectiles[0].pos.y, gProjectiles[0].pos.z);

        // on foot in first person a throw goes straight ahead, with the player's own motion added
        CombatStage(host);
        host.velocity = Vec3(0, 5.0f, 3.0f);
        gInv.Held() = Stack(ID_SNOWBALL, 8);
        UseHeldItem();
        CHECK(Near(gProjectiles[0].vel.x, 30.0f) && Near(gProjectiles[0].vel.y, 5.0f) && gProjectiles[0].vel.z == 0.0f,
              "standing, only the sideways motion goes with a throw");
        host.onGround = false;
        CombatClear();
        UseHeldItem();
        CHECK(Near(gProjectiles[0].vel.z, 3.0f), "in the air all of it");
    }

    // what an arrow meets
    {
        CombatStage(host);
        const int cow = SpawnMob(MOB_COW, Vec3(6.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 8);
        PullAndRelease(0.9f); // (not quite fully drawn: no critical extra)
        RunShots(0.3f);
        CHECK(gProjectiles.empty() && gMobs[cow].health == 4.0f && Heard(SND_COW_HURT) == 1 && gMobs[cow].vel.x > 3.0f,
              "an arrow at 52 m/s takes 6 off a cow and pushes it (%.1f left)", gMobs[cow].health);

        CombatStage(host);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 8);
        host.people.push_back({ Vec3(8.5f, 0.5f, 10.9f) });
        PullAndRelease(0.9f);
        RunShots(0.3f);
        const TestHost::Person& man = host.people[0];
        CHECK(gProjectiles.empty() && man.hurt == 6.0f && man.how == HURT_SHOT && !man.byHostile && Near(man.push.x, 2.0f, 0.05f),
              "the same for somebody of the host's (%.1f)", man.hurt);
        const int inFront = SpawnMob(MOB_COW, Vec3(4.5f, 0.5f, 10.0f), true);
        PullAndRelease(0.9f);
        RunShots(0.3f);
        CHECK(gMobs[inFront].health == 4.0f && host.people[0].hurt == 6.0f, "an animal in front of him takes the arrow");
        MobsClear();
        gWorld.SetRaw(4, 0, 10, MakeVox(ID_STONE));
        gWorld.SetRaw(4, 0, 11, MakeVox(ID_STONE));
        PullAndRelease(0.9f);
        RunShots(0.3f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].stuck && gProjectiles[0].pos.x < 4.0f && host.people[0].hurt == 6.0f,
              "a block in front of him too");

        CombatStage(host);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 8);
        host.carX = 6.0f;
        PullAndRelease(0.9f);
        RunShots(0.3f);
        CHECK(gProjectiles.empty() && host.carHurt == 6.0f && host.shotVehicle == -1, "arrows do not stick in a vehicle, they wear it down");
        host.carX = 1e9f;
        host.vehicle = 5;
        PullAndRelease(0.9f);
        CHECK(gProjectiles.size() == 1 && Near(gProjectiles[0].pos.x, 1.7f) && gProjectiles[0].vehicle == 5, "from a vehicle the shot starts outside it");
        RunShots(0.05f);
        CHECK(host.shotVehicle == 5, "and the host is told which vehicle to let it through");

        // somebody of the host's shoots at the player
        CombatStage(host);
        ShootArrowAt(Vec3(8.5f, 0.5f, 11.0f), host.player, 3, -1);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].hostile && gProjectiles[0].shooter == 3 && !gProjectiles[0].pickup &&
                  Near(gProjectiles[0].vel.Length(), 48.0f, 0.01f) && Heard(SND_CROSSBOW_SHOOT) == 1,
              "a pillager's arrow");
        RunShots(0.5f);
        CHECK(gProjectiles.empty() && host.playerHurt >= 3.0f && host.playerHurt <= 5.0f && host.lastShooter == 3,
              "hits the player for 3 to 5 (%.1f)", host.playerHurt);
        ShootArrowAt(Vec3(8.5f, 0.5f, 11.0f), Vec3(8.5f, 0.5f, 11.2f), 3, -1);
        CHECK(gProjectiles.empty(), "nobody shoots at his own feet");
        ShootArrowAt(Vec3(0.5f, 0.5f, 11.0f), Vec3(8.5f, 0.5f, 10.5f), 3, -1);
        RunShots(1.0f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].stuck, "one that misses sticks somewhere");
        RunShots(11.0f);
        CHECK(gProjectiles.empty() && gInv.CountOf(ID_ARROW) == 0, "and is gone after ten seconds; it cannot be picked up");
    }

    // the crossbow
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_CROSSBOW);
        gInv.slots[1] = Stack(ID_ARROW, 1);
        CHECK(ChargedItemsTick(0.5f, true, true) && gGame.crossbowSlot < 0, "a crossbow takes a while to load");
        CHECK(ChargedItemsTick(0.8f, true, false) && gGame.crossbowSlot == gInv.selected && gInv.CountOf(ID_ARROW) == 0 && Heard(SND_CLICK) == 1,
              "after 1.25 s the arrow is in");
        ChargedItemsTick(0.05f, false, false);
        CHECK(gProjectiles.empty(), "it stays loaded");
        ChargedItemsTick(0.05f, true, true);
        CHECK(gProjectiles.size() == 1 && Near(gProjectiles[0].vel.x, 63.0f, 0.01f) && gProjectiles[0].damage == 9.0f && gGame.crossbowSlot < 0 &&
                  Heard(SND_CROSSBOW_SHOOT) == 1 && gInv.Held().damage == 1,
              "the next click shoots: 63 m/s, 9 damage");
        CombatClear();
        gInv.offhand = Stack(ID_FIREWORK_ROCKET);
        ChargedItemsTick(1.3f, true, true);
        CHECK(gGame.crossbowRocket && gInv.offhand.Empty(), "a firework from the other hand goes in instead of an arrow");
        ChargedItemsTick(0.05f, false, false);
        ChargedItemsTick(0.05f, true, true);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].type == PJ_ROCKET, "and is shot as a rocket");
        gPlayed.clear();
        RunShots(1.0f);
        CHECK(gProjectiles.empty() && Heard(SND_FIREWORK_BLAST) == 1 && host.explosions == 1 && host.lastBlast == BLAST_SMALL,
              "which bursts on the wall");
    }

    // the trident
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_TRIDENT);
        PullAndRelease(0.3f);
        CHECK(gProjectiles.empty() && gInv.Held().id == ID_TRIDENT, "a short pull throws nothing");
        PullAndRelease(0.6f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].type == PJ_TRIDENT && Near(gProjectiles[0].vel.x, 50.0f, 0.01f) && gInv.Held().Empty() &&
                  gProjectiles[0].stack.damage == 1 && Heard(SND_TRIDENT_THROW) == 1,
              "the trident is thrown, and wears");
        RunShots(0.4f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].stuck && Heard(SND_TRIDENT_HIT) == 1 && gBolts.empty(), "it sticks in the wall; no storm, no lightning");
        RunShots(1.5f);
        CHECK(gProjectiles.empty() && gInv.slots[0].id == ID_TRIDENT && gInv.slots[0].damage == 1 && Heard(SND_PICKUP) == 1,
              "and flies back into the hand it left");

        CombatStage(host);
        const int cow = SpawnMob(MOB_COW, Vec3(6.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_TRIDENT);
        PullAndRelease(0.6f);
        RunShots(0.2f);
        CHECK(gMobs[cow].health == 2.0f && Heard(SND_TRIDENT_HIT) == 1, "a trident takes 8 off a cow");
        RunShots(2.0f);
        CHECK(gProjectiles.empty() && gInv.slots[0].id == ID_TRIDENT, "and comes back from there too");

        CombatStage(host);
        host.storm = true;
        gInv.Held() = Stack(ID_TRIDENT);
        PullAndRelease(0.6f);
        RunShots(0.4f);
        CHECK(gBolts.size() == 1 && LightningFlashActive() && host.fires.size() == 1 && host.fires[0] == 6.0f && host.explosions == 1 &&
                  host.lastBlast == BLAST_SMALL && Heard(SND_THUNDER) == 1,
              "in a storm it calls down lightning: a fire and a small blast");
        LightningTick(0.3f);
        CHECK(gBolts.size() == 1 && !LightningFlashActive(), "the flash is short");
        LightningTick(0.3f);
        CHECK(gBolts.empty(), "the bolt lasts half a second");

        CombatStage(host);
        gGame.gameMode = MODE_CREATIVE;
        gInv.Held() = Stack(ID_TRIDENT);
        PullAndRelease(0.6f);
        CHECK(gInv.Held().id == ID_TRIDENT && gProjectiles.size() == 1 && gProjectiles[0].stack.Empty(), "creative keeps its trident");
        RunShots(0.4f);
        CHECK(gBolts.size() == 1, "and has lightning in any weather");
        RunShots(4.0f);
        CHECK(gProjectiles.empty() && gInv.CountOf(ID_TRIDENT) == 1, "the thrown one just disappears");
    }

    // things that are thrown
    {
        CombatStage(host);
        const int cow = SpawnMob(MOB_COW, Vec3(6.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_SNOWBALL, 3);
        CHECK(UseHeldItem() && gProjectiles.size() == 1 && gProjectiles[0].type == PJ_SNOWBALL && Near(gProjectiles[0].vel.x, 30.0f) &&
                  gInv.Held().count == 2 && Heard(SND_THROW) == 1 && gGame.swing >= 0.0f,
              "a snowball is thrown");
        RunShots(0.4f);
        CHECK(gProjectiles.empty() && gMobs[cow].health == 10.0f && gMobs[cow].hurt > 0.0f && gMobs[cow].vel.x > 2.0f,
              "it pushes a cow without hurting it");
        MobsClear();
        host.people.push_back({ Vec3(6.5f, 0.5f, 10.9f) });
        UseHeldItem();
        RunShots(0.4f);
        CHECK(gProjectiles.empty() && host.people[0].hurt == 0.0f && Near(host.people[0].push.x, 2.5f, 0.05f), "and somebody of the host's");

        CombatStage(host);
        gInv.Held() = Stack(ID_EGG, 64);
        for (int i = 0; i < 64; ++i) {
            UseHeldItem();
            RunShots(0.6f);
        }
        bool chicks = true;
        for (const Mob& m : gMobs)
            chicks = chicks && m.kind == MOB_CHICKEN && m.baby > 0.0f && m.persistent;
        CHECK(gInv.Held().Empty() && gProjectiles.empty() && !gMobs.empty() && gMobs.size() < 24 && chicks,
              "now and then a chick comes out of a thrown egg (%d of 64)", (int)gMobs.size());

        CombatStage(host);
        gInv.Held() = Stack(ID_ENDER_PEARL, 3);
        UseHeldItem();
        RunShots(0.6f);
        CHECK(Heard(SND_PEARL_THROW) == 1 && host.moves == 1 && host.player.x > 11.0f && Near(host.playerHurt, 5.0f) && Heard(SND_TELEPORT) == 1,
              "an ender pearl takes the player to where it lands, for two and a half hearts (x %.1f)", host.player.x);
        host.vehicle = 5;
        UseHeldItem();
        RunShots(0.8f);
        CHECK(host.moves == 1 && gProjectiles.empty(), "not out of a vehicle");
        host.vehicle = -1;
        gGame.gameMode = MODE_CREATIVE;
        UseHeldItem();
        RunShots(0.6f);
        CHECK(host.moves == 2 && Near(host.playerHurt, 5.0f) && gInv.Held().count == 1, "in creative it is free and painless");

        CombatStage(host);
        gInv.Held() = Stack(ID_EXPERIENCE_BOTTLE);
        CHECK(UseHeldItem() && gInv.Held().Empty() && gProjectiles[0].type == PJ_XPBOTTLE, "a bottle o' enchanting is thrown");
        RunShots(1.5f);
        CHECK(gProjectiles.empty() && XpOnTheGround() >= 3 && XpOnTheGround() <= 11 && Heard(SND_DIG_GLASS) == 1,
              "and breaks into 3 to 11 experience (%d)", XpOnTheGround());

        CombatStage(host);
        const int blown = SpawnMob(MOB_COW, Vec3(10.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_WIND_CHARGE);
        UseHeldItem();
        RunShots(0.5f);
        CHECK(Heard(SND_WIND_THROW) == 1 && gProjectiles.empty() && host.gusts == 1 && host.gustPlayerToo && Heard(SND_WIND_BURST) == 1,
              "a wind charge bursts where it lands: the host's things are blown away, the player too");
        CHECK(gMobs[blown].vel.z > 3.0f && gMobs[blown].health == 10.0f, "and the animals, unhurt");
    }

    // fire
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_FIRE_CHARGE, 2);
        CHECK(UseHeldItem() && gProjectiles[0].type == PJ_FIREBALL && Near(gProjectiles[0].vel.x, 24.0f) && gInv.Held().count == 1 &&
                  Heard(SND_FIREBALL) == 1,
              "a fire charge is shot as a fireball");
        RunShots(0.6f);
        CHECK(gProjectiles.empty() && host.explosions == 1 && host.lastBlast == BLAST_FIRE && Heard(SND_EXPLODE) == 1,
              "which flies straight and blows up");
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_TNT));
        gTarget.valid = gTarget.voxel = true;
        gTarget.pos = Int3{ 3, 0, 10 };
        gTarget.point = Vec3(3.0f, 0.5f, 10.5f);
        gTarget.normal = Vec3(-1, 0, 0);
        CHECK(UseHeldItem() && gPrimedTnt.size() == 1 && gProjectiles.empty() && gInv.Held().Empty() && Heard(SND_IGNITE) == 1,
              "on TNT it lights the fuse instead");

        CombatStage(host);
        gInv.Held() = Stack(ID_FLINT_AND_STEEL);
        CHECK(!UseHeldItem() && gInv.Held().damage == 0, "flint and steel need something to light");
        TargetFloor();
        CHECK(UseHeldItem() && gWorld.GetBlock(3, 0, 10) == ID_FIRE && gInv.Held().damage == 1 && host.fires.empty(), "on a block: fire");
        gTarget.voxel = false;
        gTarget.point = Vec3(3.5f, 0.5f, 20.0f);
        CHECK(UseHeldItem() && host.fires.size() == 1 && host.fires[0] == 10.0f && gInv.Held().damage == 2,
              "where our fire cannot burn, on something of the host's, the host lights its own");

        CombatStage(host);
        gInv.Held() = Stack(ID_LAVA_BUCKET);
        CHECK(!UseHeldItem() && gInv.Held().id == ID_LAVA_BUCKET, "lava needs a spot");
        TargetFloor();
        gTarget.voxel = false;
        CHECK(UseHeldItem() && host.fires.size() == 4 && host.fires[0] == 9.0f && host.fires[3] == 7.0f && gInv.Held().id == ID_BUCKET,
              "poured on the host's ground it burns there");
    }

    // fireworks
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_FIREWORK_ROCKET, 4);
        CHECK(!UseHeldItem() && gInv.Held().count == 4, "a firework needs something to stand on");
        TargetFloor();
        CHECK(UseHeldItem() && gProjectiles.size() == 1 && gProjectiles[0].type == PJ_FIREWORK && gProjectiles[0].vel.z == 8.0f &&
                  gInv.Held().count == 3 && Heard(SND_FIREWORK_LAUNCH) == 1,
              "set on the ground it takes off");
        RunShots(1.0f);
        CHECK(gProjectiles.size() == 1 && gProjectiles[0].pos.z > 15.0f && Heard(SND_FIREWORK_BLAST) == 0, "climbs");
        RunShots(1.0f);
        CHECK(gProjectiles.empty() && Heard(SND_FIREWORK_BLAST) == 1 && Heard(SND_FIREWORK_TWINKLE) == 1 && CountParticles(TILE_P_SPARK_0) > 50,
              "and bursts");
        gGame.gliding = true;
        gPlayed.clear();
        CHECK(UseHeldItem() && gGame.boostTime >= 1.0f && gGame.boostTime <= 1.6f && gProjectiles.size() == 1 && gProjectiles[0].pickup &&
                  gInv.Held().count == 2 && Heard(SND_FIREWORK_LAUNCH) == 1,
              "under the elytra it pushes the player on");
        host.player = Vec3(5.5f, 0.5f, 30.0f);
        RunShots(0.05f);
        CHECK(Near(gProjectiles[0].pos.x, 4.9f) && Near(gProjectiles[0].pos.z, 30.0f), "and stays behind him");
        gGame.gliding = false;
        RunShots(0.05f);
        CHECK(gProjectiles.empty(), "until he lands");
        host.vehicle = 5;
        CHECK(UseHeldItem() && host.boost == 1.6f && gInv.Held().count == 1 && gProjectiles.empty(), "at the wheel it is strapped to the vehicle");
    }

    // spawn eggs, buckets, boats, the host's own items
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_COW_SPAWN_EGG, 2);
        CHECK(!UseHeldItem() && gMobs.empty(), "a spawn egg needs a spot");
        TargetFloor();
        CHECK(UseHeldItem() && gMobs.size() == 1 && gMobs[0].kind == MOB_COW && gMobs[0].persistent && Near(gMobs[0].pos.z, 10.0f, 0.03f) &&
                  gInv.Held().count == 1 && Heard(SND_POP) == 1,
              "there the cow stands, and it stays for good");
        host.vehicle = 5;
        CHECK(!UseHeldItem() && gMobs.size() == 1, "not from a vehicle");
        host.vehicle = -1;
        gInv.Held() = Stack(ID_CREEPER_SPAWN_EGG);
        CHECK(UseHeldItem() && gMobs.size() == 2 && gMobs[1].kind == MOB_CREEPER, "a creeper egg makes a creeper");

        CombatStage(host);
        gGame.lookDir = Vec3(0.6f, 0, -0.8f);
        gInv.Held() = Stack(ID_BUCKET);
        CHECK(!UseHeldItem() && gInv.Held().id == ID_BUCKET, "an empty bucket with no water in sight");
        host.sea = 10.5f;
        CHECK(UseHeldItem() && gInv.Held().id == ID_WATER_BUCKET && Heard(SND_SPLASH) == 1, "the host's water fills it");
        CHECK(UseHeldItem() && host.doused == 2 && gInv.Held().id == ID_BUCKET && Heard(SND_SPLASH) == 2,
              "poured out again, the host's fires there and around the player go out");

        const uint16_t boat = ItemWith(SP_BOAT), cart = ItemWith(SP_MINECART), wand = ItemWith(SP_WAND), rod = ItemWith(SP_FISHING_ROD);
        CHECK(boat && cart && wand && rod, "there is a boat, a minecart, a magic stick and a fishing rod");
        host.sea = -1000.0f;
        gInv.Held() = Stack(boat);
        CHECK(UseHeldItem() && host.lastPlaced == -1 && !gInv.Held().Empty() && gGame.messageTimer > 0.0f, "a boat wants water, the player is told");
        host.sea = 10.5f;
        host.vehiclesOk = false;
        CHECK(!UseHeldItem() && !gInv.Held().Empty(), "when the host cannot make one nothing happens");
        host.vehiclesOk = true;
        CHECK(UseHeldItem() && host.lastPlaced == HOST_BOAT && gInv.Held().Empty(), "on water the host puts a boat there");
        gInv.Held() = Stack(cart);
        TargetFloor();
        gTarget.normal = Vec3(-1, 0, 0);
        CHECK(!UseHeldItem() && host.lastPlaced == HOST_BOAT, "a minecart needs flat ground");
        gTarget.normal = Vec3(0, 0, 1);
        CHECK(UseHeldItem() && host.lastPlaced == HOST_MINECART && gInv.Held().Empty(), "there the host puts one");

        gInv.Held() = Stack(wand);
        CHECK(UseHeldItem() && host.itemUses == 1 && MeleeAttack() && host.itemAttacks == 1, "the magic stick is the host's own business");
        gInv.Held() = Stack(rod);
        CHECK(UseHeldItem() && host.itemUses == 1 && FishingIsCast(), "the fishing rod is no business of the host's: it casts");
        FishingClear();
    }

    // blows
    {
        CombatStage(host);
        int cow = SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true);
        const float tired = gSurvival.exhaustion;
        CHECK(MeleeAttack() && gMobs[cow].health == 9.0f && Heard(SND_ATTACK_STRONG) == 1 && gGame.attackTimer == 0.0f &&
                  gSurvival.exhaustion > tired && gGame.swing >= 0.0f && gMobs[cow].vel.x > 6.0f,
              "a fist takes one off a cow and knocks it back");
        gMobs[cow].hurt = 0.0f;
        gGame.attackTimer = 0.05f;
        const float charge = AttackCharge();
        MeleeAttack();
        CHECK(charge > 0.1f && charge < 0.3f && Near(gMobs[cow].health, 9.0f - (0.2f + 0.8f * charge * charge), 1e-4f) && Heard(SND_ATTACK_WEAK) == 1,
              "hitting again at once does little (%.2f left)", gMobs[cow].health);

        const float sword = Item(ID_DIAMOND_SWORD).damage;
        CombatStage(host);
        cow = SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_DIAMOND_SWORD);
        MeleeAttack();
        CHECK(sword > 1.0f && sword < 10.0f && Near(gMobs[cow].health, 10.0f - sword) && Heard(SND_ATTACK_SWEEP) == 1 && gInv.Held().damage == 1,
              "a diamond sword takes %.0f and wears by one", sword);
        gInv.Held() = Stack(ID_IRON_PICKAXE);
        gMobs[cow].hurt = 0.0f;
        gMobs[cow].health = 50.0f;
        gGame.attackTimer = 10.0f;
        MeleeAttack();
        CHECK(gInv.Held().damage == 2 && gMobs[cow].health < 50.0f, "a tool that is no sword wears by two");

        CombatStage(host);
        cow = SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true);
        host.falling = true;
        MeleeAttack();
        CHECK(gMobs[cow].health == 8.5f && Heard(SND_ATTACK_CRIT) == 1 && CountParticles(TILE_P_CRITICAL_HIT) == 12, "coming down from a jump: half as much again");
        CombatStage(host);
        cow = SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true);
        host.falling = true;
        gGame.sprinting = gGame.sprintLatch = true;
        MeleeAttack();
        CHECK(gMobs[cow].health == 9.0f && Heard(SND_KNOCKBACK) == 1 && Near(gMobs[cow].vel.x, 11.2f, 0.05f) && !gGame.sprintLatch,
              "sprinting: no critical hit but a harder push, and the sprint is over");

        CombatStage(host);
        cow = SpawnMob(MOB_COW, Vec3(5.0f, 0.5f, 10.0f), true);
        CHECK(!MeleeAttack() && gMobs[cow].health == 10.0f && gGame.attackTimer == 10.0f, "four metres are out of reach");
        gMobs[cow].pos.x = 2.5f;
        gWorld.SetRaw(1, 0, 10, MakeVox(ID_GLASS));
        CHECK(!MeleeAttack(), "no blow through a block");
        gWorld.SetRaw(1, 0, 10, MakeVox(ID_AIR));
        host.carX = 1.2f;
        CHECK(!MeleeAttack(), "nor through something of the host's");
        host.carX = 1e9f;
        CHECK(MeleeAttack(), "with the way free it lands");

        CombatStage(host);
        host.people.push_back({ Vec3(2.5f, 0.5f, 10.9f) });
        CHECK(MeleeAttack() && host.people[0].hurt == 1.0f && host.people[0].how == HURT_FIST && Near(host.people[0].push.x, 6.0f) &&
                  Near(host.people[0].push.z, 2.5f),
              "somebody of the host's is hit and pushed the same way");
        gGame.attackTimer = 10.0f;
        gInv.Held() = Stack(ID_DIAMOND_SWORD);
        MeleeAttack();
        CHECK(host.people[0].how == HURT_SWORD && Near(host.people[0].hurt, 1.0f + sword), "with a sword the host is told so");
        cow = SpawnMob(MOB_COW, Vec3(1.7f, 0.5f, 10.0f), true);
        gGame.attackTimer = 10.0f;
        MeleeAttack();
        CHECK(gMobs[cow].health < 10.0f && Near(host.people[0].hurt, 1.0f + sword), "an animal in front of him takes the blow");
    }

    // what the dead of the host's leave
    {
        CombatStage(host);
        const Vec3 at(3.5f, 0.5f, 10.5f);
        DropVillagerLoot(at, false);
        CHECK(XpOnTheGround() >= 1 && XpOnTheGround() <= 5, "a villager leaves 1 to 5 experience (%d)", XpOnTheGround());
        for (int i = 0; i < 100; ++i)
            DropVillagerLoot(at, false);
        CHECK(CountDrops(ID_ROTTEN_FLESH) > 10 && CountDrops(ID_BREAD) > 5 && CountDrops(ID_CROSSBOW) == 0, "and now and then a few things");
        gDrops.clear();
        gXpOrbs.clear();
        DropVillagerLoot(at, true);
        CHECK(XpOnTheGround() >= 5 && XpOnTheGround() <= 7, "a pillager 5 to 7 (%d)", XpOnTheGround());
        for (int i = 0; i < 100; ++i)
            DropVillagerLoot(at, true);
        CHECK(CountDrops(ID_ARROW) > 50 && CountDrops(ID_CROSSBOW) >= 1 && CountDrops(ID_ROTTEN_FLESH) == 0, "arrows, sometimes his crossbow");
    }

    CombatStage(host);
    gInv.Held() = Stack(ID_SNOWBALL);
    UseHeldItem();
    StrikeLightning(Vec3(3.5f, 0.5f, 10.0f));
    CombatClear();
    CHECK(gProjectiles.empty() && gBolts.empty(), "a new game starts with nothing in the air");
    MobsClear();
    gGame = GameState();
    gRules = GameRules();
}

static void RunFishing(float seconds) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt)
        FishingTick(dt);
}

// casts the held rod and lets the bobber fly until it lands somewhere (or a second has gone)
static void Cast() {
    UseHeldItem();
    for (int i = 0; i < 20 && gBobber.state == BOB_FLYING; ++i)
        FishingTick(0.05f);
}

static void TestFishing() {
    TestHost host;
    SetHost(&host);
    McsaSrand(1357);

    // a catch
    {
        CombatStage(host);
        host.sea = 10.5f;
        gInv.Held() = Stack(ID_FISHING_ROD);
        CHECK(UseHeldItem() && FishingIsCast() && gBobber.state == BOB_FLYING && Heard(SND_BOBBER_THROW) == 1 && gGame.swing >= 0.0f,
              "the rod casts the bobber");
        CHECK(Near(gBobber.vel.x, 21.0f) && Near(gBobber.vel.z, 3.0f) && Near(gBobber.pos.x, 1.2f), "at 21 m/s, a little upwards");
        RunFishing(1.0f);
        CHECK(gBobber.state == BOB_FLOATING && gBobber.pos.x > 6.0f && gBobber.pos.x < 11.0f && Heard(SND_BOBBER_SPLASH) == 1,
              "it lands in the host's water (x %.1f)", gBobber.pos.x);
        float waited = 0.0f;
        while (gBobber.nibble <= 0.0f && waited < 40.0f) {
            FishingTick(0.05f);
            waited += 0.05f;
        }
        CHECK(gBobber.nibble > 0.0f && waited > 4.0f && Heard(SND_BOBBER_SPLASH) == 2, "a fish bites after a while (%.1f s)", waited);
        const float tired = gSurvival.exhaustion;
        gPlayed.clear();
        CHECK(UseHeldItem() && !FishingIsCast() && Heard(SND_BOBBER_RETRIEVE) == 1, "reeling in");
        int caught = 0;
        for (const DropEntity& d : gDrops)
            caught += d.stack.count;
        CHECK(caught == 1 && gDrops[0].vel.x < 0.0f && XpOnTheGround() >= 1 && XpOnTheGround() <= 6, "brings the catch and some experience");
        CHECK(gInv.Held().damage == 1 && Near(gSurvival.exhaustion - tired, 0.05f), "the rod wears by one");

        gDrops.clear();
        Cast();
        RunFishing(0.5f);
        CHECK(gBobber.state == BOB_FLOATING && gBobber.nibble <= 0.0f, "cast again");
        UseHeldItem();
        CHECK(gDrops.empty() && gInv.Held().damage == 1, "too early: nothing, and the rod does not wear");

        // what lies by the hook comes along
        Cast();
        SpawnDrop(gBobber.pos, Stack(ID_STICK), Vec3(), 0.5f);
        UseHeldItem();
        CHECK(gDrops.size() == 1 && gDrops[0].vel.x < -5.0f && gDrops[0].pickupDelay == 0.0f, "things lying by the hook are pulled in");
    }

    // the line breaks
    {
        CombatStage(host);
        host.sea = 10.5f;
        gInv.Held() = Stack(ID_FISHING_ROD);
        Cast();
        gInv.selected = 1;
        RunFishing(0.05f);
        CHECK(!FishingIsCast(), "when the rod is put away");
        gInv.selected = 0;
        Cast();
        host.vehicle = 5;
        RunFishing(0.05f);
        CHECK(!FishingIsCast(), "when the player gets into a vehicle");
        host.vehicle = -1;
        Cast();
        host.player = Vec3(60.5f, 0.5f, 11.0f);
        RunFishing(0.05f);
        CHECK(!FishingIsCast(), "when he is more than 40 m away");
        host.player = Vec3(0.5f, 0.5f, 11.0f);
        host.vehicle = 5;
        CHECK(!UseHeldItem() && !FishingIsCast(), "nobody fishes from a vehicle");
    }

    // what the hook can catch besides fish
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_FISHING_ROD);
        Cast();
        RunFishing(1.0f);
        CHECK(gBobber.state == BOB_STUCK && gBobber.pos.z > 9.9f && gBobber.pos.z < 10.1f, "on the floor it just lies");
        UseHeldItem();
        CHECK(gInv.Held().damage == 2, "pulling it out of the ground wears the rod by two");

        CombatStage(host);
        gInv.Held() = Stack(ID_FISHING_ROD);
        const int cow = SpawnMob(MOB_COW, Vec3(4.5f, 0.5f, 10.0f), true);
        Cast();
        CHECK(gBobber.state == BOB_HOOKED_MOB && gBobber.mobId == MobIdAt(cow), "an animal is hooked");
        gMobs[cow].pos.y = 2.5f;
        RunFishing(0.05f);
        CHECK(Near(gBobber.pos.y, 2.5f) && Near(gBobber.pos.z, 10.9f), "the bobber goes along with it");
        UseHeldItem();
        CHECK(gMobs[cow].vel.x < -3.0f && gMobs[cow].vel.z > 5.0f && gInv.Held().damage == 3, "and reeling in pulls it over (the rod wears by three)");

        CombatStage(host);
        gInv.Held() = Stack(ID_FISHING_ROD);
        host.people.push_back({ Vec3(5.5f, 0.5f, 10.9f) });
        Cast();
        CHECK(gBobber.state == BOB_HOOKED_BEING && gBobber.hooked.being == 0, "somebody of the host's is hooked");
        RunFishing(0.05f);
        CHECK(Near(gBobber.pos.z, 11.2f), "the host says where the hook is");
        UseHeldItem();
        CHECK(host.flung == 1 && Near(host.flingVel.x, -7.5f, 0.05f) && Near(host.flingVel.z, 8.0f, 0.05f) && gInv.Held().damage == 3,
              "he is flung towards the player");

        CombatStage(host);
        gInv.Held() = Stack(ID_FISHING_ROD);
        host.carX = 6.0f;
        Cast();
        CHECK(gBobber.state == BOB_HOOKED_VEHICLE && gBobber.hooked.vehicle == TestHost::kCarNumber && Near(gBobber.pos.x, 6.0f), "a vehicle is hooked");
        host.carX = 7.0f;
        RunFishing(0.05f);
        CHECK(Near(gBobber.pos.x, 7.0f), "the hook moves with it");
        UseHeldItem();
        CHECK(host.flung == 1 && Near(host.flingVel.x, -5.85f, 0.05f) && gInv.Held().damage == 5, "a vehicle is pulled more gently and wears the rod by five");
        Cast();
        host.carX = 1e9f;
        RunFishing(0.05f);
        CHECK(gBobber.state == BOB_STUCK, "when it is gone the hook just lies there");
    }
    FishingClear();
}

static void TestVillagers() {
    TestHost host;
    SetHost(&host);
    McsaSrand(97531);
    const Vec3 player(0.5f, 0.5f, 11.0f);

    // how they act
    {
        CombatStage(host);
        NpcLook l = NpcStart(NPC_VILLAGER, 1, 100.0f, false);
        CHECK(l.kind == NPC_VILLAGER && l.variant == 1 && l.death < 0.0f && l.say >= 4.0f && l.say <= 44.0f && l.offer >= 0 && l.offer < 5,
              "a new villager");
        NpcFacts f;
        f.pos = Vec3(3.5f, 0.5f, 10.0f);
        f.forward = Vec3(0, 1, 0);
        f.speed = 4.0f;
        f.health = 100.0f;
        l.say = 100.0f;
        for (int i = 0; i < 20; ++i)
            NpcTick(l, 0.05f, f, player);
        CHECK(l.limbAmount > 0.5f && l.limbSwing > 5.0f, "walking swings his legs");
        CHECK(l.headYaw < -0.9f, "his head turns to the player next to him (%.2f)", l.headYaw);
        f.pos = Vec3(9.5f, 0.5f, 10.0f);
        for (int i = 0; i < 40; ++i)
            NpcTick(l, 0.05f, f, player);
        CHECK(std::fabs(l.headYaw) < 0.05f, "and back when he is further away");
        f.health = 90.0f;
        NpcTick(l, 0.05f, f, player);
        CHECK(Heard(SND_VILLAGER_HURT) == 1 && l.hurt == 0.45f, "a villager who is hurt cries out and flashes red");
        f.health = 80.0f;
        NpcTick(l, 0.05f, f, player);
        CHECK(Heard(SND_VILLAGER_HURT) == 1, "once per flash");
        l.say = 0.01f;
        NpcTick(l, 0.05f, f, player);
        CHECK(Heard(SND_VILLAGER_SAY) == 1 && l.say >= 15.0f, "now and then he says something");
        f.dead = true;
        NpcTick(l, 0.05f, f, player);
        NpcTick(l, 0.05f, f, player);
        CHECK(Heard(SND_VILLAGER_DEATH) == 1 && Near(l.death, 0.1f), "he dies once");
        NpcLook p = NpcStart(NPC_PILLAGER, 0, 100.0f, false);
        f.dead = false;
        f.health = 100.0f;
        NpcTick(p, 0.05f, f, player);
        f.health = 50.0f;
        NpcTick(p, 0.05f, f, player);
        CHECK(Heard(SND_PILLAGER_HURT) == 1, "pillagers sound like pillagers");
        CHECK(NpcStart(NPC_VILLAGER, 0, 0.0f, true).death == 10.0f, "somebody found dead is long dead");
    }

    // trading
    {
        CombatStage(host);
        const Vec3 head(3.5f, 0.5f, 11.7f);
        int offer = 0;
        VillagerTrade(0, offer, head);
        CHECK(Heard(SND_VILLAGER_NO) == 1 && gGame.messageTimer > 0.0f && gGame.swing >= 0.0f, "a villager without a profession has nothing");
        gInv.Held() = Stack(ID_WHEAT, 30);
        VillagerTrade(1, offer, head);
        CHECK(gInv.CountOf(ID_WHEAT) == 10 && gInv.CountOf(ID_EMERALD) == 1 && Heard(SND_VILLAGER_TRADE) == 1 &&
                  CountParticles(TILE_P_ENCHANTED_HIT) == 8,
              "a farmer buys 20 wheat for an emerald");
        VillagerTrade(1, offer, head);
        CHECK(gInv.CountOf(ID_WHEAT) == 10 && gInv.CountOf(ID_EMERALD) == 1 && Heard(SND_VILLAGER_NO) == 2, "ten are not enough");
        gInv = PlayerInventory();
        gInv.Held() = Stack(ID_EMERALD, 1);
        VillagerTrade(7, offer, head);
        CHECK(gInv.CountOf(ID_BREAD) == 6 && gInv.CountOf(ID_EMERALD) == 0, "he sells six bread for one (profession 7 is a farmer again)");
        gInv.Held() = Stack(ID_EMERALD, 1);
        offer = 3;
        VillagerTrade(1, offer, head);
        CHECK(gInv.CountOf(ID_GOLDEN_CARROT) == 0 && gInv.CountOf(ID_EMERALD) == 1 && Heard(SND_VILLAGER_NO) == 3, "golden carrots cost three");
        gGame.gameMode = MODE_CREATIVE;
        VillagerTrade(1, offer, head);
        CHECK(gInv.CountOf(ID_GOLDEN_CARROT) == 3 && gInv.CountOf(ID_EMERALD) == 1, "in creative one is enough, and it stays");
        gInv.Held() = Stack(ID_DIRT, 1);
        VillagerTrade(1, offer, head);
        CHECK(offer == 4 && Heard(SND_VILLAGER_YES) == 1 && gGame.message.find(ItemName(ID_EMERALD)) != std::string::npos, "anything else: the next offer");
        offer = 59;
        VillagerTrade(1, offer, head);
        CHECK(offer == 0, "the offers go round");
    }
}

static void TestPlayerHealth() {
    TestHost host;
    SetHost(&host);
    McsaSrand(8642);

    // damage
    {
        CombatStage(host);
        float health = 100.0f;
        gSurvival.healthSeen = -1.0f;
        CHECK(!HealthTick(0.05f, health, 100.0f, 0.0f) && gSurvival.healthSeen == 100.0f && gGame.deathTime < 0.0f,
              "the first look only remembers the health");
        health = 60.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(health == 60.0f && gGame.hurtTimer == 0.5f && Heard(SND_HURT) == 1 && gSurvival.healthSeen == 60.0f, "without armour a hit takes it all");
        gInv.armor[0] = Stack(ID_IRON_HELMET);
        gInv.armor[1] = Stack(ID_IRON_CHESTPLATE);
        const float block = ArmorBlock();
        health = 20.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(block > 0.2f && Near(health, 20.0f + 40.0f * block, 0.01f) && gInv.armor[0].damage == 2 && gInv.armor[1].damage == 2,
              "armour gives some of it back (%.1f) and wears", health);
        const float before = health;
        health -= 10.0f;
        HealthTick(0.05f, health, 100.0f, 10.0f);
        CHECK(Near(health, before - 10.0f) && gInv.armor[0].damage == 2, "poison and drowning go through armour");
        health -= 0.3f;
        gGame.hurtTimer = 0.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(gGame.hurtTimer == 0.0f, "a scratch is nothing");
        gGame.gameMode = MODE_CREATIVE;
        gPlayed.clear();
        const float c = health;
        health -= 20.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(Near(health, c - 20.0f) && Heard(SND_HURT) == 0 && gGame.hurtTimer == 0.5f, "in creative the host decides alone");

        // the host's own yellow hearts (body armour)
        gGame.gameMode = MODE_SURVIVAL;
        gInv.armor[0] = gInv.armor[1] = ItemStack();
        float pool = 15.0f;
        health = 80.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f, &pool);
        health -= 10.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f, &pool);
        CHECK(Near(health, 80.0f) && Near(pool, 5.0f), "a hit takes the host's yellow hearts first");
        health -= 10.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f, &pool);
        CHECK(Near(health, 75.0f) && pool == 0.0f, "then the health");
        health -= 10.0f;
        pool = 50.0f;
        HealthTick(0.05f, health, 100.0f, 10.0f, &pool);
        CHECK(Near(health, 65.0f) && pool == 50.0f, "poison and drowning go past them");

        // the totem
        gInv.armor[0] = gInv.armor[1] = ItemStack();
        gInv.Held() = Stack(ID_TOTEM_OF_UNDYING);
        health = 10.0f;
        CHECK(HealthTick(0.05f, health, 100.0f, 0.0f) && health == 50.0f && gInv.Held().Empty() && Heard(SND_TOTEM) == 1, "a totem saves him");
        CHECK(CountParticles(TILE_P_SPARK_0) == 60 && gGame.messageTimer > 0.0f && HasEffect(EFFECT_REGENERATION) && gSurvival.healthSeen == 50.0f,
              "with sparks, a message and regeneration");
        health = 10.0f;
        CHECK(!HealthTick(0.05f, health, 100.0f, 0.0f) && Near(health, 50.0f) && !HasEffect(EFFECT_ABSORPTION), "only once; its yellow hearts soak up the next hit and are gone");

        // burning
        ClearEffects(); // (the totem gave fire resistance)
        gSurvival.burn = 0.0f;
        health = 100.0f;
        CHECK(!BurnTick(0.05f, ID_AIR, false, true, health, 100.0f) && health == 100.0f, "nothing burns");
        CHECK(BurnTick(0.05f, ID_FIRE, false, true, health, 100.0f) && Near(health, 95.0f) && Near(gSurvival.burn, 7.95f) &&
                  gGame.deathCause == STR_DEATH_FIRE,
              "stepping into fire: a hit, and he catches fire");
        for (int i = 0; i < 10; ++i)
            BurnTick(0.05f, ID_FIRE, false, true, health, 100.0f);
        CHECK(Near(health, 90.0f), "in the flames a hit every half second (%.1f)", health);
        float lowest = health;
        int frames = 0;
        while (BurnTick(0.05f, ID_AIR, false, true, health, 100.0f) && frames < 400)
            ++frames;
        lowest = health;
        CHECK(frames > 150 && frames < 165 && lowest >= 45.0f && lowest <= 55.0f, "out of them he burns on for 8 s, a point a second (%.1f left)", lowest);
        BurnTick(0.05f, ID_LAVA, false, true, health, 100.0f);
        CHECK(Near(health, lowest - 20.0f) && gSurvival.burn > 14.0f && gGame.deathCause == STR_DEATH_LAVA, "lava: four points, 15 s of fire");
        CHECK(!BurnTick(0.05f, ID_AIR, true, true, health, 100.0f) && gSurvival.burn == 0.0f, "water puts him out");
        AddEffect(EFFECT_FIRE_RESISTANCE, 60.0f, 0);
        const float unburnt = health;
        for (int i = 0; i < 40; ++i)
            BurnTick(0.05f, ID_LAVA, false, true, health, 100.0f);
        CHECK(health == unburnt && gSurvival.burn > 0.0f, "fire resistance: burning without pain");
        RemoveEffect(EFFECT_FIRE_RESISTANCE);
        CHECK(!BurnTick(0.05f, ID_FIRE, false, false, health, 100.0f) && health == unburnt && gSurvival.burn == 0.0f, "creative: nothing");
        health = 100.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        gGame.deathCauseTime = -100.0f; // (what hurt him last is long ago)

        // dead
        health = 0.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(Near(gGame.deathTime, 0.05f) && gGame.deathCause == STR_DEATH_GENERIC, "while he is dead the death screen's clock runs");
        health = 100.0f;
        HealthTick(0.05f, health, 100.0f, 0.0f);
        CHECK(gGame.deathTime < 0.0f && health == 100.0f, "back alive");

        // starving
        gSurvival.food = 0.0f;
        gSurvival.foodTimer = 3.99f;
        gGame.hurtTimer = 0.0f;
        SurvivalEvents ev;
        HungerTick(0.05f, health, 100.0f, ev);
        CHECK(health == 95.0f && gGame.hurtTimer == 0.5f && gSurvival.healthSeen == 95.0f, "starving flashes red; it is no hit for the armour");
    }

    // death
    {
        CombatStage(host);
        const Vec3 at(3.5f, 0.5f, 10.5f);
        gInv.slots[0] = Stack(ID_DIRT, 5);
        gInv.armor[0] = Stack(ID_IRON_HELMET);
        gGame.screen = SCREEN_INVENTORY;
        PlayerDied(at);
        CHECK(gGame.screen == SCREEN_NONE && Heard(SND_PLAYER_DEATH) == 1 && gDrops.empty() && gInv.CountOf(ID_DIRT) == 5,
              "the rules keep his things");
        gRules.keepInventory = false;
        PlayerDied(at);
        CHECK(CountDrops(ID_DIRT) == 5 && CountDrops(ID_IRON_HELMET) == 1 && gInv.CountOf(ID_DIRT) == 0 && gInv.armor[0].Empty() &&
                  gDrops[0].pickupDelay == 2.0f,
              "otherwise they fall out");
        gDrops.clear();
        gInv.slots[0] = Stack(ID_DIRT, 5);
        gGame.gameMode = MODE_CREATIVE;
        PlayerDied(at);
        CHECK(gDrops.empty() && gInv.CountOf(ID_DIRT) == 5, "not in creative");
    }

    // a fresh world
    {
        gWorld.Clear();
        gInv = PlayerInventory();
        GiveStarterKit();
        CHECK(gInv.CountOf(ID_CRAFTING_TABLE) == 1 && gInv.CountOf(ID_OAK_PLANKS) == 16 && gInv.CountOf(ID_APPLE) == 4, "a new world starts with a kit");
        GiveStarterKit();
        CHECK(gInv.CountOf(ID_CRAFTING_TABLE) == 1, "once");
        BlockStage();
        gInv = PlayerInventory();
        GiveStarterKit();
        CHECK(gInv.CountOf(ID_CRAFTING_TABLE) == 0, "a world with blocks in it gets none");
    }
    gGame = GameState();
    gRules = GameRules();
    ResetSurvival();
}

// one frame of the hands: `pressed` went down this frame, `down` are held
static HandsResult Hands(std::initializer_list<Action> pressed, std::initializer_list<Action> down = {}, HandsFacts f = HandsFacts()) {
    gControls = Controls();
    for (Action a : down)
        gControls.Set(a, true, false);
    for (Action a : pressed)
        gControls.Set(a, true, true);
    InteractTick(0.05f);
    return HandsTick(0.05f, f);
}

// holds a button for a while (pressed in the first frame)
static HandsResult HoldHands(Action a, float seconds, HandsFacts f = HandsFacts()) {
    HandsResult last, any;
    for (float t = 0.0f; t < seconds; t += 0.05f) {
        last = t == 0.0f ? Hands({ a }, { a }, f) : Hands({}, { a }, f);
        if (last.finished)
            any = last;
    }
    if (any.finished)
        last.finished = any.finished;
    return last;
}

static void TestHands() {
    TestHost host;
    SetHost(&host);
    McsaSrand(1122);
    const Vec3 down45 = Vec3(1, 0, -1) * (1.0f / std::sqrt(2.0f));

    // screens
    {
        CombatStage(host);
        HandsResult r = Hands({ ACT_INVENTORY });
        CHECK(r.blockPad && gGame.screen == SCREEN_INVENTORY, "the inventory key opens the inventory");
        r = Hands({ ACT_HOTBAR_3 });
        CHECK(r.blockPad && !r.aimed && gInv.selected == 0, "while it is open the hands rest and GTA gets no buttons");
        r = Hands({ ACT_INVENTORY });
        CHECK(r.blockPad && gGame.screen == SCREEN_NONE, "the same key closes it");
        Hands({ ACT_INVENTORY });
        Hands({ ACT_BACK });
        CHECK(gGame.screen == SCREEN_NONE, "so does Escape");
        gGame.gameMode = MODE_CREATIVE;
        Hands({ ACT_INVENTORY });
        CHECK(gGame.screen == SCREEN_CREATIVE, "in creative the item tabs open");
        HandsFacts dead;
        dead.alive = false;
        Hands({}, {}, dead);
        CHECK(gGame.screen == SCREEN_NONE, "dying closes it");
    }

    // dead hands
    {
        CombatStage(host);
        gGame.bowDraw = 0.5f;
        gGame.spyglass = true;
        gSurvival.eatTimer = 1.0f;
        HandsFacts dead;
        dead.alive = false;
        const HandsResult r = Hands({ ACT_HOTBAR_3, ACT_INVENTORY }, {}, dead);
        CHECK(!r.aimed && !r.blockPad && gInv.selected == 0 && gGame.screen == SCREEN_NONE, "the dead do nothing");
        CHECK(gGame.bowDraw < 0.0f && !gGame.spyglass && gSurvival.eatTimer == 0.0f, "and let go of what they were doing");
    }

    // the hotbar, the hands, dropping
    {
        CombatStage(host);
        HandsResult r = Hands({ ACT_HOTBAR_3 });
        CHECK(r.aimed && !r.blockPad && gInv.selected == 2, "the hotbar keys");
        gInv.Held() = Stack(ID_DIRT, 5);
        gInv.offhand = Stack(ID_BREAD, 1);
        HandsFacts nearCar;
        nearCar.swapKeyFree = false;
        Hands({ ACT_SWAP_HANDS }, {}, nearCar);
        CHECK(gInv.Held().id == ID_DIRT, "next to a car the swap key is the host's");
        Hands({ ACT_SWAP_HANDS });
        CHECK(gInv.Held().id == ID_BREAD && gInv.offhand.id == ID_DIRT, "otherwise the hands swap");
        HandsFacts car;
        car.inVehicle = true;
        Hands({ ACT_DROP }, {}, car);
        CHECK(gDrops.empty() && gInv.Held().count == 1, "at the wheel the drop key is the host's");
        Hands({ ACT_DROP });
        CHECK(gDrops.size() == 1 && gInv.Held().Empty(), "on foot it drops the held item");
        gInv.Held() = Stack(ID_DIRT, 5);
        Hands({ ACT_DROP }, { ACT_DROP_STACK });
        CHECK(gDrops.size() == 2 && gInv.Held().Empty() && gDrops[1].stack.count == 5, "with the stack key all of it");
    }

    // fighting and mining
    {
        CombatStage(host);
        const int cow = SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true);
        HandsFacts guns;
        guns.vehicleGuns = true;
        Hands({ ACT_ATTACK }, { ACT_ATTACK }, guns);
        CHECK(gMobs[cow].health == 10.0f && gGame.swing < 0.0f, "when the button fires the vehicle's guns the hands do nothing");
        Hands({ ACT_ATTACK }, { ACT_ATTACK });
        CHECK(gMobs[cow].health == 9.0f, "otherwise they hit");

        CombatStage(host);
        gWorld.SetRaw(1, 0, 9, MakeVox(ID_DIRT));
        gGame.lookDir = down45;
        const int pig = SpawnMob(MOB_PIG, Vec3(1.5f, 0.5f, 10.0f), true);
        Hands({ ACT_ATTACK }, { ACT_ATTACK });
        CHECK(gMobs[pig].health == 9.0f, "a pig in front of the dirt is hit");
        MobsClear();
        for (int i = 0; i < 30; ++i)
            Hands({}, { ACT_ATTACK });
        CHECK(gWorld.GetBlock(1, 0, 9) == ID_DIRT, "holding the button after a hit does not dig");
        Hands({});
        HoldHands(ACT_ATTACK, 1.5f);
        CHECK(gWorld.GetBlock(1, 0, 9) == ID_AIR && CountDrops(ID_DIRT) == 1, "pressed again it digs the dirt out");
        gGame.attackTimer = 5.0f;
        gGame.lookDir = Vec3(0, 0, 1);
        Hands({ ACT_ATTACK }, { ACT_ATTACK });
        CHECK(gGame.attackTimer == 0.0f && gGame.swing >= 0.0f, "a swing at the air starts the cooldown again");
    }

    // using
    {
        CombatStage(host);
        gGame.lookDir = down45;
        gInv.Held() = Stack(ID_OAK_PLANKS, 4);
        Hands({ ACT_USE }, { ACT_USE });
        CHECK(gWorld.GetBlock(1, 0, 10) == ID_OAK_PLANKS && gInv.Held().count == 3, "the use button places the held block");

        CombatStage(host);
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_CRAFTING_TABLE));
        gInv.Held() = Stack(ID_OAK_PLANKS, 4);
        HandsResult r = Hands({ ACT_USE }, { ACT_USE });
        CHECK(r.blockPad && gGame.screen == SCREEN_CRAFTING && gInv.Held().count == 4, "on a crafting table it opens the table");
        CloseScreen();
        HandsFacts sneak;
        sneak.sneaking = true;
        Hands({ ACT_USE }, { ACT_USE }, sneak);
        CHECK(gGame.screen == SCREEN_NONE && gWorld.GetBlock(1, 0, 10) == ID_OAK_PLANKS && gInv.Held().count == 3,
              "sneaking, the block goes against it");

        CombatStage(host);
        host.people.push_back({ Vec3(2.5f, 0.5f, 10.9f) });
        Hands({ ACT_USE }, { ACT_USE });
        CHECK(host.talks == 1, "somebody of the host's in front: the host is asked (villagers trade)");
        HandsFacts car;
        car.inVehicle = true;
        Hands({ ACT_USE }, { ACT_USE }, car);
        CHECK(host.talks == 1, "not from a vehicle");
        host.people[0].centre.x = 5.5f;
        Hands({ ACT_USE }, { ACT_USE });
        CHECK(host.talks == 1, "nor further than 4 m");

        CombatStage(host);
        const int sheep = SpawnMob(MOB_SHEEP, Vec3(2.5f, 0.5f, 10.0f), true);
        gInv.Held() = Stack(ID_SHEARS);
        Hands({ ACT_USE }, { ACT_USE }, car);
        CHECK(!gMobs[sheep].sheared, "an animal is not sheared from a vehicle");
        Hands({ ACT_USE }, { ACT_USE });
        CHECK(gMobs[sheep].sheared, "on foot it is");

        CombatStage(host);
        gInv.Held() = Stack(ID_SNOWBALL, 2);
        Hands({ ACT_USE }, { ACT_USE }, car);
        CHECK(gProjectiles.size() == 1 && gInv.Held().count == 1, "from a vehicle things are still thrown");
    }

    // food, the off hand, the bow
    {
        CombatStage(host);
        gSurvival.food = 10.0f;
        gInv.Held() = Stack(ID_BREAD, 2);
        HandsResult r = HoldHands(ACT_USE, 2.0f);
        CHECK(r.finished == ID_BREAD && gInv.Held().count == 1 && gSurvival.food > 10.0f, "holding the button eats");
        gSurvival.food = 10.0f;
        gInv.Held() = ItemStack();
        gInv.offhand = Stack(ID_BREAD, 1);
        bool offhand = false;
        for (float t = 0.0f; t < 2.0f; t += 0.05f) {
            r = t == 0.0f ? Hands({ ACT_USE }, { ACT_USE }) : Hands({}, { ACT_USE });
            offhand = offhand || gGame.usingOffhand;
        }
        CHECK(offhand && gInv.offhand.Empty() && gInv.Held().Empty() && gSurvival.food > 10.0f, "with nothing in the main hand the off hand eats");
        gSurvival.food = 20.0f;
        gInv.Held() = Stack(ID_MILK_BUCKET);
        r = HoldHands(ACT_USE, 2.0f);
        CHECK(r.finished == ID_MILK_BUCKET && gInv.Held().id == ID_BUCKET, "milk is drunk even when full (the host is told)");

        CombatStage(host);
        gInv.Held() = Stack(ID_BOW);
        gInv.slots[1] = Stack(ID_ARROW, 2);
        HoldHands(ACT_USE, 1.2f);
        CHECK(gProjectiles.empty() && gGame.bowDraw > 1.0f, "the bow is drawn while the button is held");
        Hands({});
        CHECK(gProjectiles.size() == 1, "and shoots when it is let go");
    }

    // game mode and camera keys
    {
        CombatStage(host);
        gControls = Controls();
        gControls.Set(ACT_GAME_MODE, true, true);
        gControls.Set(ACT_PERSPECTIVE, true, true);
        GameKeysTick();
        CHECK(gGame.gameMode == MODE_CREATIVE && gGame.cameraMode == CAM_THIRD_BACK && gGame.messageTimer > 0.0f, "the game mode and the camera change");
        GameKeysTick();
        CHECK(gGame.gameMode == MODE_SURVIVAL && gGame.cameraMode == CAM_THIRD_FRONT, "back, and the camera goes round");
        GameKeysTick();
        CHECK(gGame.cameraMode == CAM_FIRST, "in Minecraft's order");
        const int mode = gGame.gameMode;
        gGame.screen = SCREEN_INVENTORY;
        GameKeysTick();
        CHECK(gGame.gameMode == mode && gGame.cameraMode == CAM_FIRST, "not while a screen is open");
    }
    gControls = Controls();
    gGame = GameState();
    MobsClear();
    CombatClear();
    ResetSurvival();
}

// a click in the middle of the slot at (gx, gy) of the open screen
static void ClickAt(int gx, int gy, bool right = false, bool shift = false) { ScreenClick(gx + 8.0f, gy + 8.0f, !right, right, shift); }

static void TestScreens() {
    TestHost host;
    SetHost(&host);
    McsaSrand(4711);

    // the inventory
    {
        CombatStage(host);
        OpenScreen(SCREEN_INVENTORY);
        BuildSlots();
        CHECK(gSlots.size() == 46 && gWinW == 176 && gWinH == 166, "36 slots, 4 armour, the off hand, 2x2 crafting and its result (%d)",
              (int)gSlots.size());
        CHECK(SlotAt(16, 150) && SlotAt(16, 150)->st == &gInv.slots[0] && SlotAt(16, 92)->st == &gInv.slots[9] && !SlotAt(3, 3),
              "where the slots are");
        CHECK(OverWindow(1, 1) && !OverWindow(-1, 5) && !OverWindow(5, 170) && TabAt(5, -10) == -1, "the window; no tabs here");
        CHECK(std::string(ScreenTitle()) == "Crafting", "its title");

        gInv.slots[0] = Stack(ID_DIRT, 10);
        ClickAt(8, 142);
        CHECK(gInv.cursor.id == ID_DIRT && gInv.cursor.count == 10 && gInv.slots[0].Empty() && gWorld.dirty, "a left click picks the stack up");
        ClickAt(8 + 18, 142, true);
        CHECK(gInv.slots[1].count == 1 && gInv.cursor.count == 9, "a right click puts one down");
        ClickAt(8 + 18, 142, true);
        CHECK(gInv.slots[1].count == 2 && gInv.cursor.count == 8, "and another one");
        ClickAt(8 + 18, 142);
        CHECK(gInv.slots[1].count == 10 && gInv.cursor.Empty(), "a left click puts all of it down");
        ClickAt(8 + 18, 142, true);
        CHECK(gInv.slots[1].count == 5 && gInv.cursor.count == 5, "a right click on a stack takes half");
        gInv.slots[2] = Stack(ID_STONE, 3);
        ClickAt(8 + 36, 142);
        CHECK(gInv.slots[2].id == ID_DIRT && gInv.cursor.id == ID_STONE, "something else: they swap");
        ClickAt(8 + 36, 142);
        ClickAt(8 + 18, 142);
        CHECK(gInv.slots[1].count == 10 && gInv.cursor.Empty() && gInv.slots[2].id == ID_STONE, "the same kind is put together");
        gInv.cursor.Clear();

        gInv.slots[1] = Stack(ID_DIRT, 10);
        ClickAt(8 + 18, 142, false, true);
        CHECK(gInv.slots[1].Empty() && gInv.slots[9].id == ID_DIRT && gInv.slots[9].count == 10, "shift moves from the hotbar up");
        ClickAt(8, 84, false, true);
        CHECK(gInv.slots[9].Empty() && gInv.CountOf(ID_DIRT) == 10 && gInv.slots[0].id == ID_DIRT, "and back down into the first free hotbar slot");
        gInv.slots[3] = Stack(ID_IRON_HELMET);
        ClickAt(8 + 54, 142, false, true);
        CHECK(gInv.armor[0].id == ID_IRON_HELMET && gInv.slots[3].Empty() && Heard(SND_EQUIP_GENERIC) == 1, "armour goes on with shift");
        gInv.cursor = Stack(ID_IRON_BOOTS);
        ClickAt(8, 8);
        CHECK(gInv.armor[0].id == ID_IRON_HELMET && gInv.cursor.id == ID_IRON_BOOTS, "boots do not fit the helmet slot");
        ClickAt(8, 8 + 54);
        CHECK(gInv.armor[3].id == ID_IRON_BOOTS && gInv.cursor.Empty(), "they fit the boots slot");

        // crafting in the 2x2 grid
        gInv.cursor = Stack(ID_OAK_PLANKS, 8);
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                ClickAt(98 + c * 18, 18 + r * 18, true);
        BuildSlots();
        CHECK(gInv.craft[0].count == 1 && gInv.craft[3].count == 1 && gInv.cursor.count == 4, "four planks in the grid");
        ClickAt(154, 28);
        CHECK(gInv.cursor.id == ID_OAK_PLANKS && gInv.cursor.count == 4, "the result cannot go onto planks");
        gInv.cursor.Clear();
        ClickAt(154, 28);
        CHECK(gInv.cursor.id == ID_CRAFTING_TABLE && gInv.craft[0].Empty(), "a crafting table, the planks are used");
        gInv.cursor.Clear();
        for (int i = 0; i < 4; ++i)
            gInv.craft[i] = Stack(ID_OAK_PLANKS, 3);
        ClickAt(154, 28, false, true);
        CHECK(gInv.CountOf(ID_CRAFTING_TABLE) == 3 && gInv.craft[0].Empty() && gInv.slots[8].id == ID_CRAFTING_TABLE && gInv.slots[8].count == 3,
              "shift crafts as many as it can, into the hotbar from its right end");

        // throwing out
        gInv.cursor = Stack(ID_STONE, 5);
        ScreenClick(-20.0f, 50.0f, false, true, false);
        CHECK(gInv.cursor.count == 4 && CountDrops(ID_STONE) == 1, "a right click outside the window throws one out");
        ScreenClick(-20.0f, 50.0f, true, false, false);
        CHECK(gInv.cursor.Empty() && CountDrops(ID_STONE) == 5, "a left click all of it");
        ScreenClick(40.0f, 3.0f, true, false, false);
        CHECK(gInv.cursor.Empty(), "inside the window, between the slots, nothing happens");
        CloseScreen();
    }

    // a chest and a furnace
    {
        CombatStage(host);
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_CHEST));
        OpenScreen(SCREEN_CHEST, Int3{ 2, 0, 10 });
        BuildSlots();
        CHECK(gSlots.size() == 63 && gWinH == 167, "a chest has 27 slots more");
        gInv.slots[0] = Stack(ID_DIRT, 64);
        gInv.slots[1] = Stack(ID_IRON_HELMET);
        ClickAt(8, 143, false, true);
        ClickAt(26, 143, false, true);
        CHECK(gWorld.chests[(Int3{ 2, 0, 10 })].slots[0].count == 64 && gWorld.chests[(Int3{ 2, 0, 10 })].slots[1].id == ID_IRON_HELMET &&
                  gInv.armor[0].Empty(),
              "shift puts things into the chest (armour too)");
        ClickAt(8, 18, false, true);
        CHECK(gInv.slots[8].count == 64 && gInv.slots[9].Empty(), "and takes them out into the hotbar, from its right end");
        CloseScreen();

        gWorld.SetRaw(3, 0, 10, MakeVox(ID_FURNACE));
        OpenScreen(SCREEN_FURNACE, Int3{ 3, 0, 10 });
        BuildSlots();
        gInv = PlayerInventory();
        gInv.slots[0] = Stack(ID_RAW_IRON, 3);
        gInv.slots[1] = Stack(ID_COAL, 2);
        gInv.slots[2] = Stack(ID_DIRT, 1);
        ClickAt(8, 142, false, true);
        ClickAt(26, 142, false, true);
        ClickAt(44, 142, false, true);
        FurnaceState* f = OpenFurnace();
        CHECK(f->input.id == ID_RAW_IRON && f->fuel.id == ID_COAL && gInv.slots[2].Empty() && gInv.slots[9].id == ID_DIRT,
              "shift sends ore to the input, coal to the fuel, the rest up");
        gInv.cursor = Stack(ID_DIRT, 1);
        ClickAt(56, 53);
        CHECK(f->fuel.id == ID_COAL && gInv.cursor.id == ID_DIRT, "dirt does not burn");
        gInv.cursor.Clear();
        f->output = Stack(ID_IRON_INGOT, 2);
        ClickAt(116, 35);
        CHECK(gInv.cursor.id == ID_IRON_INGOT && gInv.cursor.count == 2 && f->output.Empty(), "the output is taken");
        f->output = Stack(ID_IRON_INGOT, 1);
        ClickAt(116, 35, true);
        CHECK(gInv.cursor.count == 3, "onto what the cursor holds");
        gInv.cursor.Clear();
        f->output = Stack(ID_IRON_INGOT, 5);
        ClickAt(116, 35, false, true);
        CHECK(f->output.Empty() && gInv.slots[8].id == ID_IRON_INGOT && gInv.slots[8].count == 5, "shift sends the output to the hotbar's right end");
        f->input = Stack(ID_RAW_IRON, 2);
        ClickAt(56, 17, false, true);
        CHECK(f->input.Empty() && gInv.CountOf(ID_RAW_IRON) == 2 && gInv.slots[10].id == ID_RAW_IRON, "and the input back to the inventory's top");
        gInv.cursor = Stack(ID_DIRT, 1);
        ClickAt(116, 35);
        CHECK(f->output.Empty() && gInv.cursor.id == ID_DIRT, "nothing goes into the output");
        gInv.cursor.Clear();
        CloseScreen();
    }

    // creative
    {
        CombatStage(host);
        gGame.gameMode = MODE_CREATIVE;
        OpenScreen(SCREEN_CREATIVE);
        gGame.creativeTab = 0;
        BuildSlots();
        CHECK(gWinW == 195 && gSlots.size() == 54 && SlotAt(17, 26)->kind == SK_PALETTE && !SlotAt(17, 26)->st->Empty(),
              "the item tabs: a palette of 45 and the hotbar");
        const uint16_t first = SlotAt(17, 26)->st->id;
        ScreenClick(17, 26, true, false, false);
        CHECK(gInv.cursor.id == first && gInv.cursor.count == MaxStack(first), "a left click takes a full stack");
        ScreenClick(17, 26, true, false, false);
        CHECK(gInv.cursor.Empty(), "clicking the palette again puts it away");
        ScreenClick(17, 26, false, true, false);
        CHECK(gInv.cursor.count == 1, "a right click takes one");
        gInv.cursor.Clear();
        ScreenClick(17, 26, true, false, true);
        CHECK(gInv.CountOf(first) == MaxStack(first), "shift puts a stack into the inventory");
        ScreenScroll(3);
        BuildSlots();
        CHECK(gGame.creativeScroll == std::min(3, std::max(0, PaletteRows() - 5)), "the wheel scrolls the palette (%d rows)", PaletteRows());
        ScreenScroll(1000);
        BuildSlots();
        CHECK(gGame.creativeScroll == std::max(0, PaletteRows() - 5), "not past its end");
        float x, y, w, h;
        TabBox(2, x, y, w, h);
        CHECK(TabAt(x + 3, y + 3) == 2 && x == 54.0f && y == -28.0f, "the tabs above the window");
        TabBox(CAT_COUNT, x, y, w, h);
        CHECK(x == 195.0f - 26.0f && y == 136.0f - 4.0f, "the inventory tab at the bottom right");
        ScreenClick(x + 3, y + 3, true, false, false);
        CHECK(gGame.creativeTab == CAT_COUNT && gGame.creativeScroll == 0 && Heard(SND_CLICK) == 1 && CreativeInventoryTab(), "a click picks a tab");
        BuildSlots();
        CHECK(gSlots.size() == 27 + 4 + 1 + 1 + 9 && std::string(ScreenTitle()) == TabName(CAT_COUNT), "there the whole inventory, and the trash");
        gGame.creativeScroll = 0;
        ScreenScroll(2);
        CHECK(gGame.creativeScroll == 0, "the wheel does nothing there");
        gInv.cursor = Stack(ID_DIRT, 5);
        ClickAt(173, 112);
        CHECK(gInv.cursor.Empty() && gInv.CountOf(first) == MaxStack(first), "the trash takes what the cursor holds");
        ClickAt(173, 112, false, true);
        CHECK(gInv.CountOf(first) == 0, "shift on the trash empties the inventory");
        CloseScreen();
    }
    gGame = GameState();
    ResetSurvival();
}

static bool SameRect(const GuiRect& a, const GuiRect& b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }

static int CountSprites(const std::vector<HudPiece>& v, const GuiRect& r) {
    int n = 0;
    for (const HudPiece& p : v)
        n += p.kind == HP_SPRITE && SameRect(p.src, r);
    return n;
}

static const HudPiece* FindSprite(const std::vector<HudPiece>& v, const GuiRect& r) {
    for (const HudPiece& p : v)
        if (p.kind == HP_SPRITE && SameRect(p.src, r))
            return &p;
    return nullptr;
}

static const HudPiece* FindText(const std::vector<HudPiece>& v, const std::string& part) {
    for (const HudPiece& p : v)
        if (p.kind == HP_TEXT && p.text.find(part) != std::string::npos)
            return &p;
    return nullptr;
}

static int CountKind(const std::vector<HudPiece>& v, int kind) {
    int n = 0;
    for (const HudPiece& p : v)
        n += p.kind == kind;
    return n;
}

static void TestHud() {
    TestHost host;
    SetHost(&host);
    McsaSrand(5150);
    std::vector<HudPiece> v;
    HudFacts f;
    f.camera = Vec3(0.5f, 0.5f, 30.0f);

    // hidden
    {
        CombatStage(host);
        f.shown = false;
        BuildHud(f, v);
        CHECK(v.empty(), "with the HUD off nothing is drawn");
        ShowMessage("hello");
        BuildHud(f, v);
        CHECK(v.size() == 1 && v[0].kind == HP_TEXT && v[0].text == "hello" && v[0].anchor == AT_TOP && v[0].y == 40.0f && v[0].align == 1,
              "but messages are");
        f.shown = true;
    }

    // the hotbar and the stats
    {
        CombatStage(host);
        gInv.selected = 2;
        gInv.slots[0] = Stack(ID_DIRT, 3);
        f.health = 0.55f;
        v.clear();
        BuildHud(f, v);
        const HudPiece* bar = FindSprite(v, GUI_HOTBAR);
        CHECK(bar && bar->anchor == AT_BOTTOM && bar->x == -91.0f && bar->y == -22.0f, "the hotbar at the bottom");
        const HudPiece* sel = FindSprite(v, GUI_SELECTION);
        CHECK(sel && sel->x == -91.0f + 40.0f - 1.0f && sel->y == -23.0f, "the selection on the third slot");
        CHECK(CountKind(v, HP_STACK) == 9 && v[0].kind != HP_STACK, "nine slots");
        CHECK(CountSprites(v, GUI_HEART_CONTAINER) == 10 && CountSprites(v, GUI_HEART_FULL) == 5 && CountSprites(v, GUI_HEART_HALF) == 1,
              "55%% health: five and a half hearts");
        CHECK(FindSprite(v, GUI_HEART_FULL)->y == -39.0f && CountSprites(v, GUI_ARMOR_EMPTY) == 0, "steady, no armour row");
        CHECK(CountSprites(v, GUI_FOOD_FULL) == 10 && FindSprite(v, GUI_FOOD_FULL)->x == -91.0f + 173.0f, "full food, from the right");
        CHECK(CountSprites(v, GUI_XP_BG) == 1 && CountSprites(v, GUI_AIR) == 0 && !FindText(v, "0"), "the experience bar, empty; no air");
        CHECK(FindSprite(v, GUI_CROSSHAIR) && FindSprite(v, GUI_CROSSHAIR)->invert && FindSprite(v, GUI_CROSSHAIR)->anchor == AT_CENTRE, "the crosshair");

        gSurvival.xpProgress = 0.5f;
        gSurvival.xpLevel = 7;
        gSurvival.food = 7.0f;
        gSurvival.air = kMaxAir * 0.5f;
        gInv.armor[0] = Stack(ID_IRON_HELMET);
        gInv.offhand = Stack(ID_BREAD);
        AddEffect(EFFECT_POISON, 30.0f, 0);
        AddEffect(EFFECT_ABSORPTION, 30.0f, 1);
        gGame.selectedNameTimer = 0.5f;
        gInv.selected = 0;
        v.clear();
        BuildHud(f, v);
        GuiRect xp = GUI_XP_PROGRESS;
        xp.w = 91;
        CHECK(CountSprites(v, xp) == 1, "half the experience bar");
        const HudPiece* lvl = FindText(v, "7");
        CHECK(lvl && lvl->outline && !lvl->shadow && lvl->y == -35.0f, "the level in green with a black outline");
        CHECK(CountSprites(v, GUI_HEART_POISONED_FULL) == 5 && CountSprites(v, GUI_HEART_FULL) == 0, "poison turns the hearts green");
        CHECK(CountSprites(v, GUI_HEART_ABSORBING_FULL) == 4 && FindSprite(v, GUI_HEART_ABSORBING_FULL)->y == -49.0f,
              "absorption: four yellow hearts above (%.1f points)", gSurvival.absorption);
        CHECK(CountSprites(v, GUI_ARMOR_FULL) == 1 && CountSprites(v, GUI_ARMOR_EMPTY) == 9 && FindSprite(v, GUI_ARMOR_FULL)->y == -59.0f,
              "an iron helmet: one armour point, the row moved up");
        CHECK(CountSprites(v, GUI_FOOD_FULL) == 3 && CountSprites(v, GUI_FOOD_HALF) == 1 && CountSprites(v, GUI_FOOD_EMPTY) == 10, "7 food");
        CHECK(CountSprites(v, GUI_AIR) + CountSprites(v, GUI_AIR_BURSTING) == 5 && FindSprite(v, GUI_AIR)->y == -49.0f, "half the air: five bubbles");
        CHECK(FindSprite(v, GUI_OFFHAND) && FindSprite(v, GUI_OFFHAND)->x == -120.0f && CountKind(v, HP_STACK) == 10, "the off hand slot");
        const HudPiece* name = FindText(v, ItemName(ID_DIRT));
        CHECK(name && name->y == -59.0f && name->align == 1 && (name->color >> 24) == 127, "the held item's name fades");
        const HudPiece* ebg = FindSprite(v, GUI_EFFECT_BG);
        CHECK(CountSprites(v, GUI_EFFECT_BG) == 2 && ebg->anchor == AT_TOP_RIGHT && ebg->x == -25.0f && ebg->y == 1.0f, "two effects, top right");
        const HudPiece* poison = FindText(v, EffectName(EFFECT_POISON));
        CHECK(poison && poison->align == 2 && poison->y == 27.0f + 3.0f && FindText(v, "0:30") && FindText(v, "0:30")->y == 1.0f + 13.0f,
              "their names and time left");

        ClearEffects();
        gInv.armor[0] = ItemStack();
        f.hostAbsorb = 10.0f;
        f.health = 0.15f;
        gSurvival.food = 4.0f;
        gSurvival.saturation = 0.0f;
        AddEffect(EFFECT_HUNGER, 5.0f, 0);
        bool heartsShake = false, foodShakes = false;
        for (int i = 0; i < 10; ++i) {
            v.clear();
            BuildHud(f, v);
            for (const HudPiece& p : v) {
                if (p.kind == HP_SPRITE && SameRect(p.src, GUI_HEART_CONTAINER) && p.y != -39.0f)
                    heartsShake = true;
                if (p.kind == HP_SPRITE && SameRect(p.src, GUI_FOOD_EMPTY_HUNGER) && p.y != -39.0f)
                    foodShakes = true;
            }
        }
        CHECK(heartsShake && foodShakes, "low health and an empty stomach shake");
        CHECK(CountSprites(v, GUI_ARMOR_FULL) == 0 && CountSprites(v, GUI_HEART_ABSORBING_FULL) == 5 && CountSprites(v, GUI_FOOD_HALF_HUNGER) == 0 &&
                  CountSprites(v, GUI_FOOD_FULL_HUNGER) == 2,
              "the host's body armour shows as yellow hearts, not armour; hunger turns the food green");
        ClearEffects();

        gGame.gameMode = MODE_CREATIVE;
        v.clear();
        BuildHud(f, v);
        CHECK(CountSprites(v, GUI_HEART_CONTAINER) == 0 && CountSprites(v, GUI_XP_BG) == 0 && FindText(v, ItemName(ID_DIRT))->y == -45.0f,
              "creative: no hearts, no experience");
        gGame.cameraMode = CAM_THIRD_FRONT;
        v.clear();
        BuildHud(f, v);
        CHECK(!FindSprite(v, GUI_CROSSHAIR), "no crosshair looking at the player's face");
        gGame.cameraMode = CAM_FIRST;
        gSurvival.burn = 5.0f;
        v.clear();
        BuildHud(f, v);
        CHECK(CountKind(v, HP_OVERLAY) == 2 && v[0].kind == HP_OVERLAY && v[0].u0 > v[0].u1 && v[0].y > 0.1f && v[0].y + v[0].h > 1.0f,
              "burning in first person: two flames rise from the bottom of the screen");
        gGame.cameraMode = CAM_THIRD_BACK;
        v.clear();
        BuildHud(f, v);
        CHECK(CountKind(v, HP_OVERLAY) == 0, "not seen from behind");
        gGame.cameraMode = CAM_FIRST;
        gSurvival.burn = 0.0f;
        gWorld.SetRaw(0, 0, 30, MakeVox(ID_WATER));
        v.clear();
        BuildHud(f, v);
        CHECK(v[0].kind == HP_FILL && v[0].w == 0.0f && (v[0].color & 0xFFFFFF) == 0x2A50C8, "inside water the screen is blue");
        gWorld.SetRaw(0, 0, 30, MakeVox(ID_AIR));
    }

    // an open screen
    {
        CombatStage(host);
        f = HudFacts();
        f.camera = Vec3(0.5f, 0.5f, 30.0f);
        gInv.slots[0] = Stack(ID_DIRT, 3);
        OpenScreen(SCREEN_INVENTORY);
        BuildSlots();
        f.mouseX = 8 + 4;
        f.mouseY = 142 + 4;
        v.clear();
        BuildHud(f, v);
        CHECK(!FindSprite(v, GUI_CROSSHAIR) && FindSprite(v, GUI_WIN_INVENTORY) && FindSprite(v, GUI_WIN_INVENTORY)->anchor == AT_WINDOW,
              "the inventory window, no crosshair");
        CHECK(CountKind(v, HP_PLAYER) == 1 && FindText(v, ScreenTitle()) && FindText(v, ScreenTitle())->x == 97.0f, "the player's doll and the title");
        CHECK(CountSprites(v, GUI_SLOT_HELMET) == 1 && CountSprites(v, GUI_SLOT_SHIELD) == 1, "empty armour and off hand slots show their shapes");
        bool hover = false;
        for (const HudPiece& p : v)
            hover = hover || (p.kind == HP_FILL && p.anchor == AT_WINDOW && p.x == 8.0f && p.y == 142.0f && p.w == 16.0f);
        CHECK(hover && CountKind(v, HP_TOOLTIP) == 1 && v[v.size() - 1].text == ItemName(ID_DIRT), "the slot under the mouse lights up, with a tooltip");
        gInv.cursor = Stack(ID_STONE, 2);
        v.clear();
        BuildHud(f, v);
        CHECK(CountKind(v, HP_TOOLTIP) == 0 && v.back().kind == HP_STACK && v.back().anchor == AT_CURSOR && v.back().x == -8.0f,
              "what the cursor holds hangs on the mouse");
        gInv.cursor.Clear();
        CloseScreen();

        gWorld.SetRaw(2, 0, 10, MakeVox(ID_BLAST_FURNACE));
        OpenScreen(SCREEN_FURNACE, Int3{ 2, 0, 10 });
        FurnaceState* fs = OpenFurnace();
        fs->burnTime = 50;
        fs->burnTimeTotal = 100;
        fs->input = Stack(ID_RAW_IRON);
        fs->cookTime = 50;
        BuildSlots();
        v.clear();
        BuildHud(f, v);
        CHECK(FindSprite(v, GUI_WIN_BLAST) && FindText(v, "Inventory") && FindText(v, "Inventory")->y == 72.0f, "a blast furnace has its own window");
        const HudPiece* lit = nullptr;
        const HudPiece* arrow = nullptr;
        for (const HudPiece& p : v) {
            if (p.kind == HP_SPRITE && p.src.x == GUI_LIT.x)
                lit = &p;
            if (p.kind == HP_SPRITE && p.src.x == GUI_BURN.x && p.src.y == GUI_BURN.y)
                arrow = &p;
        }
        CHECK(lit && lit->src.h == 7 && lit->y == 43.0f && arrow && arrow->src.w == 12, "half the fire, half the arrow (blast furnace: 100 ticks)");
        CloseScreen();

        gGame.gameMode = MODE_CREATIVE;
        OpenScreen(SCREEN_CREATIVE);
        gGame.creativeTab = 0;
        BuildSlots();
        f.mouseX = 5;
        f.mouseY = -20;
        v.clear();
        BuildHud(f, v);
        CHECK(CountKind(v, HP_ICON) == TAB_COUNT && FindSprite(v, GUI_TAB_TOP_SELECTED_1) && CountSprites(v, GUI_TAB_TOP_UNSELECTED_1) == 0,
              "creative: every tab with its icon, the first one picked");
        CHECK(CountSprites(v, GUI_SCROLLER) == 1 && FindSprite(v, GUI_SCROLLER)->y == 18.0f, "the scroller at the top");
        CHECK(CountKind(v, HP_TOOLTIP) == 1 && v.back().text == TabName(0), "the tab under the mouse says its name");
        CloseScreen();
    }

    // you died
    {
        CombatStage(host);
        gGame.deathTime = 0.5f;
        gGame.deathCause = STR_DEATH_DROWN;
        gSurvival.xpTotal = 42;
        f = HudFacts();
        f.camera = Vec3(0.5f, 0.5f, 30.0f);
        f.playerName = "CJ";
        v.clear();
        BuildHud(f, v);
        const HudPiece* veil = nullptr;
        for (const HudPiece& p : v)
            if (p.kind == HP_GRADIENT)
                veil = &p;
        CHECK(veil && veil->scale == 0.5f && veil->color == 0x60500000u && veil->color2 == 0xA0803030u, "a red veil fades in");
        const HudPiece* title = FindText(v, LangStr(STR_YOU_DIED));
        CHECK(title && title->scale == 2.0f && title->y == 60.0f, "\"You died!\" twice as big");
        CHECK(FindText(v, "CJ") && FindText(v, "CJ")->y == 85.0f, "the cause, with the player's name");
        const HudPiece* score = FindText(v, std::string(LangStr(STR_SCORE)).substr(0, 3));
        CHECK(score && score->text2 == "42" && score->text.find("%s") == std::string::npos, "the score");
        CHECK(!FindText(v, LangStr(STR_RESPAWN)), "no button yet");
        gGame.deathTime = 1.5f;
        v.clear();
        BuildHud(f, v);
        const HudPiece* button = FindSprite(v, MENU_BUTTON_HI);
        CHECK(button && button->texture == HT_MENU && button->anchor == AT_QUARTER && FindText(v, LangStr(STR_RESPAWN)), "after a second the button");
    }
    gGame = GameState();
    ResetSurvival();
}

// the mesh of the chunk around the origin, in a world with just these blocks
static BlockMesh MeshOf(std::initializer_list<std::pair<Int3, Voxel>> blocks) {
    gWorld.Clear();
    for (const auto& b : blocks)
        gWorld.SetRaw(b.first.x, b.first.y, b.first.z, b.second);
    BlockMesh m;
    MeshChunk(*gWorld.FindChunk({ 0, 0, 0 }), m);
    return m;
}

static void TestBlockMesh() {
    TestHost host;
    SetHost(&host);

    {
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_STONE) } });
        CHECK(m.verts.size() == 24 && m.shade.size() == 24 && m.emissive.size() == 24 && m.tverts.empty() && m.nverts.empty(),
              "a lone stone block: six faces");
        int top = 0, bottom = 0, side = 0;
        for (size_t i = 0; i < m.verts.size(); ++i) {
            if (m.verts[i].n == (Int3{ 0, 0, 1 }))
                top += m.shade[i] == 255;
            if (m.verts[i].n == (Int3{ 0, 0, -1 }))
                bottom += m.shade[i] == 127;
            if (m.verts[i].n == (Int3{ 1, 0, 0 }))
                side += m.shade[i] == 153;
        }
        CHECK(top == 4 && bottom == 4 && side == 4, "Minecraft's light per face: top 1, east 0.6, bottom 0.5");
        const TileUV uv = AtlasTileUV(BlockFaceTile(ID_STONE, FACE_TOP, 0));
        bool inTile = true;
        for (const MeshVertex& v : m.verts)
            inTile = inTile && v.u >= uv.u0 - 1e-6f && v.u <= uv.u1 + 1e-6f && v.v >= uv.v0 - 1e-6f && v.v <= uv.v1 + 1e-6f;
        CHECK(inTile && uv.u1 > uv.u0 && uv.u1 - uv.u0 < 16.0f / ATLAS_SIZE, "its texture: one tile of the atlas");

        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_STONE) }, { { 4, 3, 3 }, MakeVox(ID_STONE) } });
        CHECK(m.verts.size() == 40, "two stones side by side: the faces between them are hidden");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_STONE) }, { { 4, 3, 4 }, MakeVox(ID_STONE) } });
        int dark = 0;
        for (size_t i = 0; i < m.verts.size(); ++i)
            if (m.verts[i].n == (Int3{ 0, 0, 1 }) && m.verts[i].z == 4.0f && m.verts[i].x == 4.0f)
                dark += m.shade[i] < 255;
        CHECK(dark == 2, "a block above the edge darkens the top's two corners there (ambient occlusion)");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_GLASS) }, { { 4, 3, 3 }, MakeVox(ID_GLASS) } });
        CHECK(m.verts.size() == 40, "glass next to glass shows no face between");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_GLASS) }, { { 4, 3, 3 }, MakeVox(ID_STONE) } });
        CHECK(m.verts.size() == 44, "glass does not hide the stone behind it");
    }

    {
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_POPPY) } });
        CHECK(m.verts.size() == 8 && m.shade[0] == 230, "a flower: two crossed sheets");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_WATER) } });
        CHECK(m.verts.empty() && m.tverts.size() == 24 && m.tanim.size() == 24, "water is translucent");
        bool still = false, flow = false;
        for (uint8_t a : m.tanim) {
            still = still || a == ANIM_WATER_STILL;
            flow = flow || a == ANIM_WATER_FLOW;
        }
        float topZ = 0.0f;
        for (const MeshVertex& v : m.tverts)
            if (v.n == (Int3{ 0, 0, 1 }))
                topZ = v.z;
        CHECK(still && flow && topZ > 3.5f && topZ < 4.0f, "still on top (a little lower than a block, %.2f), flowing on the sides", topZ);
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_WATER) }, { { 3, 3, 4 }, MakeVox(ID_WATER) } });
        int tops = 0;
        for (const MeshVertex& v : m.tverts)
            tops += v.n == (Int3{ 0, 0, 1 });
        CHECK(tops == 4, "water under water has no surface");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_LAVA) } });
        CHECK(m.verts.size() == 24 && m.emissive[0] == 1 && m.anim[0] == ANIM_LAVA_STILL, "lava glows");
        m = MeshOf({ { { 3, 3, 2 }, MakeVox(ID_STONE) }, { { 3, 3, 3 }, MakeVox(ID_FIRE) } });
        int fire = 0;
        for (uint8_t a : m.anim)
            fire += a == ANIM_FIRE_0 || a == ANIM_FIRE_1;
        CHECK(fire == 16, "fire on the floor: four leaning flames");
    }

    {
        host.ownGround = false;
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_DIRT, META_NATURAL) } });
        CHECK(m.verts.size() == 24 && m.nverts.empty(), "ground of ours is drawn as usual");
        host.ownGround = true;
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_DIRT, META_NATURAL) } });
        CHECK(m.verts.empty() && m.nverts.size() == 24 && m.nshade[0] < 128, "the host's dug ground goes apart, darker deep down");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_DIRT) } });
        CHECK(m.verts.size() == 24, "a placed block is never the host's");
        host.ownGround = false;
    }
    gWorld.Clear();
}

// counts the models' quads and where they go
struct RecordSink : ModelSink {
    int tex = -1, quads[2] = { 0, 0 };
    Vec3 lo, hi;
    uint32_t color = 0;
    void Reset() {
        tex = -1;
        quads[0] = quads[1] = 0;
        lo = Vec3(1e9f, 1e9f, 1e9f);
        hi = Vec3(-1e9f, -1e9f, -1e9f);
    }
    void Texture(int t) override { tex = t; }
    void Quad(const Vec3* p, const float*, const float*, uint32_t c) override {
        ++quads[tex == MT_ATLAS ? 1 : 0];
        color = c;
        for (int k = 0; k < 4; ++k) {
            lo = Vec3(std::min(lo.x, p[k].x), std::min(lo.y, p[k].y), std::min(lo.z, p[k].z));
            hi = Vec3(std::max(hi.x, p[k].x), std::max(hi.y, p[k].y), std::max(hi.z, p[k].z));
        }
    }
    // every tile is an 8x8 square in the middle
    bool Opaque(int, int px, int py) override { return px >= 4 && px < 12 && py >= 4 && py < 12; }
};

static void TestModels() {
    RecordSink sink;
    SetModelSink(&sink);
    const Pose stand = EntityPose(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), 1.0f);

    sink.Reset();
    DrawMob(MOB_COW, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    const float cowH = sink.hi.z;
    CHECK(sink.quads[0] > 30 && sink.quads[1] == 0 && sink.lo.z > -0.01f && cowH > 1.2f && cowH < 1.6f,
          "a cow: %d quads of entity.png, %.2f m high", sink.quads[0], cowH);
    CHECK(sink.hi.y - sink.lo.y > 1.0f && sink.hi.x - sink.lo.x < 1.0f, "longer than it is wide");
    sink.Reset();
    DrawMob(MOB_CHICKEN, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] > 20 && sink.hi.z < cowH * 0.75f, "a chicken is small (%.2f m)", sink.hi.z);
    sink.Reset();
    DrawMob(MOB_COW, stand, MobAnim(), 1.0f, 1.0f, 0.45f, 0.45f);
    CHECK(((sink.color >> 8) & 255) * 2 < ((sink.color >> 16) & 255) + 2 && (sink.color & 255) * 2 < ((sink.color >> 16) & 255) + 2,
          "a hurt animal flashes red");

    sink.Reset();
    DrawMob(MOB_CREEPER, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    const float creeperW = sink.hi.x - sink.lo.x;
    CHECK(sink.quads[0] == 36 && sink.hi.z > 1.6f && sink.hi.z < 1.75f && sink.lo.z > -0.01f, "a creeper: six cubes, 26 px high (%.2f)", sink.hi.z);
    MobAnim swelling;
    swelling.swell = 0.95f;
    sink.Reset();
    DrawMob(MOB_CREEPER, stand, swelling, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.hi.x - sink.lo.x > creeperW * 1.25f && sink.lo.z > -0.01f, "about to blow up it is a third wider, still on its feet");

    sink.Reset();
    DrawMob(MOB_SPIDER, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] == 60 && sink.lo.z > -0.01f && sink.lo.z < 0.02f, "a spider stands on its own feet, not in the air (lowest point %.3f m)", sink.lo.z);

    sink.Reset();
    DrawMob(MOB_ZOMBIE, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.lo.z > -0.01f && sink.lo.z < 0.02f, "a zombie stands on its own feet, not sunk in the ground (lowest point %.3f m)", sink.lo.z);

    sink.Reset();
    DrawMob(MOB_SKELETON, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.lo.z > -0.01f && sink.lo.z < 0.02f, "a skeleton stands on its own feet, not sunk in the ground (lowest point %.3f m)", sink.lo.z);

    sink.Reset();
    DrawMob(MOB_SOLDIER, stand, MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] == 1010 && sink.lo.z > -0.01f && sink.lo.z < 0.02f && sink.hi.z > 1.5f && sink.hi.z < 1.75f,
          "a soldier: Counter-Strike's own model, every triangle of it, on his own feet (lowest point %.3f m, %.2f m tall in his idle, %d quads)",
          sink.lo.z, sink.hi.z, sink.quads[0]);

    sink.Reset();
    HumanoidAnim a;
    PlayerAnimInput in;
    ComputePlayerAnim(a, in);
    DrawPlayerModel(stand, a, ModelStyle());
    CHECK(sink.quads[0] == 72 && sink.hi.z > 1.95f && sink.hi.z < 2.1f, "Steve: 12 cubes with their outer layer, 32 px = 2 blocks (%.2f)", sink.hi.z);
    CHECK(a.rArm.rx == 0.0f && a.lLeg.rx == 0.0f, "standing still");
    in.limbSwing = 1.0f;
    in.limbAmount = 1.0f;
    ComputePlayerAnim(a, in);
    CHECK(a.rArm.rx != 0.0f && Near(a.rArm.rx, -a.lArm.rx, 1e-3f) && Near(a.rLeg.rx, -a.lLeg.rx, 1e-3f) && a.rArm.rx * a.rLeg.rx < 0.0f,
          "walking swings arms and legs against each other");
    in = PlayerAnimInput();
    in.crouch = true;
    ComputePlayerAnim(a, in);
    CHECK(a.body.rx > 0.4f, "sneaking bends the body forward");
    in = PlayerAnimInput();
    in.attack = 0.5f;
    HumanoidAnim still;
    ComputePlayerAnim(still, PlayerAnimInput());
    ComputePlayerAnim(a, in);
    CHECK(a.rArm.rx < still.rArm.rx - 0.5f, "a swing raises the right arm");

    sink.Reset();
    Pose item;
    DrawItemModel(item, ID_STONE, 1.0f);
    CHECK(sink.quads[1] == 6 && sink.quads[0] == 0, "a block in the hand: a cube from the atlas");
    sink.Reset();
    DrawItemModel(item, ID_STICK, 1.0f);
    CHECK(sink.quads[1] == 2 + 32, "a flat item: front, back and one edge per border pixel (%d)", sink.quads[1]);
    SetModelSink(nullptr);
}

static void RunAnim(float seconds, const PlayerMotion& m) {
    for (float t = 0.0f; t < seconds; t += 0.05f)
        PlayerAnimTick(0.05f, &m);
}

static void TestPlayerAnim() {
    TestHost host;
    SetHost(&host);
    McsaSrand(31337);

    // the body's clocks
    {
        CombatStage(host);
        PlayerAnimTick(0.5f, nullptr);
        CHECK(Near(gGame.age, 0.5f), "without a player only the clock runs");
        PlayerMotion m;
        m.velocity = Vec3(4.3f, 0, 0);
        RunAnim(1.0f, m);
        CHECK(gGame.walkAmount > 0.6f && gGame.walkPhase > 5.0f && gGame.bob > 0.05f && gGame.walkDist > 2.0f, "walking swings the legs and bobs the view");
        m.standing = false;
        RunAnim(1.0f, m);
        CHECK(gGame.bob < 0.01f, "in the air the view stops bobbing");
        m.onFoot = false;
        RunAnim(1.0f, m);
        CHECK(gGame.walkAmount < 0.01f, "in a vehicle the legs rest");
        m = PlayerMotion();
        m.swimming = true;
        m.velocity = Vec3(2.0f, 0, 0);
        RunAnim(0.5f, m);
        CHECK(SwimPose() > 0.99f, "swimming lies down");
        m.velocity = Vec3();
        RunAnim(0.5f, m);
        CHECK(SwimPose() < 0.01f, "floating still stands up again");
        StartSwing();
        RunAnim(0.1f, PlayerMotion());
        CHECK(gGame.swing > 0.2f && gGame.swing < 0.5f, "a swing takes 0.3 s");
        RunAnim(0.3f, PlayerMotion());
        CHECK(gGame.swing < 0.0f, "and ends");
        gGame.sprinting = true;
        RunAnim(1.0f, PlayerMotion());
        CHECK(Near(gGame.fovMod, 1.15f, 0.01f), "sprinting widens the view");
        gGame.sprinting = false;

        gInv.Held() = Stack(ID_DIRT);
        gGame.handItem = ID_DIRT;
        gGame.handHeight = 1.0f;
        gGame.attackTimer = 10.0f;
        gInv.Held() = Stack(ID_STONE);
        RunAnim(0.1f, PlayerMotion());
        CHECK(gGame.handHeight < 0.5f && gGame.handItem == ID_DIRT, "a new item: the hand goes down first");
        RunAnim(0.5f, PlayerMotion());
        CHECK(gGame.handItem == ID_STONE && gGame.handHeight > 0.9f, "and comes up with the new one");
        m = PlayerMotion();
        m.alive = false;
        RunAnim(0.5f, m);
        CHECK(gGame.handItem == 0, "the dead hold nothing");
    }

    // what the items look like while used
    {
        CombatStage(host);
        CHECK(HeldItemTile(ID_BOW) == -1 && HeldItemTile(ID_STONE) == -1, "nothing special");
        gGame.bowDraw = 0.3f;
        CHECK(HeldItemTile(ID_BOW) == TILE_BOW_PULLING_0, "a bow being drawn");
        gGame.bowDraw = 1.0f;
        CHECK(HeldItemTile(ID_BOW) == TILE_BOW_PULLING_2, "fully drawn");
        gInv.Held() = Stack(ID_CROSSBOW);
        gGame.crossbowSlot = gInv.selected;
        gGame.crossbowRocket = true;
        CHECK(HeldItemTile(ID_CROSSBOW) == TILE_CROSSBOW_FIREWORK, "a crossbow loaded with a firework");
        gInv.Held() = Stack(ID_FISHING_ROD);
        UseHeldItem();
        CHECK(HeldItemTile(ID_FISHING_ROD) == TILE_FISHING_ROD_CAST, "a cast rod");
        FishingClear();
    }

    // the third person look and the first person hands
    {
        CombatStage(host);
        gInv.Held() = Stack(ID_DIAMOND_SWORD);
        gInv.offhand = Stack(ID_BREAD);
        gInv.armor[ARMOR_CHEST] = Stack(ID_ELYTRA);
        gInv.armor[0] = Stack(ID_IRON_HELMET);
        gGame.lookDir = Vec3(1, 0, 0);
        gGame.hurtTimer = 0.3f;
        PlayerDrawInput in = PlayerLook(Vec3(0, -1, 0), Vec3(1, 0, 0), false, true, false, 0.8f);
        CHECK(in.held == ID_DIAMOND_SWORD && in.offHeld == ID_BREAD && in.anim.holding && in.anim.holdingLeft && in.elytra && in.armor[0] == ID_IRON_HELMET,
              "the items, the elytra and the armour");
        CHECK(in.anim.crouch && in.g < 0.5f && in.r == 1.0f && in.light == 0.8f && Near(in.anim.headYaw, 0.0f) && Near(in.anim.headPitch, 0.0f),
              "sneaking, hurt, looking straight ahead");
        gGame.lookDir = Vec3(0, -1, 0);
        in = PlayerLook(Vec3(0, -1, 0), Vec3(1, 0, 0), false, false, false, 1.0f);
        CHECK(Near(in.anim.headYaw, 1.3f), "the head turns, but not all the way (%.2f)", in.anim.headYaw);
        in = PlayerLook(Vec3(0, -1, 0), Vec3(1, 0, 0), false, false, true, 1.0f);
        CHECK(in.held == 0 && in.anim.limbAmount == 0.0f, "the dead hold nothing");
        gSurvival.eatTimer = 0.8f;
        in = PlayerLook(Vec3(0, -1, 0), Vec3(1, 0, 0), false, false, false, 1.0f);
        CHECK(in.anim.armPose == ARM_EAT && Near(in.anim.useTicks, 16.0f), "eating raises the arm");

        gGame.handItem = ID_DIAMOND_SWORD;
        gGame.handHeight = 1.0f;
        FirstPersonInput h = HandInput(false);
        CHECK(h.item == ID_DIAMOND_SWORD && !h.leftHand && Near(h.eat, 0.5f) && h.lowered == 0.0f, "the main hand eats half way");
        gGame.offItem = ID_BREAD;
        gGame.usingOffhand = true;
        h = HandInput(true);
        gGame.offHeight = 0.25f;
        h = HandInput(true);
        CHECK(h.leftHand && h.item == ID_BREAD && Near(h.eat, 0.5f) && Near(h.lowered, 0.75f), "the off hand eats too, while coming up");
        gGame.usingOffhand = false;
        gSurvival.eatTimer = 0.0f;
    }

    // the dead tip over
    {
        Vec3 r(1, 0, 0), u(0, 0, 1);
        DeathTilt(0.0f, r, u);
        CHECK(Near(u.z, 1.0f), "at first upright");
        DeathTilt(1.0f, r, u);
        CHECK(Near(u.x, -1.0f, 1e-3f) && Near(r.z, 1.0f, 1e-3f), "after a second on the side");
    }

    // lava and fire near the player
    {
        CombatStage(host);
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_LAVA));
        gWorld.SetRaw(-3, 0, 10, MakeVox(ID_FIRE));
        gWorld.ticking.insert(Int3{ 3, 0, 10 });
        gWorld.ticking.insert(Int3{ -3, 0, 10 });
        HeatTick(0.05f, Vec3(0.5f, 0.5f, 10.0f));
        CHECK(HotBlocks().size() == 2 && HotBlocks()[0].d2 <= HotBlocks()[1].d2, "the lava and the fire, nearest first");
        CHECK(HotBlockAt(Vec3(3.5f, 0.5f, 10.5f)) == ID_LAVA && HotBlockAt(Vec3(-2.5f, 0.5f, 10.2f)) == ID_FIRE &&
                  HotBlockAt(Vec3(0.5f, 0.5f, 10.5f)) == ID_AIR,
              "what burns where");
        gParticles.clear();
        for (int i = 0; i < 400; ++i)
            HeatTick(0.05f, Vec3(0.5f, 0.5f, 10.0f));
        CHECK(CountParticles(TILE_P_GENERIC_0) > 20 && CountParticles(TILE_P_LAVA) >= 1 && Heard(SND_FIRE_AMBIENT) > 2,
              "fire smokes and crackles, lava pops (%d drops)", CountParticles(TILE_P_LAVA));
        BlockRulesClear();
        CHECK(HotBlocks().empty(), "a new world forgets them");
        gWorld.ticking.clear();
    }

    // bubbles
    {
        CombatStage(host);
        gGame.eyePos = Vec3(0.5f, 0.5f, 11.6f);
        BreathEffects(0.05f, false, true);
        CHECK(CountParticles(TILE_P_BUBBLE) == 8 && gGame.deathCause == STR_DEATH_DROWN, "drowning: a burst of bubbles");
        gParticles.clear();
        for (int i = 0; i < 40; ++i)
            BreathEffects(0.05f, true, false);
        CHECK(CountParticles(TILE_P_BUBBLE) >= 2 && CountParticles(TILE_P_BUBBLE) <= 6, "under water a few now and then (%d in 2 s)",
              CountParticles(TILE_P_BUBBLE));
    }

    // items in the GUI
    {
        IconQuad q[kMaxIconQuads];
        CHECK(ItemIcon(ID_STONE, q) == 3 && q[0].color == 0xFFFFFFFF && q[1].color == 0xFFCCCCCC && q[2].color == 0xFF999999, "a block: a little cube");
        CHECK(ItemIcon(ID_POPPY, q) == 1 && q[0].x[1] == 16.0f, "a flower: flat");
        CHECK(ItemIcon(ID_STICK, q) == 1 && q[0].tile == Item(ID_STICK).tile, "an item: its tile");
        CHECK(ItemIcon(0, q) == 0, "nothing: nothing");
        int w = 0;
        uint32_t col = 0;
        CHECK(!DurabilityBar(Stack(ID_DIAMOND_SWORD), &w, &col) && !DurabilityBar(Stack(ID_DIRT), &w, &col), "new tools and blocks have no bar");
        ItemStack worn = Stack(ID_DIAMOND_SWORD);
        worn.damage = (uint16_t)(Item(ID_DIAMOND_SWORD).durability / 2);
        CHECK(DurabilityBar(worn, &w, &col) && (w == 6 || w == 7) && ((col >> 16) & 255) > 200 && ((col >> 8) & 255) > 200, "half worn: half a yellow bar");
        worn.damage = (uint16_t)(Item(ID_DIAMOND_SWORD).durability - 1);
        CHECK(DurabilityBar(worn, &w, &col) && w == 0 && ((col >> 16) & 255) == 255 && ((col >> 8) & 255) < 10, "nearly broken: red");
    }
    gGame = GameState();
    ResetSurvival();
}

static void TestRenderers() {
    TestHost host;
    SetHost(&host);
    RecordSink sink;
    SetModelSink(&sink);
    const Vec3 R(1, 0, 0), U(0, 0, 1);

    // things on the ground
    {
        DropEntity d;
        d.pos = Vec3(0.5f, 0.5f, 10.0f);
        d.stack = Stack(ID_STONE, 20);
        sink.Reset();
        CHECK(DrawDrop(d, 1.0f) && sink.quads[1] == 18 && sink.lo.z > 10.0f, "twenty stone: three little cubes, floating (%d)", sink.quads[1]);
        d.stack = Stack(ID_STICK, 1);
        sink.Reset();
        DrawDrop(d, 1.0f);
        CHECK(sink.quads[1] == 34, "a stick: one flat item");
        d.stack = ItemStack();
        CHECK(!DrawDrop(d, 1.0f), "nothing: no drawing, no shadow");

        PrimedTnt t;
        t.pos = Vec3(0.5f, 0.5f, 10.0f);
        t.fuse = 2.0f;
        sink.Reset();
        DrawPrimedTnt(t, 1.0f);
        CHECK(sink.quads[1] == 6 && sink.quads[0] == 6, "lit TNT blinks white");
        t.fuse = 0.3f;
        sink.Reset();
        DrawPrimedTnt(t, 1.0f);
        CHECK(sink.quads[1] == 6 && sink.quads[0] == 0, "and dark");
        t.fuse = 0.05f;
        sink.Reset();
        DrawPrimedTnt(t, 1.0f);
        CHECK(sink.hi.z - sink.lo.z > 1.1f, "it swells just before the blast (%.2f)", sink.hi.z - sink.lo.z);

        XpOrb o;
        o.pos = Vec3(0.5f, 0.5f, 10.0f);
        o.value = 20;
        sink.Reset();
        DrawXpOrb(o, R, U);
        const float small = sink.hi.x - sink.lo.x;
        o.value = 2500;
        sink.Reset();
        DrawXpOrb(o, R, U);
        CHECK(sink.quads[0] == 1 && sink.hi.x - sink.lo.x > small, "experience orbs: bigger for more");
    }

    // particles
    {
        Particle p;
        p.pos = Vec3(0.5f, 0.5f, 10.0f);
        p.size = 0.1f;
        p.tile = TILE_P_BUBBLE;
        p.color = 0xFFFFFFFF;
        sink.Reset();
        DrawParticle(p, R, U, 0.5f);
        CHECK(sink.quads[1] == 1 && ((sink.color >> 16) & 255) == 127 && (sink.color >> 24) == 255, "a particle in the dark is darker");
        p.glow = true;
        sink.Reset();
        DrawParticle(p, R, U, 0.5f);
        CHECK(((sink.color >> 16) & 255) == 255, "a glowing one is not");
    }

    // falling blocks
    {
        BlockStage();
        gWorld.Set(0, 0, 15, MakeVox(ID_SAND));
        RunBlocks(0.05f);
        sink.Reset();
        DrawFallingBlocks(1.0f);
        CHECK(!FallingBlocks().empty() && sink.quads[1] == 6 * (int)FallingBlocks().size(), "falling sand is drawn as a cube");
        BlockRulesClear();
    }

    // what flies
    {
        Projectile pr;
        pr.type = PJ_ARROW;
        pr.pos = Vec3(0.5f, 0.5f, 10.0f);
        pr.vel = Vec3(10, 0, 0);
        sink.Reset();
        DrawProjectile(pr, R, U, 1.0f);
        CHECK(sink.quads[0] == 2 && sink.hi.x - sink.lo.x > 0.85f, "an arrow: two crossed strips along its flight");
        pr.type = PJ_SNOWBALL;
        sink.Reset();
        DrawProjectile(pr, R, U, 1.0f);
        CHECK(sink.quads[1] == 1 && sink.quads[0] == 0, "a snowball: its sprite");
        pr.type = PJ_TRIDENT;
        sink.Reset();
        DrawProjectile(pr, R, U, 1.0f);
        CHECK(sink.quads[0] > 6, "a trident: its model");

        Bolt b{ Vec3(0.5f, 0.5f, 10.0f), 0.0f, 12345u };
        sink.Reset();
        DrawBolt(b, Vec3(20.5f, 0.5f, 12.0f));
        CHECK(sink.quads[0] == 2 * (16 + 6 + 6) && sink.hi.z > 100.0f, "lightning: a jagged column 110 m high");
        b.age = 0.07f;
        sink.Reset();
        DrawBolt(b, Vec3(20.5f, 0.5f, 12.0f));
        CHECK(sink.quads[0] == 0, "it flickers");

        gBobber = Bobber();
        gBobber.active = true;
        gBobber.pos = Vec3(5.5f, 0.5f, 10.0f);
        sink.Reset();
        DrawFishingHook(R, U, 1.0f);
        DrawFishingLine(Vec3(0.5f, 0.5f, 11.5f), Vec3(0.5f, -2.0f, 11.6f));
        CHECK(sink.quads[0] == 1 + 16, "the bobber and a line of sixteen pieces");
        FishingClear();
    }

    // cracks and the camera
    {
        CHECK(CrackTile(0.0f) == TILE_DESTROY_0 && CrackTile(0.55f) == TILE_DESTROY_0 + 5 && CrackTile(1.5f) == TILE_DESTROY_0 + 9, "ten crack stages");
        gGame = GameState();
        Vec3 pos(0, 0, 10), look(0, 1, 0);
        gGame.bob = 0.1f;
        gGame.walkDist = 0.25f;
        BobView(Vec3(0, 1, 0), pos, look);
        CHECK(pos.z > 10.0f && std::fabs(pos.x) > 0.01f && look.z < 0.0f && Near(look.Length(), 1.0f, 1e-3f), "walking bobs the view");
        gGame.fovMod = 1.15f;
        CHECK(Near(FovWanted(70.0f), 80.5f, 1e-3f), "sprinting widens it");
        gGame.spyglass = true;
        CHECK(Near(FovWanted(70.0f), 8.05f, 1e-3f), "the spyglass narrows it ten times");
        gGame = GameState();
    }
    SetModelSink(nullptr);
}

static void TestCreeper() {
    TestHost host;
    SetHost(&host);
    McsaSrand(2718);
    const Vec3 player(0.5f, 0.5f, 11.0f); // (his middle; his feet are on the floor at z = 10)

    // it goes for a player in survival, swells and blows up
    {
        MobStage();
        host.explosions = 0;
        int c = SpawnMob(MOB_CREEPER, Vec3(8.5f, 0.5f, 10.0f), true);
        CHECK(c >= 0 && gMobs[c].health == 20.0f && Near(MobHeight(gMobs[c]), 1.7f), "a creeper: 10 hearts, 1.7 m");
        gGame.gameMode = MODE_CREATIVE;
        RunMobs(3.0f, player);
        CHECK(gMobs[c].swell == 0.0f && (gMobs[c].pos - Vec3(0.5f, 0.5f, 10.0f)).Length() > 3.0f, "a player in creative is left alone");
        gGame.gameMode = MODE_SURVIVAL;
        gMobs[c].pos = Vec3(8.5f, 0.5f, 10.0f);
        float t = 0.0f;
        while (gMobs[c].swell == 0.0f && t < 10.0f) {
            MobsTick(0.05f, player, 0, true);
            t += 0.05f;
        }
        const float d = (gMobs[c].pos - Vec3(0.5f, 0.5f, 10.0f)).Length();
        CHECK(t < 4.0f && d < 3.0f && d > 2.0f && Heard(SND_CREEPER_PRIMED) == 1, "it walks up to him and hisses at 3 m (%.1f s)", t);
        RunMobs(1.0f, player);
        CHECK(gMobs.size() == 1 && gMobs[c].swell > 0.6f && gMobs[c].vel.Length() < 0.5f, "it stands and swells");
        RunMobs(0.6f, player);
        CHECK(gMobs.empty() && host.explosions == 1 && host.lastBlast == BLAST_TNT && Heard(SND_EXPLODE) == 1, "after 1.5 s it blows up, and is gone");
        CHECK(gWorld.GetBlock(3, 0, 9) == ID_AIR && gDrops.size() > 0 && gGame.deathCause == STR_DEATH_EXPLOSION, "taking the floor with it");

        // running away
        MobStage();
        host.explosions = 0;
        c = SpawnMob(MOB_CREEPER, Vec3(2.5f, 0.5f, 10.0f), true);
        RunMobs(0.6f, player);
        CHECK(gMobs[c].swell > 0.2f, "it starts to swell");
        RunMobs(0.6f, Vec3(10.5f, 0.5f, 11.0f));
        CHECK(gMobs[c].swellDir < 0 && gMobs[c].swell < 0.5f, "8 m away it calms down");
        RunMobs(4.0f, Vec3(30.5f, 0.5f, 11.0f));
        CHECK(gMobs.size() == 1 && gMobs[c].swell == 0.0f && host.explosions == 0, "and does not blow up");

        // hit and killed
        MobStage();
        c = SpawnMob(MOB_CREEPER, Vec3(6.5f, 0.5f, 10.0f), true);
        MobHurt(c, 5.0f, player, 1.0f);
        CHECK(gMobs[c].panic == 0.0f && Heard(SND_CREEPER_HURT) == 1, "a hit does not scare it");
        MobHurt(c, 0.0f, player, 0.0f);
        gMobs[c].hurt = 0.0f;
        MobHurt(c, 30.0f, player, 0.0f);
        RunMobs(1.2f, Vec3(40.5f, 0.5f, 11.0f));
        int xp = 0;
        for (const XpOrb& o : gXpOrbs)
            xp += o.value;
        CHECK(gMobs.empty() && Heard(SND_CREEPER_DEATH) == 1 && CountDrops(ID_GUNPOWDER) <= 2 && xp == 5, "killed: gunpowder and 5 experience");
    }

    // only at night
    {
        MobStage();
        host.herdGround = true;
        gRules.animals = false;
        for (int i = 0; i < 100; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.empty(), "no creepers by day");
        host.dark = true;
        for (int i = 0; i < 200; ++i)
            MobsSpawnTick(0.5f, player);
        bool creepers = !gMobs.empty();
        for (const Mob& m : gMobs)
            creepers = creepers && IsMonster(m.kind) && !m.persistent;
        CHECK(creepers && gMobs.size() == 4, "at night they come out, four at most (%d)", (int)gMobs.size());
        gRules.monsters = false;
        MobsClear();
        for (int i = 0; i < 100; ++i)
            MobsSpawnTick(0.5f, player);
        CHECK(gMobs.empty(), "not when the rules say no");
        host.dark = false;
    }
    MobsClear();
    gRules = GameRules();
    gGame = GameState();
}

static void TestShapes() {
    TestHost host;
    SetHost(&host);
    McsaSrand(31415);

    // what they are made of
    {
        ShapeBox b[kMaxShapeBoxes];
        CHECK(IsShapedBlock(ID_OAK_STAIRS) && IsSolidBlock(ID_OAK_STAIRS) && !IsOpaqueBlock(ID_OAK_STAIRS) && !IsShapedBlock(ID_STONE),
              "stairs are solid, but not a full cube");
        CHECK(BlockShapeBoxes(ID_OAK_STAIRS, FACE_EAST, b) == 2 && b[0].z1 == 0.5f && b[1].x0 == 0.5f && b[1].z0 == 0.5f && b[1].z1 == 1.0f,
              "stairs: a lower slab and the half rising to the east");
        CHECK(BlockShapeBoxes(ID_OAK_STAIRS, FACE_SOUTH | META_UPSIDE, b) == 2 && b[0].z0 == 0.5f && b[1].y1 == 0.5f && b[1].z1 == 0.5f,
              "upside down: the slab on top");
        CHECK(BlockShapeBoxes(ID_STONE_SLAB, 0, b) == 1 && b[0].z1 == 0.5f, "a slab: the lower half");
        CHECK(BlockShapeBoxes(ID_STONE_SLAB, META_SLAB_TOP, b) == 1 && b[0].z0 == 0.5f, "or the upper one");
        CHECK(BlockShapeBoxes(ID_STONE_SLAB, META_SLAB_DOUBLE, b) == 1 && b[0].z0 == 0.0f && b[0].z1 == 1.0f, "or both");
        CHECK(Craft(3, 3, { ID_OAK_PLANKS, 0, 0, ID_OAK_PLANKS, ID_OAK_PLANKS, 0, ID_OAK_PLANKS, ID_OAK_PLANKS, ID_OAK_PLANKS }).id == ID_OAK_STAIRS &&
                  Craft(3, 3, { ID_STONE, ID_STONE, ID_STONE, 0, 0, 0, 0, 0, 0 }).id == ID_STONE_SLAB,
              "both are crafted as in Minecraft");
    }

    // drawn
    {
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_OAK_STAIRS, FACE_EAST) } });
        CHECK(m.verts.size() == 48, "lone stairs: two boxes, twelve faces (%d)", (int)m.verts.size() / 4);
        float top = 0.0f;
        for (const MeshVertex& v : m.verts)
            top = std::max(top, v.z);
        CHECK(top == 4.0f, "a block high");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_OAK_STAIRS, FACE_EAST) }, { { 3, 3, 2 }, MakeVox(ID_STONE) } });
        CHECK(m.verts.size() == 44 + 24, "on stone: their bottom is hidden, the stone's top is not");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_STONE_SLAB, 0) } });
        bool half = true;
        for (const MeshVertex& v : m.verts)
            half = half && v.z <= 3.5f;
        CHECK(m.verts.size() == 24 && half, "a slab: half a block high");
    }

    // walked on
    {
        TestMap map;
        BlockStage();
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_OAK_STAIRS, FACE_EAST));
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_STONE));
        std::vector<Aabb> boxes;
        GatherBoxes(map, Aabb{ 2.1f, 0.1f, 10.1f, 2.4f, 0.4f, 10.4f }, boxes);
        bool low = false;
        for (const Aabb& a : boxes)
            low = low || (a.x0 == 2.0f && a.z1 == 10.5f);
        CHECK(low, "the physics sees the stairs' boxes");
        Body b = Standing(0.5f, 0.5f);
        WalkInput in;
        in.fwd = 1.0f;
        in.look = Vec3(1, 0, 0); // east
        WalkEvents ev;
        float highest = 0.0f, highestX = 0.0f;
        for (int i = 0; i < 120; ++i) {
            WalkStep(b, in, 1.0f / 60.0f, map, ev);
            if (b.pos.z > highest) {
                highest = b.pos.z;
                highestX = b.pos.x;
            }
        }
        CHECK(highest > 11.95f && highestX < 4.0f, "walking east the player goes up the stairs onto the stone (%.2f at x %.2f)",
              highest, highestX);
    }

    // placed
    {
        CombatStage(host);
        gGame.lookDir = Vec3(1, 0, 0);
        gTarget = Target();
        gTarget.valid = gTarget.voxel = true;
        gTarget.pos = Int3{ 3, 0, 9 };
        gTarget.face = FACE_TOP;
        gTarget.point = Vec3(3.5f, 0.5f, 10.0f);
        gTarget.normal = Vec3(0, 0, 1);
        gInv.Held() = Stack(ID_OAK_STAIRS, 4);
        PlaceHeldBlock();
        const Voxel v = gWorld.Get(3, 0, 10);
        CHECK(VoxBlock(v) == ID_OAK_STAIRS && VoxMeta(v) == FACE_EAST, "stairs put down looking east rise to the east");
        gWorld.SetRaw(5, 0, 11, MakeVox(ID_STONE));
        gTarget.pos = Int3{ 5, 0, 11 };
        gTarget.face = FACE_WEST;
        gTarget.point = Vec3(5.0f, 0.5f, 11.8f);
        gTarget.normal = Vec3(-1, 0, 0);
        PlaceHeldBlock();
        CHECK(VoxMeta(gWorld.Get(4, 0, 11)) == (FACE_EAST | META_UPSIDE), "against the upper half of a side: upside down");

        gInv.Held() = Stack(ID_STONE_SLAB, 4);
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_AIR));
        gTarget.pos = Int3{ 3, 0, 9 };
        gTarget.face = FACE_TOP;
        gTarget.point = Vec3(3.5f, 0.5f, 10.0f);
        gTarget.normal = Vec3(0, 0, 1);
        PlaceHeldBlock();
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_STONE_SLAB, 0) && gInv.Held().count == 3, "a slab on the floor: the lower half");
        gTarget.pos = Int3{ 3, 0, 10 };
        gTarget.point = Vec3(3.5f, 0.5f, 10.5f);
        PlaceHeldBlock();
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_STONE_SLAB, META_SLAB_DOUBLE) && gWorld.GetBlock(3, 0, 11) == ID_AIR && gInv.Held().count == 2,
              "another on top of it makes it double");
        gInv.Held() = Stack(ID_DIAMOND_PICKAXE);
        BreakVoxel(Int3{ 3, 0, 10 }, true);
        CHECK(CountDrops(ID_STONE_SLAB) == 2, "a double slab breaks into two (%d)", CountDrops(ID_STONE_SLAB));
    }

    // panes, bars, fences, walls: they join their neighbours
    {
        ShapeBox b[kMaxShapeBoxes];
        CHECK(ShapeConnections(ID_OAK_FENCE, ID_OAK_FENCE, ID_AIR, ID_STONE, ID_GLASS_PANE) == (1 | 4),
              "a fence joins a fence and a full block, not air or a pane");
        CHECK(ShapeConnections(ID_GLASS_PANE, ID_IRON_BARS, ID_GLASS, ID_OAK_STAIRS, ID_GLASS_PANE) == (1 | 2 | 8) &&
                  ShapeConnections(ID_STONE, ID_STONE, ID_STONE, ID_STONE, ID_STONE) == 0,
              "a pane joins bars, glass and panes, not stairs; a cube joins nothing");
        CHECK(BlockShapeBoxes(ID_GLASS_PANE, 0, b, 0) == 1 && b[0].x1 - b[0].x0 == 2.0f / 16.0f, "a lone pane: its post");
        CHECK(BlockShapeBoxes(ID_OAK_FENCE, 0, b, 1) == 3 && b[1].x0 == 10.0f / 16.0f && b[1].x1 == 1.0f, "a fence: its post and two bars east");
        CHECK(BlockShapeBoxes(ID_OAK_FENCE, 0, b, 1, true) == 2 && b[0].z1 == 1.5f && b[1].z1 == 1.5f, "bumped into: 1.5 high");
        CHECK(BlockShapeBoxes(ID_COBBLESTONE_WALL, 0, b, 4 | 8) == 3 && b[1].z1 == 14.0f / 16.0f && b[2].y0 == 0.0f, "a wall north and south");
        CHECK(Craft(3, 3, { ID_OAK_PLANKS, ID_STICK, ID_OAK_PLANKS, ID_OAK_PLANKS, ID_STICK, ID_OAK_PLANKS, 0, 0, 0 }).id == ID_OAK_FENCE &&
                  Craft(3, 3, { ID_GLASS, ID_GLASS, ID_GLASS, ID_GLASS, ID_GLASS, ID_GLASS, 0, 0, 0 }).count == 16 &&
                  Craft(3, 3, { ID_COBBLESTONE, ID_COBBLESTONE, ID_COBBLESTONE, ID_COBBLESTONE, ID_COBBLESTONE, ID_COBBLESTONE, 0, 0, 0 }).id ==
                      ID_COBBLESTONE_WALL,
              "crafted as in Minecraft");

        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_OAK_FENCE) } });
        CHECK(m.verts.size() == 24, "a lone fence: its post");
        m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_OAK_FENCE) }, { { 4, 3, 3 }, MakeVox(ID_OAK_FENCE) } });
        CHECK(m.verts.size() == 2 * 4 * (6 + 5 + 5), "two fences: their bars meet, the faces between them are not drawn (%d)",
              (int)m.verts.size() / 4);

        TestMap map;
        BlockStage();
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_OAK_FENCE));
        std::vector<Aabb> boxes;
        GatherBoxes(map, Aabb{ 2.1f, 0.1f, 10.1f, 2.9f, 0.9f, 11.4f }, boxes);
        bool tall = false;
        for (const Aabb& a : boxes)
            tall = tall || a.z1 == 11.5f;
        CHECK(tall, "the player cannot jump over a fence");
    }

    // as items
    {
        IconQuad q[kMaxIconQuads];
        CHECK(ItemIcon(ID_OAK_STAIRS, q) == 6 && ItemIcon(ID_STONE_SLAB, q) == 3 && q[0].y[0] > 4.0f, "stairs and slabs in the GUI: their shape");
        CHECK(ItemIcon(ID_OAK_FENCE, q) == 15 && ItemIcon(ID_GLASS_PANE, q) == 1, "a fence: post and bars; a pane: flat");
        RecordSink rec;
        SetModelSink(&rec);
        rec.Reset();
        Pose p;
        DrawItemModel(p, ID_OAK_FENCE, 1.0f);
        CHECK(rec.quads[1] == 5 * 6, "a fence in the hand: its boxes (%d)", rec.quads[1]);
        SetModelSink(nullptr);
    }
    gWorld.Clear();
}

static void TestBeds() {
    TestHost host;
    SetHost(&host);
    McsaSrand(2718);

    // what a bed is
    {
        ShapeBox b[kMaxShapeBoxes];
        CHECK(BedOtherBlock(ID_RED_BED) == ID_RED_BED_HEAD && BedOtherBlock(ID_RED_BED_HEAD) == ID_RED_BED && BedOtherBlock(ID_STONE) == 0 &&
                  (FACE_EAST ^ 1) == FACE_WEST && (FACE_NORTH ^ 1) == FACE_SOUTH,
              "a bed: its foot and its head");
        CHECK(BlockShapeBoxes(ID_RED_BED, FACE_NORTH, b) == 3 && b[0].z0 == 3.0f / 16.0f && b[0].z1 == 9.0f / 16.0f && b[1].y0 == 0.0f &&
                  b[1].y1 == 3.0f / 16.0f && b[1].tileFace == FACE_BOTTOM,
              "the mattress, and the legs at the foot's end");
        CHECK(BlockShapeBoxes(ID_RED_BED_HEAD, FACE_NORTH, b) == 3 && b[1].y1 == 1.0f, "the head's legs at its end");
        CHECK(BlockShapeBoxes(ID_RED_BED, FACE_EAST, b, 0, true) == 1 && b[0].z0 == 0.0f && b[0].z1 == 9.0f / 16.0f, "bumped into: one box");
        const BlockDef& d = Block(ID_RED_BED);
        CHECK(BlockFaceTile(ID_RED_BED, FACE_SOUTH, FACE_NORTH) == d.tex[FACE_SOUTH] &&
                  BlockFaceTile(ID_RED_BED, FACE_WEST, FACE_EAST) == d.tex[FACE_SOUTH] &&
                  BlockFaceTile(ID_RED_BED, FACE_NORTH, FACE_EAST) == d.tex[FACE_WEST],
              "turned with its head: the foot's end faces away from it");
        float h = 0.25f, v = 0.0f;
        TurnUv(ShapeUvTurns(ID_RED_BED, FACE_EAST, FACE_TOP), h, v);
        CHECK(ShapeUvTurns(ID_RED_BED, FACE_NORTH, FACE_TOP) == 0 && h == 1.0f && v == 0.25f && ShapeUvTurns(ID_STONE_SLAB, 0, FACE_TOP) == 0,
              "its top turns with it");
        CHECK(Craft(3, 3, { ID_RED_WOOL, ID_RED_WOOL, ID_RED_WOOL, ID_OAK_PLANKS, ID_OAK_PLANKS, ID_OAK_PLANKS, 0, 0, 0 }).id == ID_RED_BED,
              "crafted as in Minecraft");
        IconQuad q[kMaxIconQuads];
        CHECK(ItemIcon(ID_RED_BED, q) == 18, "in the GUI: both halves");
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_RED_BED, FACE_NORTH) }, { { 3, 4, 3 }, MakeVox(ID_RED_BED_HEAD, FACE_NORTH) } });
        CHECK(m.verts.size() == 4 * (2 * 18 - 2), "drawn: the faces between the halves are not (%d)", (int)m.verts.size() / 4);
    }

    // put down, broken
    {
        CombatStage(host);
        gTarget.valid = gTarget.voxel = true;
        gTarget.pos = Int3{ 3, 0, 9 };
        gTarget.face = FACE_TOP;
        gTarget.point = Vec3(3.5f, 0.5f, 10.0f);
        gTarget.normal = Vec3(0, 0, 1);
        gInv.Held() = Stack(ID_RED_BED, 2);
        PlaceHeldBlock();
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_RED_BED, FACE_EAST) && gWorld.Get(4, 0, 10) == MakeVox(ID_RED_BED_HEAD, FACE_EAST) &&
                  gInv.Held().count == 1,
              "looking east: the foot here, the head beyond");
        gWorld.SetRaw(4, 2, 10, MakeVox(ID_STONE));
        gTarget.pos = Int3{ 3, 2, 9 };
        gTarget.point = Vec3(3.5f, 2.5f, 10.0f);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(3, 2, 10) == ID_AIR && gInv.Held().count == 1, "no room for the head: no bed");

        gInv.Held() = ItemStack();
        gGame.gameMode = MODE_SURVIVAL;
        BreakVoxel(Int3{ 4, 0, 10 }, true);
        CHECK(gWorld.GetBlock(3, 0, 10) == ID_AIR && gWorld.GetBlock(4, 0, 10) == ID_AIR && CountDrops(ID_RED_BED) == 1,
              "the head broken: the whole bed goes, one bed drops");
    }

    // slept in
    {
        CombatStage(host);
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_RED_BED, FACE_EAST));
        gWorld.SetRaw(4, 0, 10, MakeVox(ID_RED_BED_HEAD, FACE_EAST));
        gSleep = SleepState();
        host.hours = 13.0f;
        CHECK(UseBed(Int3{ 4, 0, 10 }) && !gSleep.asleep && gSleep.spawnSet && gSleep.spawn == (Int3{ 3, 0, 10 }) &&
                  gGame.message == LangStr(STR_NO_SLEEP),
              "by day: no sleep, but the bed is the respawn point");
        CHECK(!UseBed(Int3{ 5, 0, 10 }), "no bed there");
        host.hours = 23.5f;
        gGame.gameMode = MODE_SURVIVAL;
        int c = SpawnMob(MOB_CREEPER, Vec3(8.5f, 0.5f, 10.0f), true);
        CHECK(UseBed(Int3{ 3, 0, 10 }) && !gSleep.asleep && gGame.message == LangStr(STR_NOT_SAFE), "a creeper near: no rest");
        gMobs[c].pos = Vec3(30.5f, 0.5f, 10.0f);
        UseBed(Int3{ 3, 0, 10 });
        Vec3 spot, feet, head;
        CHECK(gSleep.asleep && SleepSpot(&spot) && spot.x == 4.0f && spot.z == 10.0f + 9.0f / 16.0f && SleepPose(&feet, &head) && head.x == 1.0f,
              "at night: asleep, lying on the bed, the head on the pillow");
        for (int i = 0; i < 40; ++i)
            SleepTick(0.05f, false);
        CHECK(gSleep.asleep && SleepFade() > 0.35f && SleepFade() < 0.45f && host.clockSet < 0.0f, "the screen goes dark");
        for (int i = 0; i < 70 && gSleep.asleep; ++i)
            SleepTick(0.05f, false);
        CHECK(!gSleep.asleep && host.clockSet == 6.0f && host.clockNextDay && SleepFade() > 0.9f, "five seconds: the morning");
        SleepTick(0.5f, false);
        CHECK(SleepFade() == 0.0f, "and the dark goes away");

        UseBed(Int3{ 3, 0, 10 });
        CHECK(!SleepTick(0.05f, true) && !gSleep.asleep && host.clockSet == 6.0f, "the sneak key: he gets up, the night goes on");

        host.moves = 0;
        BedRespawn();
        CHECK(host.moves == 1 && host.player.x == 4.0f && host.player.z == 11.0f + 9.0f / 16.0f, "dying, he comes back at his bed");
        gWorld.SetRaw(4, 0, 10, MakeVox(ID_AIR));
        BedRespawn();
        CHECK(host.moves == 1 && !gSleep.spawnSet && gGame.message == LangStr(STR_NO_SPAWN), "unless it is gone");
    }
    gSleep = SleepState();
    gWorld.Clear();
}

static void TestDoors() {
    TestHost host;
    SetHost(&host);
    McsaSrand(3141);

    // what a door is
    {
        ShapeBox b[kMaxShapeBoxes];
        CHECK(DoorOtherBlock(ID_OAK_DOOR) == ID_OAK_DOOR_UPPER && DoorOtherBlock(ID_OAK_DOOR_UPPER) == ID_OAK_DOOR &&
                  DoorOtherBlock(ID_STONE) == 0 && IsDoorBlock(ID_OAK_DOOR) && IsDoorBlock(ID_OAK_DOOR_UPPER) &&
                  !IsDoorBlock(ID_OAK_PLANKS) && !IsDoorBlock(ID_OAK_STAIRS),
              "a door: its lower half and its upper half, and nothing else");
        const BlockDef& d = Block(ID_OAK_DOOR);
        CHECK(d.shape == SHAPE_DOOR && Block(ID_OAK_DOOR_UPPER).shape == SHAPE_DOOR_UPPER && d.hardness == 2.0f && d.tool == TOOL_AXE &&
                  std::string(d.name[0]) == "Oak Door",
              "oak, worked with an axe, named as in Minecraft");
        // shut: one panel three pixels thick, flush with the edge away from the front
        CHECK(BlockShapeBoxes(ID_OAK_DOOR, FACE_EAST, b) == 1 && b[0].x0 == 0.0f && b[0].x1 == 3.0f / 16.0f && b[0].y0 == 0.0f &&
                  b[0].y1 == 1.0f && b[0].z0 == 0.0f && b[0].z1 == 1.0f,
              "facing east, shut: the panel at the west edge");
        CHECK(BlockShapeBoxes(ID_OAK_DOOR_UPPER, FACE_NORTH, b) == 1 && b[0].y0 == 0.0f && b[0].y1 == 3.0f / 16.0f && b[0].x1 == 1.0f,
              "facing north, shut: the panel at the south edge, the upper half the same");
        // open: swung flat against the side wall on the hinge's side
        CHECK(BlockShapeBoxes(ID_OAK_DOOR, FACE_EAST | DOOR_OPEN, b) == 1 && b[0].y0 == 0.0f && b[0].y1 == 3.0f / 16.0f && b[0].x1 == 1.0f,
              "facing east, open, hinge left: flat on the south wall");
        CHECK(BlockShapeBoxes(ID_OAK_DOOR, FACE_EAST | DOOR_OPEN | DOOR_HINGE, b) == 1 && b[0].y0 == 13.0f / 16.0f && b[0].y1 == 1.0f,
              "facing east, open, hinge right: flat on the north wall");
        CHECK(BlockShapeBoxes(ID_OAK_DOOR, FACE_NORTH | DOOR_OPEN, b) == 1 && b[0].x0 == 13.0f / 16.0f && b[0].x1 == 1.0f && b[0].y1 == 1.0f,
              "facing north, open, hinge left: flat on the east wall");
        IconQuad q[kMaxIconQuads];
        CHECK(ItemIcon(ID_OAK_DOOR, q) == 6, "in the GUI: both halves standing, three faces each");
        const std::vector<uint16_t>& building = CreativeItems(CAT_BUILDING);
        CHECK(std::find(building.begin(), building.end(), (uint16_t)ID_OAK_DOOR) != building.end() &&
                  std::find(building.begin(), building.end(), (uint16_t)ID_OAK_DOOR_UPPER) == building.end(),
              "in the creative tab: the door, never its upper half alone");
        BlockMesh m = MeshOf({ { { 3, 3, 3 }, MakeVox(ID_OAK_DOOR, FACE_EAST) }, { { 3, 3, 4 }, MakeVox(ID_OAK_DOOR_UPPER, FACE_EAST) } });
        CHECK(m.verts.size() == 48, "drawn: two panels, six faces each (%d)", (int)m.verts.size());
    }

    // put down, swung, broken
    {
        CombatStage(host);
        gTarget.valid = gTarget.voxel = true;
        gTarget.pos = Int3{ 3, 0, 9 };
        gTarget.face = FACE_TOP;
        gTarget.point = Vec3(3.5f, 0.5f, 10.0f);
        gTarget.normal = Vec3(0, 0, 1);
        gInv.Held() = Stack(ID_OAK_DOOR, 2);
        PlaceHeldBlock(); // he looks east (CombatStage), so the front faces him: west, hinge left
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_OAK_DOOR, FACE_WEST) && gWorld.Get(3, 0, 11) == MakeVox(ID_OAK_DOOR_UPPER, FACE_WEST) &&
                  gInv.Held().count == 1,
              "looking east: the lower half here, the upper half above it, the front facing him");
        // no room above: no door
        gWorld.SetRaw(5, 0, 11, MakeVox(ID_STONE));
        gTarget.pos = Int3{ 5, 0, 9 };
        gTarget.point = Vec3(5.5f, 0.5f, 10.0f);
        PlaceHeldBlock();
        CHECK(gWorld.GetBlock(5, 0, 10) == ID_AIR && gInv.Held().count == 1, "no room for the upper half: no door");
        gWorld.SetRaw(5, 0, 11, MakeVox(ID_AIR));

        // shut it blocks; used, it opens; the upper half shuts it again
        CHECK(gWorld.IsSolid(3, 0, 10) && gWorld.IsSolid(3, 0, 11), "shut: both halves stop feet");
        gTarget.pos = Int3{ 3, 0, 10 };
        CHECK(TargetHasUse(), "a door has a use");
        const int opened = Heard(SND_DOOR_OPEN);
        UseTargetBlock();
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_OAK_DOOR, FACE_WEST | DOOR_OPEN) &&
                  gWorld.Get(3, 0, 11) == MakeVox(ID_OAK_DOOR_UPPER, FACE_WEST | DOOR_OPEN) && !gWorld.IsSolid(3, 0, 10) &&
                  !gWorld.IsSolid(3, 0, 11) && Heard(SND_DOOR_OPEN) == opened + 1,
              "used: both halves swing open together, nothing stops feet, the door's own sound");
        const int closed = Heard(SND_DOOR_CLOSE);
        gTarget.pos = Int3{ 3, 0, 11 };
        UseTargetBlock();
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_OAK_DOOR, FACE_WEST) && gWorld.IsSolid(3, 0, 10) && Heard(SND_DOOR_CLOSE) == closed + 1,
              "the upper half used: it shuts again");

        // broken at the upper half: both halves go, one door drops
        gInv.Held() = ItemStack();
        gGame.gameMode = MODE_SURVIVAL;
        BreakVoxel(Int3{ 3, 0, 11 }, true);
        CHECK(gWorld.GetBlock(3, 0, 10) == ID_AIR && gWorld.GetBlock(3, 0, 11) == ID_AIR && CountDrops(ID_OAK_DOOR) == 1,
              "the upper half broken: the whole door goes, one door drops");
    }

    // saved and loaded: the whole state is in the cells, so it comes back as it was left
    {
        CombatStage(host);
        const uint8_t meta = FACE_NORTH | DOOR_OPEN | DOOR_HINGE;
        gWorld.SetRaw(3, 0, 10, MakeVox(ID_OAK_DOOR, meta));
        gWorld.SetRaw(3, 0, 11, MakeVox(ID_OAK_DOOR_UPPER, meta));
        const char* path = "mc_tests_door.tmp";
        FILE* f = fopen(path, "wb");
        CHECK(f != nullptr, "the door test file can be written");
        if (f) {
            WriteWorldFile(f, 1);
            fclose(f);
        }
        BlockStage();
        gWorld.Clear();
        WorldFileInfo info;
        f = fopen(path, "rb");
        CHECK(f && ReadWorldFile(f, info) && info.blocksOk, "it reads back");
        if (f)
            fclose(f);
        CHECK(gWorld.Get(3, 0, 10) == MakeVox(ID_OAK_DOOR, meta) && gWorld.Get(3, 0, 11) == MakeVox(ID_OAK_DOOR_UPPER, meta),
              "an open door, its facing and its hinge survive the save");
        std::remove(path);
    }

    // a zombie at a closed door: it stops, beats on the wood, and breaks through
    {
        MobStage();
        host.dark = true; // night, so the sun does not do the player's work for him
        gGame.gameMode = MODE_SURVIVAL;
        for (int y = -3; y <= 3; ++y) {
            if (y == 0)
                continue;
            gWorld.SetRaw(2, y, 10, MakeVox(ID_STONE));
            gWorld.SetRaw(2, y, 11, MakeVox(ID_STONE));
        }
        gWorld.SetRaw(2, 0, 10, MakeVox(ID_OAK_DOOR, FACE_EAST));
        gWorld.SetRaw(2, 0, 11, MakeVox(ID_OAK_DOOR_UPPER, FACE_EAST));
        const Vec3 player(0.5f, 0.5f, 11.0f); // his middle; his feet are on the floor at z = 10
        const int z = SpawnMob(MOB_ZOMBIE, Vec3(4.5f, 0.5f, 10.0f), true);
        CHECK(z >= 0, "a zombie on the other side of the door");
        const int hits = Heard(SND_HIT_WOOD);
        RunMobsB(3.0f, player, nullptr);
        CHECK(gWorld.GetBlock(2, 0, 10) == ID_OAK_DOOR && gMobs[z].pos.x > 3.0f && Heard(SND_HIT_WOOD) > hits,
              "three seconds: at the door, hitting it, still shut out (x %.2f)", gMobs[z].pos.x);
        RunMobsB(4.0f, player, nullptr);
        CHECK(gWorld.GetBlock(2, 0, 10) == ID_AIR && gWorld.GetBlock(2, 0, 11) == ID_AIR && CountDrops(ID_OAK_DOOR) >= 1,
              "about five seconds of hitting: the door breaks, both halves, and drops");
        RunMobsB(2.0f, player, nullptr);
        CHECK(gMobs[z].pos.x < 3.0f, "and then it comes through");
    }
    gWorld.Clear();
}

static void TestWarden() {
    TestHost host;
    SetHost(&host);
    McsaSrand(1618);
    const Vec3 player(0.5f, 0.5f, 11.0f); // (his middle; his feet are on the floor at z = 10)

    MobStage();
    int w = SpawnMob(MOB_WARDEN, Vec3(6.5f, 0.5f, 10.0f), true);
    CHECK(w >= 0 && gMobs[w].health == 500.0f && Near(MobHeight(gMobs[w]), 2.9f) && IsMonster(MOB_WARDEN) && !IsMonster(MOB_COW) &&
              Item(ID_WARDEN_SPAWN_EGG).special == SP_EGG_WARDEN,
          "a warden: 250 hearts, 2.9 m, from its egg");
    // it hears him walk, not sneak; not at all in creative
    host.velocity = Vec3(3, 0, 0);
    gGame.gameMode = MODE_CREATIVE;
    RunMobs(3.0f, player);
    CHECK(gMobs[w].anger == 0.0f, "a player in creative is not heard");
    gGame.gameMode = MODE_SURVIVAL;
    gGame.sneaking = true;
    gMobs[w].pos = Vec3(11.0f, 0.5f, 10.0f);
    RunMobs(1.5f, player);
    CHECK(gMobs[w].anger == 0.0f, "nor one who sneaks (further than it smells)");
    gGame.sneaking = false;
    gMobs[w].pos = Vec3(9.5f, 0.5f, 10.0f);
    float t = 0.0f;
    while (gMobs[w].anger < 80.0f && t < 10.0f) {
        MobsTick(0.05f, player, 0, true);
        t += 0.05f;
    }
    CHECK(t > 3.5f && t < 4.5f && Heard(SND_WARDEN_LISTENING) >= 2, "walking near it: the third footstep makes it angry (%.1f s)", t);
    host.velocity = Vec3();
    RunMobs(1.0f, player);
    CHECK(gMobs[w].roared && gMobs[w].roar > 0.0f && Heard(SND_WARDEN_ROAR) == 1 && host.playerHurt == 0.0f, "it roars first");
    // from afar: the sonic boom
    gMobs[w].pos = Vec3(9.5f, 0.5f, 10.0f);
    RunMobs(5.5f, player);
    CHECK(Heard(SND_WARDEN_SONIC_CHARGE) == 1 && Heard(SND_WARDEN_SONIC_BOOM) == 1 && host.playerHurt == 10.0f && host.gusts == 1,
          "9 m away: a sonic boom, 5 hearts, and he is thrown back (%.1f)", host.playerHurt);
    // close by: its blows
    gMobs[w].pos = Vec3(2.0f, 0.5f, 10.0f);
    gMobs[w].boomCooldown = 10.0f;
    host.playerHurt = 0.0f;
    RunMobs(1.0f, player);
    CHECK(host.playerHurt == 60.0f && Heard(SND_WARDEN_ATTACK) == 2, "next to him: a blow of 15 hearts every 0.9 s (%.0f)", host.playerHurt);
    // hit: furious, not knocked back
    gMobs[w].anger = 0.0f;
    gMobs[w].vel = Vec3();
    MobHurt(w, 6.0f, Vec3(0.5f, 0.5f, 10.0f), 1.0f);
    CHECK(gMobs[w].anger == 100.0f && gMobs[w].vel.Length() == 0.0f && gMobs[w].health == 494.0f, "a hit makes it furious; it does not budge");
    // left alone: it digs back down
    gGame.gameMode = MODE_CREATIVE;
    gMobs[w].anger = 0.0f;
    RunMobs(65.0f, Vec3(40.5f, 0.5f, 11.0f));
    CHECK(gMobs.empty() && Heard(SND_WARDEN_DIG) == 1 && CountDrops(ID_SCULK_CATALYST) == 0, "a minute after its anger is gone it digs back down");
    gGame.gameMode = MODE_SURVIVAL;
    w = SpawnMob(MOB_WARDEN, Vec3(6.5f, 0.5f, 10.0f), true);
    gMobs[w].health = 1.0f;
    MobHurt(w, 5.0f, player, 0.0f);
    RunMobs(1.5f, Vec3(40.5f, 0.5f, 11.0f));
    CHECK(CountDrops(ID_SCULK_CATALYST) == 1, "killed: a sculk catalyst");

    RecordSink rec;
    SetModelSink(&rec);
    rec.Reset();
    DrawMob(MOB_WARDEN, EntityPose(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), 1.0f), MobAnim(), 1.0f, 1, 1, 1);
    CHECK(rec.quads[0] >= 40 && rec.hi.z > 3.0f && rec.hi.z < 3.8f, "its model: over 3 m with its tendrils (%.2f)", rec.hi.z);
    SetModelSink(nullptr);
    MobsClear();
    gWorld.Clear();
}

static void TestLanguage() {
    CHECK(gLanguage == LANG_EN && std::string(ItemName(ID_STONE)) == "Stone" && std::string(LangStr(STR_SINGLEPLAYER)) == "Singleplayer" &&
              std::string(TabName(CAT_BUILDING)) == "Building Blocks" && std::string(LangStr(STR_NO_SLEEP)).find("night") != std::string::npos,
          "English by default, as Minecraft starts");
    gLanguage = LANG_TR;
    CHECK(std::string(ItemName(ID_STONE)) == "Taş" && std::string(BlockName(ID_CRAFTING_TABLE)) == "Çalışma Masası" &&
              std::string(TabName(CAT_BUILDING)) == "İnşaat Blokları" && std::string(EffectName(EFFECT_POISON)) == "Zehir" &&
              std::string(Tr("a", "b")) == "b",
          "in Turkish: everything Minecraft says changes");
    gLanguage = LANG_EN;
    CHECK(LanguageFromCode("tr") == LANG_TR && LanguageFromCode("xx") == LANG_EN && std::string(LanguageCode(LANG_TR)) == "tr" &&
              std::string(LanguageName(LANG_TR)).find("Türkçe") == 0,
          "the language codes of the ini, the names of the list");
}

static void TestBodyYaw() {
    const float look = 0.0f; // looking north (+y)
    auto run = [&](Vec3 vel, float body) {
        for (int i = 0; i < 120; ++i)
            body = BodyYawStep(body, look, vel, false, 1.0f / 60.0f);
        return body;
    };
    CHECK(std::fabs(run(Vec3(0, -4.3f, 0), 0.0f)) < 0.01f, "walking backwards Steve keeps facing forwards");
    const float strafe = run(Vec3(4.3f, 0, 0), 0.0f); // east: heading -90 degrees
    CHECK(strafe < -0.8f && strafe > -0.88f, "sideways the body turns towards it, but only 50 degrees (%.2f)", strafe);
    CHECK(std::fabs(run(Vec3(0, 4.3f, 0), 1.5f)) < 0.05f, "walking forwards it comes round to the look");
    const float still = run(Vec3(0, 0, 0), 2.5f);
    CHECK(std::fabs(still - 0.873f) < 0.01f, "standing, it is dragged within 50 degrees of the head (%.2f)", still);
    CHECK(std::fabs(BodyYawStep(1.0f, look, Vec3(0, 0, 0), true, 0.5f)) < 0.05f, "a swing turns it to the look");
}

static void RunChatLine(const char* line) {
    OpenChat(line);
    TypeKey(TK_ENTER);
}

static void TestChat() {
    TestHost host;
    SetHost(&host);
    CombatStage(host);
    ChatClear();
    for (auto& sl : gInv.slots)
        sl.Clear();
    gGame.gameMode = MODE_SURVIVAL;

    // typing
    OpenChat("", 'T');
    TypeChar('t');
    CHECK(gGame.screen == SCREEN_CHAT && gChatInput.text.empty() && TextInputActive(), "T opens the chat, without its t");
    for (char c : std::string("hi"))
        TypeChar((uint32_t)c);
    TypeChar(0x15F); // ş
    TypeKey(TK_BACKSPACE);
    TypeKey(TK_ENTER);
    CHECK(gGame.screen == SCREEN_NONE && !gChatLines.empty() && gChatLines.back().text == "<Steve> hi", "Enter sends a message");
    TypeChar('/');
    CHECK(gGame.screen == SCREEN_CHAT && gChatInput.text == "/", "/ opens it with a slash");
    for (char c : std::string("gam"))
        TypeChar((uint32_t)c);
    TypeKey(TK_TAB);
    const std::string first = gChatInput.text;
    TypeKey(TK_TAB);
    CHECK(first == "/gamemode" && gChatInput.text == "/gamerule" && ChatSuggestions().empty(), "Tab completes, again: the next one");
    TypeKey(TK_ESCAPE);
    CHECK(gGame.screen == SCREEN_NONE, "Escape closes it");
    ChatPrint(std::string(200, 'w'));
    bool narrow = true;
    for (const ChatLine& l : gChatLines)
        narrow = narrow && GuiTextWidth(l.text) <= kChatWidth;
    CHECK(narrow && gChatLines.size() >= 4, "long lines wrap at the chat's width");

    // commands
    RunChatLine("/gamemode creative");
    CHECK(gGame.gameMode == MODE_CREATIVE && gChatLines.back().text.find("Creative") != std::string::npos, "/gamemode");
    RunChatLine("/gamemode s");
    RunChatLine("/give diamond 70");
    RunChatLine("/give @s minecraft:oak_log");
    CHECK(gInv.CountOf(ID_DIAMOND) == 70 && gInv.CountOf(ID_OAK_LOG) == 1 && gChatLines.back().color == 0xFFFFFFFF, "/give, in stacks");
    RunChatLine("/give no_such_thing");
    CHECK(gChatLines.back().color == kChatError, "an unknown item is an error");
    RunChatLine("/clear diamond");
    CHECK(gInv.CountOf(ID_DIAMOND) == 0 && gInv.CountOf(ID_OAK_LOG) == 1, "/clear an item");
    RunChatLine("/time set night");
    RunChatLine("/weather thunder");
    CHECK(host.clockSet == 19.0f && host.weather == 2, "/time set night (19:00), /weather thunder");
    RunChatLine("/tp 10 20 ~5");
    CHECK(host.player.x == 10.0f && host.player.y == 20.0f && host.player.z == 16.0f, "/tp, ~ is the player's own (%.1f)", host.player.z);
    host.player = Vec3(0.5f, 0.5f, 11.0f);
    RunChatLine("/setblock 2 2 10 stone");
    RunChatLine("/fill 0 0 12 1 1 12 glass");
    CHECK(gWorld.GetBlock(2, 2, 10) == ID_STONE && gWorld.GetBlock(1, 1, 12) == ID_GLASS && gWorld.GetBlock(0, 0, 12) == ID_GLASS,
          "/setblock and /fill");
    MobsClear();
    RunChatLine("/summon creeper 3 0 10");
    CHECK(gMobs.size() == 1 && gMobs[0].kind == MOB_CREEPER, "/summon");
    RunChatLine("/effect give regeneration 10 1");
    RunChatLine("/xp add 2 levels");
    CHECK(HasEffect(EFFECT_REGENERATION) && gSurvival.xpLevel >= 2, "/effect and /xp");
    RunChatLine("/gamerule keepInventory false");
    CHECK(!gRules.keepInventory, "/gamerule");
    RunChatLine("/gamerule keepInventory true");
    RunChatLine("/wanted 3");
    RunChatLine("/kill");
    CHECK(host.wanted == 3 && host.killed, "/wanted (GTA's police), /kill");
    RunChatLine("/nonsense");
    CHECK(gChatLines.back().color == kChatError, "an unknown command");
    OpenChat("");
    TypeKey(TK_UP);
    CHECK(gChatInput.text == "/nonsense", "up: what was sent before");
    TypeKey(TK_ESCAPE);
    CHECK(ItemByName("minecraft:diamond_sword") == ID_DIAMOND_SWORD && ItemByName("red_bed_head") == 0, "names of things");

    // the creative search
    OpenScreen(SCREEN_CREATIVE);
    gGame.creativeTab = TAB_SEARCH;
    for (char c : std::string("diamond s"))
        TypeChar((uint32_t)c);
    const std::vector<uint16_t>& found = PaletteItems();
    CHECK(TextInputActive() && std::find(found.begin(), found.end(), ID_DIAMOND_SWORD) != found.end() &&
              std::find(found.begin(), found.end(), ID_STONE) == found.end(),
          "the search tab finds things by name (%d)", (int)found.size());
    TypeKey(TK_DELETE);
    gLanguage = LANG_TR;
    for (const char* c = "ELMAS KILIÇ"; *c; ++c)
        TypeChar((unsigned char)*c < 0x80 ? (uint32_t)*c : 0); // ("ELMAS KILI": the Ç's bytes are not characters)
    TypeChar(0xC7); // Ç
    const std::vector<uint16_t>& tr = PaletteItems();
    CHECK(std::find(tr.begin(), tr.end(), ID_DIAMOND_SWORD) != tr.end(), "in Turkish too, whatever the case (%s, %d)",
          CreativeSearchText().c_str(), (int)tr.size());
    gLanguage = LANG_EN;
    TypeKey(TK_DELETE);
    TypeKey(TK_ESCAPE);
    CHECK(gGame.screen == SCREEN_NONE, "Escape closes the inventory from the search box");
    gGame.creativeTab = 0;
    ChatClear();
    MobsClear();
    gWorld.Clear();
}

static void TestDebugScreen() {
    TestHost host;
    SetHost(&host);
    host.hasPlayer = true;
    host.player = Vec3(10.5f, -3.25f, 21.0f);
    gGame = GameState();
    gGame.lookDir = Vec3(0, 1, 0);
    HudFacts f;
    f.title = "GTA SA Minecraft test";
    std::vector<HudPiece> v;
    BuildHud(f, v);
    CHECK(!FindText(v, "XYZ:") && FindSprite(v, GUI_HOTBAR), "no debug screen at first");
    gGame.debugScreen = true;
    v.clear();
    BuildHud(f, v);
    const HudPiece* xyz = FindText(v, "XYZ: 10.500 / -3.250 / 20.000");
    CHECK(xyz && FindText(v, "Facing: north") && FindText(v, "GTA SA Minecraft test"), "F3: where he is (feet) and looks");
    gGame.debugScreen = false;
    gGame.hideHud = true;
    v.clear();
    BuildHud(f, v);
    CHECK(!FindSprite(v, GUI_HOTBAR) && !FindSprite(v, GUI_CROSSHAIR), "F1: no HUD");
    gGame = GameState();
}

static void TestSave() {
    const char* kPath = "mc_tests_world.tmp";
    BlockStage();
    ResetSurvival();
    gGame = GameState();
    gGame.gameMode = MODE_CREATIVE;
    gGame.cameraMode = CAM_FIRST;
    gInv.selected = 4;
    gInv.slots[4] = Stack(ID_DIAMOND_PICKAXE);
    gInv.slots[4].damage = 12;
    gInv.slots[20] = Stack(ID_BREAD, 17);
    gInv.armor[ARMOR_FEET] = Stack(ID_IRON_BOOTS);
    gInv.offhand = Stack(ID_TOTEM_OF_UNDYING);
    gSurvival.food = 13.0f;
    gSurvival.saturation = 2.5f;
    gSurvival.xpLevel = 6;
    gSurvival.xpTotal = 80;
    gSurvival.xpProgress = 0.4f;
    gWorld.SetRaw(3, 4, 12, MakeVox(ID_FURNACE, 2));
    const int stones = CountBlocks(ID_STONE);

    FILE* f = fopen(kPath, "wb");
    CHECK(f != nullptr, "the test file can be written");
    if (!f)
        return;
    gSleep.spawnSet = true;
    gSleep.spawn = Int3{ 7, -8, 9 };
    WriteWorldFile(f, 1);
    const uint32_t tail = 0xABCD1234; // a section of the host's own
    fwrite(&tail, 4, 1, f);
    fclose(f);

    BlockStage();
    gWorld.Clear();
    ResetSurvival();
    gGame = GameState();
    WorldFileInfo info;
    f = fopen(kPath, "rb");
    CHECK(f && ReadWorldFile(f, info) && info.version == 4 && info.hostValue == 1 && info.blocksOk && info.hostPart && gSleep.spawnSet && gSleep.spawn == (Int3{ 7, -8, 9 }), "it reads back");
    gSleep = SleepState();
    uint32_t got = 0;
    CHECK(f && fread(&got, 4, 1, f) == 1 && got == tail, "and leaves the file where the host's own part begins");
    if (f)
        fclose(f);
    CHECK(gGame.gameMode == MODE_CREATIVE && gGame.cameraMode == CAM_FIRST && gInv.selected == 4, "game mode, view and hotbar slot");
    CHECK(gInv.slots[4].id == ID_DIAMOND_PICKAXE && gInv.slots[4].damage == 12 && gInv.slots[20].id == ID_BREAD &&
              gInv.slots[20].count == 17 && gInv.armor[ARMOR_FEET].id == ID_IRON_BOOTS && gInv.offhand.id == ID_TOTEM_OF_UNDYING,
          "the inventory, the armour and the off hand");
    CHECK(gSurvival.food == 13.0f && gSurvival.saturation == 2.5f && gSurvival.xpLevel == 6 && gSurvival.xpTotal == 80 &&
              Near(gSurvival.xpProgress, 0.4f),
          "hunger and experience");
    CHECK(CountBlocks(ID_STONE) == stones && gWorld.Get(3, 4, 12) == MakeVox(ID_FURNACE, 2), "and every block (%d stones)",
          CountBlocks(ID_STONE));

    // something that is not a world file changes nothing
    f = fopen(kPath, "wb");
    const uint32_t junk[2] = { 0x12345678, 3 };
    fwrite(junk, sizeof(junk), 1, f);
    fclose(f);
    gInv.selected = 7;
    f = fopen(kPath, "rb");
    CHECK(f && !ReadWorldFile(f, info) && gInv.selected == 7 && CountBlocks(ID_STONE) == stones, "a foreign file is refused");
    if (f)
        fclose(f);
    remove(kPath);
    BlockStage();
    gWorld.Clear();
    ResetSurvival();
    gGame = GameState();
}

// ---------------------------------------------------------------- the game being played (GameState.h)
static void TestGameState() {
    gGame = GameState();
    ResetSurvival();
    gPlayed.clear();
    ShowMessage("hello", 1.5f);
    CHECK(gGame.message == "hello" && gGame.messageTimer == 1.5f, "a message for the line above the hotbar");
    gGame.age = 12.0f;
    NoteDamage(STR_DEATH_FALL);
    CHECK(gGame.deathCause == STR_DEATH_FALL && gGame.deathCauseTime == 12.0f, "what hurt last is remembered for the death screen");

    // the attack cooldown: a fist is ready after a quarter of a second, a sword takes longer
    gGame.attackTimer = 0.125f;
    CHECK(Near(AttackCharge(), 0.5f), "a fist is half ready after an eighth of a second (%.2f)", AttackCharge());
    gGame.attackTimer = 10.0f;
    CHECK(AttackCharge() == 1.0f, "and fully ready later");
    gInv.Held() = Stack(ID_DIAMOND_SWORD);
    const float swordSpeed = Item(ID_DIAMOND_SWORD).attackSpeed;
    gGame.attackTimer = 0.5f / swordSpeed;
    CHECK(swordSpeed > 0.0f && swordSpeed < 4.0f && Near(AttackCharge(), 0.5f), "a sword recharges slower (%.2f attacks a second)",
          swordSpeed);

    // the arm swings, and does not start over in the first half of a swing
    gGame.swing = -1.0f;
    StartSwing();
    CHECK(gGame.swing == 0.0f, "a swing starts");
    gGame.swing = 0.3f;
    StartSwing();
    CHECK(gGame.swing == 0.3f, "but not again before it is half over");
    gGame.swing = 0.6f;
    StartSwing();
    CHECK(gGame.swing == 0.0f, "after that it does");
    gGame.swing = gGame.offSwing = -1.0f;
    gGame.offhandActive = true;
    StartSwing();
    CHECK(gGame.offSwing == 0.0f && gGame.swing == -1.0f, "the off hand swings when it is the one in use");
    gGame.offhandActive = false;

    // tools wear out and break; not in creative, and blocks never do
    gInv.Held() = Stack(ID_WOODEN_PICKAXE);
    const int durability = Item(ID_WOODEN_PICKAXE).durability;
    DamageHeldItem();
    CHECK(durability > 1 && gInv.Held().damage == 1, "using a tool wears it");
    gGame.gameMode = MODE_CREATIVE;
    DamageHeldItem(5);
    CHECK(gInv.Held().damage == 1, "not in creative");
    gGame.gameMode = MODE_SURVIVAL;
    gGame.message.clear();
    DamageHeldItem(durability);
    CHECK(gInv.Held().Empty() && Heard(SND_TOOL_BREAK) == 1 && !gGame.message.empty(), "a worn out tool breaks, and says so");
    gInv.Held() = Stack(ID_STONE);
    DamageHeldItem();
    CHECK(gInv.Held().damage == 0 && gInv.Held().count == 1, "a block in the hand does not wear");

    gGame = GameState();
    ResetSurvival();
}

// ---------------------------------------------------------------- controls (Controls.h)
static void TestControls() {
    // Minecraft's own default keys
    CHECK(kActions[ACT_ATTACK].key == KEY_MOUSE_LEFT && kActions[ACT_USE].key == KEY_MOUSE_RIGHT &&
              kActions[ACT_PICK_BLOCK].key == KEY_MOUSE_MIDDLE,
          "mouse buttons: attack, use, pick block");
    CHECK(kActions[ACT_JUMP].key == KEY_SPACE && kActions[ACT_SNEAK].key == KEY_LSHIFT && kActions[ACT_SPRINT].key == KEY_LCTRL,
          "Space jumps, Shift sneaks, Ctrl sprints");
    CHECK(kActions[ACT_INVENTORY].key == KEY_E && kActions[ACT_DROP].key == KEY_Q && kActions[ACT_SWAP_HANDS].key == KEY_F &&
              kActions[ACT_PERSPECTIVE].key == KEY_F5 && kActions[ACT_BACK].key == KEY_ESCAPE,
          "E inventory, Q drop, F swap hands, F5 perspective, Esc back");
    for (int i = 0; i < 9; ++i)
        CHECK(kActions[ACT_HOTBAR_1 + i].key == KEY_1 + i, "hotbar slot %d is key %d", i + 1, i + 1);
    for (int a = 0; a < ACT_COUNT; ++a) {
        CHECK(kActions[a].name && kActions[a].name[0] && kActions[a].key != KEY_NONE && kActions[a].key < KEY_COUNT,
              "action %d has a name and a key", a);
        for (int b = a + 1; b < ACT_COUNT; ++b)
            CHECK(kActions[a].key != kActions[b].key, "%s and %s share a key", kActions[a].name, kActions[b].name);
    }

    // what the host fills in and the game asks for
    gControls = Controls();
    CHECK(!ActionDown(ACT_JUMP) && !ActionPressed(ACT_JUMP), "nothing is pressed at first");
    gControls.Set(ACT_JUMP, true, true);
    CHECK(ActionDown(ACT_JUMP) && ActionPressed(ACT_JUMP) && !ActionDown(ACT_SNEAK), "a key goes down");
    gControls.Set(ACT_JUMP, true, false);
    CHECK(ActionDown(ACT_JUMP) && !ActionPressed(ACT_JUMP), "and is held");
    gControls.Set(ACT_JUMP, false, false);
    CHECK(!ActionDown(ACT_JUMP) && !ActionPressed(ACT_JUMP), "and comes up again");
    gControls = Controls();
}

// the soldier's real model, found in the repo next to the source (the game reads its
// copy from the MinecraftSA folder; the tests read the converter's output)
static const char* SoldierBinPath() {
    static const char* paths[] = { "cs-soldier/cs_soldier.bin", "../cs-soldier/cs_soldier.bin",
                                   "../../cs-soldier/cs_soldier.bin", "../../../cs-soldier/cs_soldier.bin" };
    for (const char* p : paths) {
        FILE* f = std::fopen(p, "rb");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return nullptr;
}

static bool LoadSoldierBinFromRepo() {
    const char* p = SoldierBinPath();
    if (!p)
        return false;
    FILE* f = std::fopen(p, "rb");
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes((size_t)std::max<long>(n, 0));
    const bool ok = n > 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok && LoadSoldierModel(bytes.data(), bytes.size());
}

// regression pin for the shipped seam: the converter's REAL output file in the
// repo (cs-soldier/cs_soldier.bin - never a synthetic bin) must load whole in
// the shipped core loader, with the exact skeleton and sequence counts the
// converter baked. The boss install's first eye test was refused in-game with
// "cs_soldier.bin is not loaded" because the bridge looked for the file loose
// in the MinecraftSA folder while the install had copied the converter's
// cs-soldier folder as-is; the bridge now resolves both layouts
// (SoldierFilePath in src/gta/Textures.cpp). This test pins the other half of
// that seam - converter file vs core loader - so the two can never drift
// apart silently again: if a rebake changes these counts on purpose, update
// the pins here with it.
static void TestSoldierRealAsset() {
    TestHost host;
    SetHost(&host);
    UnloadSoldierModel();
    CHECK(LoadSoldierBinFromRepo(), "the real shipped cs_soldier.bin loads in the core loader");
    CHECK(SoldierModelReady(), "and the model reports itself ready");
    CHECK(SoldierBoneCount() == 55, "the shipped file carries the SEAL's 55 bones (got %d)", SoldierBoneCount());
    CHECK(SoldierSequenceCount() == 28, "the shipped file carries 28 layered sequences (got %d)", SoldierSequenceCount());
}

// the real-model round of the CS project: the file, the no-fake gate, the poses of
// Counter-Strike's own sequences, and the wall probe in his brain
static void TestSoldierModel() {
    TestHost host;
    SetHost(&host);
    McsaSrand(31337);

    // the law: no model file, no soldier - and no box wearing his name either
    {
        WideStage();
        UnloadSoldierModel();
        CHECK(!SoldierModelReady(), "the model can be unloaded");
        CHECK(SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true) < 0,
              "without cs_soldier.bin the soldier refuses to spawn");
        RecordSink sink;
        SetModelSink(&sink);
        sink.Reset();
        DrawMob(MOB_SOLDIER, EntityPose(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), 1.0f),
                MobAnim(), 1.0f, 1.0f, 1.0f, 1.0f);
        CHECK(sink.quads[0] == 0 && sink.quads[1] == 0, "and nothing at all is drawn for him (%d quads)", sink.quads[0]);
        CHECK(LoadSoldierBinFromRepo() && SoldierModelReady(), "the model loads back from the repo file");
    }

    // the poses are the game's own numbers, measured off the converted file -
    // in the v3 format's own terms now: legs and upper body are separate layers,
    // composed exactly the way the game composes them (workstream M).
    {
        auto spanOf = [](const SoldierDrawState& st, float& lo, float& hi) {
            return SoldierComposedSpan(st, &lo, &hi);
        };
        SoldierDrawState aim; // standing at aim, level pitch row, all facing one way
        aim.legCategory = SP_IDLE;
        aim.upCategory = SP_AIM;
        aim.upVariant = 0; // the level row
        float lo = 0, hi = 0;
        CHECK(spanOf(aim, lo, hi) && lo > -0.05f && lo < 0.15f && hi > 1.35f && hi < 1.50f,
              "aim stance composed from layers: %.2f m tall (lowest %.3f)", hi, lo);
        SoldierDrawState idle;
        idle.legCategory = SP_IDLE;
        idle.upCategory = SP_IDLE;
        CHECK(spanOf(idle, lo, hi) && hi > 1.35f && hi < 1.50f, "his idle stands %.2f m to the head joint", hi);
        float loMin = 1e9f, loMax = -1e9f, hiMin = 1e9f, hiMax = -1e9f;
        for (int ph = 0; ph < 6; ++ph) {
            SoldierDrawState walk;
            walk.legCategory = SP_WALK;
            walk.gait = ph / 6.0f; // the gaits run on the gait phase, not on seconds
            walk.upCategory = SP_IDLE;
            float l2 = 0, h2 = 0;
            spanOf(walk, l2, h2);
            loMin = std::min(loMin, l2); loMax = std::max(loMax, l2);
            hiMin = std::min(hiMin, h2); hiMax = std::max(hiMax, h2);
        }
        CHECK(hiMax - hiMin > 0.02f || loMax - loMin > 0.02f,
              "the walk cycle really moves (span travels %.3f/%.3f m over the cycle)", loMax - loMin, hiMax - hiMin);
        for (int v = 0; v < 3; ++v)
            CHECK(SoldierPoseSpan(SP_DEATH, SL_FULL, v, 99.0f, &lo, &hi) && hi < 0.7f && lo > -0.25f,
                  "death %d ends lying on the ground (%.2f m tall, lowest %.3f)", v, hi, lo);
        // firing is the aim with the game's recoil in it: the rifle's muzzle sits
        // somewhere else than it does at rest - that difference is the kick
        SoldierDrawState shoot = aim;
        shoot.upCategory = SP_SHOOT;
        shoot.upTime = 0.1f;
        Vec3 aimMz, aimBt, shootMz, shootBt;
        const Pose stand = EntityPose(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), 1.0f);
        CHECK(SoldierGunPose(stand, aim, &aimMz, &aimBt) && SoldierGunPose(stand, shoot, &shootMz, &shootBt),
              "the model answers for its rifle in aim and in shoot");
        const float kick = (shootMz - aimMz).Length();
        CHECK(kick > 0.02f, "the shoot pose kicks his rifle - the game's own recoil data (%.3f m from the aim)", kick);
        // a v2 file is refused, loudly: patch the loaded store's own header copy
        {
            FILE* f = std::fopen(SoldierBinPath(), "rb");
            std::fseek(f, 0, SEEK_END);
            const long n = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            std::vector<unsigned char> bytes((size_t)n);
            const bool rd = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
            std::fclose(f);
            bytes[4] = 2; // version field: 3 -> 2
            CHECK(rd && !LoadSoldierModel(bytes.data(), bytes.size()), "a version-2 soldier file is refused, not half-read");
            CHECK(LoadSoldierBinFromRepo(), "and the real v3 file loads back");
        }
    }

    // his wall probe: a wall across the patrol road bends his path around it; no wall, no bend
    for (int trial = 0; trial < 2; ++trial) {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        CHECK(s >= 0, "a soldier spawns for the wall test");
        const Vec3 anchor = gMobs[s].anchor;
        const float a = (float)gMobs[s].patrolStep * 1.0472f + (float)(gMobs[s].id % 6) * 0.5f;
        const Vec3 dir(std::cos(a), std::sin(a), 0.0f);
        host.wall = trial == 0;
        host.wallC = anchor + dir * 6.5f;
        host.wallDir = Vec3(-dir.y, dir.x, 0.0f);
        host.wallHalf = 2.2f;
        const Vec3 far = anchor + dir * 200.0f + Vec3(0, 0, 1.0f);
        const std::vector<BeingInfo> none;
        const int step0 = gMobs[s].patrolStep;
        float maxDev = 0.0f, maxAlong = 0.0f;
        for (int i = 0; i < 180; ++i) { // one patrol leg only: the tracking stops when
            MobsTick(0.05f, far, 0, true, &none); // he reaches the post and turns for the next
            const Vec3 rel = gMobs[s].pos - anchor;
            maxAlong = std::max(maxAlong, rel.x * dir.x + rel.y * dir.y);
            maxDev = std::max(maxDev, std::fabs(rel.x * -dir.y + rel.y * dir.x));
            if (gMobs[s].patrolStep != step0 || gMobs[s].patrolWait > 0.0f)
                break;
        }
        if (trial == 0)
            CHECK(maxDev > 1.6f && maxAlong > 7.5f,
                  "a wall bends his patrol around it (deviation %.2f m, reached %.1f m along the road)", maxDev, maxAlong);
        else
            CHECK(maxDev < 0.5f, "no wall: he walks his post straight (deviation %.2f m)", maxDev);
        host.wall = false;
    }
}

// The fix round's own proofs: the model's aim IS the brain's facing, a burst is
// seen as well as heard, the gait keeps the ground, and a bad ground sample
// cannot swallow the soldier.
struct GunBoundsSink : ModelSink {
    Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
    int tex = -1, tris = 0;
    void Texture(int t) override { tex = t; }
    void Quad(const Vec3* p, const float*, const float*, uint32_t) override {
        if (tex != MT_SOLDIER + 2)
            return;
        ++tris;
        for (int k = 0; k < 4; ++k) {
            lo = Vec3(std::min(lo.x, p[k].x), std::min(lo.y, p[k].y), std::min(lo.z, p[k].z));
            hi = Vec3(std::max(hi.x, p[k].x), std::max(hi.y, p[k].y), std::max(hi.z, p[k].z));
        }
    }
    bool Opaque(int, int, int) override { return true; }
};

static void TestSoldierAimFix() {
    TestHost host;
    SetHost(&host);
    McsaSrand(4242);
    const std::vector<BeingInfo> none;
    CHECK(LoadSoldierBinFromRepo() && SoldierModelReady(), "the model loads for the fix-round proofs");

    // ---- A: his rifle points where his brain faces, at every quarter turn.
    // The stand is built exactly as the bridge builds it, and the gun line is
    // measured through the model's own evaluated pose.
    for (int qi = 0; qi < 4; ++qi) {
        const float yaw = qi * kPi * 0.5f;
        const Vec3 fwd(-std::sin(yaw), std::cos(yaw), 0.0f);
        const Vec3 right(fwd.y, -fwd.x, 0.0f);
        const Pose stand = EntityPose(Vec3(0, 0, 0), right, Vec3(0, 0, 1), fwd, 1.0f);
        SoldierDrawState aimSt;
        aimSt.legCategory = SP_IDLE;
        aimSt.upCategory = SP_AIM;
        aimSt.upVariant = 0; // the level pitch row (the bake's order: level, up, down)
        Vec3 muzzle, butt;
        CHECK(SoldierGunPose(stand, aimSt, &muzzle, &butt), "yaw %d: the model answers for its rifle", qi * 90);
        Vec3 d = muzzle - butt;
        d.z = 0.0f;
        const float dl = d.Length();
        const float dot = dl > 0.01f ? Clamp((d.x * fwd.x + d.y * fwd.y) / dl, -1.0f, 1.0f) : 0.0f;
        const float ang = std::acos(dot) * 180.0f / kPi;
        CHECK(dl > 0.5f && ang < 6.0f, "yaw %d: the gun line is %.2f deg off his facing (rifle %.2f m)", qi * 90, ang, dl);
        const float side = muzzle.x * right.x + muzzle.y * right.y;
        CHECK(side > 0.03f, "yaw %d: the muzzle rides on his right side (%.3f m), not mirrored", qi * 90, side);
        // and the DRAWN model agrees: the rifle's own triangles sit ahead of him
        GunBoundsSink gs;
        SetModelSink(&gs);
        MobAnim anim;
        anim.soldier = aimSt;
        DrawMob(MOB_SOLDIER, stand, anim, 1.0f, 1.0f, 1.0f, 1.0f);
        SetModelSink(nullptr);
        const Vec3 gc = (gs.lo + gs.hi) * 0.5f;
        const float gFwd = gc.x * fwd.x + gc.y * fwd.y;
        const float gSide = gc.x * right.x + gc.y * right.y;
        CHECK(gs.tris > 20 && gFwd > 0.45f && std::fabs(gSide) < gFwd * 0.5f,
              "yaw %d: the drawn rifle sits ahead of him (%.2f m fwd, %.2f m side)", qi * 90, gFwd, gSide);
    }

    // ---- E: the file's own strides, and a pose change cross-fades instead of snapping
    CHECK(Near(SoldierStride(SP_WALK), 1.553f, 0.06f), "the walk stride is measured off the cycle (%.3f m)", SoldierStride(SP_WALK));
    CHECK(Near(SoldierStride(SP_RUN), 2.970f, 0.10f), "the run stride is measured off the cycle (%.3f m)", SoldierStride(SP_RUN));
    CHECK(SoldierGaitStride(SP_RUN, 1) > 0.2f && SoldierGaitStride(SP_RUN, 3) > 2.0f,
          "the strafe and back gaits carry their own strides (%.3f / %.3f m)", SoldierGaitStride(SP_RUN, 1), SoldierGaitStride(SP_RUN, 3));
    {
        RecordSink sink;
        SetModelSink(&sink);
        MobAnim anim;
        SoldierDrawState walkSt;
        walkSt.legCategory = SP_WALK;
        walkSt.upCategory = SP_IDLE;
        SoldierDrawState idleSt;
        idleSt.legCategory = SP_IDLE;
        idleSt.upCategory = SP_IDLE;
        walkSt.hasPrev = true;
        walkSt.prevBones = SoldierComposeLocals(idleSt, walkSt.prevLocals.data(), 64);
        walkSt.fade = 0.5f;
        anim.soldier = walkSt;
        sink.Reset();
        DrawMob(MOB_SOLDIER, EntityPose(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 0), 1.0f),
                anim, 1.0f, 1.0f, 1.0f, 1.0f);
        CHECK(sink.quads[0] == 1010, "mid-fade he is still ONE figure of 1010 quads, not two (%d)", sink.quads[0]);
        SetModelSink(nullptr);
    }

    // ---- B: a burst is SEEN: the shoot pose holds across it and every shot
    // leaves a flash/tracer in the core's list for the bridge to draw
    {
        WideStage();
        gShotFx.clear();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(14.5f, 0.5f, 10.0f), true);
        FaceMob(s, kPi * 0.5f); // facing the man in the street
        ProvokeByAim(host, Vec3(0.5f, 0.5f, 11.0f), &none); // the law's ignition
        int shootTicks = 0, holdRun = 0, bestHold = 0;
        bool fxSeen = false;
        for (int i = 0; i < 60; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 0.5f, 11.0f), 0, true, &none);
            if (!gShotFx.empty())
                fxSeen = true;
            if (gMobs[s].draw.upCategory == SP_SHOOT) {
                ++shootTicks;
                bestHold = std::max(bestHold, ++holdRun);
            } else {
                holdRun = 0;
            }
        }
        CHECK(fxSeen, "his shots leave a visible flash and tracer, not only a sound");
        CHECK(shootTicks >= 4 && bestHold >= 4,
              "the shoot pose holds through a burst (%d shoot ticks, longest hold %d)", shootTicks, bestHold);
    }

    // ---- E: gait and ground agree - measured over a STEADY hunt run (one
    // gait, one speed, no firing and no planting): the distance his feet
    // travel and the distance his body travels are the same
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, -kPi * 0.5f); // east
        gMobs[s].bossState = 3; // BS_HUNT
        gMobs[s].huntPhase = 0;
        gMobs[s].huntDest = Vec3(60.5f, 0.5f, 10.0f);
        float gait0 = 0.0f;
        Vec3 prev(0, 0, 0);
        float travelled = 0.0f;
        for (int i = 0; i < 37; ++i) {
            // the player far behind him: never seen, so the run never breaks
            MobsTick(0.05f, Vec3(-59.5f, 0.5f, 11.0f), 0, true, &none);
            if (i == 4) {
                gait0 = gMobs[s].draw.gait;
                prev = gMobs[s].pos;
            } else if (i > 4) {
                const Vec3 p2 = gMobs[s].pos;
                travelled += std::sqrt((p2.x - prev.x) * (p2.x - prev.x) + (p2.y - prev.y) * (p2.y - prev.y));
                prev = p2;
            }
        }
        const float byGait = (gMobs[s].draw.gait - gait0) * SoldierStride(SP_RUN);
        CHECK(travelled > 4.0f && std::fabs(travelled - byGait) / travelled < 0.12f,
              "gait and ground agree on the steady run: body %.2f m, feet %.2f m (drift %.1f%%)", travelled, byGait,
              travelled > 0.01f ? std::fabs(travelled - byGait) / travelled * 100.0f : 0.0f);
    }

    // ---- D: a strip of street whose ground sample falls away (or vanishes)
    // cannot swallow him: he holds his ground over a blink, and even where the
    // bad ground persists he only ever FALLS a little before it comes back
    for (int mode = 0; mode < 2; ++mode) {
        WideStage();
        for (int x = 8; x <= 9; ++x)
            for (int y = -3; y <= 3; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_AIR)); // the strip has no floor of ours either
        host.groundMode = 1;
        host.stripX0 = 8.0f;
        host.stripX1 = 9.2f;
        host.stripFound = mode == 0; // mode 0: the sample lands 40 m down; mode 1: it finds nothing
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        FaceMob(s, -kPi * 0.5f);
        gMobs[s].aggro = true; // white-box: the fight is staged, not sighted into being
        gMobs[s].fightStarted = true;
        gMobs[s].bossState = 2; // running at the man: the strip is crossed at combat speed
        gMobs[s].detect = 1.0f;
        float minZ = 1e9f;
        for (int i = 0; i < 70; ++i) {
            MobsTick(0.05f, Vec3(43.5f, 0.5f, 11.0f), 0, true, &none);
            if (i > 5)
                minZ = std::min(minZ, gMobs[s].pos.z);
        }
        CHECK(minZ > 9.0f, "mode %d: the bad strip never swallows him (lowest %.2f m)", mode, minZ);
        CHECK(gMobs[s].pos.x > 10.0f && Near(gMobs[s].pos.z, 10.0f, 0.05f),
              "mode %d: he crosses the strip and stands on the street again (x %.1f, z %.2f)", mode, gMobs[s].pos.x, gMobs[s].pos.z);
        host.groundMode = 0;
        host.stripFound = true;
    }
}


// ------------------------------------------------------------------
// THE BOSS: the solo soldier finished whole - senses, talk-first, body,
// combat trade, mobility, the street's war (K), the traffic (L), the
// two-layer body (M), the map in his head (A) and his voice (H).
// (Boss states, for the numbers below: 0 calm, 1 command, 2 combat,
// 3 hunt, 4 suspicious.)
static bool LoadNavGraphFromRepo() {
    static const char* paths[] = { "cs-soldier/nav_graph.bin", "../cs-soldier/nav_graph.bin",
                                   "../../cs-soldier/nav_graph.bin", "../../../cs-soldier/nav_graph.bin" };
    for (const char* p : paths) {
        FILE* f = std::fopen(p, "rb");
        if (!f)
            continue;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> bytes((size_t)std::max<long>(n, 0));
        const bool ok = n > 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (ok)
            return LoadNavGraph(bytes.data(), bytes.size());
    }
    return false;
}

// The pose-bounds regression (repair round after the first boss eye test):
// every draw state the brain can select is composed exactly as the renderer
// composes it, and every skinned vertex must stay a man's size from the
// model's origin - an exploded pose can never ship again. It also pins the
// two bake defects that test exposed: the crouch must touch the ground (it
// hovered at +0.47 m), and no gait may sink through or leave the ground
// plane (the file's directional "gaits" turned out to be one-shot events
// that did both, and were replaced by the run cycle - see the converter).
// The one state allowed below -0.15 m is named, not slop: mid-fall in
// death1 the rifle, rigid on the falling hand, dangles to -0.57 m (the
// game's own data - in Counter-Strike the weapon drops as its own body;
// detaching ours on death is a separate step). The floor of -0.60 m
// leaves it no room to grow.
static void TestSoldierPoseBounds() {
    TestHost host;
    SetHost(&host);
    CHECK(LoadSoldierBinFromRepo() && SoldierModelReady(), "the bounds sweep meets the real shipped model");

    struct LegOpt { int cat, var; float t[3]; bool gait; };
    const LegOpt legs[] = {
        { SP_IDLE, 0, { 0.0f, 1.3f, 2.7f }, false },
        { SP_WALK, 0, { 0.0f, 0.37f, 0.71f }, true },
        { SP_RUN, 0, { 0.0f, 0.37f, 0.71f }, true },
        { SP_RUN, 1, { 0.0f, 0.37f, 0.71f }, true },
        { SP_RUN, 2, { 0.0f, 0.37f, 0.71f }, true },
        { SP_RUN, 3, { 0.0f, 0.37f, 0.71f }, true },
        { SP_CROUCH, 0, { 0.0f, 0.5f, 0.97f }, false },
        { SP_JUMP, 0, { 0.0f, 0.27f, 0.54f }, false },
    };
    struct UpOpt { int cat, var; float t[3]; };
    const UpOpt ups[] = {
        { SP_IDLE, 0, { 0.0f, 0.7f, 1.3f } },
        { SP_AIM, 0, { 0.0f, 0.03f, 0.06f } }, { SP_AIM, 1, { 0.0f, 0.03f, 0.06f } },
        { SP_AIM, 2, { 0.0f, 0.03f, 0.06f } }, { SP_AIM, 3, { 0.0f, 0.03f, 0.06f } },
        { SP_AIM, 4, { 0.0f, 0.03f, 0.06f } }, { SP_AIM, 5, { 0.0f, 0.03f, 0.06f } },
        { SP_SHOOT, 0, { 0.0f, 0.15f, 0.32f } }, { SP_SHOOT, 1, { 0.0f, 0.15f, 0.32f } },
        { SP_SHOOT, 2, { 0.0f, 0.15f, 0.32f } }, { SP_SHOOT, 3, { 0.0f, 0.15f, 0.32f } },
        { SP_SHOOT, 4, { 0.0f, 0.15f, 0.32f } }, { SP_SHOOT, 5, { 0.0f, 0.15f, 0.32f } },
        { SP_RELOAD, 0, { 0.0f, 1.5f, 2.95f } }, { SP_RELOAD, 1, { 0.0f, 1.5f, 2.95f } },
        { SP_FLINCH, 0, { 0.0f, 0.03f, 0.06f } }, { SP_FLINCH, 1, { 0.0f, 0.03f, 0.06f } },
    };
    const float twists[3] = { 0.0f, 1.92f, -1.92f };
    float worstR = 0.0f, worstLo = 1e9f, worstHi = -1e9f, crouchLo = 1e9f;
    int bad = 0, swept = 0;
    auto sweep = [&](const SoldierDrawState& st, bool crouchPose) {
        float mr = 0, lo = 0, hi = 0;
        ++swept;
        if (!SoldierSkinnedExtents(st, &mr, &lo, &hi)) {
            ++bad;
            return;
        }
        worstR = std::max(worstR, mr);
        worstLo = std::min(worstLo, lo);
        worstHi = std::max(worstHi, hi);
        if (crouchPose)
            crouchLo = std::min(crouchLo, lo);
        if (mr > 3.5f || lo < -0.60f || hi > 2.90f) {
            ++bad;
            printf("bounds: OUT legs(cat %d var %d t %.2f gait %.2f) up(cat %d var %d t %.2f) full(cat %d var %d t %.2f) twist %.2f -> radial %.2f z [%.2f, %.2f]\n",
                   st.legCategory, st.legVariant, st.legTime, st.gait, st.upCategory, st.upVariant, st.upTime,
                   st.fullCategory, st.fullVariant, st.fullTime, st.aimYaw - st.legYaw, mr, lo, hi);
        }
    };
    for (const LegOpt& L : legs)
        for (const UpOpt& U : ups)
            for (float tw : twists)
                for (int k = 0; k < 3; ++k) {
                    SoldierDrawState st;
                    st.legCategory = L.cat;
                    st.legVariant = L.var;
                    if (L.gait)
                        st.gait = L.t[k];
                    else
                        st.legTime = L.t[k];
                    st.upCategory = U.cat;
                    st.upVariant = U.var;
                    st.upTime = U.t[k];
                    st.legYaw = 0.0f;
                    st.aimYaw = tw;
                    sweep(st, L.cat == SP_CROUCH && (U.cat == SP_AIM || U.cat == SP_SHOOT) && U.var >= 3);
                }
    for (int v = 0; v < 3; ++v)
        for (float t : { 0.0f, 0.6f, 1.2f, 3.4f }) {
            SoldierDrawState st;
            st.fullCategory = SP_DEATH;
            st.fullVariant = v;
            st.fullTime = t;
            sweep(st, false);
        }
    CHECK(bad == 0, "all %d composed states keep the man a man's size (worst radial %.2f m, lowest %.2f m, highest %.2f m)",
          swept, worstR, worstLo, worstHi);
    CHECK(crouchLo <= 0.05f, "crouched, his soles touch the street (lowest vertex %.3f m - the bake once hovered at +0.47)", crouchLo);
}

// The transition fuzz (diagnostic round): the bounds sweep above measures
// STATIC composed states through SoldierSkinnedExtents - no cross-fade, and
// (until this round) a twist rule the renderer did not share for whole-body
// poses. The user's second eye test still saw the figure deform, in motion.
// This drives the RENDER composition itself (SoldierRenderExtents /
// SoldierPoseExplodes: layer merge, waist twist, cross-fade snapshot and
// all) through the transitions the brain can actually chain - every pose-key
// pair at least once, deaths in and out, then a seeded random walk over the
// full variant space - asserting the vertex watchdog's bounds on every
// composed frame, under each bisect toggle that changes the compose.
static void TestSoldierTransitionFuzz() {
    TestHost host;
    SetHost(&host);
    CHECK(LoadSoldierBinFromRepo() && SoldierModelReady(), "the transition fuzz meets the real shipped model");

    struct Lcg {
        uint32_t s = 987654321u;
        float next() {
            s = s * 1664525u + 1013904223u;
            return (float)(s >> 8) * (1.0f / 16777216.0f);
        }
        int pick(int n) { return (int)(next() * (float)n) % n; }
    };
    const int legKeys[][2] = { { SP_IDLE, 0 }, { SP_WALK, 0 }, { SP_RUN, 0 }, { SP_RUN, 1 },
                               { SP_RUN, 2 },  { SP_RUN, 3 },  { SP_CROUCH, 0 }, { SP_JUMP, 0 } };
    const int upKeys[][2] = { { SP_IDLE, 0 },
                              { SP_AIM, 0 }, { SP_AIM, 1 }, { SP_AIM, 2 }, { SP_AIM, 3 }, { SP_AIM, 4 }, { SP_AIM, 5 },
                              { SP_SHOOT, 0 }, { SP_SHOOT, 1 }, { SP_SHOOT, 2 }, { SP_SHOOT, 3 }, { SP_SHOOT, 4 }, { SP_SHOOT, 5 },
                              { SP_RELOAD, 0 }, { SP_RELOAD, 1 }, { SP_FLINCH, 0 }, { SP_FLINCH, 1 } };
    const int upReps[][2] = { { SP_IDLE, 0 }, { SP_AIM, 0 }, { SP_AIM, 3 }, { SP_SHOOT, 0 },
                              { SP_SHOOT, 3 }, { SP_RELOAD, 0 }, { SP_FLINCH, 0 } };

    auto makeState = [](const int* leg, const int* up, Lcg& rng, int deathVar) {
        SoldierDrawState st;
        if (deathVar >= 0) {
            st.fullCategory = SP_DEATH;
            st.fullVariant = deathVar;
            st.fullTime = rng.next() * 3.0f;
        } else {
            st.legCategory = leg[0];
            st.legVariant = leg[1];
            st.upCategory = up[0];
            st.upVariant = up[1];
            st.legTime = rng.next() * 3.0f;
            st.gait = rng.next() * 4.0f;
            st.upTime = rng.next() * 3.0f;
        }
        st.legYaw = rng.next() * 6.2831853f - 3.14159265f;
        st.aimYaw = st.legYaw + (rng.next() * 2.0f - 1.0f) * 3.4f; // past the spine's clamp too
        st.fade = 1.0f;
        return st;
    };

    int evals = 0, bad = 0;
    float worstR = 0.0f, worstLo = 1e9f, worstHi = -1e9f;
    auto drive = [&](const SoldierDrawState& from, const SoldierDrawState& target, Lcg& rng, float fadeStep) {
        SoldierDrawState nxt = target;
        // the brain's own snapshot step: the pose being left is composed and
        // carried into the new state for the fade (Mobs.cpp does exactly this)
        SoldierLocal prev[64];
        if (SoldierComposeLocals(from, prev, 64)) {
            for (int i = 0; i < 64; ++i)
                nxt.prevLocals[i] = prev[i];
            nxt.prevBones = SoldierBoneCount();
            nxt.hasPrev = nxt.prevBones > 0;
        }
        for (float f = 0.0f; f <= 1.001f; f += fadeStep) {
            nxt.fade = f;
            nxt.legTime = target.legTime + f * 0.16f; // his clocks keep running through the fade
            nxt.upTime = target.upTime + f * 0.16f;
            nxt.fullTime = target.fullTime + f * 0.16f;
            nxt.gait = target.gait + f * 0.1f;
            float mr = 0, lo = 0, hi = 0;
            ++evals;
            const bool finite = SoldierRenderExtents(nxt, &mr, &lo, &hi);
            if (!finite || SoldierPoseExplodes(nxt) || mr > kSoldierWatchdogRadius || lo < kSoldierWatchdogLoZ ||
                hi > kSoldierWatchdogHiZ) {
                ++bad;
                if (bad <= 5)
                    printf("fuzz: EXPLODES leg %d/%d up %d/%d full %d/%d fade %.2f twist %.2f -> finite %d "
                           "r %.2f z [%.2f, %.2f]\n",
                           nxt.legCategory, nxt.legVariant, nxt.upCategory, nxt.upVariant, nxt.fullCategory,
                           nxt.fullVariant, nxt.fade, nxt.aimYaw - nxt.legYaw, finite ? 1 : 0, mr, lo, hi);
            }
            worstR = std::max(worstR, mr);
            worstLo = std::min(worstLo, lo);
            worstHi = std::max(worstHi, hi);
        }
    };

    // the twist rule, pinned: a whole-body death pose must not change by one
    // bit when the aim yaw moves against the legs (the renderer once twisted
    // deaths; every measurement of them never did)
    {
        SoldierDrawState a;
        a.fullCategory = SP_DEATH;
        a.fullVariant = 1;
        a.fullTime = 0.7f;
        a.legYaw = 0.3f;
        a.aimYaw = 0.3f;
        SoldierDrawState b = a;
        b.aimYaw = 0.3f + 1.92f;
        float r1 = 0, l1 = 0, h1 = 0, r2 = 0, l2 = 0, h2 = 0;
        CHECK(SoldierRenderExtents(a, &r1, &l1, &h1) && SoldierRenderExtents(b, &r2, &l2, &h2) && r1 == r2 &&
                  l1 == l2 && h1 == h2,
              "a whole-body death pose never twists: the yaw difference changes nothing (r %.3f vs %.3f)", r1, r2);
    }

    // The fade's own pin - the root cause this round's fuzz caught: at fade 0
    // the drawn pose IS the previous pose, so its extents must match the
    // previous state's own extents (a tolerance for the renormalisation's
    // last ulp). The blend once normalised its quaternion by a length with
    // the fade weight multiplied into the w term (w * wq for wq * wq): fade 0
    // divided near-identity quaternions by ~0 and threw vertices 1e8 m out,
    // mid-fade merely bent the man - and no static audit could ever see it,
    // because no static audit composes a faded pose.
    {
        SoldierDrawState from;
        from.legCategory = SP_IDLE;
        from.upCategory = SP_IDLE;
        from.legTime = 0.7f;
        from.upTime = 0.7f;
        SoldierDrawState nxt;
        nxt.legCategory = SP_IDLE;
        nxt.upCategory = SP_AIM;
        nxt.upVariant = 0;
        nxt.legTime = 0.7f;
        nxt.upTime = 0.0f;
        SoldierLocal prev[64];
        CHECK(SoldierComposeLocals(from, prev, 64), "the fade pin composes its previous pose");
        for (int i = 0; i < 64; ++i)
            nxt.prevLocals[i] = prev[i];
        nxt.prevBones = SoldierBoneCount();
        nxt.hasPrev = true;
        float ra = 0, la = 0, ha = 0, rb = 0, lb = 0, hb = 0;
        CHECK(SoldierRenderExtents(from, &ra, &la, &ha), "the fade pin measures the previous pose");
        nxt.fade = 0.0f;
        CHECK(SoldierRenderExtents(nxt, &rb, &lb, &hb) && std::fabs(rb - ra) < 0.01f && std::fabs(lb - la) < 0.01f &&
                  std::fabs(hb - ha) < 0.01f,
              "fade 0 draws the previous pose itself (r %.3f vs %.3f, z [%.3f, %.3f] vs [%.3f, %.3f])", rb, ra, lb,
              hb, la, ha);
        nxt.fade = 0.5f;
        CHECK(SoldierRenderExtents(nxt, &rb, &lb, &hb) && rb <= kSoldierWatchdogRadius && lb >= kSoldierWatchdogLoZ &&
                  hb <= kSoldierWatchdogHiZ,
              "mid-fade stays a man's size (r %.3f, z [%.3f, %.3f])", rb, lb, hb);
    }

    Lcg rng;
    // every ordered pair of representative pose keys, and deaths in and out
    std::vector<SoldierDrawState> states;
    for (const auto& lk : legKeys)
        for (const auto& uk : upReps)
            states.push_back(makeState(lk, uk, rng, -1));
    for (size_t i = 0; i < states.size(); ++i)
        for (size_t j = 0; j < states.size(); ++j)
            if (i != j)
                drive(states[i], states[j], rng, 0.5f);
    for (const SoldierDrawState& s : states) {
        const SoldierDrawState d = makeState(legKeys[0], upKeys[0], rng, (int)(rng.next() * 2.99f));
        drive(s, d, rng, 0.5f);
        drive(d, s, rng, 0.5f);
    }
    // the random walk over the full variant space, chained like a real fight
    {
        SoldierDrawState cur = states[0];
        for (int step = 0; step < 1200; ++step) {
            const int* lk = legKeys[rng.pick(8)];
            const int* uk = upKeys[rng.pick(17)];
            const int dv = rng.next() < 0.06f ? (int)(rng.next() * 2.99f) : -1;
            const SoldierDrawState target = makeState(lk, uk, rng, dv);
            drive(cur, target, rng, 0.2f);
            cur = target;
            cur.fade = 1.0f;
            cur.hasPrev = false;
        }
    }
    // the same walk under the compose-changing bisect toggles
    for (int mode = 0; mode < 2; ++mode) {
        gBossTest.blendOff = mode == 0;
        gBossTest.layersOff = mode == 1;
        SoldierDrawState cur = states[1];
        for (int step = 0; step < 250; ++step) {
            const int* lk = legKeys[rng.pick(8)];
            const int* uk = upKeys[rng.pick(17)];
            const SoldierDrawState target = makeState(lk, uk, rng, -1);
            drive(cur, target, rng, 0.5f);
            cur = target;
            cur.fade = 1.0f;
            cur.hasPrev = false;
        }
    }
    gBossTest.blendOff = false;
    gBossTest.layersOff = false;
    CHECK(bad == 0,
          "the render composition survives %d transition frames inside the watchdog bounds (worst radial %.2f m, "
          "z [%.2f, %.2f])",
          evals, worstR, worstLo, worstHi);
}

// Death resets only the current pressure (repair round, defect B): the moment
// the player dies, a hunt in progress stands down - no cross-city route is
// replanned to the respawn, and the posted boss is not swept away by the
// distance rule while his player lies in a hospital across the map.
static void TestBossDeathReset() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    CHECK(LoadSoldierBinFromRepo(), "the death-reset proofs meet his real model");
    auto findBoss = []() {
        for (size_t i = 0; i < gMobs.size(); ++i)
            if (gMobs[i].kind == MOB_SOLDIER)
                return (int)i;
        return -1;
    };
    WideStage();
    gGame.deathTime = -1.0f;
    const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
    CHECK(s >= 0, "the boss spawns for the death-reset proof");
    gMobs[s].bossState = 3; // BS_HUNT, mid-hunt, route planned, pressure on
    gMobs[s].fightStarted = true;
    gMobs[s].huntT = 5.0f;
    gMobs[s].route.push_back(Vec3(60.5f, 0.5f, 10.0f));
    gGame.deathTime = 0.5f; // the player dies, by his hand, across the street
    RunMobsB(0.1f, Vec3(30.5f, 0.5f, 11.0f), nullptr);
    const int b = findBoss();
    CHECK(b >= 0 && gMobs[b].bossState == 0, "death stands him down to calm at once (state %d)", b >= 0 ? gMobs[b].bossState : -1);
    CHECK(b >= 0 && gMobs[b].route.empty() && !gMobs[b].fightStarted && !gMobs[b].aggro && gMobs[b].primedT > 30.0f,
          "the hunt is wiped and the slate is clean: no route, no fight, no enemy - "
          "the death law itself did not change under the tracker law");
    gGame.deathTime = -1.0f; // respawned in a hospital some 600 m away
    const float xAtRespawn = gMobs[b].pos.x;
    RunMobsB(3.0f, Vec3(600.5f, 0.5f, 11.0f), nullptr);
    const int b2 = findBoss();
    CHECK(b2 >= 0, "600 m and a respawn later he is still posted - the distance sweep does not erase him");
    // LAW UPDATE (the tracker law): the respawned man is not hunted - he is
    // TRACKED. The state is the pursuit (5), and in 3 s the boss has
    // already marched a chase-pace distance toward the hospital door.
    CHECK(b2 >= 0 && gMobs[b2].bossState == 5 && gMobs[b2].pos.x > xAtRespawn + 10.0f,
          "and the tracker re-acquired across the city: he is coming for the respawned player (state %d, x %.1f)",
          b2 >= 0 ? gMobs[b2].bossState : -1, b2 >= 0 ? gMobs[b2].pos.x : 0.0f);
    gGame.deathTime = -1.0f;
}

// CleanSA mode, the core's side of the gate: with the flag off nothing changes;
// with it on, no mob but the soldier may come into existence through any door -
// direct spawn, the natural spawn tick, or the /summon command - the refusal
// names the mode, and the Minecraft-world commands are refused while the
// soldier's own commands stay live.
static void TestCleanSA() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    CHECK(LoadSoldierBinFromRepo() && SoldierModelReady(), "the CleanSA proofs meet his real model");
    WideStage(); // resets rules, game and mobs: the flag starts OFF
    CHECK(!gRules.cleanSA, "CleanSA starts off");
    CHECK(SpawnMob(MOB_ZOMBIE, Vec3(2.5f, 0.5f, 10.0f), true) >= 0, "with the flag off a zombie still spawns");
    MobsClear();

    gRules.cleanSA = true;
    CHECK(SpawnMob(MOB_ZOMBIE, Vec3(2.5f, 0.5f, 10.0f), true) < 0, "CleanSA refuses a zombie");
    CHECK(SpawnMob(MOB_COW, Vec3(2.5f, 0.5f, 10.0f), true) < 0, "CleanSA refuses an animal");
    CHECK(SpawnMob(MOB_CREEPER, Vec3(2.5f, 0.5f, 10.0f), true) < 0, "CleanSA refuses a creeper");
    CHECK(SpawnMob(MOB_SKELETON, Vec3(2.5f, 0.5f, 10.0f), true) < 0, "CleanSA refuses a skeleton");
    CHECK(gMobs.empty(), "no Minecraft mob exists in CleanSA");
    CHECK(SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true) >= 0, "the soldier still spawns in CleanSA");
    MobsClear();

    MobsSpawnTick(2.0f, Vec3(0.5f, 0.5f, 10.0f)); // the tick itself refuses (the bridge never calls it here)
    CHECK(gMobs.empty(), "the natural spawn tick brings nothing in CleanSA");

    gChatLines.clear();
    RunCommand("summon zombie 3 0 10");
    CHECK(!gChatLines.empty() && gChatLines.back().color == kChatError &&
              gChatLines.back().text.find("CleanSA") != std::string::npos,
          "/summon zombie is refused and the refusal names CleanSA");
    CHECK(gMobs.empty(), "and no zombie came from the refused summon");
    RunCommand("summon soldier 3 0 10");
    CHECK(gMobs.size() == 1 && gMobs[0].kind == MOB_SOLDIER, "/summon soldier still works in CleanSA");
    MobsClear();
    RunCommand("gamemode creative");
    CHECK(!gChatLines.empty() && gChatLines.back().color == kChatError &&
              gChatLines.back().text.find("CleanSA") != std::string::npos && gGame.gameMode == MODE_SURVIVAL,
          "/gamemode is refused in CleanSA and the mode never flips");
    RunCommand("give diamond 1");
    CHECK(gChatLines.back().color == kChatError, "/give is refused in CleanSA");

    gRules = GameRules(); // flag off again: nothing about the gate is sticky
    CHECK(SpawnMob(MOB_ZOMBIE, Vec3(2.5f, 0.5f, 10.0f), true) >= 0, "with the flag off again a zombie spawns again");
    MobsClear();
    gRules = GameRules();
}

static void TestBoss() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> none;
    CHECK(LoadSoldierBinFromRepo(), "the boss tests meet his real model");

    // ---- C: the meter fills by what he can actually see of you
    {
        // LAW NOTE (the tracker law): the meter is priced here on fixed
        // geometry - a man standing his ground while his eyes are measured.
        // A tracking boss walks at the player and would stroll through the
        // sight-prop wall to point-blank range, which prices his legs, not
        // his eyes; the helper runs with the tracker flipped off and
        // restores it. The wall's veto under the tracker itself is pinned
        // by TestBossTracker (zero damage while fully walled).
        auto ticksToDetect = [&](bool wall, float wallZ) {
            gTrackerAlways = false;
            WideStage();
            host.wall = wall;
            host.wallC = Vec3(0.5f, 6.5f, wallZ);
            host.wallDir = Vec3(1, 0, 0);
            host.wallHalf = 3.0f;
            const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
            FaceMob(s, 0.0f);
            // the senses are priced on a man already his enemy (the latch is
            // set directly here: the LAW's triggers are proven in
            // TestBossAggroLaw - this block prices the eyes, not the law)
            gMobs[s].aggro = true;
            gMobs[s].fightStarted = true;
            gMobs[s].detect = 0.0f;
            int ticks = -1;
            for (int i = 0; i < 40; ++i) {
                MobsTick(0.05f, Vec3(0.5f, 12.5f, 11.0f), 0, true, &none);
                if (gMobs[s].detect >= 1.0f) {
                    ticks = i + 1;
                    break;
                }
            }
            host.wall = false;
            gTrackerAlways = true;
            return ticks;
        };
        // the test wall's z band gives three honest fractions: nothing (its band
        // swallows every sample line), two of three points (only the head's line
        // stays under its top), and the open street
        CHECK(ticksToDetect(true, 10.0f) < 0, "behind a full wall he sees nothing at all");
        const int peek = ticksToDetect(true, 11.57f); // only the head's sample line clears this wall's top
        const int open = ticksToDetect(false, 0.0f);
        CHECK(open > 0 && open <= 6, "wide open at 12 m he has you in a blink (%d ticks)", open);
        CHECK(peek > open && peek <= 30, "a peek of head and shoulders still fills the meter - slower, but it fills (%d vs %d ticks)", peek, open);
    }
    // ---- C: night trades range for hearing, and a gun in his face is fast news
    {
        WideStage();
        host.dark = true;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FaceMob(s, 0.0f);
        gMobs[s].aggro = true; // an enemy at night: the range itself is priced
        gMobs[s].fightStarted = true;
        // LAW NOTE (the tracker law): the run is 0.3 s, not 1.5 - under the
        // tracker he CLOSES while he is being measured (about 1.9 m in
        // 0.3 s), and the pin prices the RANGE: at 48 m he is still outside
        // the night reach of 45, so the meter stays empty either way.
        RunMobsB(0.3f, Vec3(0.5f, 50.5f, 11.0f), nullptr); // 50 m: beyond the night range of 45
        CHECK(gMobs[s].detect == 0.0f, "at night his reach is 45 m, not 60 (detect %.2f)", gMobs[s].detect);
        NotePlayerShot(Vec3(0.5f, -19.5f, 11.0f), 70.0f); // a shot 20 m behind him
        RunMobsB(0.3f, Vec3(0.5f, 50.5f, 11.0f), nullptr);
        CHECK(gMobs[s].alerted, "but at night his ears reach further - a shot behind him turns him");
        host.dark = false;
        WideStage();
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s2, 0.0f);
        FaceMob(s2, 0.0f);
        NotePlayerShot(Vec3(0.5f, -79.5f, 11.0f), 70.0f); // by day, 80 m is beyond a shot's carry
        RunMobsB(0.3f, Vec3(0.5f, -79.5f, 11.0f), nullptr); // and the man himself is out of sight behind him
        CHECK(!gMobs[s2].alerted, "by day a shot 80 m off is not heard");
    }
    {
        auto ticksToDetect55 = [&](bool aiming) {
            WideStage();
            host.aimingAtHim = aiming;
            const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
            FaceMob(s, 0.0f);
            gMobs[s].aggro = true; // already his enemy: this prices the meter,
            gMobs[s].fightStarted = true; // not the law that made him one
            gMobs[s].detect = 0.0f;
            int ticks = -1;
            for (int i = 0; i < 30; ++i) {
                MobsTick(0.05f, Vec3(0.5f, 55.5f, 11.0f), 0, true, &none);
                if (gMobs[s].detect >= 1.0f) {
                    ticks = i + 1;
                    break;
                }
            }
            host.aimingAtHim = false;
            return ticks;
        };
        const int passive = ticksToDetect55(false);
        const int aimed = ticksToDetect55(true);
        CHECK(passive > 0 && aimed > 0 && aimed < passive,
              "being aimed at fills his meter faster than being seen does (%d vs %d ticks at 55 m)", aimed, passive);
    }
    // ---- G, SUPERSEDED TWICE. First by the engagement law (2026-11-07,
    // morning): no talk-first, nothing to do with a peaceful player. Then
    // by the TRACKER LAW (the user's order, 2026-11-07, later): reaching
    // the player and genuinely seeing him IS the engagement - a peaceful
    // man in the open is fought on sight, with no provocation asked and
    // none recorded (aggro stays false: the bookkeeping still belongs to
    // the provocations alone). Commands stay gated off (kGuardTalkEnabled).
    {
        WideStage();
        gPlayed.clear();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        RunMobsB(1.0f, Vec3(0.5f, 12.5f, 11.0f), nullptr); // a peaceful man walks up
        CHECK(gMobs[s].bossState == 2 && !gMobs[s].aggro,
              "under the tracker law a peaceful first sighting is a FIGHT: contact by sight alone, no provocation recorded (state %d)", gMobs[s].bossState);
        CHECK(Heard(SND_BOSS_CMD1) + Heard(SND_BOSS_CMD2) + Heard(SND_BOSS_CMD3) == 0, "and no order is spoken");
        RunMobsB(11.0f, Vec3(0.5f, 12.5f, 11.0f), nullptr); // the man just stands there
        CHECK(gMobs[s].bossState == 2 && host.playerHurt > 0.0f && gMobs[s].detect >= 1.0f,
              "eleven seconds at arm's length: the fight is his whole world - he fires, and the meter stands full (state %d)",
              gMobs[s].bossState);
        CHECK(gMobs[s].primedT == 0.0f, "and a stranger's face is not remembered (primed %.0f s)", gMobs[s].primedT);
        CHECK(Heard(SND_BOSS_DOWN1) == 0, "and there is no stand-down, because nothing ever stood up");
    }
    {
        WideStage(); // a sprint past him: no provocation - and no need of one
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        RunMobsB(1.0f, Vec3(0.5f, 12.5f, 11.0f), nullptr);
        host.velocity = Vec3(6.0f, 0.0f, 0.0f); // he breaks into a sprint
        RunMobsB(0.3f, Vec3(0.5f, 12.5f, 11.0f), nullptr);
        CHECK(gMobs[s].bossState == 2 && !gMobs[s].aggro, "a sprint in front of him is a fight by sight alone (state %d)",
              gMobs[s].bossState);
        host.velocity = Vec3();
    }
    {
        WideStage(); // aim at him: THAT is a provocation - war, and no talk after
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        RunMobsB(1.0f, Vec3(0.5f, 12.5f, 11.0f), nullptr);
        host.aimingAtHim = true;
        RunMobsB(0.8f, Vec3(0.5f, 12.5f, 11.0f), nullptr);
        CHECK(gMobs[s].bossState == 2 && gMobs[s].aggro, "a gun held on him is war - by the law, not by a window");
        host.aimingAtHim = false;
        RunMobsB(3.0f, Vec3(0.5f, -160.5f, 11.0f), nullptr); // the man breaks away, far off
        const Vec3 front = gMobs[s].pos + Vec3(-std::sin(gMobs[s].aimYaw), std::cos(gMobs[s].aimYaw), 0.0f) * 10.0f + Vec3(0, 0, 1.0f);
        RunMobsB(0.6f, front, nullptr); // and shows himself again, peaceful as you like
        CHECK(gMobs[s].bossState != 1, "and this encounter never becomes a negotiation (state %d)", gMobs[s].bossState);
    }
    {
        WideStage(); // gunfire in the air is weather, not a provocation - the
                     // FIGHT here opens on his eyes, not on the shots
        gPlayed.clear();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        NotePlayerShot(Vec3(0.5f, 12.5f, 11.0f), 70.0f); // shots, moments before he lays eyes on the man
        RunMobsB(0.6f, Vec3(0.5f, 12.5f, 11.0f), nullptr);
        CHECK(gMobs[s].bossState == 2 && !gMobs[s].aggro,
              "under the tracker law the seen man is fought - and the shots are still not the reason: no provocation is recorded (state %d)", gMobs[s].bossState);
        CHECK(Heard(SND_BOSS_CMD1) + Heard(SND_BOSS_CMD2) + Heard(SND_BOSS_CMD3) == 0, "and no order is given");
    }
    // ---- B: the body - head is body, and the magazine arithmetic holds
    {
        WideStage();
        const int a = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        const int b = SpawnMob(MOB_SOLDIER, Vec3(9.5f, 0.5f, 10.0f), true);
        MobHurt(a, 20.0f, Vec3(3.5f, 0.5f, 11.8f), 0.0f); // a blow from head height
        MobHurt(b, 20.0f, Vec3(9.5f, 0.5f, 10.2f), 0.0f); // and one from the feet
        CHECK(Near(gMobs[a].health, gMobs[b].health, 0.01f),
              "a hit at his head costs exactly a hit at his body (%.1f vs %.1f) - there is no headshot", gMobs[a].health, gMobs[b].health);
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        for (int i = 0; i < 30; ++i) { // one full magazine of the reference hit (44 half hearts raw)
            gMobs[s].hurt = 0.0f;
            MobHurt(s, 44.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        }
        CHECK(gMobs[s].death < 0.0f && Near(gMobs[s].health, 3340.0f, 2.0f),
              "thirty solid hits leave him standing (health %.0f)", gMobs[s].health);
        // the rulebook's kevlar wears at half the old flat rate, so the vest
        // lasts about twice as long and the whole man takes ~6400 raw to
        // finish: the count is five magazines now, not four - CS's own law
        for (int i = 0; i < 120; ++i) {
            gMobs[s].hurt = 0.0f;
            MobHurt(s, 44.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        }
        CHECK(gMobs[s].death >= 0.0f || gMobs[s].health <= 0.0f, "about five magazines do finish him - hard to kill, not unkillable");
    }
    // ---- F: an empty rifle at arm's length is a pistol; at range, a reload
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 6.5f, 11.0f), nullptr); // the law's ignition
        RunMobsB(0.8f, Vec3(0.5f, 6.5f, 11.0f), nullptr); // the fight is on, 6 m out
        CHECK(gMobs[s].bossState == 2, "engaged");
        gMobs[s].mag = 0;
        gMobs[s].burstPause = 0.0f;
        RunMobsB(2.5f, Vec3(0.5f, 6.5f, 11.0f), nullptr);
        CHECK(gMobs[s].pistolOut && gMobs[s].pistolMag < 15 && gMobs[s].reloadT <= 0.0f,
              "empty at 6 m: the pistol comes out instead of a reload (pistol rounds left %d)", gMobs[s].pistolMag);
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 30.5f, 11.0f), nullptr);
        RunMobsB(0.8f, Vec3(0.5f, 30.5f, 11.0f), nullptr);
        gMobs[s].mag = 0;
        gMobs[s].burstPause = 0.0f;
        RunMobsB(1.0f, Vec3(0.5f, 30.5f, 11.0f), nullptr);
        CHECK(gMobs[s].reloadT > 0.0f && !gMobs[s].pistolOut, "empty at 30 m: he reloads the rifle, the pistol stays holstered");
    }
    // ---- F: he crouches for the long shot
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 30.5f, 11.0f), nullptr);
        bool crouchedAiming = false;
        for (int i = 0; i < 40; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 30.5f, 11.0f), 0, true, &none);
            if (gMobs[s].crouch && gMobs[s].draw.upVariant >= 3)
                crouchedAiming = true;
        }
        CHECK(crouchedAiming, "settled at range he shoots from a crouch - the pose rows say so");
    }
    // ---- F: the grenade answers a man who will not leave cover
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), nullptr); // seen, fighting
        MobHurt(s, 2700.0f, Vec3(0.5f, 20.5f, 10.0f), 0.0f); // badly wounded (under the 35% line of the bigger pool): he fights from cover now
        gMobs[s].hurt = 0.0f;
        host.wall = true;
        host.wallC = Vec3(0.5f, 10.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 3.5f;
        bool thrown = false;
        const int blasts0 = host.explosions;
        for (int i = 0; i < 160 && !thrown; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.0f), 0, true, &none);
            if (!gBossNades.empty() || host.explosions > blasts0)
                thrown = true;
        }
        host.wall = false;
        CHECK(thrown, "a target held behind cover for seconds is answered with a grenade");
        // and the grenade itself, once live, is the core's own damage
        WideStage();
        host.playerHurt = 0.0f;
        const int blasts1 = host.explosions;
        gBossNades.push_back(BossNade{ Vec3(0.5f, 2.5f, 12.0f), Vec3(0, 0, 0), 0.05f });
        RunMobsB(0.3f, Vec3(0.5f, 0.5f, 11.0f), nullptr);
        CHECK(host.explosions > blasts1 && host.playerHurt > 0.0f,
              "his grenade bursts on the core's channel and hurts (%.1f half hearts)", host.playerHurt);
        gBossNades.clear();
    }
    // ---- D: a fence is jumped, a wall is walked around - both on the hunt,
    // the movement whose whole job is closing the distance. The fence leaks a
    // standing man's head (a third of him), so the hunted man ducks: with his
    // middle at 10.2 the fence swallows every sample and the hunt begins.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 14.5f, 11.0f), nullptr); // seen, fighting
        host.wall = true;
        host.wallC = Vec3(0.5f, 6.5f, 8.0f); // its band tops out at 11: a fence
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 8.0f;
        float maxZ = 0.0f;
        float dEnd = 1e9f;
        for (int i = 0; i < 260; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 14.5f, 10.2f), 0, true, &none); // ducked behind it
            maxZ = std::max(maxZ, gMobs[s].pos.z);
            dEnd = (gMobs[s].pos - Vec3(0.5f, 14.5f, 10.0f)).Length();
            if (dEnd < 3.0f)
                break;
        }
        host.wall = false;
        // past the fence his sight line clears and the ducked man is found: he
        // ends north of the fence, fighting - that IS the hunt succeeding.
        // "North of it" is the fence's own line (y 6.5) with the man in his
        // sight right now - the exact settle point of a fight is his combat
        // movement's business, and a 7.5 line used to leave this check to the
        // dice exactly as its tall-wall twin did (see the next block).
        CHECK(maxZ > 10.8f && gMobs[s].pos.y > 6.5f && gMobs[s].bossState == 2 && gMobs[s].lostT < 0.5f,
              "the fence is jumped on the hunt and the man found behind it (peak %.2f m, ends y %.1f in state %d)",
              maxZ, gMobs[s].pos.y, gMobs[s].bossState);
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), nullptr); // he has the man's measure
        host.wall = true;
        host.wallC = Vec3(0.5f, 6.5f, 10.0f); // band to 13: a 3 m wall, and blind
        host.wallDir = Vec3(1, 0, 0);
        // the wall is 18 m long and the man stands 14 m behind it - measured
        // the hard way: his combat strafe orbits a man 14 m off in a sweep
        // +-8 m wide, and around a shorter wall's end that orbit kept glimpsing
        // him, resetting the hunt clock and leaving "did he get around it" to
        // the dice of whichever C library rolled them. At this size no orbit
        // position and no corner graze can see the man: the pursuit has to
        // walk around an end on foot, which is the deed this test exists for.
        // (Longer walls than this are the city graph's job in the game.)
        host.wallHalf = 9.0f;
        float maxZ = 0.0f;
        float peakLost = 0.0f; // his longest blind stretch: proof he truly lost the man
        float dEnd = 1e9f;
        // re-engaged means: fighting again, the man in his sight right now, and
        // his body past the wall's cover - north of its line or out past an end
        auto reengaged = [&]() {
            return gMobs[s].bossState == 2 && gMobs[s].lostT < 0.05f &&
                   (gMobs[s].pos.y > 6.5f || std::fabs(gMobs[s].pos.x - 0.5f) > host.wallHalf);
        };
        for (int i = 0; i < 640; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.0f), 0, true, &none);
            maxZ = std::max(maxZ, gMobs[s].pos.z);
            peakLost = std::max(peakLost, gMobs[s].lostT);
            dEnd = (gMobs[s].pos - Vec3(0.5f, 20.5f, 10.0f)).Length();
            if (reengaged())
                break;
        }
        host.wall = false;
        CHECK(maxZ < 10.6f && peakLost > 0.5f && reengaged(),
              "the tall wall is not jumped - the hunt walks around its end and re-engages (peak %.2f m, blind %.2f s, ends y %.1f in state %d, %.1f m off)",
              maxZ, peakLost, gMobs[s].pos.y, gMobs[s].bossState, dEnd);
    }
    // ---- K1: the street may start a fight with him; he finishes it, and only that one
    {
        WideStage();
        TestHost::Person atk, bys;
        atk.centre = Vec3(0.5f, 8.5f, 11.0f);
        bys.centre = Vec3(4.5f, 8.5f, 11.0f);
        host.people = { atk, bys };
        const std::vector<BeingInfo> beings = { BeingInfo{ 0, atk.centre }, BeingInfo{ 1, bys.centre } };
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        MobProvoked(s, 0, atk.centre); // the man in the street opens fire on him
        RunMobsB(3.0f, Vec3(90.5f, 0.5f, 11.0f), &beings);
        CHECK(host.people[0].hurt > 0.0f, "an attacker from the street is engaged and hit (%.1f)", host.people[0].hurt);
        CHECK(host.people[1].hurt == 0.0f, "the bystander next to him is never touched");
        const std::vector<BeingInfo> gone;
        RunMobsB(9.0f, Vec3(90.5f, 0.5f, 11.0f), &gone); // the attacker is gone from the world
        CHECK(gMobs[s].bossState != 2, "with the attacker gone he stands down again (state %d)", gMobs[s].bossState);
        host.people.clear();
    }
    // ---- K2/L3, the accounting: relays and rams are capped, never lethal surprises
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        MobProxyDamage(s, 10000.0f, Vec3(0.5f, 0.5f, 10.0f), -1); // a whole street's volley in one event
        const float poolLoss = (4000.0f - gMobs[s].health) + (1200.0f - gMobs[s].armor);
        CHECK(Near(poolLoss, 300.0f, 1.0f), "the cap is a tenth of his health INCOMING; CS kevlar pricing lands 300 of pool for it - 200 health, 100 wear (lost %.0f)", poolLoss);
        WideStage();
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        MobVehicleHit(s2, 25.0f, Vec3(0.5f, 0.5f, 10.0f), true); // a car at full tilt
        const float ramLoss = (4000.0f - gMobs[s2].health) + (1200.0f - gMobs[s2].armor);
        CHECK(Near(ramLoss, 1050.0f, 2.0f) && gMobs[s2].death < 0.0f,
              "a full-speed ram is capped at 35%% of his health incoming; CS kevlar pricing lands 1050 of pool - and never kills outright (lost %.0f)", ramLoss);
        CHECK(gMobs[s2].bossState == 2 && gMobs[s2].detect >= 1.0f, "and he is up and fighting the driver at once");
    }
    // ---- L: the driver is a target, and the car is a body that takes damage.
    // The smart pack restaged this on the traced world: the host now answers
    // ShotTrace with a real vehicle box (the nose toward him - the engine bay
    // faces his muzzle), and his rounds resolve on the first thing the line
    // meets. Driver hits still go through the glass; the rounds that miss the
    // man land on the car at the traced point and pay the zone price there.
    // (Tyres are pinned exactly in TestSmartPack: from a chest aim at this
    // range the traced crossings honestly land on the upper body, and the old
    // staging's pops were an artifact of the deleted closest-approach box.)
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        host.vehicle = host.vehBoxNum;
        ProvokeByAim(host, Vec3(0.5f, 13.6f, 10.95f), nullptr); // the driver provoked him
        host.vehPoseOn = true;
        host.vehBoxOn = true;
        host.vehBoxCentre = Vec3(0.5f, 14.5f, 10.5f);
        host.vehBoxFwd = Vec3(0, -1, 0);
        host.vehCentre = host.vehBoxCentre;
        host.vehFwd = host.vehBoxFwd;
        host.vehSeat = Vec3(0.5f, 13.6f, 10.95f);
        host.vehVel = Vec3();
        host.playerHurt = 0.0f;
        host.vehHurt.clear();
        host.vehTyrePops.clear();
        for (int i = 0; i < 360; ++i)
            MobsTick(0.05f, Vec3(0.5f, 13.6f, 10.95f), 0, false, &none);
        CHECK(host.playerHurt > 0.0f, "the driver is hit through his own windshield (%.1f half hearts)", host.playerHurt);
        CHECK(host.vehHurt[host.vehBoxNum] > 0.0f, "and the car itself takes his fire on the traced line (%.1f half hearts)",
              host.vehHurt[host.vehBoxNum]);
        host.vehPoseOn = false;
        host.vehBoxOn = false;
        host.vehicle = -1;
    }
    // ---- H: his voice answers his states
    {
        WideStage();
        gPlayed.clear();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(3.5f, 0.5f, 10.0f), true);
        MobHurt(s, 5.0f, Vec3(0.5f, 0.5f, 10.0f), 0.0f);
        CHECK(Heard(SND_BOSS_HURT) >= 1, "a hurt boss grunts");
        WideStage();
        gPlayed.clear();
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s2, 0.0f);
        // LAW NOTE (the tracker law): the idle check-in lives on his POST -
        // the patrol wait. A tracking boss has no post: the gate takes him
        // off it the first tick. The voice is priced with the tracker
        // flipped off (the patrol's own law), and the switch restored.
        gTrackerAlways = false;
        for (int i = 0; i < 12; ++i) {
            // each pass is its own idle moment, cooldown and all: without the
            // reset, the first attempt's silence roll (one in five) sets a 5 s
            // cooldown that vetoes all eleven retries, and the whole check is
            // a single 80/20 flip of wherever the dice happen to sit
            gMobs[s2].patrolWait = 5.0f;
            gMobs[s2].idleVoiceT = 0.01f;
            gMobs[s2].voiceCd = 0.0f;
            MobsTick(0.05f, Vec3(90.5f, 0.5f, 11.0f), 0, true, &none);
        }
        CHECK(Heard(SND_BOSS_IDLE1) + Heard(SND_BOSS_IDLE2) >= 1, "on his post he checks in by radio now and then");
        gTrackerAlways = true;
    }
    // ---- M: legs walk the movement, the rifle holds the aim
    {
        CHECK(LoadSoldierBinFromRepo(), "the model loads for the layer proofs");
        // the stand carries the legs' facing (east), as the bridge builds it
        // from his leg yaw; the aim lives in the twist on top of it
        const Pose stand = EntityPose(Vec3(0, 0, 0), Vec3(0, -1, 0), Vec3(0, 0, 1), Vec3(1, 0, 0), 1.0f);
        SoldierDrawState st; // aim north (+y), strafing east (+x)
        st.legCategory = SP_RUN;
        st.legVariant = 2; // the right-strafe variant
        st.legTime = 0.3f;
        st.aimYaw = 0.0f;
        st.legYaw = -kPi / 2.0f;
        st.upCategory = SP_AIM;
        st.upVariant = 0;
        int legSamples = 0;
        for (int ph = 0; ph < 8; ++ph) {
            st.gait = ph / 8.0f; // the cycle is driven by the gait phase
            Vec3 d1;
            if (SoldierLegDir(stand, st, &d1))
                ++legSamples;
        }
        CHECK(legSamples == 8, "the leg direction answers through the cycle");
        // The strafe variants ride the run cycle itself, yawed by the leg
        // layer - the repair round measured the file's own "left"/"right"
        // sequences and they are one-shot events, not gaits (root travel up
        // to 1.4 m, the pelvis sinking to the floor, the figure leaving the
        // ground plane entirely by the last frames). Looping them was a large
        // part of the first eye test's "a bit broken all over", so the bake
        // now carries the run cycle for variants 1/2/3 and the yaw does the
        // strafing, the way Counter-Strike itself plays it.
        CHECK(Near(SoldierGaitStride(SP_RUN, 2), SoldierGaitStride(SP_RUN, 0)) && SoldierGaitStride(SP_RUN, 0) > 2.5f,
              "the strafe rides the run cycle's own stride (%.2f m, shared with the forward %.2f m)",
              SoldierGaitStride(SP_RUN, 2), SoldierGaitStride(SP_RUN, 0));
        st.gait = 0.3f;
        {
            SoldierDrawState fwdSt = st;
            fwdSt.legVariant = 0;
            SoldierLocal la2[64], lb2[64];
            SoldierComposeLocals(st, la2, 64);
            SoldierComposeLocals(fwdSt, lb2, 64);
            float diff = 0.0f;
            for (int i = 0; i < 64; ++i)
                diff = std::max(diff, (la2[i].pos - lb2[i].pos).Length());
            CHECK(diff < 0.001f, "the strafe variant IS the forward run cycle - the file's directional sequences are events, not gaits (strongest bone %.3f m)", diff);
        }
        st.upCategory = SP_SHOOT;
        st.upVariant = 0;
        Vec3 mz, bt;
        CHECK(SoldierGunPose(stand, st, &mz, &bt), "the gun line answers");
        Vec3 gd = mz - bt;
        gd.z = 0.0f;
        gd = gd * (1.0f / std::max(0.001f, gd.Length()));
        CHECK(gd.y > 0.98f && std::fabs(gd.x) < 0.19f, "his rifle still points north (%.2f, %.2f) - no statue glide", gd.x, gd.y);
        // the waist twist is rotations only: no bone changes length
        SoldierDrawState straight = st;
        straight.legYaw = 0.0f;
        SoldierLocal la[64], lb[64];
        const int na = SoldierComposeLocals(st, la, 64);
        const int nb = SoldierComposeLocals(straight, lb, 64);
        const int* parents = SoldierBoneParents();
        float worst = 0.0f;
        for (int i = 0; i < na && i < nb; ++i) {
            if (!parents || parents[i] < 0)
                continue;
            const float da = (la[i].pos - la[parents[i]].pos).Length();
            const float db = (lb[i].pos - lb[parents[i]].pos).Length();
            worst = std::max(worst, std::fabs(da - db));
        }
        CHECK(na == nb && worst < 0.005f, "the twist stretches nothing (worst bone drift %.4f m)", worst);
        // aim pitch rows: up, level, down - in that order on the gun line
        auto gunPitch = [&](int variant) {
            SoldierDrawState a;
            a.legCategory = SP_IDLE;
            a.upCategory = SP_AIM;
            a.upVariant = variant;
            Vec3 m2, b2;
            if (!SoldierGunPose(stand, a, &m2, &b2))
                return 0.0f;
            Vec3 d = m2 - b2;
            return std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y));
        };
        const float pUp = gunPitch(1), pLevel = gunPitch(0), pDown = gunPitch(2); // the bake's order: level, up, down
        CHECK(pUp > pLevel && pLevel > pDown && pUp > 0.2f && pDown < -0.2f,
              "the aim rows pitch the rifle up, level, down (%.2f / %.2f / %.2f rad)", pUp, pLevel, pDown);
    }
    // ---- A: the map in his head - first a toy, then the real city
    {
        // a four-node toy graph, built byte by byte in the documented format
        std::vector<unsigned char> g;
        auto put = [&](const void* d, size_t n) { const unsigned char* c = (const unsigned char*)d; g.insert(g.end(), c, c + n); };
        auto u32 = [&](uint32_t v) { put(&v, 4); };
        auto f32 = [&](float v) { put(&v, 4); };
        put("MCNAV1", 6);
        u32(4);
        u32(6);
        const float pts[4][3] = { { 0, 0, 0 }, { 10, 0, 0 }, { 20, 0, 0 }, { 20, 10, 0 } };
        for (auto& pt : pts) { f32(pt[0]); f32(pt[1]); f32(pt[2]); }
        const uint32_t rows[5] = { 0, 1, 3, 5, 6 };
        for (uint32_t r : rows) u32(r);
        const uint32_t tg[6] = { 1, 0, 2, 1, 3, 2 };
        for (uint32_t t : tg) u32(t);
        uint32_t sum = 0;
        for (unsigned char c : g) sum += c;
        u32(sum);
        CHECK(LoadNavGraph(g.data(), g.size()), "a hand-built graph loads");
        std::vector<Vec3> route;
        CHECK(NavRoute(Vec3(0, 0, 0), Vec3(20, 10, 0), &route) && Near(NavRouteLength(route), 30.0f, 0.1f),
              "and routes along its streets (%.1f m)", NavRouteLength(route));
        std::vector<unsigned char> bad = g;
        bad[20] ^= 0xFF;
        CHECK(!LoadNavGraph(bad.data(), bad.size()), "a corrupted graph is refused, checksum and all");
        CHECK(LoadNavGraphFromRepo(), "the real city graph loads");
        CHECK(NavNearest(Vec3(2480, -1660, 13), 60.0f) >= 0, "Grove Street is on his map");
        CHECK(NavRoute(Vec3(2480, -1660, 13), Vec3(2030, -1410, 13), &route),
              "Grove to County General Hospital routes");
        const float hosp = NavRouteLength(route);
        CHECK(hosp > 900.0f && hosp < 1250.0f, "by street it is %.0f m, not the crow's 515", hosp);
        CHECK(NavRoute(Vec3(2480, -1660, 13), Vec3(1240, -1330, 13), &route), "Grove to All Saints routes too");
        const float saints = NavRouteLength(route);
        CHECK(saints > 1.3f * 1283.0f && saints < 2600.0f,
              "across the district the streets run %.0f m against 1283 straight - over 1.3x, as streets do", saints);
        CHECK(!NavRoute(Vec3(2480, -1660, 13), Vec3(1650, -2400, 13), &route),
              "the airport's ped island is the game's own gap: no route is invented for it");
    }
}

// =====================================================================
// THE SMART PACK (2026-11-06): combat navigation on the graph, the relentless
// pursuit, the chase sprint, the deeper pool, vehicle damage his eye can see,
// and the Counter-Strike polish - each pinned where it can be proven here.
// What only the game can prove is listed in CS-BOSS-fa.md's acceptance script.
// ------------------------------------------------------------------
// The rulebook pour: Counter-Strike 1.6's own numbers, priced in the core.
// Every family pinned on the pure laws (CsRules) first, then end to end
// through the boss's fight where the plumbing is the point.
static void TestCsRules() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    const CsWeapon& m4 = CsWeaponAt(CSW_M4A1);
    const CsWeapon& usp = CsWeaponAt(CSW_USP);

    // ---- the armory table itself: the two he carries, and the shape of the rest
    {
        CHECK(CSW_COUNT == 25, "the full armory is on the table: 24 firearms and the knife (%d rows)", CSW_COUNT);
        CHECK(Near(m4.baseDamage, 33.0f) && Near(m4.rangeMod, 0.97f) && m4.mag == 30 && m4.reserve == 90 &&
                  Near(m4.reloadSec, 3.07f) && m4.price == 3100 && m4.penPower == 35 && m4.penSurfaces == 2 &&
                  Near(m4.cycleSec, 0.09f),
              "the M4A1's row is the game's own (33 dmg, RM 0.97, 30/90, 3.07 s, $3100, 5.56 pen 35 x2, cycle 0.09)");
        CHECK(Near(usp.baseDamage, 34.0f) && Near(usp.baseDamageSil, 30.0f) && Near(usp.rangeMod, 0.79f) &&
                  usp.mag == 12 && usp.price == 500 && usp.penSurfaces == 1 && usp.thinOnly,
              "the USP's row likewise (34/30 silenced, RM 0.79, 12 rounds, thin surfaces only)");
        CHECK(Near(CsWeaponAt(CSW_AK47).baseDamage, 36.0f) && Near(CsWeaponAt(CSW_AWP).baseDamage, 115.0f) &&
                  Near(CsWeaponAt(CSW_AWP).rangeMod, 0.99f) && CsWeaponAt(CSW_DEAGLE).penSurfaces == 2 &&
                  !CsWeaponAt(CSW_DEAGLE).thinOnly && CsWeaponAt(CSW_M3).penSurfaces == 0,
              "and the rest of the table carries the measured constants (AK 36, AWP 115/0.99, the Deagle penetrates like a rifle, the shotgun not at all)");
    }
    // ---- the range law: Damage x RM ^ (distance / 500 units), 500 units = 12.7 m
    {
        CHECK(Near(CsRangeFalloff(m4, 0.0f), 1.0f), "point blank is whole");
        CHECK(Near(CsRangeFalloff(m4, 12.7f), 0.97f, 0.001f), "one 500-unit step costs exactly the modifier (%.4f)", CsRangeFalloff(m4, 12.7f));
        CHECK(Near(CsRangeFalloff(m4, 25.4f), 0.9409f, 0.001f), "two steps square it (%.4f)", CsRangeFalloff(m4, 25.4f));
        CHECK(CsRangeFalloff(m4, 300.0f) == 0.0f, "beyond the weapon's own range the bullet never arrives");
        CHECK(Near(CsShotHh(m4, 0.0f, CSZ_CHEST, false, 0.0f, nullptr), 6.0f, 0.01f),
              "the anchor holds: an M4A1 chest hit up close is the 6 half hearts it always was");
        CHECK(Near(CsShotHh(m4, 60.0f, CSZ_CHEST, false, 0.0f, nullptr), 5.20f, 0.02f),
              "at 60 m the true curve still pays 5.20 - the old linear fudge paid 3.3 (%.2f)",
              CsShotHh(m4, 60.0f, CSZ_CHEST, false, 0.0f, nullptr));
        CHECK(Near(CsShotHh(usp, 25.4f, CSZ_CHEST, false, 0.0f, nullptr), 3.86f, 0.02f),
              "the USP at 25.4 m: 34 x 0.79^2 on the anchor scale (%.2f)", CsShotHh(usp, 25.4f, CSZ_CHEST, false, 0.0f, nullptr));
    }
    // ---- the zones: 4 / 1 / 1.25 / 0.75, classified by the hit's true height
    {
        CHECK(CsZoneFromHeight(1.70f) == CSZ_HEAD && CsZoneFromHeight(1.20f) == CSZ_CHEST &&
                  CsZoneFromHeight(0.90f) == CSZ_STOMACH && CsZoneFromHeight(0.40f) == CSZ_LEGS,
              "heights classify: head, chest, stomach, legs");
        CHECK(Near(CsShotHh(m4, 0.0f, CSZ_HEAD, false, 0.0f, nullptr), 24.0f, 0.01f) &&
                  Near(CsShotHh(m4, 0.0f, CSZ_STOMACH, false, 0.0f, nullptr), 7.5f, 0.01f) &&
                  Near(CsShotHh(m4, 0.0f, CSZ_LEGS, false, 0.0f, nullptr), 4.5f, 0.01f),
              "point blank: a head hit pays 24, a stomach 7.5, a leg 4.5 - the 4x / 1.25x / 0.75x of the game");
        const float hLevel = CsHitHeight(Vec3(0.5f, 0.5f, 11.5f), Vec3(0, 1, 0), Vec3(0.5f, 10.5f, 10.0f), 10.0f);
        CHECK(Near(hLevel, 1.5f, 0.01f) && CsZoneFromHeight(hLevel) == CSZ_HEAD,
              "a level line at head height prices as a head hit (%.2f m)", hLevel);
        Vec3 d = Vec3(0, 10, -0.5f);
        d = d * (1.0f / d.Length());
        const float hChest = CsHitHeight(Vec3(0.5f, 0.5f, 11.6f), d, Vec3(0.5f, 10.5f, 10.0f), 10.0f);
        CHECK(Near(hChest, 1.1f, 0.02f) && CsZoneFromHeight(hChest) == CSZ_CHEST,
              "a line falling to the chest prices as a chest hit (%.2f m)", hChest);
    }
    // ---- kevlar: the split, the wear, the spill, and what it never covers
    {
        float wear = 0.0f;
        CHECK(Near(CsArmorSplit(33.0f, 1.35f, 1000.0f, &wear), 22.275f, 0.01f) && Near(wear, 5.3625f, 0.01f),
              "M4A1 vs kevlar: 22.28 of 33 reaches health, the vest wears 5.36 (health %.2f, wear %.2f)",
              CsArmorSplit(33.0f, 1.35f, 1000.0f, &wear), wear);
        CHECK(Near(CsArmorSplit(33.0f, 1.0f, 2.0f, &wear), 29.0f, 0.01f) && Near(wear, 2.0f),
              "a vest with 2 left dies inside the hit and spills the rest: 29 to health");
        float wearHh = 0.0f;
        CHECK(Near(CsShotHh(m4, 0.0f, CSZ_CHEST, true, 20.0f, &wearHh), 4.05f, 0.01f) && Near(wearHh, 0.975f, 0.01f),
              "the player in the game's vest: a point-blank chest hit costs him 4.05 and wears 0.98 off the vest");
        CHECK(Near(CsShotHh(m4, 0.0f, CSZ_LEGS, true, 20.0f, &wearHh), 4.5f, 0.01f) && Near(wearHh, 0.0f),
              "but his legs were never armoured - full price, no wear");
        CHECK(Near(CsShotHh(m4, 0.0f, CSZ_HEAD, true, 20.0f, &wearHh), 24.0f, 0.01f) && Near(wearHh, 0.0f),
              "and the game's vest has no helmet: a head hit is whole");
    }
    // ---- penetration, the pure law (FireBullets3's shape on SA's surfaces)
    {
        const CsPen thin = CsPenetrate(m4, HS_WOOD_THIN, 0.0f);
        CHECK(thin.passes && Near(thin.dmgMul, 0.6f), "the M4A1 passes a wooden fence and keeps 60%%");
        CHECK(CsPenetrate(m4, HS_WOOD, 0.2f).passes && !CsPenetrate(m4, HS_WOOD, 2.0f).passes,
              "solid wood: 0.2 m of it passes, 2 m stops the 5.56 dead (its power is 0.89 m)");
        CHECK(!CsPenetrate(m4, HS_STONE, 0.0f).passes, "a world wall of unknown thickness is a wall - nothing passes on faith");
        const CsPen lip = CsPenetrate(m4, HS_STONE, 0.1f);
        CHECK(lip.passes && Near(lip.dmgMul, 0.5f), "but a measured 0.1 m concrete lip passes at the concrete tax (power x0.25 = 0.22 m)");
        CHECK(CsPenetrate(m4, HS_METAL, 0.1f).passes && !CsPenetrate(m4, HS_METAL, 0.2f).passes,
              "metal taxes the power to 0.13 m: a thin sheet passes, a plate does not");
        CHECK(CsPenetrate(usp, HS_GLASS, 0.0f).passes && Near(CsPenetrate(usp, HS_GLASS, 0.0f).dmgMul, 0.75f),
              "the USP passes glass at the object price of 75%%");
        CHECK(!CsPenetrate(usp, HS_WOOD, 0.05f).passes && CsPenetrate(usp, HS_WOOD_THIN, 0.0f).passes,
              "but never a real wooden wall (the pre-Source class law) - a thin fence only");
        CHECK(!CsPenetrate(usp, HS_OBJECT, 0.5f).passes && CsPenetrate(usp, HS_OBJECT, 0.3f).passes,
              "a prop thicker than the .45's power (0.38 m) stops it; a thinner one does not");
        CHECK(!CsPenetrate(CsWeaponAt(CSW_M3), HS_GLASS, 0.0f).passes, "the shotgun penetrates nothing");
    }
    // ---- the HE curve: the measured heart, linear to the edge, halved by armour
    {
        CHECK(Near(CsHeHh(0.0f, 5.5f, 96.0f, false, 0.0f, nullptr), 17.45f, 0.01f),
              "at the grenade's heart: the full 96, on the anchor scale");
        CHECK(Near(CsHeHh(2.75f, 5.5f, 96.0f, false, 0.0f, nullptr), 8.73f, 0.01f), "halfway out: half of it");
        CHECK(CsHeHh(5.5f, 5.5f, 96.0f, false, 0.0f, nullptr) == 0.0f && CsHeHh(9.0f, 5.5f, 96.0f, false, 0.0f, nullptr) == 0.0f,
              "at the edge and beyond: nothing");
        float wear = 0.0f;
        CHECK(Near(CsHeHh(0.0f, 5.5f, 96.0f, true, 20.0f, &wear), 8.73f, 0.01f) && Near(wear, 4.36f, 0.01f),
              "in a vest the heart pays half to health - the measured 48 - and wears the vest by the law");
    }
    // ---- the recoil patterns: deterministic shape, seeded sway
    {
        const CsRecoil r0 = CsRecoilPattern(CSW_M4A1, 0, 42);
        CHECK(r0.pitchDeg == 0.0f && r0.yawDeg == 0.0f, "the first shot of a spray is never moved by recoil");
        bool climb = true, bounded = true;
        float prev = 0.0f;
        for (int i = 1; i <= 30; ++i) {
            const CsRecoil r = CsRecoilPattern(CSW_M4A1, i, 42);
            if (r.pitchDeg < prev - 1e-6f)
                climb = false;
            prev = r.pitchDeg;
            if (std::fabs(r.yawDeg) > 2.4f + 1e-6f)
                bounded = false;
        }
        CHECK(climb && Near(CsRecoilPattern(CSW_M4A1, 10, 42).pitchDeg, 5.3f, 0.01f),
              "the rifle climbs steadily to 5.3 degrees by the tenth shot");
        CHECK(bounded && Near(CsRecoilPattern(CSW_M4A1, 30, 42).pitchDeg, 6.0f, 0.01f),
              "and the sway never leaves the triangle (cap 2.4 degrees, pitch capped at 6)");
        const CsRecoil a = CsRecoilPattern(CSW_M4A1, 15, 42), b = CsRecoilPattern(CSW_M4A1, 15, 42);
        CHECK(a.pitchDeg == b.pitchDeg && a.yawDeg == b.yawDeg, "same spray, same shot, same seed: the same offset - the pattern is a law, not a dice roll");
        bool seedMatters = false;
        for (uint32_t seed = 1; seed <= 6 && !seedMatters; ++seed)
            if (CsRecoilPattern(CSW_M4A1, 15, seed).yawDeg != a.yawDeg)
                seedMatters = true;
        CHECK(seedMatters, "but a new spray sways anew - 1.6's semi-random heart");
        const CsRecoil u8 = CsRecoilPattern(CSW_USP, 8, 7);
        CHECK(Near(u8.pitchDeg, 4.4f, 0.01f) && std::fabs(u8.yawDeg) <= 0.15f,
              "the USP climbs a straight line to its 4.4 cap, fretted by at most a whisper sideways");
    }
    // ---- end to end: the boss's fight prices all of it
    {
        // a vest on the player: his hits cost less and the vest wears
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 12.5f, 11.0f), none);
        host.armorPoints = 100.0f;
        RunMobsB(3.0f, Vec3(0.5f, 12.5f, 11.0f), none);
        CHECK(host.playerHurt > 0.0f && host.armorWorn > 0.0f && host.armorPoints < 100.0f,
              "a vested player is hurt less and his vest wears by the law (hurt %.1f, worn %.1f, vest %.0f left)",
              host.playerHurt, host.armorWorn, host.armorPoints);
        host.armorPoints = 0.0f;
        host.armorWorn = 0.0f;
    }
    {
        // surfaces without end: fence after fence after fence - the M4A1's
        // bullet passes two and dies in the third, exactly its count
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 12.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 12.5f, 11.0f), none);
        const float hurt0 = host.playerHurt;
        host.traceScriptOn = true;
        host.traceScript = HostHit();
        host.traceScript.hit = true;
        host.traceScript.dist = 2.0f;
        host.traceScript.point = Vec3(0.5f, 2.5f, 11.0f);
        host.traceScript.surface = HS_WOOD_THIN;
        RunMobsB(4.0f, Vec3(0.5f, 12.5f, 11.0f), none);
        CHECK(host.playerHurt == hurt0, "two fences are his limit: the third stops every round - no damage leaks through");
        host.traceScriptOn = false;
    }
    {
        // one fence in one shot's line: paired runs, same seed, same
        // everything, ONE measured shot each - the fenced run must pay
        // exactly that shot's 40% wood tax less than the open run
        auto runPair = [&](bool fence, int seed) {
            WideStage();
            McsaSrand(seed);
            const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
            FaceMob(s, 0.0f);
            ProvokeByAim(host, Vec3(0.5f, 5.5f, 11.0f), none);
            RunMobsB(1.0f, Vec3(0.5f, 5.5f, 11.0f), none); // engaged at 5 m
            host.playerHurt = 0.0f;
            // the measured window starts from an identical stance in both
            // runs - same spot, same trigger state: the law's ignition is
            // 0.7 s of live combat in which he closes and strafes, and where
            // he stands when the window opens otherwise decides whether the
            // scripted fence even sits in the first shot's line (one pair
            // measured a 0.00 discount that way, another lost whole hits to
            // spray-roll divergence). The pair may differ ONLY by the fence.
            gMobs[s].pos = Vec3(0.5f, 0.5f, 10.0f);
            gMobs[s].vel = Vec3();
            // and the window holds exactly ONE rifle round (pistol empty
            // too, so the empty rifle means a reload, not a second weapon):
            // the pair prices a single shot's tax, no more - a summed window
            // let whole extra hits and a lucky head-zone first shot (4x)
            // masquerade as the fence's price
            gMobs[s].mag = 1;
            gMobs[s].pistolMag = 0;
            gMobs[s].burstShots = 0;
            gMobs[s].burstPause = 0.05f;
            gMobs[s].sprayIndex = 0.0f;
            gMobs[s].shotTimer = 0.0f;
            if (fence) {
                host.traceFirst = HostHit();
                host.traceFirst.hit = true;
                host.traceFirst.dist = 2.0f;
                host.traceFirst.point = Vec3(0.5f, 3.0f, 11.3f);
                host.traceFirst.surface = HS_WOOD_THIN;
                host.traceCalls = 0;
                host.traceFirstOn = true;
            }
            RunMobsB(2.0f, Vec3(0.5f, 5.5f, 11.0f), none);
            host.traceFirstOn = false;
            return host.playerHurt;
        };
        for (int seed = 777; seed <= 779; ++seed) {
            const float open = runPair(false, seed);
            const float fenced = runPair(true, seed);
            CHECK(open > 4.0f, "the open-line pair run lands its one shot (%.1f)", open);
            const float share = open > 0.0f ? (open - fenced) / open : 0.0f;
            CHECK(share > 0.30f && share < 0.55f,
                  "one fence in the shot's line costs that shot 40%% of itself, no more, no less (pair %d: %.2f of %.1f)",
                  seed, share, open);
        }
    }
    {
        // CS's footstep law: a creeping man is silent; a jogging man is news.
        // LAW NOTE (the tracker law): the law of footsteps is priced here on
        // a POSTED man, ears only. A tracking boss walks around to the
        // creeping man's ground and sees him point-blank - true, and not
        // this block's question. The block runs with the tracker flipped
        // off and restores it; the tracker's own arrival at a silent hidden
        // man is pinned by TestBossTracker.
        gTrackerAlways = false;
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f); // facing +y; the man is behind him, ear-work only
        host.velocity = Vec3(0.0f, 1.5f, 0.0f); // a creep: over the old walk band
        RunMobsB(1.5f, Vec3(0.5f, -7.5f, 11.0f), none); // 8 m behind him
        CHECK(!gMobs[s].alerted, "a creeping man at his back is not heard - in CS, walking makes no sound");
        host.velocity = Vec3(0.0f, 3.0f, 0.0f); // a jog: running footsteps
        RunMobsB(0.5f, Vec3(0.5f, -7.5f, 11.0f), none);
        CHECK(gMobs[s].alerted, "but a jogging man at his back is heard at once");
        host.velocity = Vec3();
        gTrackerAlways = true;
    }
}

static void TestSmartPack() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // ---- the vehicle zones, priced exactly (the numbers the bridge will pay
    // onto the game's 1000-point vehicle health at x4: 36 / 72 / 18 a hit)
    {
        const Vec3 centre(0.5f, 0.5f, 10.6f), fwd(0, 1, 0); // nose north
        const BossVehZone body = BossVehicleZone(Vec3(0.2f, 0.4f, 11.0f), centre, fwd, 9.0f);
        CHECK(Near(body.dmg, 9.0f, 0.01f) && !body.tyre, "a plain body hit pays the base vehicle price (%.2f)", body.dmg);
        const BossVehZone engine = BossVehicleZone(Vec3(0.5f, 1.9f, 11.0f), centre, fwd, 9.0f);
        CHECK(Near(engine.dmg, 18.0f, 0.01f) && !engine.tyre, "an engine-bay hit pays double (%.2f)", engine.dmg);
        const BossVehZone wheel = BossVehicleZone(Vec3(0.9f, 1.9f, 10.2f), centre, fwd, 9.0f);
        CHECK(Near(wheel.dmg, 4.5f, 0.01f) && wheel.tyre,
              "a wheel-band hit pays half and bursts the tyre - even at the nose, the low band wins (%.2f)", wheel.dmg);
        const BossVehZone rear = BossVehicleZone(Vec3(0.5f, -1.4f, 11.0f), centre, fwd, 9.0f);
        CHECK(Near(rear.dmg, 9.0f, 0.01f) && !rear.tyre, "the rear is bodywork, not an engine (%.2f)", rear.dmg);
        const BossVehZone usp = BossVehicleZone(Vec3(0.2f, 0.4f, 11.0f), centre, fwd, 9.0f * (6.18f / 6.0f));
        CHECK(Near(usp.dmg, 9.27f, 0.01f), "the USP's vehicle price scales with the pistol - at its true damage now, a hair over the rifle's (%.2f)", usp.dmg);
    }
    // ---- the shot resolution, end to end: a scripted trace (the geometry is
    // the bridge's; the DECISION is the core's, and it is what is pinned here)
    {
        // a car standing in his line shields the man behind it, at wheel height
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none); // the fight is on
        RunMobsB(0.6f, Vec3(0.5f, 20.5f, 11.0f), none);
        const float hurt0 = host.playerHurt;
        host.vehBoxOn = true;
        host.vehBoxCentre = Vec3(0.5f, 8.5f, 10.4f);
        host.vehBoxFwd = Vec3(0, 1, 0);
        host.traceScriptOn = true;
        host.traceScript = HostHit();
        host.traceScript.hit = true;
        host.traceScript.dist = 3.0f;
        host.traceScript.vehicle = host.vehBoxNum;
        host.traceScript.point = Vec3(0.5f, 8.5f, 10.1f); // below the centre line: the wheel band
        host.vehHurt.clear();
        host.vehTyrePops.clear();
        RunMobsB(6.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        CHECK(host.playerHurt == hurt0, "a vehicle in the line takes every round - the man behind it is untouched");
        const float paid = host.vehHurt[host.vehBoxNum];
        CHECK(paid > 0.0f && Near(std::fmod(paid, 4.5f), 0.0f, 0.01f),
              "and every shielded round paid the wheel price of 4.5 (total %.1f)", paid);
        CHECK(host.vehTyrePops[host.vehBoxNum] >= 1, "wheel-band hits burst tyres through the bridge effect (%d)",
              host.vehTyrePops[host.vehBoxNum]);
        host.traceScriptOn = false;
        host.vehBoxOn = false;
    }
    {
        // the same car, struck on the engine bay: double price, no bursts
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 20.5f, 11.0f), none);
        host.vehBoxOn = true;
        host.vehBoxCentre = Vec3(0.5f, 8.5f, 10.4f);
        host.vehBoxFwd = Vec3(0, -1, 0); // the nose points at him: the front face is the engine bay
        host.traceScriptOn = true;
        host.traceScript = HostHit();
        host.traceScript.hit = true;
        host.traceScript.dist = 3.0f;
        host.traceScript.vehicle = host.vehBoxNum;
        host.traceScript.point = Vec3(0.5f, 7.0f, 11.0f); // on the nose, above the wheel band
        host.vehHurt.clear();
        host.vehTyrePops.clear();
        RunMobsB(6.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        const float paid = host.vehHurt[host.vehBoxNum];
        CHECK(paid > 0.0f && Near(std::fmod(paid, 18.0f), 0.0f, 0.05f),
              "engine-bay rounds pay 18 each through the whole shot path (total %.1f)", paid);
        CHECK(host.vehTyrePops[host.vehBoxNum] == 0, "and no tyre bursts from the engine bay");
        host.traceScriptOn = false;
        host.vehBoxOn = false;
    }
    {
        // a wall on the line: the round dies there, whatever his point tests say
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 20.5f, 11.0f), none);
        const float hurt0 = host.playerHurt;
        host.traceScriptOn = true;
        host.traceScript = HostHit();
        host.traceScript.hit = true;
        host.traceScript.dist = 3.0f;
        host.traceScript.point = Vec3(0.5f, 8.5f, 11.0f); // a wall, no vehicle, nobody on its surface
        host.vehHurt.clear();
        RunMobsB(6.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        CHECK(host.playerHurt == hurt0 && host.vehHurt.empty(),
              "a traced wall stops his rounds cold - no damage leaks through it");
        host.traceScriptOn = false;
    }
    // ---- the A* budget: a hard wall on cost, respected to the node
    {
        CHECK(LoadNavGraphFromRepo(), "the real city graph loads for the budget proofs");
        std::vector<Vec3> route;
        CHECK(!NavRouteBudget(Vec3(2480, -1660, 13), Vec3(1240, -1330, 13), &route, 100),
              "a 100-node budget cannot cross the city, and says so");
        CHECK(NavLastRouteExpanded() <= 100, "the search stopped at the budget, not past it (%d)", NavLastRouteExpanded());
        CHECK(NavRouteBudget(Vec3(2480, -1660, 13), Vec3(2030, -1410, 13), &route, 6000),
              "Grove to County General routes inside the combat budget");
        CHECK(NavLastRouteExpanded() > 100 && NavLastRouteExpanded() <= 6000,
              "it expanded %d nodes doing it", NavLastRouteExpanded());
        CHECK(NavRouteBudget(Vec3(2480, -1660, 13), Vec3(1240, -1330, 13), &route, 6000),
              "and Grove to All Saints - the longest measured chase route - fits the budget too (%d expanded)",
              NavLastRouteExpanded());
        UnloadNavGraph();
    }
    // ---- no idling mid-combat, and a reload is walked, never stood for
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 30.5f, 11.0f), none);
        float idleRun = 0.0f, idleWorst = 0.0f;
        for (int i = 0; i < 240; ++i) { // 12 s against a man standing his ground at 30 m
            MobsTick(0.05f, Vec3(0.5f, 30.5f, 11.0f), 0, true, none);
            if (gMobs[s].bossState == 2) {
                const bool inactive = !gMobs[s].moving && gMobs[s].burstShots == 0 && gMobs[s].reloadT <= 0.0f &&
                                      gMobs[s].firePlantT <= 0.0f && gMobs[s].dodgeT <= 0.0f;
                idleRun = inactive ? idleRun + 0.05f : 0.0f;
                idleWorst = std::max(idleWorst, idleRun);
            }
        }
        CHECK(idleWorst < 1.6f, "in an open fight he is never simply standing there (worst idle %.2f s)", idleWorst);
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 30.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 30.5f, 11.0f), none); // engaged at 30 m
        gMobs[s].mag = 0;
        gMobs[s].burstShots = 0;
        gMobs[s].burstPause = 0.1f;
        bool sawReload = false, refilled = false;
        Vec3 reloadStart;
        float reloadTravel = 0.0f;
        Vec3 prev = gMobs[s].pos;
        for (int i = 0; i < 160 && !refilled; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 30.5f, 11.0f), 0, true, none);
            if (gMobs[s].reloadT > 0.0f) {
                if (!sawReload) {
                    sawReload = true;
                    reloadStart = gMobs[s].pos;
                }
                reloadTravel = (gMobs[s].pos - reloadStart).Length();
            } else if (sawReload && gMobs[s].mag == 30) {
                refilled = true;
            }
            prev = gMobs[s].pos;
        }
        (void)prev;
        CHECK(sawReload && refilled, "the empty rifle is reloaded at range (not the pistol - that is for arm's length)");
        CHECK(reloadTravel > 3.0f, "and he walks the reload - %.1f m covered while the magazine was out", reloadTravel);
    }
    // ---- the graph sims: a two-row street of nodes, a wall across the middle
    auto buildGraph = [&]() {
        const Vec3 nodes[9] = { Vec3(0, 0, 10), Vec3(10, 0, 10), Vec3(20, 0, 10), Vec3(30, 0, 10),
                                Vec3(0, 20, 10), Vec3(10, 20, 10), Vec3(20, 20, 10), Vec3(30, 20, 10),
                                Vec3(0, -14, 10) };
        const int edges[][2] = { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 4, 5 }, { 5, 6 },
                                 { 6, 7 }, { 0, 4 }, { 3, 7 }, { 0, 8 } };
        std::vector<std::vector<uint32_t>> adj(9);
        for (auto& e : edges) {
            adj[e[0]].push_back((uint32_t)e[1]);
            adj[e[1]].push_back((uint32_t)e[0]);
        }
        std::vector<unsigned char> g;
        auto put = [&](const void* d, size_t n) { const unsigned char* c = (const unsigned char*)d; g.insert(g.end(), c, c + n); };
        auto u32 = [&](uint32_t v) { put(&v, 4); };
        auto f32 = [&](float v) { put(&v, 4); };
        put("MCNAV1", 6);
        u32(9);
        uint32_t total = 0;
        for (auto& a : adj)
            total += (uint32_t)a.size();
        u32(total);
        for (auto& n : nodes) { f32(n.x); f32(n.y); f32(n.z); }
        uint32_t off = 0;
        u32(0);
        for (auto& a : adj) { off += (uint32_t)a.size(); u32(off); }
        for (auto& a : adj)
            for (uint32_t t : a)
                u32(t);
        uint32_t sum = 0;
        for (unsigned char c : g)
            sum += c;
        u32(sum);
        return LoadNavGraph(g.data(), g.size());
    };
    auto extendFloor = [&]() {
        for (int x = -12; x <= 36; ++x)
            for (int y = 15; y <= 26; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
    };
    // ---- the chase: a fleeing man, a wall between the streets, contact -
    // around a graph column, with no stall and a bounded count of repaths
    {
        WideStage();
        extendFloor();
        CHECK(buildGraph(), "the two-street graph loads");
        host.wall = true;
        host.wallC = Vec3(15, 10, 10);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 10.0f; // the wall spans x 5..25: its ends are NOT the columns
        const int s = SpawnMob(MOB_SOLDIER, Vec3(15.0f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(20.0f, 4.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(20.0f, 4.5f, 11.0f), none); // south of the wall: the fight is on
        CHECK(gMobs[s].bossState == 2, "the chase starts as a fight (state %d)", gMobs[s].bossState);
        // the man breaks away to the north street and runs west, shooting as he goes
        float px = 25.0f;
        host.velocity = Vec3(-3.5f, 0, 0);
        NotePlayerShot(Vec3(px, 20.0f, 11.0f), 70.0f);
        const float hurtPhase2 = host.playerHurt;
        const int attempts0 = NavRouteAttempts();
        float simT = 0.0f, stimT = 0.0f, stallRun = 0.0f, stallWorst = 0.0f;
        float crossX = 1e9f, caughtT = -1.0f;
        Vec3 prevPos = gMobs[s].pos;
        for (int i = 0; i < 1300; ++i) {
            const float dt = 0.05f;
            simT += dt;
            stimT += dt;
            if (px > -8.0f)
                px -= 3.5f * dt;
            else
                host.velocity = Vec3();
            if (stimT > 2.0f) {
                stimT = 0.0f;
                NotePlayerShot(Vec3(px, 20.0f, 11.0f), 70.0f);
            }
            MobsTick(dt, Vec3(px, 20.0f, 11.0f), 0, true, none);
            const Vec3 p = gMobs[s].pos;
            if (crossX > 1e8f && p.y > 10.0f)
                crossX = p.x; // where he crossed the wall's line says HOW he got north
            const float speed = (p - prevPos).Length() / dt;
            prevPos = p;
            const float dFlat = std::sqrt((p.x - px) * (p.x - px) + (p.y - 20.0f) * (p.y - 20.0f));
            const bool deliberate = gMobs[s].burstShots > 0 || gMobs[s].firePlantT > 0.0f || gMobs[s].reloadT > 0.0f;
            if ((gMobs[s].bossState == 2 || gMobs[s].bossState == 3) && dFlat > 4.0f && speed < 0.4f && !deliberate) {
                stallRun += dt;
                stallWorst = std::max(stallWorst, stallRun);
            } else {
                stallRun = 0.0f;
            }
            // contact, for a rifleman: he has run the fleeing man down into his
            // own fighting band and is holding him there in plain sight - the
            // rest of the loop just confirms he never lets the range open again
            if (caughtT < 0.0f && dFlat < 13.5f && gMobs[s].lostT < 0.05f)
                caughtT = simT;
            if (caughtT > 0.0f && simT > caughtT + 10.0f)
                break; // ten seconds of held contact is the proof; do not run the clock out
        }
        const int attempts = NavRouteAttempts() - attempts0;
        CHECK(caughtT > 0.0f, "the fleeing man is run down to fighting distance across the wall's street (caught at %.1f s)",
              caughtT);
        CHECK(host.playerHurt > hurtPhase2 + 1.0f,
              "and the man he ran down is being hit, not just followed (%.1f half hearts)", host.playerHurt - hurtPhase2);
        CHECK(crossX < 2.5f || crossX > 27.5f,
              "he crossed the wall's line at a graph column (x %.1f) - routed around, not walked through", crossX);
        CHECK(stallWorst < 1.7f, "no stall outlasts the repath budget (worst %.2f s)", stallWorst);
        CHECK(attempts <= (int)(simT * 1.6f) + 6,
              "repaths stayed throttled through a zigzag chase (%d in %.0f s)", attempts, simT);
        host.wall = false;
        host.velocity = Vec3();
    }
    // ---- the repath throttle under a direction-changing target: the man
    // reverses every second; the navigator may re-aim, but it may not
    // re-plan faster than its own throttle, and no single plan may expand
    // past the hard budget
    if (NavReady()) {
        WideStage();
        extendFloor();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(20.0f, 4.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        float px = 15.0f;
        host.velocity = Vec3(3.5f, 0, 0);
        ProvokeByAim(host, Vec3(px, 20.0f, 11.0f), none);
        RunMobsB(0.6f, Vec3(px, 20.0f, 11.0f), none);
        NotePlayerShot(Vec3(px, 20.0f, 11.0f), 70.0f);
        const int attempts0 = NavRouteAttempts();
        int maxExpanded = 0;
        float flipT = 0.0f, stimT = 0.0f, dirSign = 1.0f;
        for (int i = 0; i < 400; ++i) { // 20 s of zigzag
            const float dt = 0.05f;
            flipT += dt;
            stimT += dt;
            if (flipT >= 1.0f) {
                flipT = 0.0f;
                dirSign = -dirSign;
                host.velocity = Vec3(3.5f * dirSign, 0, 0);
            }
            px += 3.5f * dirSign * dt;
            if (px > 28.0f) px = 28.0f;
            if (px < 2.0f) px = 2.0f;
            if (stimT > 2.0f) {
                stimT = 0.0f;
                NotePlayerShot(Vec3(px, 20.0f, 11.0f), 70.0f);
            }
            MobsTick(dt, Vec3(px, 20.0f, 11.0f), 0, true, none);
            maxExpanded = std::max(maxExpanded, NavLastRouteExpanded());
        }
        const int attempts = NavRouteAttempts() - attempts0;
        CHECK(attempts <= (int)(20.0f * 1.6f) + 6,
              "a target reversing every second cannot stampede the planner (%d repaths in 20 s)", attempts);
        CHECK(maxExpanded <= 6000, "no repath ever expanded past the hard budget (worst %d)", maxExpanded);
        CHECK(gMobs[s].bossState == 2 || gMobs[s].bossState == 3,
              "and he is still on the man after 20 s of zigzag (state %d)", gMobs[s].bossState);
        host.velocity = Vec3();
    }
    // ---- the retreat: a wounded man falls back ONTO a graph node behind him
    {
        WideStage();
        extendFloor();
        CHECK(NavReady(), "the graph is still loaded for the retreat");
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 24.5f, 11.0f), none);
        RunMobsB(0.4f, Vec3(0.5f, 24.5f, 11.0f), none); // seen far north: lastSeen is the north street
        gMobs[s].health = 1200.0f; // under the wounded line of the deeper pool
        gMobs[s].hurt = 0.0f;
        host.wall = true;
        host.wallC = Vec3(0.5f, 14, 10);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 7.0f; // the man is behind it now, unreachable
        bool snapped = false;
        float bestD = 1e9f, maxY = -1e9f;
        // the retreat is measured from where it begins: the law's ignition
        // is 1.1 s of live combat first, and a fighting man closes distance
        // in it - he starts this fall-back already north of his post
        const float retreatStartY = gMobs[s].pos.y;
        for (int i = 0; i < 140; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 24.5f, 11.0f), 0, true, none);
            if ((gMobs[s].chaseDest - Vec3(0, -14, 10)).Length() < 0.6f)
                snapped = true;
            bestD = std::min(bestD, (gMobs[s].pos - Vec3(0, -14, 10)).Length());
            maxY = std::max(maxY, gMobs[s].pos.y);
        }
        CHECK(snapped, "the fallback he retreats to is the graph node behind him, not a blind backward line");
        CHECK(bestD < 2.0f, "and he arrives on it (closest %.1f m)", bestD);
        // his prime momentum may slide him a couple of metres north on the
        // first second; what must never happen is a deliberate walk back
        // toward the threat (the old hunt flip carried him 13 m north)
        CHECK(maxY < retreatStartY + 2.0f,
              "he never walks back toward the threat while falling back (max y %.1f from %.1f)", maxY, retreatStartY);
        host.wall = false;
        UnloadNavGraph();
    }
    // ---- the relentless hunt: clues renew it past its own clock; a cold
    // trail still ends it - he is relentless, not undead
    {
        WideStage();
        extendFloor(); // the hunt walks the north street too: it needs its floor
        // ...and its search legs reach a full block past the street in every
        // direction - they need theirs too, or the hunter walks off the world.
        // (The fix round's exit-line search reaches ~55 m past the last seen
        // point, so the floor now runs to y=130: in the real city there is
        // ground under every street he sweeps - a test floor that ends at 48
        // dropped him through the world mid-sweep, a fixture's lie, not his.)
        for (int x = -40; x < 40; ++x)
            for (int y = 15; y < 130; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none); // the law's ignition
        RunMobsB(0.6f, Vec3(0.5f, 20.5f, 11.0f), none); // the fight is on
        host.wall = true;
        host.wallC = Vec3(0.5f, 10.5f, 10);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        int stateAt50 = -1;
        float stimT = 0.0f;
        for (int i = 0; i < 1100; ++i) { // 55 s of a hidden man who keeps shooting
            const float dt = 0.05f;
            stimT += dt;
            if (stimT > 3.0f) {
                stimT = 0.0f;
                NotePlayerShot(Vec3(0.5f, 20.5f, 11.0f), 70.0f);
            }
            MobsTick(dt, Vec3(0.5f, 20.5f, 10.2f), 0, true, none);
            if (i == 1000)
                stateAt50 = gMobs[s].bossState;
        }
        CHECK(stateAt50 == 2 || stateAt50 == 3,
              "past the hunt's own 45 s he is still on the man while the shots keep coming (state %d)", stateAt50);
        // LAW UPDATE (the tracker law): the second half of this block used
        // to pin the cold trail's death - stand-down to suspicion, then
        // calm, and a boss who never marches off. That death is exactly
        // what the tracker law abolished: the man is not "gone", he is 200 m
        // north, alive, and the feed knows it. The floor is extended under
        // his road first (the march needs ground, like any street), then:
        // no stand-down state ever appears once the hunt's own clues age
        // out, and he ends the window ON the man's ground - he marched off
        // after him because he IS there.
        for (int x = -40; x < 40; ++x)
            for (int y = 130; y < 230; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
        bool stoodDown = false;
        for (int i = 0; i < 4000; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 220.5f, 11.0f), 0, true, none);
            if (i > 200 && (gMobs[s].bossState == 0 || gMobs[s].bossState == 4))
                stoodDown = true; // past the clues' age there is no calm left in him
        }
        CHECK(!stoodDown, "under the tracker law the trail cannot die while the player lives: no stand-down, ever");
        const float dMan = (gMobs[s].pos - Vec3(0.5f, 220.5f, 10.0f)).Length();
        CHECK((gMobs[s].bossState == 2 || gMobs[s].bossState == 3 || gMobs[s].bossState == 5) && dMan < 60.0f && gMobs[s].pos.y > 150.0f,
              "and he marched the 200 m and arrived on the man's ground (state %d, %.0f m short, y %.0f)",
              gMobs[s].bossState, dMan, gMobs[s].pos.y);
        host.wall = false;
    }
}

static int InspectWorldFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    WorldFileInfo info;
    const bool ours = ReadWorldFile(f, info);
    fclose(f);
    int items = 0;
    for (const ItemStack& st : gInv.slots)
        items += st.count;
    printf("%s: %s, version %u, blocks %s, host part %s, %u chunks, %d items, food %.1f, level %d\n", path,
           ours ? "a world file" : "NOT a world file", info.version, info.blocksOk ? "ok" : "INCOMPLETE",
           info.hostPart ? "follows" : "none", (unsigned)gWorld.chunks.size(), items, gSurvival.food, gSurvival.xpLevel);
    return ours && info.blocksOk ? 0 : 1;
}

// ================================================================ the fix round of 2026-11-07
// The eye verdict on the installed smart pack + rulebook build, two counts:
// (1) "he still has dumb in him - he saw me, he fired, and then he walked
// away from me": a live fight broke at range, the prey ran quiet past
// gunshot-hearing, the hunt's senses-only stand-down called the trail cold
// seconds later, and a primed patrol walked him off at walk speed - wearing
// combat's last crouch in his torso the whole way, because crouch was
// written in combat and cleared by nobody.
// (2) "when I get into a car he loses me, as if I turned invisible": the
// senses priced the target's own car as a wall on the line to his seat.
// These sims reproduce both shapes and pin the fixes.
static void LongStage() {
    BlockStage();
    for (int x = -15; x <= 15; ++x)
        for (int y = -15; y <= 175; ++y)
            gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
    MobsClear();
    ResetSurvival();
    gGame = GameState();
    gRules = GameRules();
    gPlayed.clear();
}

static void TestBossFixRound() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // ---- the warm trail: a fight breaks at range, the prey runs silent
    // past hearing, and the hunt must follow his exit line - never decay
    // into a patrol, and never travel crouched
    {
        LongStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 35.5f, 11.0f), none); // the law's ignition
        bool sawCrouchInCombat = false;
        for (int i = 0; i < 80; ++i) { // 4 s of open fight at 35 m
            MobsTick(0.05f, Vec3(0.5f, 35.5f, 11.0f), 0, true, none);
            if (gMobs[s].crouch && gMobs[s].bossState == 2)
                sawCrouchInCombat = true;
        }
        CHECK(gMobs[s].bossState == 2 && host.playerHurt > 0.0f,
              "the warm-trail sim opens with a real fight (state %d, hurt %.1f)", gMobs[s].bossState, host.playerHurt);
        CHECK(gMobs[s].fightWarmT > 30.0f, "and the exchange has stamped the trail warm (%.0f s left)",
              gMobs[s].fightWarmT);
        CHECK(sawCrouchInCombat, "and he really crouched in that fight (the travel ban below has teeth)");
        // the break: north, behind a wall at y=44, on to y=120 - hidden,
        // silent, and well past the 70 m his ears give a gunshot
        host.wall = true;
        host.wallC = Vec3(0.5f, 44.0f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        float py = 35.5f;
        host.velocity = Vec3(0, 5.5f, 0);
        bool stoodDownWarm = false, crouchTravel = false;
        float maxY = gMobs[s].pos.y;
        for (int i = 0; i < 3000; ++i) { // 150 s: the sweep has streets to walk
            const float dt = 0.05f;
            if (py < 120.0f)
                py = std::min(120.0f, py + 5.5f * dt);
            else
                host.velocity = Vec3();
            MobsTick(dt, Vec3(0.5f, py, 11.0f), 0, true, none);
            const Mob& b = gMobs[s];
            if (b.fightWarmT > 0.0f && (b.bossState == 4 || b.bossState == 0))
                stoodDownWarm = true;
            if (b.crouch && b.bossState != 2)
                crouchTravel = true;
            maxY = std::max(maxY, b.pos.y);
        }
        CHECK(!stoodDownWarm, "while the trail is warm he never decays into patrol or calm");
        CHECK(!crouchTravel, "and he never travels crouched - crouch is a shooting stance, not a gait");
        CHECK(maxY > 90.0f, "the hunt sweeps the prey's exit line, not a disc around the stale spot (reached y %.0f)",
              maxY);
        CHECK(gMobs[s].bossState == 2 || gMobs[s].bossState == 3,
              "and at the end he is still on the man - fighting or hunting (state %d)", gMobs[s].bossState);
        host.wall = false;
        host.velocity = Vec3();
    }
    // ---- the driver: getting into a car must not break the lock, and
    // getting out must not either - his senses read the driver through his
    // own glass, not against his own bodywork
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none); // the law's ignition
        RunMobsB(2.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        CHECK(gMobs[s].bossState == 2 && host.playerHurt > 0.0f,
              "the driver sim opens with a fight on foot (state %d)", gMobs[s].bossState);
        host.vehicle = TestHost::kCarNumber;
        host.vehPoseOn = true;
        host.vehCentre = Vec3(0.5f, 20.5f, 10.6f);
        host.vehSeat = Vec3(0.5f, 20.5f, 11.35f);
        host.vehFwd = Vec3(0, 1, 0);
        host.vehVel = Vec3();
        host.player = Vec3(0.5f, 20.5f, 11.35f);
        host.ownCarBlocks = true; // his own bodywork now stands on every plain line to his seat
        const float hurtIn = host.playerHurt;
        bool lockHeld = true;
        float worstLost = 0.0f;
        for (int i = 0; i < 120; ++i) { // 6 s seated
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.35f), 0, true, none);
            if (gMobs[s].bossState != 2)
                lockHeld = false;
            worstLost = std::max(worstLost, gMobs[s].lostT);
        }
        CHECK(lockHeld && worstLost < 0.6f,
              "seated, he is still seen: the lock never breaks (worst lost %.2f s)", worstLost);
        CHECK(host.playerHurt > hurtIn + 1.0f,
              "and the fire continues at the driver through his glass (hurt %.1f -> %.1f)", hurtIn, host.playerHurt);
        host.vehicle = -1;
        host.vehPoseOn = false;
        host.ownCarBlocks = false;
        host.player = Vec3(0.5f, 20.5f, 11.0f);
        const float hurtOut = host.playerHurt;
        bool lockHeld2 = true;
        for (int i = 0; i < 60; ++i) { // 3 s back on foot
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.0f), 0, true, none);
            if (gMobs[s].bossState != 2)
                lockHeld2 = false;
        }
        CHECK(lockHeld2 && host.playerHurt > hurtOut,
              "and stepping out changes nothing: still locked, still firing (hurt %.1f -> %.1f)",
              hurtOut, host.playerHurt);
    }
    // ---- the ram: priced in the core, answered at once - and a DODGED
    // attempt by the player's car still makes the driver his enemy, while
    // the street's own traffic earns only the dodge
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        MobVehicleHit(s, 25.0f, Vec3(0.5f, -3.5f, 10.0f), true);
        CHECK(gMobs[s].aggro && gMobs[s].bossState == 2 && gMobs[s].ramT > 0.5f && gMobs[s].detect >= 1.0f,
              "rammed by the player: full detection, a visible knockdown, war at once");
        CHECK(Near(gMobs[s].health, 3300.0f, 2.0f), "and the ram's health price is the capped share (%.0f)",
              gMobs[s].health);
        WideStage();
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s2, 0.0f);
        RunMobsB(0.2f, Vec3(90.5f, 0.5f, 11.0f), none); // settle: a dodge needs his feet on the ground
        // (under the tracker law he is already marching on the far player
        // when the threat lands - state TRACK - and the threat's promotion
        // covers TRACK too: the attempt makes the enemy, from any peace)
        MobVehicleThreat(s2, Vec3(0.5f, -8.5f, 10.0f), Vec3(0, 12.0f, 0), true);
        CHECK(gMobs[s2].aggro && gMobs[s2].bossState == 2 && gMobs[s2].dodgeT > 0.0f,
              "a car driven straight at him: he dodges - and the driver is his enemy anyway. A missed ram is still a ram");
        WideStage();
        const int s3 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s3, 0.0f);
        RunMobsB(0.2f, Vec3(90.5f, 0.5f, 11.0f), none); // settle, as above
        MobVehicleThreat(s3, Vec3(0.5f, -8.5f, 10.0f), Vec3(0, 12.0f, 0), false);
        CHECK(!gMobs[s3].aggro && gMobs[s3].dodgeT > 0.0f,
              "the street's own traffic earns the dodge and nothing more");
    }
    // ---- damage he can feel: the channels the bridge drives, the feedback
    // scaled by the blow, and the kill cost in the player's own terms
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        int bullets = 0; // a rifle-class hit on his perception body, relayed
        while (gMobs[s].death < 0.0f && bullets < 400) {
            MobProxyDamage(s, 60.0f, Vec3(0.5f, 5.5f, 11.0f), -2);
            ++bullets;
        }
        CHECK(gMobs[s].death >= 0.0f && bullets >= 100 && bullets <= 115,
              "he dies - to %d relayed rifle hits, about three and a half magazines of them", bullets);
        WideStage();
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        int punches = 0; // a fist's own weight, through the same door
        while (gMobs[s2].death < 0.0f && punches < 900) {
            MobProxyDamage(s2, 12.0f, Vec3(0.5f, 2.5f, 11.0f), -2);
            ++punches;
        }
        CHECK(gMobs[s2].death >= 0.0f && punches >= 500 && punches <= 570,
              "and he dies to fists too - %d punches of patient work", punches);
        WideStage();
        const int s3 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s3, 0.0f);
        gPlayed.clear();
        MobHurt(s3, 400.0f, Vec3(0.5f, 5.5f, 11.0f), 0.0f);
        CHECK(gMobs[s3].staggerT > 0.2f && gMobs[s3].flinchT >= 0.4f,
              "a heavy hit staggers him: no fire, barely a step, while it runs (stagger %.2f s)", gMobs[s3].staggerT);
        CHECK(Heard(SND_BOSS_HURT) >= 1, "and a hit that size is always heard - no throttle swallows it");
        WideStage();
        const int s4 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        MobHurt(s4, 20.0f, Vec3(0.5f, 5.5f, 11.0f), 0.0f);
        CHECK(gMobs[s4].staggerT == 0.0f, "a light hit earns a flinch, not a stagger");
    }
    // ---- the grenade under CleanSA: a visible arc (as data), a surface
    // detonation through the host, and the CS curve on the core channel
    {
        LongStage();
        gRules.cleanSA = true;
        host.groundMode = 1;     // the bridge's collision ground, scripted
        host.groundZVal = 10.0f; // LongStage's street level
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none);
        host.wall = true; // the target digs in: the flush comes due
        host.wallC = Vec3(0.5f, 10.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        const int gren0 = Heard(SND_BOSS_GREN1);
        bool thrown = false;
        for (int i = 0; i < 400 && !thrown; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.0f), 0, true, none);
            if (!gBossNades.empty())
                thrown = true;
        }
        CHECK(thrown, "under CleanSA a dug-in target is still answered with a grenade");
        CHECK(Heard(SND_BOSS_GREN1) > gren0, "and the throw is called out - his grenade line fires");
        float arcPeak = 0.0f, minZ = 1e9f;
        Vec3 rest;
        bool landed = false;
        const int blasts0 = host.explosions;
        host.playerHurt = 0.0f;
        for (int i = 0; i < 200 && host.explosions == blasts0; ++i) {
            if (!gBossNades.empty()) {
                const BossNade& n = gBossNades[0];
                arcPeak = std::max(arcPeak, n.pos.z);
                minZ = std::min(minZ, n.pos.z);
                if (n.vel.LengthSq() == 0.0f) {
                    landed = true;
                    rest = n.pos;
                }
            }
            const Vec3 pp = landed ? rest + Vec3(1.0f, 0.0f, 1.0f) : Vec3(0.5f, 20.5f, 11.0f);
            MobsTick(0.05f, pp, 0, true, none);
        }
        CHECK(landed && arcPeak > 11.5f, "the flight is a real arc (peak %.1f m)", arcPeak);
        CHECK(minZ >= 9.99f, "and it never sinks under the street (lowest %.2f m)", minZ);
        CHECK(host.explosions > blasts0, "the detonation fires - through the host, on the surface");
        const float expect = CsHeHh(1.41421f, 5.5f, 96.0f, false, 0.0f, nullptr);
        CHECK(Near(host.playerHurt, expect, 0.6f), "and the hurt is the CS curve at the parked distance (%.1f vs %.1f)",
              host.playerHurt, expect);
        host.wall = false;
        host.groundMode = 0;
        gRules.cleanSA = false;
    }
}

// The engagement law, twice over. The morning's law (the user's order,
// 2026-11-07): by default the boss has NOTHING to do with the player -
// three provocations, and only those, make the player his enemy. The
// TRACKER LAW (the user's order, later the same day, picked from the
// options menu) superseded it FOR THE BOSS: he always knows where the
// player is and comes for him from the summon; combat opens on genuine
// sight; the provocations keep their bookkeeping (aggro, the warm slate)
// but no longer gate anything. This test pins the shipped law first,
// then - with the switch flipped off - pins that the morning's law
// returns whole, because the switch is the team step's per-role choice.
static void TestBossAggroLaw() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    {
        WideStage();
        gPlayed.clear();
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        RunMobsB(2.0f, Vec3(0.5f, 8.5f, 11.0f), none);    // walks up
        RunMobsB(3.0f, Vec3(0.5f, 3.5f, 11.0f), none);    // stands at 3 m
        NotePlayerShot(Vec3(0.5f, -30.5f, 11.0f), 70.0f); // shots, away from him
        RunMobsB(2.0f, Vec3(0.5f, 3.5f, 11.0f), none);
        // LAW UPDATE (the tracker law): no provocation was offered and none
        // is recorded - aggro stays false the whole time - yet the fight is
        // on, because he came to the man and his eyes filled the meter.
        // Contact is by sight now; the provocations are bookkeeping.
        CHECK(!gMobs[s].aggro && gMobs[s].bossState == 2 && host.playerHurt > 0.0f,
              "no provocation at all: he fights the calm man on sight alone - and the bookkeeping stays clean (state %d)", gMobs[s].bossState);
        CHECK(Heard(SND_BOSS_CMD1) + Heard(SND_BOSS_CMD2) + Heard(SND_BOSS_CMD3) == 0,
              "and no order is ever shouted at a calm man");
        host.aimingAtHim = true; // a passing sweep of the crosshair
        RunMobsB(0.3f, Vec3(0.5f, 3.5f, 11.0f), none);
        host.aimingAtHim = false;
        RunMobsB(0.5f, Vec3(0.5f, 3.5f, 11.0f), none);
        CHECK(!gMobs[s].aggro, "a 0.3 s sweep across him still does not trip the provocation line of 0.45 s");
        host.aimingAtHim = true; // held aim: the provocation itself
        RunMobsB(0.6f, Vec3(0.5f, 3.5f, 11.0f), none);
        host.aimingAtHim = false;
        CHECK(gMobs[s].aggro && gMobs[s].bossState == 2, "and a weapon held on him still lands in the bookkeeping: the player is his enemy");
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        MobProxyDamage(s, 12.0f, Vec3(0.5f, 2.5f, 11.0f), -2); // one fist, through his perception body
        CHECK(gMobs[s].aggro && gMobs[s].bossState == 2, "and one punch is enough: damage dealt is an enemy made");
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 12.5f, 11.0f), none);
        CHECK(gMobs[s].aggro, "provoked, he is an enemy");
        gGame.deathTime = 0.5f; // the player dies
        RunMobsB(0.2f, Vec3(0.5f, 12.5f, 11.0f), none);
        CHECK(!gMobs[s].aggro && gMobs[s].bossState == 0,
              "the death itself still zeroes everything: slate clean, chase over, he stands down");
        gGame.deathTime = -1.0f; // and respawns
        RunMobsB(1.0f, Vec3(0.5f, 12.5f, 11.0f), none);
        // LAW UPDATE (the tracker law): the slate stays clean - but he is
        // NOT a stranger. The tracker re-acquired the moment the man
        // breathed again; at 12 m in the open the pursuit completes inside
        // the window, so he is tracking (5) or already fighting (2).
        CHECK(!gMobs[s].aggro && (gMobs[s].bossState == 5 || gMobs[s].bossState == 2),
              "respawned, the slate is still clean - but the tracker has him again: he is already coming (state %d)", gMobs[s].bossState);
        gGame.deathTime = -1.0f;
    }
    {
        // THE FLIP-OFF PIN: with the tracker switch off, the morning's
        // provocation law returns whole - a calm man at arm's length is
        // scenery again: no enemy, no engagement, no shot, empty meter.
        gTrackerAlways = false;
        WideStage();
        gPlayed.clear();
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        RunMobsB(2.0f, Vec3(0.5f, 8.5f, 11.0f), none);
        RunMobsB(3.0f, Vec3(0.5f, 3.5f, 11.0f), none);
        CHECK(!gMobs[s].aggro && gMobs[s].bossState == 0 && host.playerHurt == 0.0f && gMobs[s].detect == 0.0f,
              "tracker off: the provocation law returns whole - a calm player is scenery again");
        gTrackerAlways = true;
        CHECK(gTrackerAlways, "and the switch is restored for everything after");
    }
}

// =====================================================================
// THE STATE-CHURN FIX (the second eye verdict, 2026-11-07): the session
// log's own transition lines convicted the shipped brain - combat flipped
// to hunt after 1.2 s without line of sight, at every range down to
// thirteen metres, and flipped back seconds later, for minutes: two
// brains swapping several times a minute is the dithering the eye saw,
// and the hunt's search destinations are the "he walked off somewhere
// else". Separately, a wounded fallback could be dragged across the
// block by its own nav snap and read as a retreat order. The laws now:
// combat holds a 5 s grace of zero sensory contact, pressing the
// last-seen point; a hunt inside 28 m does not exist; the fallback is a
// bound of at most 15 m, and only under the wounded line.
static void TestBossStateChurn() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    host.velocity = Vec3();

    // ---- (a) two-second gaps in the line, over and over: the fight never
    // leaves combat, and while he is blind his steering destination is the
    // last-seen vicinity - he presses the corner, he does not wander off
    // into a hunt's search pattern
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 20.5f, 11.0f);
        ProvokeByAim(host, man, none);
        RunMobsB(1.5f, man, none); // a real fight, at fighting distance
        CHECK(gMobs[s].bossState == 2 && host.playerHurt > 0.0f,
              "the churn sim opens with a real fight (state %d)", gMobs[s].bossState);
        host.wallC = Vec3(0.5f, 10.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        bool leftCombat = false;
        float worstDest = 0.0f;
        for (int cyc = 0; cyc < 6; ++cyc) {
            host.wall = false;
            for (int i = 0; i < 40; ++i) { // 2 s in the open
                MobsTick(0.05f, man, 0, true, none);
                if (gMobs[s].bossState != 2)
                    leftCombat = true;
            }
            host.wall = true; // the line breaks - behind the wall's band
            for (int i = 0; i < 40; ++i) { // a 2 s gap, the log's own dose
                MobsTick(0.05f, man, 0, true, none);
                const Mob& b = gMobs[s];
                if (b.bossState != 2)
                    leftCombat = true;
                if (i >= 2) { // the press owns his steering while he is blind
                    Vec3 d = b.chaseDest - Vec3(0.5f, 20.5f, 10.0f);
                    d.z = 0.0f;
                    worstDest = std::max(worstDest, d.Length());
                }
            }
        }
        host.wall = false;
        CHECK(!leftCombat, "six 2 s gaps in the line and he never leaves combat - no churn");
        CHECK(worstDest < 5.0f,
              "and while blind his destination stays the last-seen vicinity (worst %.1f m off it)", worstDest);
    }
    // ---- (b) the hunt's entry discipline: at close range even a long,
    // silent blindness never becomes a hunt (the distance gate, not the
    // clock, is what holds him); far away, the successor - the hunt in
    // its day, the tracker feed since the tenacity leash (2026-11-07) -
    // engages only after the full grace has run out in silence
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none);
        RunMobsB(1.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        host.wall = true;
        host.wallC = Vec3(0.5f, 10.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 30.0f; // a long wall: he cannot round it inside the window
        bool hunted = false;
        float peakLost = 0.0f;
        for (int i = 0; i < 200; ++i) { // 10 s blind at ~20 m, silent
            MobsTick(0.05f, Vec3(0.5f, 20.5f, 11.0f), 0, true, none);
            if (gMobs[s].bossState == 3)
                hunted = true;
            peakLost = std::max(peakLost, gMobs[s].lostT);
        }
        host.wall = false;
        CHECK(peakLost > 5.5f, "the blindness really outlasted the grace (peak lost %.1f s)", peakLost);
        CHECK(!hunted && gMobs[s].bossState == 2,
              "and still no hunt: at twenty metres losing sight is a combat problem (state %d)", gMobs[s].bossState);
    }
    {
        WideStage();
        for (int x = -15; x <= 15; ++x) // the far street has a floor too
            for (int y = 15; y <= 175; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 20.5f, 11.0f), none);
        RunMobsB(1.0f, Vec3(0.5f, 20.5f, 11.0f), none);
        // LAW UPDATE (the tenacity leash, the user's amendment of
        // 2026-11-07): this block used to pin the hunt taking the far
        // trail. A trail 80 m cold is not the hunt's any more - past the
        // leash the hunt hands the chase back to the tracker feed the
        // tick it forms, because a search worked on stale ground is a
        // pause the feed never ordered. What survives of the entry
        // discipline, and is pinned here: the grace is still combat's
        // (no successor state before it runs out), the feed takes the
        // trail inside the same window the hunt used to, and the road
        // actually closes - the pause is gone, not renamed.
        float trackAt = -1.0f;
        bool earlyExit = false;
        const Vec3 farMan(0.5f, 100.5f, 11.0f);
        auto flatFar = [&]() {
            Vec3 d = gMobs[s].pos - farMan;
            d.z = 0.0f;
            return d.Length();
        };
        const float dStart = flatFar();
        for (int i = 0; i < 240; ++i) { // 12 s: the man is simply gone, 80 m off
            const float t = (float)i * 0.05f;
            MobsTick(0.05f, farMan, 0, true, none);
            if (gMobs[s].bossState == 5 && trackAt < 0.0f)
                trackAt = t;
            if (gMobs[s].bossState != 2 && t < 4.8f)
                earlyExit = true;
        }
        CHECK(!earlyExit, "no successor before the grace runs out - the first seconds are combat's");
        CHECK(trackAt > 4.8f && trackAt < 9.0f,
              "and then the feed takes the genuinely cold, far trail (at %.1f s)", trackAt);
        CHECK(flatFar() < dStart - 15.0f,
              "and the trail is walked, not contemplated (%.0f -> %.0f m)", dStart, flatFar());
    }
    // ---- (c) retreat discipline: at 75% health no fallback exists at all;
    // below the wounded line the fallback is one short bound, never a walk
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 24.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 24.5f, 11.0f), none);
        gMobs[s].health = 3000.0f; // 75%: hurt, winning, nowhere near the wounded line
        gMobs[s].hurt = 0.0f;
        host.wall = true;
        host.wallC = Vec3(0.5f, 14.0f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        bool anyFallback = false;
        for (int i = 0; i < 200; ++i) { // 10 s blind at 75%
            MobsTick(0.05f, Vec3(0.5f, 24.5f, 11.0f), 0, true, none);
            if (gMobs[s].hasFallback)
                anyFallback = true;
        }
        host.wall = false;
        CHECK(!anyFallback, "at 75% health there is no retreat order to give - no fallback exists");
        CHECK(gMobs[s].bossState == 2, "and the fight is still a fight (state %d)", gMobs[s].bossState);
    }
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        ProvokeByAim(host, Vec3(0.5f, 24.5f, 11.0f), none);
        RunMobsB(0.6f, Vec3(0.5f, 24.5f, 11.0f), none);
        gMobs[s].health = 1200.0f; // 30%: under the wounded line - the fallback is legal
        gMobs[s].hurt = 0.0f;
        host.wall = true;
        host.wallC = Vec3(0.5f, 14.0f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 9.0f;
        bool chosen = false;
        float boundLen = 0.0f;
        Vec3 boundPt;
        for (int i = 0; i < 100 && !chosen; ++i) {
            MobsTick(0.05f, Vec3(0.5f, 24.5f, 11.0f), 0, true, none);
            if (gMobs[s].hasFallback) {
                chosen = true;
                boundPt = gMobs[s].fallbackPoint;
                Vec3 off = boundPt - gMobs[s].pos;
                off.z = 0.0f;
                boundLen = off.Length();
            }
        }
        CHECK(chosen, "wounded under the line, a fallback is chosen");
        CHECK(boundLen <= 15.5f, "and it is a short bound to near cover - %.1f m, not a walk away", boundLen);
        float bestD = 1e9f;
        for (int i = 0; i < 240; ++i) { // and he completes the bound, then holds
            MobsTick(0.05f, Vec3(0.5f, 24.5f, 11.0f), 0, true, none);
            Vec3 d = gMobs[s].pos - boundPt;
            d.z = 0.0f;
            bestD = std::min(bestD, d.Length());
        }
        host.wall = false;
        CHECK(bestD < 2.5f, "and he stands on it (closest %.1f m) - the fall-back finishes, short", bestD);
    }
}

// ------------------------------------------------------------------
// The hybrid step (2026-11-07): the game's own ped locomotion walks him,
// this brain commands. The harness has no game, so the test plays the
// bridge's part with a fake body: it reports a position before each tick
// (as MobDriverReport requires) and, after the tick, walks that position
// straight at the issued objective at the objective's speed - the honest
// miniature of feet that pathfind on the street's own nodes. What the sims
// pin is the CONTRACT, not the walking: the entity tracks the reported
// body; under the tracker law an objective engages the player from the
// summon (before it, the aggro law's peacetime contract stood here - the
// flip-off pin in TestBossAggroLaw keeps that law priced); the objective
// refreshes when the tactic changes; and when the body stops, the soldier
// stops - the core never walks him itself underneath the commander.
struct FakeDriverBody {
    Vec3 pos;
    bool frozen = false; // the body reports but does not walk (a wedged body)
    // the native start-up (ramp): the game's seek does not teleport the
    // feet to full pace - a man starting FROM STANDSTILL first turns in
    // place (at zero horizontal speed, the seek's face-the-target
    // phase; the turn he owes is the heading he must change - a full
    // about-face costs 0.7 s, facing the order already costs almost
    // none), then accelerates from a stand over 0.8 s. (A new order
    // while the feet already flow is not a start-up: the bridge moves
    // the anchor and writes the speed class on the living seek.) The
    // instant Walk above could never model any of this, which is
    // exactly the blind spot the statue regression walked through
    // (2026-11-07).
    bool ramp = false;
    bool rampUp = false;   // a start-up is currently spinning up
    float rampT = 0.0f;    // seconds since the feet left standstill
    float rampTurn = 0.0f; // the turn this start-up owes (by heading change)
    float rampSpeed = 0.0f; // the start-up's current pace
    float facing = 0.0f;   // his heading (the codebase's yaw: dir = (-sin, cos))
    // the tracker repair round: a body that falls out of the world. While
    // `falling`, every step drops it 9 m and it stops answering orders;
    // `lieGrounded` selects the liar's variant - the standing flag keeps
    // claiming solid ground while the z plunges (the fall detector's
    // below-ground witness exists for exactly that stream).
    bool falling = false;
    bool lieGrounded = false;
    void Report(int s, float dt) const { MobDriverReport(s, pos, !falling || lieGrounded, dt); }
    void Walk(int s, float dt) {
        if (falling) {
            pos.z -= 9.0f * dt;
            return;
        }
        if (frozen)
            return;
        const Mob& m = gMobs[s];
        if (!m.drvObjValid || m.drvObjSpeed <= 0.01f) {
            rampUp = false; // under a stand order the feet stop: the next
            rampSpeed = 0.0f; // move order starts from standstill again
            return;
        }
        Vec3 to = m.drvObj - pos;
        to.z = 0.0f;
        const float d = to.Length();
        if (d < 0.05f)
            return;
        float pace = m.drvObjSpeed;
        if (ramp) {
            if (!rampUp) {
                rampUp = true;
                rampT = 0.0f;
                rampSpeed = 0.0f;
                const Vec3 want = to * (1.0f / d);
                const Vec3 fwd(-std::sin(facing), std::cos(facing), 0.0f);
                const float dot = std::max(-1.0f, std::min(1.0f, fwd.x * want.x + fwd.y * want.y));
                rampTurn = 0.35f * (1.0f - dot); // 0 facing it, 0.7 s back-to-front
            }
            rampT += dt;
            if (rampT < rampTurn)
                return; // turning to face the order: the feet have not started
            rampSpeed = std::min(m.drvObjSpeed, rampSpeed + m.drvObjSpeed * dt / 0.8f);
            pace = rampSpeed;
        }
        const float step = std::min(d, pace * dt);
        pos = pos + to * (step / d);
        pos.z = m.drvObj.z;
        if (step > 0.0f)
            facing = std::atan2(-to.x, to.y); // walking turns him onto his road
    }
};

static void RunDriven(FakeDriverBody& drv, int s, float seconds, const Vec3& player,
                      const std::vector<BeingInfo>* beings) {
    const float dt = 0.05f;
    for (float t = 0.0f; t < seconds; t += dt) {
        drv.Report(s, dt);
        MobsTick(dt, player, 0, true, beings);
        drv.Walk(s, dt);
    }
}

static void TestBossDriver() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // (b) LAW UPDATE (the tracker law, 2026-11-07): this block used to pin
    // the aggro law's peacetime - no objective engages an unprovoking man.
    // The tracker law inverted it: from the summon the driven body is
    // ordered at the man, at chase pace, and the fight opens by sight
    // alone inside the window. (The peacetime contract survives behind the
    // switch - TestBossAggroLaw's flip-off pin prices it.)
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 25.5f, 11.0f); // 25 m off, in the open, doing nothing
        std::vector<Vec3> objs;
        std::vector<float> speeds;
        int seenCount = 0;
        bool sawCombat = false;
        for (int i = 0; i < 80; ++i) { // 4 s of driven approach
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            drv.Walk(s, 0.05f);
            const Mob& m = gMobs[s];
            if (m.bossState == 2)
                sawCombat = true;
            if (m.drvObjCount != seenCount) {
                seenCount = m.drvObjCount;
                objs.push_back(m.drvObj);
                speeds.push_back(m.drvObjSpeed);
            }
        }
        CHECK(sawCombat, "a driven man in the open is fought on sight alone - no provocation asked");
        CHECK(!objs.empty(), "his feet were given orders from the first ticks");
        bool ranAtHim = false;
        for (size_t i = 0; i < objs.size(); ++i) {
            Vec3 d = objs[i] - man;
            d.z = 0.0f;
            if (speeds[i] > 2.6f && d.Length() < 6.0f)
                ranAtHim = true;
        }
        CHECK(ranAtHim, "and the orders run AT the watching man: chase pace, aimed at his ground");
    }

    // (a) the entity tracks the driver body's transform exactly: a jump of
    // the body (a native detour around a car the core never ordered) is
    // simply where he is next frame - he is where his body is.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 25.5f, 11.0f);
        RunDriven(drv, s, 0.5f, man, none);
        drv.pos = drv.pos + Vec3(7.0f, -3.0f, 0.0f);
        drv.Report(s, 0.05f);
        MobsTick(0.05f, man, 0, true, none);
        const Mob& m = gMobs[s];
        CHECK(std::fabs(m.pos.x - drv.pos.x) < 1e-4f && std::fabs(m.pos.y - drv.pos.y) < 1e-4f &&
              std::fabs(m.pos.z - drv.pos.z) < 1e-4f,
              "the soldier stands exactly where his driver body reports");
    }

    // (c) the objective refreshes when the tactic changes: a fight opens
    // with a running order toward the man; wounding him trades it for the
    // bounded fallback; losing him far away trades combat for the hunt.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 20.5f, 11.0f);
        host.aimingAtHim = true;
        RunDriven(drv, s, 0.7f, man, none);
        host.aimingAtHim = false;
        bool sawRunOrder = false;
        for (int i = 0; i < 40 && !sawRunOrder; ++i) { // 2 s of fight
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            drv.Walk(s, 0.05f);
            const Mob& m = gMobs[s];
            if (m.drvObjValid && m.drvObjSpeed > 2.6f) {
                Vec3 d = m.drvObj - man;
                d.z = 0.0f;
                if (d.Length() < 14.0f)
                    sawRunOrder = true; // a running order at the man's ground
            }
        }
        CHECK(gMobs[s].bossState == 2, "the aim provocation opened the fight");
        CHECK(sawRunOrder, "the fight's first orders run at the man (the intercept)");

        gMobs[s].health = 1200.0f; // a wound past the threshold (30% of his 4000)
        bool sawFallback = false;
        for (int i = 0; i < 60 && !sawFallback; ++i) { // 3 s
            const Vec3 before = gMobs[s].pos;
            const int count0 = gMobs[s].drvObjCount;
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            drv.Walk(s, 0.05f);
            const Mob& m = gMobs[s];
            if (m.drvObjCount != count0 && m.drvObjValid) {
                Vec3 d = m.drvObj - before;
                d.z = 0.0f;
                Vec3 toMan = man - before;
                toMan.z = 0.0f;
                // the fallback: a bounded bound AWAY from the man, never a
                // fresh advance order wearing the wounded man's name
                if (d.Length() > 3.0f && d.Length() <= 15.5f && d.Dot(toMan) < 0.0f)
                    sawFallback = true;
            }
        }
        CHECK(sawFallback, "wounded past the threshold, his orders become the bounded fallback");

        const int countAtLoss = gMobs[s].drvObjCount;
        const Vec3 far(0.5f, 140.5f, 11.0f); // the man vanishes, far beyond the hunt gate
        // LAW UPDATE (the tenacity leash, 2026-11-07): the successor to a
        // FAR cold loss is the tracker feed now, not the hunt - the hunt
        // is a close tool and the feed owns stale ground. The deed this
        // pin has always carried is unchanged: the loss ends nothing,
        // and fresh movement orders keep flowing under the successor.
        bool sawTrack = false;
        for (int i = 0; i < 300 && !sawTrack; ++i) { // up to 15 s: grace, then the feed
            drv.Report(s, 0.05f);
            MobsTick(0.05f, far, 0, true, none);
            drv.Walk(s, 0.05f);
            if (gMobs[s].bossState == 5 && gMobs[s].drvObjCount > countAtLoss)
                sawTrack = true;
        }
        CHECK(sawTrack, "a far, cold loss trades combat for the tracker feed, with fresh orders under it");
    }

    // (d) one writer: with the body frozen mid-fight (it reports, it does
    // not walk), the soldier does not move a hair - the brain commands, the
    // feet execute, and nobody else touches his position.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 20.5f, 11.0f);
        host.aimingAtHim = true;
        RunDriven(drv, s, 0.7f, man, none);
        host.aimingAtHim = false;
        RunDriven(drv, s, 1.0f, man, none); // the fight is on, the feet are walking
        drv.frozen = true;
        drv.Report(s, 0.05f); // one report syncs the entity onto the body exactly
        const Vec3 stuck = drv.pos;
        const int count0 = gMobs[s].drvObjCount;
        RunDriven(drv, s, 3.0f, man, none);
        const Mob& m = gMobs[s];
        CHECK(m.bossState == 2, "the fight still burns around a wedged body");
        Vec3 d = m.pos - stuck;
        d.z = 0.0f;
        CHECK(d.Length() < 0.01f, "a frozen body means a frozen soldier: the core never walks him itself");
        CHECK(m.drvObjCount > count0, "the commander kept issuing orders the whole time");
    }
}

// ------------------------------------------------------------------
// The jetpack conviction (the user's report, 2026-11-07): with the flight
// cheat on, his shots "have no effect". The mechanism lived in BossShoot:
// the senses are honest 3D, but every range gate on the bullet's line read
// the brain's HORIZONTAL range - a target above him (a jetpack hover, a
// rooftop: same geometry) put its body samples' true line distance past
// every gate, so no round could ever register. The control below proves
// the channel on the ground; the second block is the conviction geometry
// itself - 14 m of ground range, the man's chest ~19.6 m of true line -
// which scored exactly zero before the fix and takes real damage after it.
static void TestBossJetpack() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 20.5f, 11.0f);
        ProvokeByAim(host, man, none);
        RunMobsB(3.0f, man, none);
        CHECK(host.playerHurt > 0.0f, "control: the same man on the ground takes damage");
    }
    {
        WideStage();
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        // hovering: his feet 13 m above the boss's level, 14 m of ground
        // range away - his middle at (0.5, 14.5, 24) over a boss whose
        // feet stand at z 10. The true line to his chest is ~19.6 m; the
        // old gates cut the samples at the ground range + 2 = 16 m.
        const Vec3 air(0.5f, 14.5f, 24.0f);
        ProvokeByAim(host, air, none);
        RunMobsB(5.0f, air, none);
        CHECK(gMobs[s].bossState == 2, "he fights the man above him (his senses were always 3D)");
        CHECK(host.playerHurt > 0.0f,
              "a hovering man takes real damage: the bullet's range is its own true line now");
    }
}

// ------------------------------------------------------------------
// The single-soldier invariant (the user's eye verdict, 2026-11-07): he
// died at Grove, respawned at the hospital, summoned again beside
// himself - and the new man was a GHOST: aiming did nothing, shooting
// did nothing, he just wandered, because the perception/driver body
// serves the FIRST soldier it finds and the original still lived at
// Grove with it. The invariant: a soldier spawn that finds him living
// never creates a second - it re-anchors the same man (same mob id, so
// the bridge's body never changes hands) to the summon point, reborn
// fresh: calm, the board cleared, health and kevlar whole.
static void TestBossReanchor() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // (a) two summons: one man, at the second point, fresh and whole,
    // under the id the first summon gave him
    {
        WideStage();
        const int s1 = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        CHECK(s1 >= 0, "the first summon stands");
        const uint32_t id1 = gMobs[s1].id;
        gMobs[s1].health = 500.0f; // a fought man: wounds the re-anchor must wipe
        gMobs[s1].armor = 100.0f;
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(400.5f, 400.5f, 10.0f), true);
        CHECK(s2 == s1, "the second summon returns the same man, not a new slot");
        int soldiers = 0;
        for (const Mob& mm : gMobs)
            if (mm.kind == MOB_SOLDIER && mm.death < 0.0f)
                ++soldiers;
        CHECK(soldiers == 1, "exactly one living soldier exists after two summons");
        const Mob& m = gMobs[s2];
        CHECK(m.id == id1, "under the same mob id - the bridge's body never changes hands");
        CHECK(std::fabs(m.pos.x - 400.5f) < 0.01f && std::fabs(m.pos.y - 400.5f) < 0.01f,
              "he stands at the second summon point");
        CHECK(m.health >= 3999.0f && m.armor >= 1199.0f, "health and kevlar are whole again");
        CHECK(m.bossState == 0 && !m.aggro && m.fightWarmT == 0.0f && !m.fightStarted,
              "the slate is clean: calm, unprovoked, no warm trail, no fight remembered");
        CHECK(m.reanchor, "the re-anchor word is set for the bridge to place his body");
        CHECK(std::fabs(m.anchor.x - 400.5f) < 0.01f && std::fabs(m.anchor.y - 400.5f) < 0.01f,
              "his post moved with him");
    }

    // (b) the relay serves the re-anchored man at his NEW ground: a shot
    // through his body there starts the fight there (aggro + combat, the
    // perception-body road of MobHurt), and his health pays for it
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        RunDriven(drv, s, 0.3f, Vec3(0.5f, 25.5f, 11.0f), none);
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(300.5f, 0.5f, 10.0f), true);
        CHECK(s2 == s, "still the same man after the driven frames");
        drv.pos = gMobs[s].pos;   // the bridge's re-placement: the body at his feet again
        gMobs[s].reanchor = false; // ...and the word is spent, as the bridge spends it
        drv.Report(s, 0.05f);
        MobsTick(0.05f, Vec3(300.5f, 25.5f, 11.0f), 0, true, none);
        CHECK(std::fabs(gMobs[s].pos.x - 300.5f) < 0.01f,
              "the reported body holds him at the new ground, not back at Grove");
        const float hp0 = gMobs[s].health;
        MobProxyDamage(s, 200.0f, Vec3(300.5f, 25.5f, 11.0f), -2); // the player's shot, through the body
        CHECK(gMobs[s].aggro && gMobs[s].bossState == 2,
              "a shot at the re-anchored man makes the player his enemy - the relay is his");
        CHECK(gMobs[s].health < hp0, "and the relay priced the hit on his real health");
    }

    // (c) a summon in the middle of a fight clears the board
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        MobProxyDamage(s, 200.0f, Vec3(0.5f, 25.5f, 11.0f), -2);
        CHECK(gMobs[s].bossState == 2 && gMobs[s].aggro, "the fight is on before the summon");
        const int s2 = SpawnMob(MOB_SOLDIER, Vec3(50.5f, 50.5f, 10.0f), true);
        CHECK(s2 == s && gMobs[s].bossState == 0 && !gMobs[s].aggro &&
                  gMobs[s].fightWarmT == 0.0f && gMobs[s].health >= 3999.0f,
              "the mid-fight summon stands him down: calm, the board cleared, whole again");
    }
}

// ------------------------------------------------------------------
// The driver's hop (the user's own idea, 2026-11-07: «پرش کردن مثل سی
// جی»): the game's feet walk AROUND fences the legacy legs hopped, so
// the commander orders the hop itself - when the body has honestly
// stalled against a live objective AND the legacy probe reads a
// jumpable obstacle ahead (blocked at the shin, open at the head).
// The harness has no game to jump with, so it plays the bridge's part:
// it watches the order word (drvHop), clears it as the bridge would,
// and counts the orders. What the sims pin is the ORDERING law: a
// stalled body against a fence earns hops on the cooldown's cadence; a
// tall wall earns none (the head probe fails: reroute country); a stall
// against nothing earns none; and a body that walks when ordered is
// never hop-ordered at all.
static void TestBossDriverHop() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // one driven fight against a frozen body; returns the hop order
    // times and how many movement objectives the commander issued (the
    // proof the stall machinery was live). The tall-wall case provokes
    // by a relayed shot instead of an aim: the wall blocks his sight
    // entirely, so the aim provocation alone never opens the fight
    // there - the perception body's road needs no eyes.
    auto FightFrozen = [&](float wallZ, bool wallOn, bool viaShot, std::vector<float>& orders,
                           int& stateOut, int& objCount) {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        drv.frozen = true;
        host.wall = wallOn;
        host.wallC = Vec3(0.5f, 1.6f, wallZ); // a step north of him, across his road
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 6.0f;
        host.aimingAtHim = true;
        const Vec3 man(0.5f, 35.5f, 11.0f);
        float t = 0.0f;
        for (int i = 0; i < 160; ++i) { // 8 s
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            t += 0.05f;
            if (i == 20) {
                host.aimingAtHim = false; // the provocation has landed by now
                if (viaShot)
                    MobProxyDamage(s, 100.0f, man, -2); // one relayed shot: the fight is on
            }
            if (gMobs[s].drvHop) {
                orders.push_back(t);
                gMobs[s].drvHop = false; // the bridge executes it and clears the word
            }
        }
        host.wall = false;
        stateOut = gMobs[s].bossState;
        objCount = gMobs[s].drvObjCount;
    };

    // (a) a low, fence-height wall (its top at z 11: the shin probe at
    // +0.35 meets it, the head probe at +1.35 passes over it)
    {
        std::vector<float> orders;
        int state = -1, objs = 0;
        FightFrozen(8.0f, true, false, orders, state, objs);
        CHECK(state == 2, "the fight opened - his orders run at the man, into the fence");
        CHECK(!orders.empty(), "a stalled body against a fence earns a hop order");
        for (size_t i = 1; i < orders.size(); ++i)
            CHECK(orders[i] - orders[i - 1] >= 1.5f,
                  "hop orders keep the cooldown's cadence - never a pogo stick");
    }

    // (b) a tall wall (its top at z 13: the head probe meets it too) -
    // no hop is ever the answer to a building
    {
        std::vector<float> orders;
        int state = -1, objs = 0;
        FightFrozen(10.0f, true, true, orders, state, objs);
        CHECK(objs > 0, "his orders streamed at the man behind the tall wall (the stall was live)");
        CHECK(orders.empty(), "a tall wall earns no hop order - the reroute stays the answer");
    }

    // (c) no wall at all: a body stalled against thin air (wedged on
    // something the probe cannot see) earns no hop either - the order
    // needs the probe's reading, not just the stall
    {
        std::vector<float> orders;
        int state = -1, objs = 0;
        FightFrozen(8.0f, false, false, orders, state, objs);
        CHECK(state == 2, "the fight opened in the open");
        CHECK(orders.empty(), "a stall with no obstacle ahead earns no hop order");
    }

    // (d) a body that WALKS when ordered: the same fight, the feet
    // flowing - his reported velocity never reads as a stall, so no hop
    // is ever ordered underneath a moving man
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        host.wall = true;
        host.wallC = Vec3(0.5f, 1.6f, 8.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 6.0f;
        host.aimingAtHim = true;
        int hops = 0;
        for (int i = 0; i < 160; ++i) { // 8 s
            drv.Report(s, 0.05f);
            MobsTick(0.05f, Vec3(0.5f, 35.5f, 11.0f), 0, true, none);
            drv.Walk(s, 0.05f);
            if (i == 20)
                host.aimingAtHim = false;
            if (gMobs[s].drvHop) {
                ++hops;
                gMobs[s].drvHop = false;
            }
        }
        host.wall = false;
        CHECK(gMobs[s].bossState == 2, "the fight ran with a walking body");
        CHECK(hops == 0, "a walking body is never hop-ordered");
    }
}

// ------------------------------------------------------------------
// The statue (the user's eye verdict on the repair build, 2026-11-07:
// «مثل مجسمه ایستاده تکون نمی‌خوره فقط تیر میزنه») - the regression
// this sim exists to keep dead. The hop's stall clock read RAW reported
// speed: under 0.6 m/s for 0.45 s against a live objective was called a
// stall. But a native seek starting from standstill spends its first
// moments below 0.6 m/s HONESTLY - the turn-to-face phase has zero
// horizontal speed, then the pace ramps up - so a seek start-up with a
// fence in the probe line earned a hop order at 0.45 s, the hop task
// took the primary slot away from the seek before the feet ever
// stepped, and the cycle repeated forever: a statue that only shoots.
// The harness's instant Walk could never see it (full pace from the
// first step); the ramp body can. The law: a start-up is never a
// stall, and a start-up ends in real movement.
static void TestBossDriverStartup() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // (a) the calm patrol start-up - where the statue froze first. Leg
    // one runs clean and completes. The moment leg two's objective
    // lands, the fence is stood squarely across that leg, inside the
    // probe's reach, and he is faced AWAY from the post (at a post he
    // scans; the worst facing is the honest test): an about-face
    // (zero-speed turn) plus the walk ramp keeps him under the old
    // law's 0.6 m/s line for ~0.8 s with a jumpable fence in the
    // probe's strip. The old clock ordered a hop inside that window;
    // the new law must not - and the leg must still complete.
    {
        // LAW NOTE (the tracker law): the patrol this block walks exists
        // only under the provocation law - a tracking boss leaves his post
        // for the bystander at once. The start-up law priced here is
        // state-agnostic (it guards EVERY seek birth), so the block runs
        // with the tracker flipped off and restores it; block (b) prices
        // the same law in combat, under the tracker itself.
        gTrackerAlways = false;
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        drv.ramp = true;
        const Vec3 man(0.5f, 25.5f, 11.0f); // a bystander who never provokes
        int hops = 0;
        bool waited = false, fenced = false;
        Vec3 post2;
        float post2Dist = 1e9f;
        int seenCount = -1;
        for (int i = 0; i < 400; ++i) { // 20 s of calm patrol
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            drv.Walk(s, 0.05f);
            if (gMobs[s].patrolWait > 0.0f)
                waited = true; // a leg completed under him
            if (waited && !fenced && gMobs[s].drvObjValid && gMobs[s].drvObjSpeed > 0.01f &&
                gMobs[s].drvObjCount != seenCount) {
                Vec3 leg = gMobs[s].drvObj - drv.pos;
                leg.z = 0.0f;
                if (leg.Length() > 4.0f) {
                    // leg two has just been ordered: fence it, face him away
                    const Vec3 legDir = leg * (1.0f / leg.Length());
                    post2 = gMobs[s].drvObj;
                    host.wall = true;
                    host.wallC = drv.pos + legDir * 1.2f + Vec3(0, 0, -2.0f);
                    host.wallDir = Vec3(-legDir.y, legDir.x, 0);
                    host.wallHalf = 3.0f;
                    drv.facing = std::atan2(legDir.x, -legDir.y); // facing exactly away
                    fenced = true;
                }
            }
            if (gMobs[s].drvObjCount != seenCount)
                seenCount = gMobs[s].drvObjCount;
            if (fenced) {
                Vec3 d = post2 - drv.pos;
                d.z = 0.0f;
                post2Dist = std::min(post2Dist, d.Length());
            }
            if (gMobs[s].drvHop) {
                ++hops;
                gMobs[s].drvHop = false; // the bridge executes it and clears the word
            }
        }
        host.wall = false;
        CHECK(waited && fenced, "the patrol ran: a leg completed and the second leg was fenced mid-start-up");
        CHECK(hops == 0,
              "a patrol start-up (about-face, then the walk ramp) is never read as a stall - no hop is ordered while his feet are still spinning up");
        CHECK(post2Dist < 2.0f,
              "and the fenced leg completes: he reaches the second post on his own feet");
        gTrackerAlways = true;
    }

    // (b) the same law under fire: the fight opens, he advances in his
    // burst rhythm (advance, plant, burst) through repeated start-ups,
    // and none of them is read as a stall either.
    {
        WideStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        drv.ramp = true; // his feet spin up the way the game's feet do
        host.wall = true;
        host.wallC = Vec3(0.5f, 1.6f, 8.0f); // the hop test's fence: shin-high, head-clear,
        host.wallDir = Vec3(1, 0, 0);        // a step north of him, across the road to the man
        host.wallHalf = 6.0f;
        host.aimingAtHim = true;
        const Vec3 man(0.5f, 35.5f, 11.0f);
        int hops = 0;
        float closest = 1e9f;
        for (int i = 0; i < 200; ++i) { // 10 s
            drv.Report(s, 0.05f);
            MobsTick(0.05f, man, 0, true, none);
            drv.Walk(s, 0.05f);
            if (i == 20)
                host.aimingAtHim = false; // the provocation has landed by now
            if (gMobs[s].drvHop) {
                ++hops;
                gMobs[s].drvHop = false; // the bridge executes it and clears the word
            }
            Vec3 d = man - drv.pos;
            d.z = 0.0f;
            closest = std::min(closest, d.Length());
        }
        host.wall = false;
        CHECK(gMobs[s].bossState == 2, "the fight opened under the start-up body");
        CHECK(hops == 0,
              "no hop is ordered under fire either - his burst-rhythm start-ups are not stalls");
        // his combat rhythm closes in bounds - advance, plant, burst - so
        // the floor is the statue's own negation, not a sprint time: the
        // statue never left its spawn point at all, while a living boss
        // walks out of the spawn strip and closes a solid share of the
        // road on his own feet
        CHECK(closest < 30.0f && drv.pos.y > 4.0f,
              "and the start-ups end in real approach: he leaves the spawn strip and closes a solid share of the road to the man");
    }
}

// =====================================================================
// THE TRACKER LAW (the user's explicit order, 2026-11-07 - picked from
// the options menu over the gentler variant): the boss always knows
// where the player is and comes for him from the summon - no provocation,
// no cold trail, no stand-down while the player lives. Combat still
// opens only on genuine sensory contact, his rounds still die on walls,
// and he travels on his own feet at his own speeds. These sims pin the
// law end to end; the stage is one long north street so the road itself
// is what is measured.
static void TrackerStage() {
    BlockStage();
    for (int x = -20; x <= 20; ++x)
        for (int y = -20; y <= 850; ++y)
            gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
    MobsClear();
    ResetSurvival();
}

static void TestBossTracker() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    CHECK(LoadSoldierBinFromRepo(), "the tracker proofs meet his real model");

    // (a) 800 m of road, zero provocations, a man standing still: he
    // leaves, closes real distance in every window, never stands down,
    // and arrives fighting. Priced on the FALLBACK road (no graph at
    // all - the probe walks the whole way): the cross-map case the design
    // names, where the graph cannot reach. The graph road is the same
    // BossChaseTo mover, proven by the atlas and smart-pack sims.
    {
        TrackerStage();
        UnloadNavGraph();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 800.5f, 11.0f);
        auto distTo = [&]() {
            Vec3 d = gMobs[s].pos - Vec3(0.5f, 800.5f, 10.0f);
            d.z = 0.0f;
            return d.Length();
        };
        float prev = distTo();
        CHECK(prev > 790.0f, "the tracker sim opens 800 m apart (%.0f)", prev);
        bool stoodDown = false;
        for (int seg = 0; seg < 4; ++seg) { // 4 x 30 s of road
            for (int i = 0; i < 600; ++i) {
                MobsTick(0.05f, man, 0, true, none);
                if (seg > 0 || i > 20) {
                    const int st = gMobs[s].bossState;
                    if (st == 0 || st == 4)
                        stoodDown = true;
                }
            }
            const float d = distTo();
            CHECK(d < prev - 50.0f, "every 30 s of road closes real distance (%.0f -> %.0f)", prev, d);
            prev = d;
        }
        for (int i = 0; i < 600; ++i) // the arrival window
            MobsTick(0.05f, man, 0, true, none);
        CHECK(!stoodDown, "and he never stood down on the way: no patrol, no suspicion, only the road");
        CHECK(gMobs[s].bossState == 2 && distTo() < 30.0f && host.playerHurt > 0.0f,
              "he arrives on foot and the fight opens on sight (state %d, %.0f m out, hurt %.1f)",
              gMobs[s].bossState, distTo(), host.playerHurt);
        CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
    }

    // (b) a silent man ducking behind a wall with no reachable end: the
    // pursuit comes to his ground and works it - and while every sample
    // line dies on the wall he deals ZERO damage and opens no fight. The
    // tracker is knowledge of where to go, never a firing solution. When
    // the wall comes down, the fight opens at once.
    {
        TrackerStage();
        host.wall = true;
        host.wallC = Vec3(0.5f, 10.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 60.0f; // no end he can reach inside the window
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 20.5f, 10.2f); // ducking: every sample line dies on the wall
        host.playerHurt = 0.0f; // (a)'s fight is not this block's evidence
        bool fought = false, stoodDown = false;
        for (int i = 0; i < 240; ++i) { // 12 s of silence behind the wall
            MobsTick(0.05f, man, 0, true, none);
            const int st = gMobs[s].bossState;
            if (st == 2)
                fought = true;
            if (st == 0 || st == 4)
                stoodDown = true;
        }
        CHECK(host.playerHurt == 0.0f && !fought,
              "fully walled, he deals no damage and opens no fight: the tracker is not a firing solution");
        CHECK(!stoodDown, "and the pursuit never stood down while the man hid in silence");
        // he came as far as the world lets him: pressed to the wall's face,
        // working along it for a way through (the wall has no end in this
        // window; a finite building in the game is routed around by road)
        CHECK(gMobs[s].pos.y > 8.0f, "he came to the barrier itself and works it (y %.1f)", gMobs[s].pos.y);
        host.wall = false; // the wall comes down: NOW his eyes may work
        bool opened = false;
        for (int i = 0; i < 400 && !opened; ++i) {
            MobsTick(0.05f, man, 0, true, none);
            if (gMobs[s].bossState == 2 && host.playerHurt > 0.0f)
                opened = true;
        }
        CHECK(opened, "the fight opens only when a real line of sight exists - and then at once");
    }

    // (c) death on the road: the chase zeroes where he stands, he holds
    // there while the player is down, and the respawn re-acquires from
    // his own ground - priced on the fallback road, like (a).
    {
        TrackerStage();
        UnloadNavGraph();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 300.5f, 11.0f);
        RunMobsB(10.0f, man, none);
        CHECK(gMobs[s].bossState == 5, "mid-pursuit at 300 m: he is on the road (state %d)", gMobs[s].bossState);
        CHECK(gMobs[s].pos.y > 40.0f, "and he has real road behind him (y %.0f)", gMobs[s].pos.y);
        gGame.deathTime = 0.5f; // the player dies on the road
        RunMobsB(1.0f, man, none);
        CHECK(gMobs[s].bossState == 0, "the death zeroes the chase where it stands (state %d)", gMobs[s].bossState);
        const Vec3 held = gMobs[s].pos;
        RunMobsB(3.0f, man, none); // still dead
        CHECK((gMobs[s].pos - held).Length() < 3.0f, "and he holds there - no walk home, no patrol");
        gGame.deathTime = -1.0f; // respawned, 400 m further on
        const Vec3 man2(0.5f, 700.5f, 11.0f);
        auto dist2 = [&]() {
            Vec3 d = gMobs[s].pos - Vec3(0.5f, 700.5f, 10.0f);
            d.z = 0.0f;
            return d.Length();
        };
        const float d0 = dist2();
        RunMobsB(3.0f, man2, none);
        CHECK(gMobs[s].bossState == 5 && dist2() < d0 - 10.0f,
              "respawned, the tracker re-acquires from where he stands and he is coming again (state %d, %.0f -> %.0f)",
              gMobs[s].bossState, d0, dist2());
        gGame.deathTime = -1.0f;
        CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
    }

    // (d) a fast car: it outruns him honestly (12 m/s against his 6.4)
    // and he never gives up - no stand-down in 25 s of flight, real road
    // covered - and when the car stops, he arrives and engages.
    {
        TrackerStage();
        UnloadNavGraph();
        host.playerHurt = 0.0f;
        host.vehicle = 77; // the test car's number, the host's own convention
        host.vehPoseOn = true;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        Vec3 seat(0.5f, 65.5f, 10.6f); // the seat starts 65 m ahead of him
        host.vehCentre = seat;
        host.vehSeat = seat;
        host.velocity = Vec3(0.0f, 12.0f, 0.0f); // and pulls away at 12 m/s
        bool gaveUp = false;
        for (int i = 0; i < 500; ++i) { // 25 s of flight
            seat = seat + Vec3(0.0f, 12.0f * 0.05f, 0.0f);
            host.vehCentre = seat;
            host.vehSeat = seat;
            MobsTick(0.05f, seat + Vec3(0, 0, 0.4f), 0, true, none);
            if (i > 20) {
                const int st = gMobs[s].bossState;
                if (st == 0 || st == 4)
                    gaveUp = true;
            }
        }
        CHECK(!gaveUp, "a fast car does not shake him off: no stand-down in 25 s of flight");
        CHECK(gMobs[s].pos.y > 80.0f, "he kept coming the whole way (y %.0f)", gMobs[s].pos.y);
        host.velocity = Vec3(); // the car stops
        bool arrived = false;
        for (int i = 0; i < 1800 && !arrived; ++i) { // up to 90 s
            MobsTick(0.05f, seat + Vec3(0, 0, 0.4f), 0, true, none);
            Vec3 d = gMobs[s].pos - (seat - Vec3(0, 0, 0.6f));
            d.z = 0.0f;
            if (d.Length() < 30.0f && (gMobs[s].bossState == 2 || host.playerHurt > 0.0f))
                arrived = true;
        }
        CHECK(arrived, "and when the car stops he arrives and engages (state %d, hurt %.1f)",
              gMobs[s].bossState, host.playerHurt);
        host.vehicle = -1;
        host.vehPoseOn = false;
        CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
    }

    // (e) the driver contract under the law: a driven body far from a
    // standing man is ordered at his ground, at chase pace, in the
    // tracker state - no provocation involved anywhere. LAW UPDATE (the
    // long-haul law, 2026-11-07): the staging moved from 400 m to 250 m -
    // past the user's 300 m line the CORE owns his feet by law and there
    // is no driver body to carry (that road is TestBossTravelModes' own
    // sim); the driver contract is exercised inside the body's country,
    // where the law says the body drives.
    {
        TrackerStage();
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 250.5f, 11.0f);
        RunDriven(drv, s, 3.0f, man, none);
        const Mob& m = gMobs[s];
        CHECK(m.bossState == 5, "driven, the tracker state owns the road (state %d)", m.bossState);
        CHECK(!m.drvCoreTravel, "inside the body's country the body drives (250 m is his ground)");
        CHECK(m.drvObjValid && m.drvObjSpeed > 2.6f, "the objective runs: chase pace on the driver body (%.1f m/s)", m.drvObjSpeed);
        Vec3 d = m.drvObj - Vec3(0.5f, 250.5f, 10.0f);
        d.z = 0.0f;
        CHECK(d.Length() < 6.0f, "and it is aimed at the far man's ground (%.1f m off)", d.Length());
    }
}

// A synthetic ped graph in the loader's own format (the TestSmartPack
// builder's pattern), for the travel sims that need a road of their own.
static bool BuildTestGraph(const std::vector<Vec3>& nodes, const std::vector<std::pair<int, int>>& edges) {
    std::vector<std::vector<uint32_t>> adj(nodes.size());
    for (auto& e : edges) {
        adj[e.first].push_back((uint32_t)e.second);
        adj[e.second].push_back((uint32_t)e.first);
    }
    std::vector<unsigned char> g;
    auto put = [&](const void* d, size_t n) { const unsigned char* c = (const unsigned char*)d; g.insert(g.end(), c, c + n); };
    auto u32 = [&](uint32_t v) { put(&v, 4); };
    auto f32 = [&](float v) { put(&v, 4); };
    put("MCNAV1", 6);
    u32((uint32_t)nodes.size());
    uint32_t total = 0;
    for (auto& a : adj)
        total += (uint32_t)a.size();
    u32(total);
    for (auto& n : nodes) { f32(n.x); f32(n.y); f32(n.z); }
    uint32_t off = 0;
    u32(0);
    for (auto& a : adj) { off += (uint32_t)a.size(); u32(off); }
    for (auto& a : adj)
        for (uint32_t t : a)
            u32(t);
    uint32_t sum = 0;
    for (unsigned char c : g)
        sum += c;
    u32(sum);
    return LoadNavGraph(g.data(), g.size());
}

// =====================================================================
// THE LONG-HAUL LAW (the tracker repair round, 2026-11-07): the session
// of that day watched the driver body fall out of the world at 510 m
// and be adopted, falling, for thirteen minutes - z without bound, x/y
// frozen, no gone line, no new objective (the standing order deduped,
// by design, over a corpse). Three roots, all convicted from code: the
// report path adopted positions with no sanity gate; the bridge's
// gone-checks (pool ref, health, ped state) all pass forever on a body
// whose health we ourselves top up; and the legacy legs' ground-hold
// protection never runs in driver mode because MoveMob is skipped by
// design there. The law: a fall detector on the report itself, and the
// user's own 300 m as the body's country - inside it the native body
// drives, beyond it the core walks him and the body is parked as the
// glued sensor (320 out / 280 back: hysteresis around his number).
static void TestBossTravelModes() {
    TestHost host;
    SetHost(&host);
    McsaSrand(4242);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    const float dt = 0.05f;
    auto flatD = [](const Vec3& a, const Vec3& b) {
        Vec3 d = a - b;
        d.z = 0.0f;
        return d.Length();
    };

    // (A) THE FALL, both streams: an honest body whose standing flag
    // drops as it falls, and a liar whose flag keeps claiming ground
    // while the z plunges. Either way: gone declared inside a second,
    // the fall never adopted past the evidence threshold, his feet
    // restored to the last sane ground, the bridge word set - and once
    // the bridge obeys it, core travel keeps closing on the man.
    for (int variant = 0; variant < 2; ++variant) {
        TrackerStage(); // a long floor: the core-travel leg after the fall runs 50+ m
        UnloadNavGraph();
        host.wall = false;
        host.vehicle = -1;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody drv{ gMobs[s].pos };
        const Vec3 man(0.5f, 60.5f, 11.0f);
        RunDriven(drv, s, 1.0f, man, none); // a healthy driven approach
        CHECK(gMobs[s].drvOn, "fall sim %d: the body drives before anything is wrong", variant);
        drv.falling = true;
        drv.lieGrounded = (variant == 1);
        float goneAt = -1.0f, minZ = 99.0f;
        for (int i = 0; i < 40; ++i) { // 2 s of falling reports
            drv.Report(s, dt);
            MobsTick(dt, man, 0, true, none);
            drv.Walk(s, dt);
            minZ = std::min(minZ, gMobs[s].pos.z);
            if (!gMobs[s].drvOn && goneAt < 0.0f)
                goneAt = (i + 1) * dt;
        }
        CHECK(goneAt > 0.0f && goneAt <= 1.0f,
              "fall sim %d: the falling body is declared gone within a second (%.2f s)", variant, goneAt);
        CHECK(gMobs[s].drvDropBody, "fall sim %d: the bridge word is set - retire and kill this body", variant);
        CHECK(minZ > 7.5f, "fall sim %d: the fall is never adopted past the evidence threshold (min z %.2f)", variant, minZ);
        CHECK(gMobs[s].pos.z > 9.9f, "fall sim %d: his feet are restored to the last sane ground (z %.2f)", variant, gMobs[s].pos.z);
        MobDriverGone(s); // the bridge obeys the word
        CHECK(!gMobs[s].drvDropBody, "fall sim %d: the retirement clears the word", variant);
        const float d0 = flatD(gMobs[s].pos, man);
        for (int i = 0; i < 200; ++i)
            MobsTick(dt, man, 0, true, none);
        CHECK(flatD(gMobs[s].pos, man) < d0 - 20.0f,
              "fall sim %d: core travel takes his feet and keeps closing (%.0f -> %.0f m)", variant, d0, flatD(gMobs[s].pos, man));
        CHECK(gMobs[s].bossState == 5 || gMobs[s].bossState == 2,
              "fall sim %d: the pursuit itself never blinked (state %d)", variant, gMobs[s].bossState);
        CHECK(gMobs[s].pos.z > 9.9f, "fall sim %d: and his feet stay on the ground the whole way", variant);
    }

    // (B) THE LONG HAUL: the man waits 800 m off. An honest bridge parks
    // the body while the core owns the stretch and stands it back up at
    // his feet inside the engage line. The mode flips exactly once each
    // way, no tick ever moves him faster than his own pace (no teleport
    // hides in either handover), and the road ends the way the tracker
    // law promises: arrival and a fight on sight.
    {
        TrackerStage();
        UnloadNavGraph();
        host.wall = false;
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 800.5f, 11.0f);
        FakeDriverBody body{ gMobs[s].pos };
        bool parked = false, alive = true, wasCore = false, sawCoreLegs = false;
        int coreOn = 0, coreOff = 0;
        float maxStep = 0.0f;
        bool engaged = false;
        for (int i = 0; i < 4000 && !engaged; ++i) { // up to 200 s of road
            Mob& m = gMobs[s];
            if (m.drvDropBody) { MobDriverGone(s); alive = false; parked = true; }
            if (m.drvCoreTravel && !parked) {
                if (m.drvOn)
                    MobDriverGone(s);
                parked = true;
            }
            if (!m.drvCoreTravel && parked && m.drvReengageT <= 0.0f) {
                parked = false; // the mode entry: the body is placed at
                alive = true;   // his feet - the sensor comes back as him
                body.pos = m.pos;
            }
            if (!parked && alive)
                body.Report(s, dt);
            const Vec3 before = gMobs[s].pos;
            MobsTick(dt, man, 0, true, none);
            if (!parked && alive)
                body.Walk(s, dt);
            maxStep = std::max(maxStep, flatD(gMobs[s].pos, before));
            if (gMobs[s].drvCoreTravel != wasCore) {
                if (gMobs[s].drvCoreTravel) ++coreOn; else ++coreOff;
                wasCore = gMobs[s].drvCoreTravel;
            }
            if (gMobs[s].drvCoreTravel && !gMobs[s].drvOn)
                sawCoreLegs = true;
            if (flatD(gMobs[s].pos, man) < 30.0f && (gMobs[s].bossState == 2 || host.playerHurt > 0.0f))
                engaged = true;
        }
        CHECK(coreOn == 1 && coreOff == 1, "long haul: core travel engages once and hands back once (%d/%d)", coreOn, coreOff);
        CHECK(sawCoreLegs, "long haul: beyond the line his own legs carry him - no body is adopted out there");
        CHECK(maxStep < 0.45f, "long haul: no teleport in either handover (largest tick step %.2f m)", maxStep);
        CHECK(engaged, "long haul: 800 m of road ends in arrival and a fight (state %d, %.0f m out)",
              gMobs[s].bossState, flatD(gMobs[s].pos, man));
    }

    // (C) THE BOUNDARY: hysteresis around the user's 300. A man hovering
    // inside the band (285-315 m) never flips the mode at all; a man
    // swinging across the whole band flips it a bounded number of times
    // - the law answers the crossing, it does not flap.
    {
        TrackerStage();
        UnloadNavGraph();
        host.wall = false;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        FakeDriverBody body{ gMobs[s].pos };
        auto runBand = [&](float dNear, float dFar, float legSeconds, int legs) {
            int flips = 0;
            bool was = gMobs[s].drvCoreTravel;
            for (int leg = 0; leg < legs; ++leg) {
                const float d = (leg % 2 == 0) ? dFar : dNear;
                for (float t = 0.0f; t < legSeconds; t += dt) {
                    body.pos = gMobs[s].pos; // a carried body: the distance scripted is the distance read
                    body.Report(s, dt);
                    const Vec3 mp(gMobs[s].pos.x, gMobs[s].pos.y + d, 11.0f);
                    MobsTick(dt, mp, 0, true, none);
                    if (gMobs[s].drvCoreTravel != was) {
                        ++flips;
                        was = gMobs[s].drvCoreTravel;
                    }
                }
            }
            return flips;
        };
        const int hoverFlips = runBand(285.0f, 315.0f, 1.0f, 20); // 20 s inside the band
        CHECK(hoverFlips == 0, "boundary: hovering at the user's 300 never flaps the mode (%d flips)", hoverFlips);
        const int swingFlips = runBand(250.0f, 350.0f, 5.0f, 8); // 40 s across the whole band
        CHECK(swingFlips >= 6 && swingFlips <= 9,
              "boundary: full swings flip the mode once per crossing, no more (%d flips in 8 legs)", swingFlips);
    }
    CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
}

// =====================================================================
// THE TENACITY PASS (the user's amendment, 2026-11-07: «مثل سگ ولم
// نکنه»): the tracker feed never dies, but the pursuit used to spend
// the feed's gift on stale ground - the hunt working a disc the prey
// had long left, or running out its 45-second clock toward a stand-
// down the feed never ordered (the session fossil: combat flipped to
// hunt at 510 m and the clock owned the next 45 s). The leash: past
// 72 m from the boss - beyond the search's own farthest stride - the
// hunt hands the chase back to the feed the same tick. Near prey the
// hunt keeps every bit of its corner craft; the re-acquire out of a
// brief loss was already fast and is pinned, not changed.
static void TestBossTenacity() {
    TestHost host;
    SetHost(&host);
    McsaSrand(99);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    const float dt = 0.05f;
    auto flatD = [](const Vec3& a, const Vec3& b) {
        Vec3 d = a - b;
        d.z = 0.0f;
        return d.Length();
    };

    // the zigzag-and-car script: 14 s of zigzag on foot, 5 s in a car,
    // 15 s of zigzag again. Net-net he must LOSE ground the whole way,
    // and the boss must never once leave the pursuit family for patrol.
    {
        TrackerStage();
        UnloadNavGraph();
        host.wall = false;
        host.playerHurt = 0.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        Vec3 manPos(0.5f, 120.5f, 11.0f);
        Vec3 seat(0.5f, 0.5f, 10.6f);
        const float d0 = flatD(gMobs[s].pos, manPos);
        int badState = -1;
        float yAtCarStart = 0.0f, yAtCarEnd = 0.0f;
        for (int i = 0; i < 680; ++i) { // 34 s of tricks
            const float t = i * dt;
            Vec3 mp;
            if (t < 14.0f || t >= 19.0f) { // on foot, zigzagging
                host.vehicle = -1;
                host.vehPoseOn = false;
                const int leg = (int)(t / 0.8f);
                const float sx = (leg % 2 == 0) ? 3.98f : -3.98f;
                const Vec3 vel(sx, 3.34f, 0.0f); // 5.2 m/s at +/-50 degrees
                host.velocity = vel;
                manPos = manPos + vel * dt;
                mp = manPos;
            } else { // the car hop: 11 m/s straight north
                if (host.vehicle != 77) {
                    seat = Vec3(manPos.x, manPos.y, 10.6f);
                    host.vehicle = 77;
                    host.vehPoseOn = true;
                    yAtCarStart = gMobs[s].pos.y;
                }
                host.velocity = Vec3(0.0f, 11.0f, 0.0f);
                seat = seat + Vec3(0.0f, 11.0f * dt, 0.0f);
                host.vehCentre = seat;
                host.vehSeat = seat;
                mp = seat + Vec3(0, 0, 0.4f);
                manPos = Vec3(seat.x, seat.y, 11.0f);
                if (t + dt >= 19.0f)
                    yAtCarEnd = gMobs[s].pos.y;
            }
            MobsTick(dt, mp, 0, true, none);
            if (i > 20 && badState < 0) {
                const int st = gMobs[s].bossState;
                if (st != 5 && st != 3 && st != 2)
                    badState = st;
            }
        }
        host.vehicle = -1;
        host.vehPoseOn = false;
        host.velocity = Vec3();
        const float d1 = flatD(gMobs[s].pos, manPos);
        CHECK(badState < 0, "tenacity: zigzag and car hops never shake him into patrol (worst state %d)", badState);
        CHECK(yAtCarEnd > yAtCarStart + 15.0f, "tenacity: he kept running through the car phase (y %.0f -> %.0f)", yAtCarStart, yAtCarEnd);
        CHECK(d1 < d0 - 40.0f, "tenacity: net-net the tricks LOSE ground (%.0f -> %.0f m)", d0, d1);
    }

    // the brief-loss re-acquire: a wall eats the contact at 40 m, the
    // hunt takes the ground (near prey - the leash does not apply), and
    // the moment the wall drops the fight resumes inside 0.6 s.
    {
        TrackerStage(); // a long floor: the fight closes most of the 40 m first
        UnloadNavGraph();
        host.wall = false;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 40.5f, 11.0f);
        for (int i = 0; i < 16; ++i) // the fight opens on sight
            MobsTick(dt, man, 0, true, none);
        CHECK(gMobs[s].bossState == 2, "re-acquire: the fight is on before the wall (state %d)", gMobs[s].bossState);
        host.wall = true;
        // the wall goes up between the two men, and the man falls back
        // behind it to 75 m: the combat press walks to his last-seen
        // point during the grace, so the prey himself must still be
        // beyond the hunt's distance gate when the grace runs out -
        // that is the gate's own law, and the sim stages it honestly
        host.wallC = Vec3(0.5f, (gMobs[s].pos.y + man.y) * 0.5f, 10.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 30.0f; // too wide to round inside the window
        const Vec3 manFar(0.5f, 75.5f, 11.0f);
        bool sawHunt = false;
        for (int i = 0; i < 160; ++i) { // 8 s behind the wall
            MobsTick(dt, manFar, 0, true, none);
            if (gMobs[s].bossState == 3)
                sawHunt = true;
        }
        CHECK(sawHunt, "re-acquire: a near loss still belongs to the hunt, not the feed (the leash is honest)");
        host.wall = false; // the wall drops and the man is back on the open street
        int ticks = 0;
        while (gMobs[s].bossState != 2 && ticks < 40) {
            MobsTick(dt, man, 0, true, none);
            ++ticks;
        }
        CHECK(ticks <= 12, "re-acquire: sight restored, the fight resumes inside 0.6 s (%d ticks)", ticks);
    }
    CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
}

// =====================================================================
// THE OFF-PATH FREEDOM (the user's amendment, 2026-11-07: he is not
// bound to where pedestrians walk): where the direct line is honestly
// traversable he cuts across country; where it is not - a real wall -
// the chooser refuses and the graph road stands, whole. Priced against
// the graph-only road the freedom replaces (the switch's other side).
static void TestBossOffPath() {
    TestHost host;
    SetHost(&host);
    McsaSrand(2024);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    const float dt = 0.05f;
    auto stageLot = [&]() {
        WideStage();
        for (int x = -12; x <= 36; ++x) // the north street's ground
            for (int y = 15; y <= 26; ++y)
                gWorld.SetRaw(x, y, 9, MakeVox(ID_STONE));
        // the road around the lot: A(0,0) B(0,20) C(15,20) D(30,20) E(30,0)
        // - 70 m of street against 30 m of open country
        CHECK(BuildTestGraph({ Vec3(0, 0, 10), Vec3(0, 20, 10), Vec3(15, 20, 10), Vec3(30, 20, 10), Vec3(30, 0, 10) },
                             { { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 } }),
              "off-path: the lot's road graph loads");
        host.wall = false;
    };
    auto flatD = [](const Vec3& a, const Vec3& b) {
        Vec3 d = a - b;
        d.z = 0.0f;
        return d.Length();
    };
    // arrival is a PLACE, not a state: sight opens the fight at 30 m on
    // any road, so the stopwatch runs until he is actually on the man
    auto runToContact = [&](int s, const Vec3& man, float* maxPerp, float* crossY) {
        int ticks = 0;
        while (flatD(gMobs[s].pos, man) >= 13.0f && ticks < 800) { // 40 s cap
            MobsTick(dt, man, 0, true, none);
            ++ticks;
            if (maxPerp)
                *maxPerp = std::max(*maxPerp, std::fabs(gMobs[s].pos.y - 0.5f));
            if (crossY && *crossY < -900.0f && gMobs[s].pos.x >= 15.0f)
                *crossY = gMobs[s].pos.y;
        }
        return ticks;
    };
    const Vec3 man(30.5f, 0.5f, 11.0f);

    // the graph-only road (the freedom switched off): the price of the road
    gOffPathCut = false;
    stageLot();
    int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
    FaceMob(s, 0.0f);
    const int tGraph = runToContact(s, man, nullptr, nullptr);
    CHECK(tGraph < 800, "off-path: the graph road still arrives (%d ticks)", tGraph);

    // the cut: open country, straight across, materially sooner, and the
    // road he takes is the direct line, not a waypoint's shadow
    gOffPathCut = true;
    stageLot();
    s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
    FaceMob(s, 0.0f);
    float maxPerp = 0.0f;
    const int tCut = runToContact(s, man, &maxPerp, nullptr);
    CHECK(tCut < 800, "off-path: the cut arrives (%d ticks)", tCut);
    CHECK(tCut < (int)(tGraph * 0.65f), "off-path: the cut is materially sooner than the road (%d vs %d ticks)", tCut, tGraph);
    CHECK(maxPerp < 6.0f, "off-path: the cut holds the direct line (max %.1f m off it)", maxPerp);

    // the walled lot: the chooser refuses, the road stands, and he
    // crosses the wall's line only where the road crosses it - north,
    // at the street, never through the wall
    stageLot();
    host.wall = true;
    host.wallC = Vec3(15, 0.5f, 10);
    host.wallDir = Vec3(0, 1, 0);
    host.wallHalf = 12.0f; // the wall spans y -11.5..12.5 at x=15
    s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
    FaceMob(s, 0.0f);
    float crossY = -999.0f;
    const int tWall = runToContact(s, man, nullptr, &crossY);
    host.wall = false;
    CHECK(tWall < 800, "off-path: walled, the road still arrives (%d ticks)", tWall);
    CHECK(crossY > 13.0f, "off-path: he crosses the wall's line at the street, not through it (y %.1f)", crossY);
    CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
}

// =====================================================================
// THE JUMP IN BOTH MODES (the user's amendment, 2026-11-07: «پرشم
// نداره» - in his latest play he never saw one): driver mode's hop is
// the repair round's own, already pinned. Core mode always had the
// walker's probe reflex for the goal line; this round adds the stall
// witness for the wedges the reflex cannot see - progress is the only
// witness, a fresh destination owns a fair start, and the probe toward
// the destination must read the jumpable class before a hop exists.
static void TestBossCoreJump() {
    TestHost host;
    SetHost(&host);
    McsaSrand(5150);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;
    const float dt = 0.05f;

    // (i) THE DEED: a fence across the road in core mode - he hops it
    // once, visibly, and continues to the man. Exactly one airborne
    // episode in the fence's corridor: a hop, not a pogo stick.
    {
        TrackerStage();
        UnloadNavGraph();
        host.wall = true;
        host.wallC = Vec3(0.5f, 30.5f, 8.0f); // its top at z=11: shin reads
        host.wallDir = Vec3(1, 0, 0);        // it, the head line passes
        host.wallHalf = 8.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 60.5f, 11.0f);
        bool crossed = false, wasAir = false;
        int episodes = 0;
        float maxZ = 0.0f;
        for (int i = 0; i < 600; ++i) { // 30 s
            MobsTick(dt, man, 0, true, none);
            const Mob& m = gMobs[s];
            if (m.pos.y < 33.0f) {
                const bool air = !m.onGround && m.vel.z > 4.0f;
                if (air && !wasAir)
                    ++episodes;
                wasAir = air;
                if (std::fabs(m.pos.y - 30.5f) < 3.0f)
                    maxZ = std::max(maxZ, m.pos.z);
            }
            if (m.pos.y > 30.5f)
                crossed = true;
        }
        host.wall = false;
        CHECK(crossed, "core jump: he crosses the fence line instead of wedging on it");
        CHECK(maxZ > 10.8f, "core jump: the crossing is a visible hop (peak z %.2f)", maxZ);
        CHECK(episodes == 1, "core jump: exactly one hop for the fence (%d episodes)", episodes);
        CHECK(gMobs[s].bossState == 2, "core jump: and the road ends in the fight (state %d)", gMobs[s].bossState);
    }

    // (ii) THE WITNESS: his feet are pinned by an outside hand (the
    // harness holds his position, the way a real wedge would) with his
    // route's waypoint leading EAST - the walker's reflex probes the
    // goal line and honestly sees nothing - while a fence stands 1.2 m
    // north, across the line to his destination. Only the progress
    // witness can convict this wedge: no approach for the fair start
    // plus the window, the probe toward the destination reads the
    // jumpable class, and exactly one hop is ordered - no spam under
    // the cooldown. Unpinned, he walks his route and hops the fence on
    // it on the reflex's word, like any other road. The off-path freedom
    // is switched OFF for this sim: with the cut live the destination
    // line and the goal line reunite, and the reflex - rightly - takes
    // the fence itself; the witness is priced against the route here,
    // and the freedom has its own sim above.
    {
        gOffPathCut = false;
        TrackerStage();
        CHECK(BuildTestGraph({ Vec3(0.5f, 0.5f, 10), Vec3(6.5f, 0.5f, 10), Vec3(6.5f, 60.5f, 10) },
                             { { 0, 1 }, { 1, 2 } }),
              "core jump witness: the little route graph loads");
        host.wall = true;
        host.wallC = Vec3(0.5f, 1.7f, 8.0f);
        host.wallDir = Vec3(1, 0, 0);
        host.wallHalf = 8.0f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 60.5f, 11.0f);
        float hopAt = -1.0f, hopVel = 0.0f;
        for (int i = 0; i < 60 && hopAt < 0.0f; ++i) { // up to 3 s pinned
            MobsTick(dt, man, 0, true, none);
            if (gMobs[s].coreHopCount == 1) {
                hopAt = (i + 1) * dt;
                hopVel = gMobs[s].vel.z;
            }
            Mob& m = gMobs[s]; // the outside hand: his feet do not move
            m.pos = Vec3(0.5f, 0.5f, 10.0f);
            m.vel = Vec3();
            m.onGround = true;
        }
        CHECK(hopAt >= 1.5f && hopAt <= 2.5f,
              "core jump witness: the hop is ordered on the law's clock, not before (%.2f s)", hopAt);
        CHECK(hopVel > 4.0f, "core jump witness: and it is a real jump's order (vel.z %.1f)", hopVel);
        for (int i = 0; i < 20; ++i) { // 1 s more of pinning: the cooldown holds
            MobsTick(dt, man, 0, true, none);
            Mob& m = gMobs[s];
            m.pos = Vec3(0.5f, 0.5f, 10.0f);
            m.vel = Vec3();
            m.onGround = true;
        }
        CHECK(gMobs[s].coreHopCount == 1, "core jump witness: no hop spam under the cooldown (%d)", gMobs[s].coreHopCount);
        bool continued = false;
        for (int i = 0; i < 300 && !continued; ++i) { // unpinned: the road resumes
            MobsTick(dt, man, 0, true, none);
            if (gMobs[s].pos.y > 10.0f)
                continued = true;
        }
        host.wall = false;
        gOffPathCut = true;
        CHECK(continued, "core jump witness: unpinned, he walks his route over the fence and on");
    }
    CHECK(LoadNavGraphFromRepo(), "the city graph is restored for the sims after");
}
// the POI database, the zone/gang map and the interior door families
// extracted from the game's own files (atlas-assets/), the vehicle road
// graph from the other half of nodes*.dat, and the brain wired to all of
// it: destination thinking, interior camping, turf-aware search.
// (POI kinds: 1 hospital, 2 police, 3 ammu, 4 pay'n'spray, 6 safehouse.)
static bool LoadVehicleGraphFromRepo() {
    static const char* paths[] = { "cs-soldier/vehicle_graph.bin", "../cs-soldier/vehicle_graph.bin",
                                   "../../cs-soldier/vehicle_graph.bin", "../../../cs-soldier/vehicle_graph.bin" };
    for (const char* p : paths) {
        FILE* f = std::fopen(p, "rb");
        if (!f)
            continue;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<unsigned char> bytes((size_t)std::max<long>(n, 0));
        const bool ok = n > 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
        std::fclose(f);
        if (ok)
            return LoadVehicleGraph(bytes.data(), bytes.size());
    }
    return false;
}

static float Dist2D(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

static void TestAtlas() {
    TestHost host;
    SetHost(&host);
    McsaSrand(777);
    const std::vector<BeingInfo> noBeings;
    const std::vector<BeingInfo>* none = &noBeings;

    // ---- the datasets: counts, and spot checks against the game files
    {
        CHECK(AtlasPoiCount() == 292, "the atlas holds %d places (292 extracted)", AtlasPoiCount());
        int kinds[19] = {};
        for (int i = 0; i < AtlasPoiCount(); ++i) {
            AtlasPoiInfo info;
            if (AtlasPoiGet(i, info) && info.kind >= 0 && info.kind < 19)
                ++kinds[info.kind];
        }
        CHECK(kinds[1] >= 6 && kinds[2] >= 10 && kinds[3] >= 10 && kinds[6] >= 25 && kinds[7] >= 40 && kinds[13] >= 10,
              "per kind: %d hospitals, %d police, %d ammu, %d safehouses, %d food, %d 24/7",
              kinds[1], kinds[2], kinds[3], kinds[6], kinds[7], kinds[13]);
        const int hosp = AtlasNearestPoi(Vec3(2050, -1401, 14), 1, false);
        AtlasPoiInfo hi;
        CHECK(hosp >= 0 && AtlasPoiGet(hosp, hi) && Dist2D(hi.door, Vec3(2050.07f, -1401.21f, 0)) < 2.0f,
              "County General Hospital sits where laehospital1 stands in the files");
        const char* fam = nullptr;
        Vec3 en, ex;
        const int dCarls = AtlasDoorNear(Vec3(2495.33f, -1690.75f, 13.78f), 3.0f);
        CHECK(dCarls >= 0 && AtlasDoorGet(dCarls, fam, en, ex) && std::strcmp(fam, "CARLS") == 0,
              "the Johnson house door is an enex record, not a guess");
        const int dAmmu = AtlasDoorNear(Vec3(1368.35f, -1279.06f, 12.55f), 3.0f);
        CHECK(dAmmu >= 0 && AtlasDoorGet(dAmmu, fam, en, ex) && std::strcmp(fam, "AMMUN1") == 0,
              "the Market Ammu-Nation door likewise");
        const int dLspd = AtlasDoorNear(Vec3(1554.95f, -1674.99f, 15.33f), 3.0f);
        CHECK(dLspd >= 0 && AtlasDoorGet(dLspd, fam, en, ex) && std::strcmp(fam, "POLICE1") == 0,
              "and the LSPD door likewise");
        CHECK(AtlasHeavenFamAt(Vec3(2496.05f, -1692.73f, 1013.75f), 10.0f, fam) && std::strcmp(fam, "CARLS") == 0,
              "the heaven above Ganton belongs to the CARLS family");
        int first = -1, count = 0;
        CHECK(AtlasDoorFamRange("CARLS", first, count) && count == 1, "CARLS has its one street door");
        CHECK(AtlasDoorFamRange("AMMUN1", first, count) && count == 2,
              "AMMUN1's one heaven serves two street doors (LS and SF) - the shared-template truth");
        CHECK(AtlasDoorCount() == 225, "%d street doors after dedupe", AtlasDoorCount());
        CHECK(AtlasZoneGangAt(Vec3(2500, -1650, 14)) == 1, "Ganton is Grove Street Families ground");
        CHECK(AtlasZoneGangAt(Vec3(2027, -1400, 14)) == 2, "Jefferson is Ballas ground");
        CHECK(AtlasZoneGangAt(Vec3(1670, -2290, 14)) == 0, "the airport is nobody's turf");
        CHECK(std::strncmp(AtlasZoneNameAt(Vec3(2500, -1650, 14)), "GAN", 3) == 0,
              "and the zone under Grove is named from the GAN family (%s)", AtlasZoneNameAt(Vec3(2500, -1650, 14)));
        // destination thinking, as a pure function: running west past
        // Jefferson, the hospital is the refuge his run is aimed at
        const int refuge = AtlasRefugeAhead(Vec3(2105, -1440, 14), Vec3(-5.5f, 0, 0), Vec3(2130, -1442, 14));
        AtlasPoiInfo ri;
        CHECK(refuge >= 0 && AtlasPoiGet(refuge, ri) && ri.kind == 1 &&
                  Dist2D(ri.door, Vec3(2050.07f, -1401.21f, 0)) < 3.0f,
              "a man running west at Jefferson is read as running to County General");
        CHECK(AtlasRefugeAhead(Vec3(2105, -1440, 14), Vec3(5.5f, 0, 0), Vec3(2130, -1442, 14)) < 0 ||
                  AtlasPoiGet(AtlasRefugeAhead(Vec3(2105, -1440, 14), Vec3(5.5f, 0, 0), Vec3(2130, -1442, 14)), ri),
              "and the reader never invents a destination behind his back");
    }
    // ---- the vehicle road graph: the car half of nodes*.dat
    {
        CHECK(LoadVehicleGraphFromRepo(), "the vehicle road graph loads");
        CHECK(VehicleNodeCount() == 30587 && VehicleEdgeCount() == 62936,
              "it holds %d road nodes and %d directed edges", VehicleNodeCount(), VehicleEdgeCount());
        std::vector<Vec3> route;
        float dist = 0.0f;
        CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(1670, -2290, 14), &route, &dist),
              "Grove Street to Los Santos International routes BY ROAD");
        CHECK(dist > 1500.0f && dist < 4000.0f && route.size() > 100,
              "the airport run is %.0f m over %d waypoints - the route the ped graph cannot make",
              dist, (int)route.size());
        CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(-1420, -330, 14), &route, &dist) && dist > 6000.0f,
              "and Grove to Easter Bay airport crosses the bay by bridge (%.0f m)", dist);
        UnloadVehicleGraph();
    }
    // ---- the brain: a prey fleeing toward the hospital is intercepted at
    // its door, not at a bare point on his run line
    {
        CHECK(LoadNavGraphFromRepo(), "the city graph loads for the atlas brain sims");
        WideStage();
        host.groundMode = 1;
        host.groundZVal = 13.4f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(2190, -1445, 13.4f), true);
        FaceMob(s, 1.5708f); // facing the prey, west
        host.wall = true;    // a wall across the street at x=2110: the run breaks his eye line there
        host.wallC = Vec3(2110, -1440, 13.4f);
        host.wallDir = Vec3(0, 1, 0);
        host.wallHalf = 14.0f;
        ProvokeByAim(host, Vec3(2150, -1440, 14.4f), none);
        RunMobsB(1.5f, Vec3(2150, -1440, 14.4f), none);
        CHECK(gMobs[s].bossState == 2, "the hospital run opens with a real fight (state %d)", gMobs[s].bossState);
        float px = 2150.0f;
        host.velocity = Vec3(-5.5f, 0, 0);
        bool huntSeen = false;
        Vec3 huntDest;
        for (int i = 0; i < 800 && !huntSeen; ++i) { // 40 s: the run, the break, the hunt
            if (px > 2056.0f)
                px = std::max(2056.0f, px - 5.5f * 0.05f);
            else
                host.velocity = Vec3();
            MobsTick(0.05f, Vec3(px, -1440, 14.4f), 0, true, none);
            if (gMobs[s].bossState == 3) {
                huntSeen = true;
                huntDest = gMobs[s].huntDest;
            }
        }
        CHECK(huntSeen, "the fight breaks into a hunt when the wall eats his eye line");
        CHECK(huntSeen && Dist2D(huntDest, Vec3(2050.07f, -1401.21f, 0)) < 5.0f,
              "and the hunt runs at the hospital's door (%.0f, %.0f) - destination thinking, not a bare intercept",
              huntDest.x, huntDest.y);
        host.wall = false;
        host.velocity = Vec3();
    }
    // ---- the brain: a prey who ducks into the Johnson house is camped at
    // its door - and the camp holds while he stays inside
    {
        WideStage();
        host.groundMode = 1;
        host.groundZVal = 13.4f;
        const int s = SpawnMob(MOB_SOLDIER, Vec3(2462, -1652, 13.4f), true);
        FaceMob(s, -2.43f); // facing the Johnson house door
        ProvokeByAim(host, Vec3(2495.33f, -1690.75f, 14.78f), none);
        RunMobsB(1.5f, Vec3(2495.33f, -1690.75f, 14.78f), none);
        CHECK(gMobs[s].bossState == 2, "the camp sim opens with a fight at the door (state %d)", gMobs[s].bossState);
        bool campSet = false, campHeld = true, doorSearch = false;
        for (int i = 0; i < 1100; ++i) { // 55 s with the prey inside the heaven
            MobsTick(0.05f, Vec3(2496.05f, -1692.73f, 1014.75f), 0, true, none);
            const Mob& b = gMobs[s];
            if (b.campFam && std::strcmp(b.campFam, "CARLS") == 0)
                campSet = true;
            if (campSet && (b.bossState == 0 || b.bossState == 4))
                campHeld = false; // he never stands down while the man is inside
            if (campSet && b.huntPhase == 1 && b.searchPts.size() == 1 &&
                Dist2D(b.searchPts[0], Vec3(2495.33f, -1690.75f, 0)) < 3.0f)
                doorSearch = true; // the hunt's search IS the family's door list
        }
        CHECK(campSet, "ducking into the Johnson house names the family: he camps CARLS");
        CHECK(campHeld, "and the camp holds the whole time the prey stays inside");
        CHECK(doorSearch, "and his search points are the family's street doors, not a disc");
        RunMobsB(1.5f, Vec3(2495.33f, -1690.75f, 14.78f), none); // he comes back out
        CHECK(gMobs[s].campFamFirst < 0, "coming back out ends the camp - at the true door");
        host.groundMode = 0;
    }
    // ---- the brain: on Ballas ground the search slices faster than on
    // Grove ground - the same vanish, two turfs, one stopwatch.
    // LAW UPDATE (the tenacity leash, 2026-11-07): the vanish is staged
    // INSIDE the hunt's country now - the prey ducks into an interior
    // heaven 25 m past the fight (55 m of street from the boss) instead
    // of standing 400 m off. A 400 m vanish is the tracker feed's work
    // under the new law and no hunt ever forms on it; the turf pricing
    // this sim measures lives in the hunt's dwell, which only exists on
    // near ground. The dwell is the same measurement it always was.
    auto turfScan = [&](const Vec3& bossAt, float faceYaw, const Vec3& preyAt, const Vec3& preyGone) {
        WideStage();
        host.groundMode = 1;
        host.groundZVal = 13.4f;
        const int s = SpawnMob(MOB_SOLDIER, bossAt, true);
        FaceMob(s, faceYaw);
        ProvokeByAim(host, preyAt, none);
        RunMobsB(1.0f, preyAt, none);
        float t = 0.0f, scanStart = -1.0f, scanDur = -1.0f;
        for (int i = 0; i < 1800 && scanDur < 0.0f; ++i) { // up to 90 s
            MobsTick(0.05f, preyGone, 0, true, none);
            t += 0.05f;
            const Mob& b = gMobs[s];
            if (scanStart < 0.0f && b.huntPhase == 1 && b.scanT > 0.0f)
                scanStart = t;
            if (scanStart >= 0.0f && b.searchIdx > 0)
                scanDur = t - scanStart;
        }
        host.groundMode = 0;
        return scanDur;
    };
    {
        // (the Ballas vanish rides at z=1200, above every named heaven:
        // at the usual heaven height that spot's nearest family is
        // SVSFMD, whose street doors are in San Fierro - the hunt would
        // camp a door 4700 m away and no scan could ever be measured.
        // An unnamed interior is the honest staging here: he ducked
        // inside, the atlas cannot name the family from that spot, and
        // the hunt works the street he vanished from - on both turfs.)
        const float ballas = turfScan(Vec3(2200, -1180, 13.4f), -1.5708f, Vec3(2230, -1180, 14.4f), Vec3(2255, -1180, 1200.0f));
        const float grove = turfScan(Vec3(2370, -1660, 13.4f), -1.5708f, Vec3(2400, -1660, 14.4f), Vec3(2425, -1660, 1014.4f));
        CHECK(ballas > 0.0f && grove > 0.0f, "both turfs produce a measured scan (Ballas %.2f s, Grove %.2f s)", ballas, grove);
        CHECK(ballas < grove - 0.25f,
              "on Ballas ground he slices a corner in %.2f s against %.2f s at home - he does not linger on enemy turf",
              ballas, grove);
    }
    UnloadNavGraph();
}

// ---------------------------------------------------------------- the Driver 2 cop (step 1)
// A fake body with the one steering sign the SDK does not document INVERTED
// on purpose (command + turns him LEFT here): the spike's calibration must
// catch it and flip the session, or every sim below steers him backwards.
// THE COMPLETE UPDATE's fake autopilot, shared by both cop fakes: the
// bodies are driven by the FEED (CopBodyDriveTarget) - the channel the
// game's own ProcessAI reads - because on the real host the pedal
// fields are that channel's OUTPUTS, not inputs (fix round 4's
// conviction). A suite that steers its fake cars by the pedals proves
// a fiction; from this update the fakes model the channel that moves
// the car. The pedal commands are still recorded (the core writes
// them, and the profile sims read them) - they just move nothing.
// The model is deliberately plain: turn toward the feed point at a
// bounded rate (a rolling car's rate, floored so a creeping car can
// begin), accelerate toward the cruise, brake to arrive inside 6 m of
// the point, and back toward a point that sits well behind within
// 16 m (ProcessAI's answer to a rear destination - the recovery's
// reverse rides exactly this). It follows no roads: the core's feed
// points walk the road graph, so the straight line to the next point
// is the road's own segment at these scales.
static void MirrorStep(Vec3& pos, float& yaw, float& speed, Vec3& vel,
                       const Vec3& feedPt, float feedCruise, bool feedLive, float dt) {
    const Vec3 fwd(std::sin(yaw), std::cos(yaw), 0.0f);
    if (!feedLive || feedCruise < 0.5f) {
        speed -= speed * 2.0f * dt;
        if (std::fabs(speed) < 0.05f)
            speed = 0.0f;
        pos = pos + fwd * speed * dt;
        vel = fwd * speed;
        return;
    }
    Vec3 to = feedPt - pos;
    to.z = 0.0f;
    const float dist = to.Length();
    if (dist < 1.0f) {
        speed -= speed * 2.0f * dt;
        if (std::fabs(speed) < 0.05f)
            speed = 0.0f;
        pos = pos + fwd * speed * dt;
        vel = fwd * speed;
        return;
    }
    const Vec3 dir = to * (1.0f / dist);
    const float cosA = dir.x * fwd.x + dir.y * fwd.y;
    const bool behind = cosA < -0.45f && dist < 16.0f;
    float target;
    if (behind) {
        target = -std::fmin(feedCruise * 0.5f, 8.0f);
    } else {
        target = feedCruise;
        if (dist < 7.0f)
            target = std::fmin(target, std::fmax(2.0f, dist * 0.8f)); // arrival
        if (cosA < 0.2f)
            target = std::fmin(target, 5.0f); // a sharp turn, slowly
    }
    const float accel = target > speed ? 7.0f : 11.0f;
    speed += Clamp(target - speed, -accel * dt, accel * dt);
    const float aimYaw = behind ? std::atan2(-dir.x, -dir.y) : std::atan2(dir.x, dir.y);
    float err = aimYaw - yaw;
    while (err > 3.14159265f)
        err -= 6.2831853f;
    while (err < -3.14159265f)
        err += 6.2831853f;
    const float rate = 1.9f * Clamp(std::fabs(speed) / 4.0f, 0.3f, 1.0f);
    yaw += Clamp(err, -rate * dt, rate * dt);
    const Vec3 nf(std::sin(yaw), std::cos(yaw), 0.0f);
    pos = pos + nf * speed * dt;
    vel = nf * speed;
}

struct CopFakeHost : TestHost {
    bool carUp = false;
    Vec3 carPos, carVel;
    float carYaw = 0.0f; // fwd = (sin yaw, cos yaw, 0)
    float carSpeed = 0.0f;
    float carHealth = 1.0f;
    float cmdGas = 0, cmdBrake = 0, cmdSteer = 0;
    bool cmdSiren = false;
    float struckRamQ = 0, struckShotQ = 0, impactQ = 0;
    float struckWorldQ = 0, struckDriverQ = 0, impactDvQ = 0;
    float displayHealth = 1000.0f; // the last book display the core commanded
    bool burnedOut = false, driverDown = false;
    bool freeze = false; // the stuck sim's wall: the body answers nothing
    int SpawnCopBody(const Vec3& at, float, const Vec3& face) override {
        carUp = true;
        carPos = at;
        carVel = Vec3();
        carSpeed = 0.0f;
        carHealth = 1.0f;
        cmdGas = cmdBrake = cmdSteer = 0.0f;
        const Vec3 d = face - at;
        carYaw = std::atan2(d.x, d.y);
        return 7;
    }
    bool CopBodyReport(int, CopBody* out) override {
        if (!carUp)
            return false;
        out->ok = true;
        out->pos = carPos;
        out->fwd = Vec3(std::sin(carYaw), std::cos(carYaw), 0.0f);
        out->vel = carVel;
        out->health = carHealth;
        out->gas = cmdGas;
        out->brake = cmdBrake;
        out->steer = cmdSteer;
        out->siren = cmdSiren;
        out->struckRam = struckRamQ;
        out->struckShot = struckShotQ;
        out->struckWorld = struckWorldQ;
        out->struckDriver = struckDriverQ;
        out->impactDv = impactDvQ;
        struckRamQ = struckShotQ = struckWorldQ = struckDriverQ = impactDvQ = 0.0f;
        // the body's own drive-state words, the mirror model's: an
        // armed feed reads as the goto mission at the feed's cruise.
        // The 5x update's landing pair, modelled like the bridge's: the
        // narrow char saturates at 127, the float holds the ask whole
        out->mission = feedLive ? 9 : 0;
        out->cruise = (int)std::fmin(feedCruise, 127.0f);
        out->cruiseF = feedCruise;
        return true;
    }
    void CopBodySetHealth(int, float book01) override {
        displayHealth = book01 > 0.0f ? 380.0f + 620.0f * book01 : 90.0f;
    }
    void CopBodyRetire(int, bool burnOut) override {
        carUp = false;
        burnedOut = burnOut;
        driverDown = burnOut; // the unit goes down together, or not at all
    }
    // the feed: the last drive intent the core named - what moves him
    Vec3 feedPt;
    float feedCruise = 0.0f;
    bool feedLive = false;
    void CopBodyDriveTarget(int, const Vec3& at, float cruise) override {
        feedPt = at;
        feedCruise = cruise;
        feedLive = cruise > 0.05f;
    }
    void CopBodyPlace(int, const Vec3& at, float headingDeg) override {
        carPos = at;
        carYaw = headingDeg * 0.0174533f;
        carSpeed = 0.0f;
        carVel = Vec3();
    }
    // the corner probes' own-shell truth: a plain-oracle segment that
    // STARTS at his centre and runs under ~2.2 m reads his own bodywork
    // (that is the geometry the complete update convicted); the
    // skip-veh reading with his own veh removes exactly that shell
    bool LineBlocked(const Vec3& a, const Vec3& b) override {
        if (carUp) {
            Vec3 d0 = a - carPos;
            d0.z = 0.0f;
            Vec3 seg = b - a;
            seg.z = 0.0f;
            if (d0.Length() < 0.2f && seg.Length() < 2.2f)
                return true;
        }
        return TestHost::LineBlocked(a, b);
    }
    bool LineBlockedSkipVeh(const Vec3& a, const Vec3& b, int veh) override {
        if (veh == 7) // his own car is simply not on this line
            return TestHost::LineBlocked(a, b);
        if (veh >= 0 && veh == vehicle)
            return TestHost::LineBlockedSkipVeh(a, b, veh);
        return LineBlocked(a, b);
    }
    // the world-only reading (the plough law's probe verb): solid
    // geometry alone - in this fake, the wall segment; no vehicle's
    // shell stands on it, neither his own bodywork (the prefix in
    // LineBlocked above) nor the player's car (the base host's seat
    // shell, switched off for the call)
    bool LineBlockedWorld(const Vec3& a, const Vec3& b) override {
        const bool keep = ownCarBlocks;
        ownCarBlocks = false;
        const bool r = TestHost::LineBlocked(a, b);
        ownCarBlocks = keep;
        return r;
    }
    int steerFlips = 0;
    float lastSteerSign = 0.0f;
    void CopBodyDrive(int, float gas, float brake, float steer, bool siren) override {
        if (steer != 0.0f && lastSteerSign != 0.0f && (steer > 0.0f) != (lastSteerSign > 0.0f))
            ++steerFlips;
        if (steer != 0.0f)
            lastSteerSign = steer;
        cmdGas = gas;
        cmdBrake = brake;
        cmdSteer = steer;
        cmdSiren = siren;
    }
    float PlayerVehicleImpact() override {
        const float q = impactQ;
        impactQ = 0.0f;
        return q;
    }
    void PlaceCar(const Vec3& at, float yawDeg) {
        carPos = at;
        carYaw = yawDeg * 0.0174533f;
        carSpeed = 0.0f;
        carVel = Vec3();
    }
    void StepCar(float dt) {
        if (!carUp || freeze) {
            carVel = Vec3();
            carSpeed = 0.0f; // the wall stops him whole, driveline too
            return;
        }
        // the feed moves him now (MirrorStep above) - the pedal fields
        // above are recorded, and read by the profile sims, but they
        // are outputs on the real host and inputs nowhere
        MirrorStep(carPos, carYaw, carSpeed, carVel, feedPt, feedCruise, feedLive, dt);
    }
};

// walk a point along a road-route polyline by arc distance
struct RoadWalker {
    std::vector<Vec3> pts;
    std::vector<float> cum; // cumulative length at each point
    float total = 0.0f;
    void Build(const std::vector<Vec3>& p) {
        pts = p;
        cum.assign(pts.size(), 0.0f);
        for (size_t i = 1; i < pts.size(); ++i) {
            Vec3 d = pts[i] - pts[i - 1];
            d.z = 0.0f;
            total += d.Length();
            cum[i] = total;
        }
    }
    Vec3 PosAt(float s, Vec3* dirOut) const {
        if (pts.empty())
            return Vec3();
        s = Clamp(s, 0.0f, total);
        size_t i = 1;
        while (i < cum.size() && cum[i] < s)
            ++i;
        if (i >= pts.size())
            i = pts.size() - 1;
        const float segLen = cum[i] - cum[i - 1];
        const float t = segLen > 0.001f ? (s - cum[i - 1]) / segLen : 0.0f;
        Vec3 d = pts[i] - pts[i - 1];
        d.z = 0.0f;
        if (dirOut)
            *dirOut = d.Length() > 0.001f ? d * (1.0f / d.Length()) : Vec3(0, 1, 0);
        return pts[i - 1] + (pts[i] - pts[i - 1]) * t;
    }
    float NearestDist(const Vec3& p) const {
        float best = 0.0f, bestD = 1e18f;
        for (size_t i = 1; i < pts.size(); ++i) {
            Vec3 a = pts[i - 1], b = pts[i];
            Vec3 ab = b - a;
            ab.z = 0.0f;
            const float L2 = ab.x * ab.x + ab.y * ab.y;
            float t = L2 > 0.001f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / L2 : 0.0f;
            t = Clamp(t, 0.0f, 1.0f);
            const Vec3 q = a + ab * t;
            const float d = (p - q).Length();
            if (d < bestD) {
                bestD = d;
                best = cum[i - 1] + std::sqrt(L2) * t;
            }
        }
        return best;
    }
};

static void TestCop() {
    CopFakeHost host;
    SetHost(&host);
    WideStage();
    CopReset();
    // the shared threat record is staged, not inherited: an earlier
    // test's boss fight must not leak into the spike's quiet world (in
    // the rounds before the complete update the leak sat hidden - the
    // spike-skip law let an informed summon's felony 659 ride along
    // unnoticed; the anchor-transition restore made honesty necessary)
    ThreatNoteBoss(-1, false, Vec3(), false);
    CHECK(LoadVehicleGraphFromRepo(), "the cop sims ride the real vehicle graph");
    const float dt = 0.02f;
    const Vec3 grove(2486.0f, -1654.0f, 13.5f);
    std::vector<Vec3> roadPts;
    CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(1690, -2280, 13), &roadPts, nullptr) && roadPts.size() > 4,
          "the Grove-to-airport road exists for the chase sims");
    RoadWalker road;
    road.Build(roadPts);

    // ---- piece 0's spike, end to end on the fake body: calibration flips
    // the inverted sign, the block route runs, the body stops at its end
    {
        host.PlaceCar(grove, 0.0f); // staging only; the summon re-stands him
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        host.hasPlayer = true;
        host.player = man;
        host.vehicle = -1;
        host.velocity = Vec3();
        CHECK(CopSummon(grove, man), "the cop's body stands up");
        const Vec3 start = host.carPos;
        for (int i = 0; i < 60 * 50 && !gCop.spikeDone; ++i) {
            CopTick(dt, man);
            host.StepCar(dt);
        }
        CHECK(gCop.spikeDone, "the spike route ran to its end");
        // the complete update re-reads this expectation: the calibration
        // commands a pedal steer and reads the turn - but under the
        // mirror model (as on the real host since round 4) the pedals
        // move nothing, so the calibration second drives straight on
        // its feed and concludes NOT MEASURABLY, flip unset. What the
        // spike must still prove is that it RUNS and concludes; the
        // sign itself is now only the drawn wheels' concern.
        CHECK(gCop.steerCalDone,
              "steer calibration ran and concluded (flip %d - an autopilot-driven body gives it nothing to measure)",
              (int)gCop.steerFlip);
        CHECK((host.carPos - start).Length() > 120.0f,
              "the body actually drove the block (%.0f m)", (host.carPos - start).Length());
        CHECK(gCop.mode == COP_IDLE, "after the spike he stands by, felony watching");
    }

    // ---- (a) pursuit: a fleeing car on the real road graph - the field
    // brings him down the road, the intercept takes over, distance closes
    // ---- (f) bail-out mid-chase: the hunt stays on the MAN
    // ---- (g) a fresh car mid-chase: the swap cleanses nothing
    {
        float copS = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CopReset();
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the chase cop stands up");
        // the spike would drive him off; this sim prices the chase, so the
        // block is skipped the way a second summon in a live chase would
        // find him: straight to standby with the calibration already his
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true; // this fake body is inverted, as calibrated above
        gCop.spikeDone = true;
        float manS = copS + 25.0f;
        host.vehicle = 42;
        auto runChase = [&](float seconds, float manSpeed, bool onFoot) {
            float minDist = 1e9f;
            bool pursuedThroughout = true;
            for (float t = 0.0f; t < seconds; t += dt) {
                manS += manSpeed * dt;
                Vec3 md;
                const Vec3 mp = road.PosAt(manS, &md);
                host.velocity = md * manSpeed;
                host.player = mp;
                CopTick(dt, mp);
                host.StepCar(dt);
                host.carPos.z = road.PosAt(road.NearestDist(host.carPos), &md).z;
                // the fake integrates x/y only; the real body rides the
                // ground, so its z is the road's z under it - without
                // this the original's own 5.9 m sight-height gate blinds
                // him on every ramp, which is the fake lying, not him
                const float d = Dist2D(host.carPos, mp);
                if (d < minDist)
                    minDist = d;
                if (t > 1.0f && gCop.mode != COP_PURSUE)
                    pursuedThroughout = false;
            }
            return std::pair<float, bool>(minDist, pursuedThroughout);
        };
        // ignition: 25 m ahead, 35 m/s past him - speeding, seen, noted
        {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            host.velocity = md * 35.0f;
            CopTick(dt, mp);
            host.StepCar(dt);
        }
        CHECK(gCop.felony >= 659.0f, "speeding past him is noted at the placid value (felony %.0f)", gCop.felony);
        // the chase itself at 20 m/s of flight
        const auto chase = runChase(30.0f, 20.0f, false);
        CHECK(chase.second, "the pursuit never stood down during the chase");
        CHECK(chase.first < 13.0f, "he closed on the fleeing car (closest %.1f m)", chase.first);
        CHECK(gThreat.copPursuing, "the shared record carries his pursuit for the boss");
        // (b) batter: felony at the ladder's top - the target leaves the
        // tail and sits on the man's flank
        {
            gCop.felony = 3000.0f;
            bool flankSeen = false;
            for (float t = 0.0f; t < 2.0f; t += dt) {
                manS += 15.0f * dt;
                Vec3 md;
                const Vec3 mp = road.PosAt(manS, &md);
                host.velocity = md * 15.0f;
                CopTick(dt, mp);
                host.StepCar(dt);
                const Vec3 rel = gCop.target - mp;
                const float side = rel.x * md.y + rel.y * -md.x; // his TRUE right
                if (gCop.batter && side > 1.2f)
                    flankSeen = true;
            }
            CHECK(flankSeen, "batter mode puts his aim on the flank, not the tail");
            gCop.felony = 700.0f;
            bool tailSeen = false;
            for (float t = 0.0f; t < 2.0f; t += dt) {
                manS += 15.0f * dt;
                Vec3 md;
                const Vec3 mp = road.PosAt(manS, &md);
                host.velocity = md * 15.0f;
                CopTick(dt, mp);
                host.StepCar(dt);
                const Vec3 rel = gCop.target - mp;
                const float along = rel.x * md.x + rel.y * md.y;
                if (!gCop.batter && along < -1.2f)
                    tailSeen = true;
            }
            CHECK(tailSeen, "at ordinary felony the CarTail is behind him again");
        }
        // (c) stuck: the body frozen mid-chase - the reverse recovery
        // fires on the original's clock, and frees him when the wall
        // goes. THE 5x UPDATE restages the act on a FRESH anchor (the
        // stageBlastCop discipline): the (b) acts leave a force
        // behind them, and at 5x leave speeds its stood-down units
        // ping out inside two seconds and retire this fake's ONE
        // shared body from under the anchor mid-staging - the first
        // 5x run's log reads the anchor's body gone and its brain
        // stood down before the freeze could even be staged. The
        // recovery pin is about one unit's witness, so it gets one
        // unit, summoned clean, with the chase proven live first
        {
            CopReset();
            float copSc = road.NearestDist(grove) + 30.0f;
            Vec3 dir;
            const Vec3 copAt = road.PosAt(copSc, &dir);
            host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
            CHECK(CopSummon(copAt, road.PosAt(copSc + 25.0f, nullptr)), "the stuck cop stands up");
            gCop.mode = COP_IDLE;
            gCop.steerCalDone = true;
            gCop.steerFlip = true;
            gCop.spikeDone = true;
            gCop.felony = 2000.0f;
            host.vehicle = 42;
            host.velocity = Vec3();
            const Vec3 chaseMan = road.PosAt(copSc + 18.0f, nullptr);
            host.player = chaseMan;
            CopTick(dt, chaseMan);
            host.StepCar(dt);
            CHECK(gCop.mode == COP_PURSUE, "the freeze is staged from a live chase");
            host.freeze = true;
            bool recoverySeen = false;
            for (float t = 0.0f; t < 4.0f && !recoverySeen; t += dt) {
                CopTick(dt, chaseMan);
                host.StepCar(dt);
                if (gCop.recoveryT > 0.0f)
                    recoverySeen = true;
            }
            CHECK(recoverySeen, "frozen against the wall, the recovery clock fired");
            host.freeze = false; // freed WHILE the recovery still runs:
                                 // its reverse is what the next check prices.
                                 // THE 5x UPDATE grades the completion on
                                 // the core's OWN criterion, read off this
                                 // unit's state - not the fake body's
                                 // speed: felony 2000 fills the ladder
                                 // mid-act and the ladder's feeds share
                                 // this one body, so the body's speed is
                                 // the last feed written (the first 5x
                                 // run read +5.5 m/s forward off it while
                                 // the core's own log printed the anchor
                                 // "backed 37.8 m clear"). The criterion
                                 // is the completion line's own: the
                                 // recovery clock runs out AND the unit
                                 // stands >= 1.5 m from recoveryFrom, the
                                 // point the witness fire recorded
            const Vec3 recFrom = gCop.recoveryFrom;
            bool reversed = false;
            for (float t = 0.0f; t < 2.0f; t += dt) {
                CopTick(dt, chaseMan);
                host.StepCar(dt);
                if (gCop.recoveryT <= 0.0f && (gCop.pos - recFrom).Length() >= 1.5f)
                    reversed = true;
            }
            CHECK(reversed, "the recovery completes on the core's own criterion (he stands %.1f m from where it began)",
                  (gCop.pos - recFrom).Length());
        }
        // (f) bail-out: the man leaves the car and runs - the hunt stays
        // on HIM: no stand-down, no felony mercy, and the car reaches him.
        // Staged mid-chase, as the amendment names it: driving at him,
        // 25 m back, pursuit live - THEN he bails.
        {
            // staged on the road itself: the cop placed behind the man's
            // mark, facing down it - the (a) staging, reused honestly
            manS = road.NearestDist(host.carPos) + 60.0f;
            Vec3 cd;
            const Vec3 copAt2 = road.PosAt(manS - 25.0f, &cd);
            host.PlaceCar(copAt2, std::atan2(cd.x, cd.y) * 57.29578f);
            host.vehicle = 42;
            const auto warm = runChase(2.0f, 18.0f, false);
            CHECK(warm.second, "the bail-out is staged from a live chase");
            host.vehicle = -1;
            const auto foot = runChase(9.0f, 6.0f, true);
            CHECK(foot.second, "on foot, the pursuit never stood down");
            CHECK(foot.first < 2.5f, "the car reached the man himself (closest %.1f m) - no mercy bubble", foot.first);
            CHECK(gCop.felony > 658.0f, "no felony decay merely for being on foot (%.0f)", gCop.felony);
        }
        // (g) the swap: a fresh car, mid-chase - felony and pursuit ride
        // straight onto it; a swap cleanses nothing
        {
            host.vehicle = 99;
            const float felonyAtSwap = gCop.felony;
            const auto swap = runChase(10.0f, 15.0f, false);
            CHECK(swap.second, "in a fresh car, the pursuit never stood down");
            CHECK(gCop.felony >= felonyAtSwap && gCop.felony > 658.0f,
                  "the felony crossed the swap intact (%.0f -> %.0f)", felonyAtSwap, gCop.felony);
            CHECK(swap.first < 16.0f, "he closed on the swapped car too (closest %.1f m)", swap.first);
        }
    }

    // ---- (d) the felony lifecycle: noted, pursuit, the position lost,
    // the slow bleed at and under the trigger (the original's own rates)
    {
        CopReset();
        host.PlaceCar(grove, 180.0f);
        const Vec3 man = grove + Vec3(0.0f, -20.0f, 0.0f);
        CHECK(CopSummon(grove, man), "the felony cop stands up");
        host.vehicle = 42;
        host.velocity = Vec3(0.0f, -35.0f, 0.0f);
        CopTick(dt, man);
        CHECK(gCop.felony >= 659.0f, "the speeding note landed whole (%.0f)", gCop.felony);
        // finish the spike with the man parked at the summon spot
        host.vehicle = -1;
        host.velocity = Vec3();
        for (int i = 0; i < 60 * 50 && !gCop.spikeDone; ++i) {
            CopTick(dt, man);
            host.StepCar(dt);
        }
        // seen again at standby range: the pursuit opens
        const Vec3 fwd(std::sin(host.carYaw), std::cos(host.carYaw), 0.0f);
        const Vec3 nearMan = host.carPos + fwd * 15.0f;
        host.vehicle = 42;
        CopTick(dt, nearMan);
        CHECK(gCop.mode == COP_PURSUE, "over the trigger and seen: he pursues");
        // gone: 400 m off, on foot, still - the original's 256 ticks lose
        // the position and the driving stands down; the felony itself does
        // NOT bleed above the trigger (AdjustFelony's own arithmetic)
        const Vec3 gone = host.carPos + Vec3(400.0f, 0.0f, 0.0f);
        host.vehicle = -1;
        for (float t = 0.0f; t < 6.0f; t += dt) {
            CopTick(dt, gone);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_IDLE, "256 unseen ticks: position lost, pursuit stood down");
        CHECK(gCop.felony >= 658.0f, "above the trigger the felony does not bleed (%.0f)", gCop.felony);
        gCop.felony = 600.0f;
        for (float t = 0.0f; t < 3.1f; t += dt) {
            CopTick(dt, gone);
            host.StepCar(dt);
        }
        CHECK(gCop.felony <= 596.0f, "at the trigger the felony bleeds 1 per 31 ticks (%.0f)", gCop.felony);
    }

    // ---- (h) his survival book: priced on the original's scale, the SA
    // body only displays it. An RPG-priced hit is expensive, not death;
    // bullets retire him only at the book's own total; fire drains at
    // the priced rate and no faster.
    // ---- (i) the one-unit law: a headshot-priced hit on the DRIVER
    // lands in the same book and the driver does not die.
    {
        CopReset();
        host.burnedOut = false;
        host.driverDown = false;
        float copS = road.NearestDist(Vec3(2480, -1660, 13)) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the survival cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        float manS = copS + 25.0f;
        host.vehicle = 42;
        auto tickChase = [&](float seconds) {
            for (float t = 0.0f; t < seconds; t += dt) {
                manS += 15.0f * dt;
                Vec3 md;
                const Vec3 mp = road.PosAt(manS, &md);
                host.velocity = md * 15.0f;
                host.player = mp;
                CopTick(dt, mp);
                host.StepCar(dt);
            }
        };
        {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            host.velocity = md * 35.0f;
            CopTick(dt, mp);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE || gCop.felony >= 659.0f, "the survival chase is live");
        // an RPG-priced blast: 300 of the host's points at the anchor = 8100 of his
        host.struckShotQ = 300.0f;
        tickChase(0.1f);
        CHECK(gCop.totalDamage > 7900.0f && gCop.totalDamage < 8300.0f,
              "the RPG hit billed his book at the anchor (%.0f)", gCop.totalDamage);
        CHECK(CopActive() && !host.burnedOut, "one RPG does not retire him - he fights on");
        CHECK(host.displayHealth > 700.0f && host.displayHealth <= 1000.0f,
              "the display rides his book, above the city's burn band (%.0f)", host.displayHealth);
        // (i) a headshot-priced hit on the driver: 100 of his points bill
        // the unit's book; the driver himself does not die
        const float bookBefore = gCop.totalDamage;
        host.struckDriverQ = 100.0f;
        tickChase(0.1f);
        CHECK(gCop.totalDamage - bookBefore > 2600.0f && gCop.totalDamage - bookBefore < 2800.0f,
              "the driver hit billed the unit's book (%.0f)", gCop.totalDamage - bookBefore);
        CHECK(CopActive() && !host.driverDown, "the driver does not die on his own - one unit");
        // fire: five seconds of the world's drain at 25 host points a
        // second bills exactly the anchor rate, no faster
        {
            const float before = gCop.totalDamage;
            for (int i = 0; i < 250; ++i) {
                host.struckWorldQ += 0.5f; // 25/s at dt
                tickChase(dt);
            }
            const float billed = gCop.totalDamage - before;
            CHECK(billed > 3300.0f && billed < 3450.0f,
                  "five seconds of fire billed at the priced rate (%.0f)", billed);
            CHECK(CopActive(), "the fire interval did not retire him early");
        }
        // sustained bullets: 30 host points a tick retires him only when
        // the book itself crosses its line - and then he burns, as one
        // unit. THE 5x UPDATE: the line is 135000 now (the prices that
        // fill it never moved), so the same bullet stream needs ~150
        // ticks past this block's earlier damage, not ~35. THE
        // 39-PACKAGE restages the reading: item 33 jumped this block's
        // felony to the cap at its first blast, and item 16's handover
        // now refills a fallen pursuer's slot inside the very tick he
        // falls - so the retirement is read off the host's burn flags
        // (they land with the fall itself), and the line's exactness
        // is pinned separately below, on a stage where nothing can
        // ram and nothing can refill
        {
            int ticks = 0;
            while (!host.burnedOut && ticks < 400) {
                host.struckShotQ = 30.0f;
                tickChase(dt);
                ++ticks;
            }
            CHECK(host.burnedOut && host.driverDown,
                  "sustained bullets retired him, burning as one unit");
            CHECK(ticks > 60, "he did not retire early under the bullet stream (ticks %d)", ticks);
        }
        // the line itself, exact: a fresh anchor, the man 500 m off
        // (no contact is possible inside the window, so every point in
        // the book is a bullet point), the same 30-points-a-tick
        // stream. The book sampled after every tick must stand at or
        // under the line until the burn tick - and the burn tick's own
        // 810 must be the step that crosses it
        {
            CopReset();
            host.burnedOut = false;
            host.driverDown = false;
            host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
            CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the book-line cop stands up");
            gCop.mode = COP_IDLE;
            gCop.steerCalDone = true;
            gCop.steerFlip = true;
            gCop.spikeDone = true;
            host.vehicle = 42;
            host.velocity = Vec3();
            const Vec3 farMan = road.PosAt(copS + 500.0f, nullptr);
            host.player = farMan;
            float peak = 0.0f;
            int ticks = 0;
            while (!host.burnedOut && ticks < 400) {
                host.struckShotQ = 30.0f;
                CopTick(dt, farMan);
                host.StepCar(dt);
                ++ticks;
                if (!host.burnedOut)
                    peak = gCop.totalDamage;
            }
            CHECK(host.burnedOut && host.driverDown,
                  "the distant bullet stream retired him too, as one unit");
            CHECK(peak <= 135000.0f && peak + 810.0f > 135000.0f,
                  "the book crossed its 135000 line on a bullet tick, never early (peak %.0f)", peak);
        }
    }

    // ---- (j) fix round 3: the blast cap. One frame can strip the host
    // body whole (a point-blank RPG takes it 1000 -> 0 in the host's own
    // arithmetic); billed raw at the anchor that is the entire book in
    // one tick - the eye test's one-RPG death, logged at 27503. The cap
    // makes one tick bill at most one blast-class price (8100) across
    // the shot, world and driver channels together: an RPG costs what
    // the design always said (~8100), three-plus of them retire him,
    // and a blast that hits the car and the driver in the same frame
    // is still ONE blast.
    {
        auto stageBlastCop = [&]() {
            CopReset();
            host.burnedOut = false;
            host.driverDown = false;
            float copS = road.NearestDist(Vec3(2480, -1660, 13)) + 30.0f;
            Vec3 dir;
            const Vec3 copAt = road.PosAt(copS, &dir);
            host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
            CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the blast-cap cop stands up");
            gCop.mode = COP_IDLE;
            gCop.steerCalDone = true;
            gCop.steerFlip = true;
            gCop.spikeDone = true;
            host.vehicle = 42;
            Vec3 md;
            const Vec3 mp = road.PosAt(copS + 25.0f, &md);
            host.velocity = md * 35.0f;
            host.player = mp;
            CopTick(dt, mp);
            host.StepCar(dt);
        };
        auto oneTick = [&]() {
            Vec3 md;
            // THE 39-PACKAGE: the man is re-placed 60 m ahead, not 25 -
            // item 33 puts this block's felony at the cap from the
            // first blast, the anchor batters at full pace, and at
            // 25 m he closed and rammed between blasts, polluting the
            // per-blast deltas this block pins to the point. At 60 m
            // re-placed every tick, no contact is ever made and every
            // point in the book is a blast point
            const Vec3 mp = road.PosAt(road.NearestDist(host.carPos) + 60.0f, &md);
            host.velocity = md * 15.0f;
            host.player = mp;
            CopTick(dt, mp);
            host.StepCar(dt);
        };
        stageBlastCop();
        // THE 5x UPDATE's arithmetic, pinned whole: seventeen
        // full-strip frames - the first sixteen bill one blast each
        // (16 x 8100 = 129600 of the 135000 book) and he fights on;
        // the seventeenth crosses the deepened line. Before the
        // update the same frames retired him at the fourth; an
        // RPG-class blast still bills its one 8100 price - the book
        // is five times deeper, the prices never moved
        for (int hit = 1; hit <= 17; ++hit) {
            const float before = gCop.totalDamage;
            host.struckShotQ = 1019.0f; // the host body stripped whole, and below
            oneTick();
            if (hit < 17) {
                CHECK(std::fabs(gCop.totalDamage - before - 8100.0f) < 1.0f,
                      "blast %d billed one blast-class price, not the raw frame (%.0f)",
                      hit, gCop.totalDamage - before);
                CHECK(CopActive() && !host.burnedOut, "blast %d of 17 does not retire him", hit);
            }
        }
        // THE 39-PACKAGE: the seventeenth blast's own delta can no
        // longer be read off the slot - item 16's handover refills a
        // fallen pursuer inside his fall tick, so by the time the sim
        // looks, the slot may already hold his replacement with a
        // fresh book. The retirement's witness is the burn itself,
        // and the slot shows either the wreck or the fresh handover
        CHECK(host.burnedOut,
              "the seventeenth blast retires him - ~17 RPG-class hits at the 5x book, never one");
        CHECK(gCop.mode == COP_WRECKED || gCop.totalDamage < 8100.0f,
              "the fallen book is spent: the slot holds the wreck or his fresh replacement");
        // a blast that strips car and driver in the SAME frame is one blast
        stageBlastCop();
        {
            const float before = gCop.totalDamage;
            host.struckShotQ = 1000.0f;
            host.struckDriverQ = 1000.0f;
            oneTick();
            CHECK(std::fabs(gCop.totalDamage - before - 8100.0f) < 1.0f,
                  "car and driver stripped together bill one blast (%.0f)",
                  gCop.totalDamage - before);
            CHECK(CopActive() && !host.burnedOut, "he survives the combined blast");
        }
        // the driver channel alone, stripped whole, is likewise one blast
        stageBlastCop();
        {
            const float before = gCop.totalDamage;
            host.struckDriverQ = 1019.0f;
            oneTick();
            CHECK(std::fabs(gCop.totalDamage - before - 8100.0f) < 1.0f,
                  "the driver stripped whole bills one blast (%.0f)",
                  gCop.totalDamage - before);
            CHECK(CopActive(), "the unit is not the driver's life - he drives on");
        }
    }

    // ---- (e) shared knowledge: the boss in combat, a fresh cop starts
    // informed; the record is one, read both ways
    {
        CopReset();
        WideStage();
        CHECK(LoadSoldierBinFromRepo(), "the shared-knowledge proof meets his real model");
        const int s = SpawnMob(MOB_SOLDIER, Vec3(0.5f, 0.5f, 10.0f), true);
        CHECK(s >= 0, "the boss stands for the shared-knowledge proof");
        FaceMob(s, 0.0f);
        const Vec3 man(0.5f, 16.5f, 11.0f);
        const std::vector<BeingInfo> noBeings;
        ProvokeByAim(host, man, &noBeings);
        bool engaged = false;
        for (int i = 0; i < 400 && !engaged; ++i) {
            MobsTick(0.05f, man, 0, true, &noBeings);
            engaged = gThreat.bossEngaged;
        }
        CHECK(engaged, "the boss's fight is on the shared record");
        const Vec3 copAt(8.5f, 0.5f, 10.0f);
        CHECK(CopSummon(copAt, man), "a cop summoned mid-fight stands up");
        CHECK(gCop.informed && gCop.felony == 659.0f,
              "he starts informed by the record, at the fight's own felony (informed %d, felony %.0f)",
              (int)gCop.informed, gCop.felony);
        MobsClear();
    }

    // ---- (w) piece 3: his own constants from the disc - the decoded
    // CHICAGO.LCF row (external model 0, the cop) rides the profile, and
    // his power coefficient gains the pursuit pedal by exactly its ratio
    {
        const CopProfileData& prof = CopProfile();
        CHECK(prof.powerRatio == 4096 && prof.traction == 4096 && prof.susCoeff == 4096 &&
                  prof.mass == 4096 && prof.wheelSize == 49 && prof.frictionScaleRatio == 68,
              "the profile is the decoded row: power/traction/sus/mass 4096, wheel 49, friction 68");
        CHECK(std::fabs(prof.wheelDisp[0][2] - 222.0f / 153.6f) < 1e-4f &&
                  std::fabs(prof.wheelDisp[1][2] + 207.0f / 153.6f) < 1e-4f &&
                  std::fabs(prof.wheelDisp[0][2] - prof.wheelDisp[1][2] - 429.0f / 153.6f) < 1e-4f,
              "his wheels sit at the post-FixCarCos placements (wheelbase %.3f m)",
              prof.wheelDisp[0][2] - prof.wheelDisp[1][2]);
        // fix round 3: the geometry factor itself is pinned - at the
        // convicted 153.6 units = 1 m his wheelbase is a real sedan's
        // 2.79 m and his collision cage is 4.97 m long; at the old 256
        // both came out at 0.6x (1.68 m / 2.98 m), the drawn body's
        // "much smaller than the other cars" in numbers
        CHECK(prof.wheelDisp[0][2] - prof.wheelDisp[1][2] > 2.7f &&
                  prof.wheelDisp[0][2] - prof.wheelDisp[1][2] < 2.9f,
              "his wheelbase is a real car's (%.3f m)",
              prof.wheelDisp[0][2] - prof.wheelDisp[1][2]);
        CHECK(std::fabs(prof.colBox[2] - 382.0f / 153.6f) < 1e-4f &&
                  2.0f * prof.colBox[2] > 4.8f && 2.0f * prof.colBox[2] < 5.1f,
              "his collision cage is a real car's length (%.3f m)", 2.0f * prof.colBox[2]);
        CHECK(prof.cog[1] < -0.6f && prof.cog[1] > -0.65f && prof.cog[2] == 0.0f,
              "his cog is the data's own (0, -95, 0) - FixCarCos never touches it");
        CHECK(CopProfilePowerGain() == 1.0f,
              "his power coefficient is the reference 1.0 - the gain is exactly one");
        CHECK(std::fabs(CopProfileGrip() - 2.125f) < 1e-6f,
              "his grip is (68/32) x (4096/4096) = 2.125 (%.4f)", CopProfileGrip());
        CHECK(std::fabs(CopProfilePedal(0.457f) - 0.457f) < 1e-6f,
              "at the decoded row the pedal passes through ungained");
        CopProfileOverrideForTest(2666); // the weakest Chicago car's row
        CHECK(std::fabs(CopProfilePowerGain() - 2666.0f / 4096.0f) < 1e-6f,
              "a 2666 row gains the pedal by exactly 2666/4096 (%.4f)", CopProfilePowerGain());
        CHECK(std::fabs(CopProfilePedal(0.457f) - 0.457f * 2666.0f / 4096.0f) < 1e-5f,
              "the pedal scales by the decoded ratio (%.4f)", CopProfilePedal(0.457f));
        CopProfileOverrideForTest(4500); // the strongest row
        CHECK(CopProfilePedal(0.95f) == 1.0f,
              "a 4500 row at near-full pedal clamps at his own ceiling, never above");
        CopProfileOverrideForTest(-1);
        CHECK(CopProfilePowerGain() == 1.0f, "the override restores the decoded row");

        // in the drive itself: close pursuit of a stationary man on foot,
        // the cop staged mid-band (error 8 of his units = 1.45 m/s under
        // the cornering pace) - the gas on the fake body is the law's own
        // arithmetic, gained by the profile and nothing else
        CopReset();
        float copS = road.NearestDist(Vec3(2480, -1660, 13)) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the profile cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        const float manS = copS + 25.0f;
        host.vehicle = 42;
        {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            host.velocity = md * 35.0f;
            CopTick(dt, mp);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE, "the profile chase is live");
        auto stagedGas = [&]() {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            const Vec3 cp = road.PosAt(manS - 8.0f, &md);
            host.PlaceCar(cp, std::atan2(md.x, md.y) * 57.29578f);
            host.carVel = md * (1085.0f * (25.0f / 138.0f) - 1.45f); // the 5x speed1 - 8 units
            host.carSpeed = host.carVel.Length();
            host.vehicle = -1; // the man on foot: no pacing blend
            host.velocity = Vec3();
            host.player = mp;
            CopTick(dt, mp);
            return host.cmdGas;
        };
        const float gasHis = stagedGas();
        CHECK(std::fabs(gasHis - 8.0f / 17.5f) < 0.03f,
              "his own row drives the band pedal (gas %.3f, law %.3f)", gasHis, 8.0f / 17.5f);
        CopProfileOverrideForTest(2666);
        const float gasWeak = stagedGas();
        CHECK(std::fabs(gasWeak - gasHis * 2666.0f / 4096.0f) < 0.03f,
              "a weaker row pulls exactly its ratio in the drive (gas %.3f vs %.3f)",
              gasWeak, gasHis * 2666.0f / 4096.0f);
        CopProfileOverrideForTest(-1);
    }

    // ---- (k) fix round 5: the victim-knows law reads EVERY damage
    // channel, the stuck witness waits out a fresh drive order, and the
    // autopilot feed is a driving point, never an arrival
    {
        // (k1) the driver channel: the man shoots the DRIVER out of the
        // car while standing where the cop's eyes cannot hold him -
        // before this round that damage billed the book in silence and
        // the page printed "position lost" over the dying car
        CopReset();
        float copS = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        host.vehicle = -1; // on foot
        host.velocity = Vec3();
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the round-5 cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        // the staging starts from a clean page: an earlier block's
        // shared-knowledge state can leave a summon informed, and an
        // informed anchor at felony 659 would pursue through the whole
        // control window and wander off the staged ground
        gCop.felony = 0.0f;
        gCop.known = false;
        gCop.unseenT = 0.0f;
        const Vec3 nearMan = road.PosAt(copS + 8.0f, nullptr);
        const Vec3 farMan = road.PosAt(copS + 300.0f, nullptr);
        CopTick(dt, nearMan); // one close look: the position is known
        host.StepCar(dt);
        CHECK(gCop.known, "seen up close, the position is known");
        for (float t = 0.0f; t < 6.0f; t += dt) { // the control: silence
            CopTick(dt, farMan);                  // loses the position
            host.StepCar(dt);
        }
        CHECK(!gCop.known, "six unseen seconds at 300 m: position lost (the control)");
        CopTick(dt, nearMan);
        host.StepCar(dt);
        CHECK(gCop.known, "one close look re-knows the position");
        for (float t = 0.0f; t < 3.0f; t += dt) {
            host.struckDriverQ = 5.0f; // his bullets are in the driver
            CopTick(dt, farMan);       // while he stands 300 m off
            host.StepCar(dt);
        }
        CHECK(gCop.known, "while the driver is being shot, position-lost is impossible");
        CHECK(gCop.felony >= 659.0f, "hitting the driver is offence 5, noted whole (felony %.0f)",
              gCop.felony);
        CHECK(gCop.mode == COP_PURSUE, "the pursuit opens on the victim's own word");
        // (k2) the fair start: the pursuit cycles down and back up with
        // the body frozen - inside the window a frozen car is not yet
        // stuck evidence; past it, the original's witness fires
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        const Vec3 chaseMan = road.PosAt(copS + 40.0f, nullptr);
        host.vehicle = 42;
        gCop.felony = 0.0f;
        CopTick(dt, chaseMan); // the case collapses: he stands down
        host.StepCar(dt);
        CHECK(gCop.mode == COP_IDLE, "the staged stand-down lands before the re-entry");
        host.freeze = true; // the wall: the body answers nothing
        gCop.felony = 2000.0f;
        const int stratBefore = gCop.recoverStrategy;
        for (float t = 0.0f; t < 1.0f; t += dt) {
            CopTick(dt, chaseMan);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE, "the pursuit re-opened on the frozen body");
        CHECK(gCop.recoveryT == 0.0f && gCop.recoverStrategy == stratBefore,
              "inside the fair start, stillness is not stuck evidence yet");
        bool fired = false;
        for (float t = 0.0f; t < 1.5f && !fired; t += dt) {
            CopTick(dt, chaseMan);
            host.StepCar(dt);
            fired = gCop.recoveryT > 0.0f || gCop.recoverStrategy != stratBefore;
        }
        CHECK(fired, "past the fair start, a truly wedged car still reverses out");
        host.freeze = false;
        // (k3) the feed: far, the autopilot's point is tens of metres
        // down the road; close, it is projected through the prey while
        // the brain's own target stays the true aim
        CopReset();
        gThreat.bossEngaged = true; // the informed summon: known at once
        float copS3 = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dir3;
        const Vec3 copAt3 = road.PosAt(copS3, &dir3);
        host.PlaceCar(copAt3, std::atan2(dir3.x, dir3.y) * 57.29578f);
        CHECK(CopSummon(copAt3, road.PosAt(copS3 + 25.0f, nullptr)), "the feed cop stands up");
        gThreat.bossEngaged = false; // the boss's field, handed back
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        gCop.felony = 2000.0f;
        host.vehicle = 42;
        host.velocity = Vec3();
        const Vec3 farMan3 = road.PosAt(copS3 + 150.0f, nullptr);
        for (float t = 0.0f; t < 0.3f; t += dt) {
            CopTick(dt, farMan3);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE, "the feed pursuit is live");
        CHECK(gCop.feedLive && (gCop.feedPoint - gCop.pos).Length() >= 20.0f,
              "far: the feed is a driving point tens of metres out (%.0f m)",
              (gCop.feedPoint - gCop.pos).Length());
        const float manS3 = road.NearestDist(host.carPos) + 12.0f;
        const Vec3 closeMan = road.PosAt(manS3, nullptr);
        CopTick(dt, closeMan);
        host.StepCar(dt);
        const Vec3 toTarget = gCop.target - gCop.pos;
        const Vec3 pastTarget = gCop.feedPoint - gCop.target;
        CHECK(gCop.feedLive && pastTarget.Dot(toTarget) > 0.0f &&
                  (gCop.feedPoint - gCop.pos).Length() > toTarget.Length(),
              "close: the feed is projected through the prey, past the brain's aim");
    }

    // ---- (l) THE COMPLETE UPDATE's pins, one per conviction the 02:00
    // session's log carried:
    // (l1) the bumper-corner probes read through his own shell now:
    // the fake's PLAIN oracle models the bodywork standing on any short
    // segment out of his centre (the geometry of the conviction), so
    // before the fix both corners read blocked at every range forever
    // and the blocked branch's -21.7 leaked into every desire; with the
    // probes on the skip-veh reading they read the road's truth: clear
    // (l2) the feed contract across a whole chase: while a unit
    // pursues, its feed is live with a POSITIVE cruise on every tick -
    // the core never hands the bridge a dead or negative feed again -
    // and in the far mode the desire itself is the speed rows' word,
    // positive at every range
    // (l3) recovery is served through the feed and COMPLETES: wedged
    // (the frozen body), the recovery that fires names a point BEHIND
    // him at a positive pace; unfrozen, he reverses out on it, moves
    // clear of the wedge, and no second recovery follows inside the
    // fair window
    // (l4) the anchor joins the pursuit: an informed summon (felony
    // 659, position known from the record) still runs the steering
    // calibration - the session's only sign source - and then slot 0
    // leaves the spike MID-ROUTE and pursues, where the old transition
    // loop skipped COP_SPIKE unconditionally and he sat in mode 1 for
    // whole sessions at felony 4096
    {
        // (l1) + (l2) share one chase staging
        CopReset();
        ThreatNoteBoss(-1, false, Vec3(), false);
        host.freeze = false;
        float copSL = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dirL;
        const Vec3 copAtL = road.PosAt(copSL, &dirL);
        host.PlaceCar(copAtL, std::atan2(dirL.x, dirL.y) * 57.29578f);
        CHECK(CopSummon(copAtL, road.PosAt(copSL + 25.0f, nullptr)), "the (l) cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = false;
        gCop.spikeDone = true;
        gCop.felony = 2000.0f;
        host.vehicle = 42;
        // the model's premise, so the pin is not vacuous: the plain
        // oracle DOES read his own bodywork on the probe's segment
        {
            Vec3 right(std::cos(0.0f), 0.0f, 0.0f);
            const Vec3 base = gCop.pos + gCop.fwd * 1.5625f;
            (void)right;
            CHECK(host.LineBlocked(gCop.pos, base),
                  "the fake's plain oracle reads his own bodywork on the probe segment (the conviction's geometry)");
        }
        float manSL = copSL + 55.0f;
        int feedViol = 0, farNeg = 0, probeBad = 0, ticks = 0;
        for (float t = 0.0f; t < 25.0f; t += dt) {
            manSL += 15.0f * dt;
            Vec3 md;
            const Vec3 mp = road.PosAt(manSL, &md);
            host.player = mp;
            host.velocity = md * 15.0f;
            CopTick(dt, mp);
            host.StepCar(dt);
            if (gCop.mode == COP_PURSUE) {
                ++ticks;
                if (!gCop.feedLive || gCop.feedCruise <= 0.0f)
                    ++feedViol;
                if ((mp - gCop.pos).Length() > 40.0f && gCop.desiredSpeed <= 0.0f)
                    ++farNeg;
                if (!gCop.frontLClear || !gCop.frontRClear)
                    ++probeBad;
            }
        }
        CHECK(ticks > 500, "the (l) chase ran as a pursuit (%d ticks)", ticks);
        CHECK(feedViol == 0, "in pursuit the feed is live with a positive cruise on every tick (%d violations)", feedViol);
        CHECK(farNeg == 0, "in the far mode the desire is positive at every range (%d negative samples)", farNeg);
        CHECK(probeBad == 0, "the corner probes read the road, not his own bodywork (%d blocked samples)", probeBad);

        // (l3) recovery through the feed - staged from a live chase
        // first, for the (c) reason: the 25 s loop above can end
        // stood down at 5x, and the freeze must land on a pursuing
        // body. The man goes back inside the peripheral gate and
        // stays staged there; the pursuit's openness is proven, then
        // the wall
        {
            const Vec3 nose(std::sin(host.carYaw), std::cos(host.carYaw), 0.0f);
            const Vec3 chaseMan = host.carPos + nose * 18.0f;
            host.vehicle = 42;
            host.velocity = Vec3();
            host.player = chaseMan;
            CopTick(dt, chaseMan);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE, "the (l3) freeze is staged from a live chase");
        host.freeze = true;
        const int recBefore = gCop.recoverCount;
        bool recStarted = false;
        for (float t = 0.0f; t < 8.0f && !recStarted; t += dt) {
            CopTick(dt, host.player);
            host.StepCar(dt);
            recStarted = gCop.recoverCount > recBefore;
        }
        CHECK(recStarted, "wedged, the recovery fires");
        // the recovery branch names its feed on the ticks after the
        // start tick (the start tick only sets the clock) - sample a
        // few ticks in, still inside the 22-tick manoeuvre
        for (float t = 0.0f; t < 0.15f; t += dt) {
            CopTick(dt, host.player);
            host.StepCar(dt);
        }
        CHECK(gCop.feedLive && gCop.feedCruise > 0.0f,
              "the recovery is SERVED through the feed (live, cruise %.0f)", gCop.feedCruise);
        {
            Vec3 behind = gCop.feedPoint - gCop.pos;
            behind.z = 0.0f;
            CHECK(behind.Dot(gCop.fwd) < 0.0f, "the recovery's feed point stands behind him");
        }
        const Vec3 wedge = gCop.pos;
        host.freeze = false;
        bool reversed = false, completed = false;
        float movedMax = 0.0f;
        for (float t = 0.0f; t < 6.0f; t += dt) {
            CopTick(dt, host.player);
            host.StepCar(dt);
            if (gCop.speed < -1.0f)
                reversed = true;
            const float mv = (gCop.pos - wedge).Length();
            if (mv > movedMax)
                movedMax = mv;
            if (gCop.recoveryT <= 0.0f && movedMax >= 1.5f)
                completed = true;
        }
        CHECK(completed && movedMax >= 1.5f,
              "the recovery completes and he is out (moved %.1f m; the shared body reversed: %d - on this "
              "one-body fake the ladder's feeds share his body and the last writer owns its direction, so "
              "the reverse's own motion is graded in (c), where the body is his alone)",
              movedMax, (int)reversed);
        const int recAfter = gCop.recoverCount;
        for (float t = 0.0f; t < 3.0f; t += dt) {
            CopTick(dt, host.player);
            host.StepCar(dt);
        }
        CHECK(gCop.recoverCount == recAfter, "no recovery spam after the completion (%d)", gCop.recoverCount);

        // (l4) the anchor leaves the spike for the pursuit
        CopReset();
        gThreat.bossEngaged = true; // the informed summon again
        host.PlaceCar(grove, 0.0f);
        CHECK(CopSummon(grove, Vec3(2496.0f, -1666.0f, 13.5f)), "the anchor stands up informed");
        CHECK(gCop.mode == COP_SPIKE && gCop.felony >= 659.0f, "he starts on the spike, felony live");
        host.vehicle = -1;
        host.velocity = Vec3();
        const Vec3 manL(2496.0f, -1666.0f, 13.5f);
        bool joined = false;
        for (float t = 0.0f; t < 12.0f && !joined; t += dt) {
            CopTick(dt, manL);
            host.StepCar(dt);
            joined = gCop.mode == COP_PURSUE;
        }
        CHECK(gCop.steerCalDone, "the calibration still ran before anything else");
        CHECK(joined && gCop.spikeDone,
              "the anchor leaves the spike and pursues himself (mode %d)", (int)gCop.mode);
        gThreat.bossEngaged = false;
    }

    // ---- (m) THE 5x UPDATE: the momentum channel's arithmetic, pinned
    // directly - the live path in CopTick calls this same function, so
    // the pins grade the shipping law, not a copy of it
    {
        float spin = 0.0f, closing = 0.0f;
        // square-on at speed: closing 40 throws 0.6 of it, no spin
        Vec3 dv = CopKickCompute(Vec3(0, 50, 0), Vec3(0, 10, 0), Vec3(0, 4, 0), &spin, &closing);
        CHECK(std::fabs(dv.Length() - 24.0f) < 0.1f && std::fabs(spin) < 0.001f,
              "square hit: 0.6 of the 40 m/s closing, no spin (dv %.1f, spin %.2f)", dv.Length(), spin);
        CHECK(std::fabs(closing - 40.0f) < 0.1f, "the closing is read off the velocities (%.1f)", closing);
        // the cap, stated not hidden: a 120 m/s closing throws 30
        dv = CopKickCompute(Vec3(0, 120, 0), Vec3(0, 0, 0), Vec3(0, 4, 0), &spin, &closing);
        CHECK(std::fabs(dv.Length() - 30.0f) < 0.1f,
              "a 120 m/s closing throws at the stated 30 m/s cap (%.1f)", dv.Length());
        // a flank hit: 2 m off the line of travel spins, signed by side
        const Vec3 dvL = CopKickCompute(Vec3(0, 50, 0), Vec3(0, 10, 0), Vec3(2, 4, 0), &spin, &closing);
        const float spinL = spin;
        const Vec3 dvR = CopKickCompute(Vec3(0, 50, 0), Vec3(0, 10, 0), Vec3(-2, 4, 0), &spin, &closing);
        CHECK(dvL.Length() > 15.0f && dvR.Length() > 15.0f && spinL < -1.0f && spin > 1.0f,
              "flank hits throw and spin, signed by the side (spin %.2f / %.2f)", spinL, spin);
        // lesser contacts scale smoothly; taps stay taps
        dv = CopKickCompute(Vec3(0, 15.5f, 0), Vec3(0, 10, 0), Vec3(0, 4, 0), &spin, &closing);
        CHECK(std::fabs(dv.Length() - 1.65f) < 0.05f,
              "mid-ramp: closing 5.5 throws 0.6 x 5.5 x 0.5 = 1.65 (%.2f)", dv.Length());
        dv = CopKickCompute(Vec3(0, 12, 0), Vec3(0, 10, 0), Vec3(0, 4, 0), &spin, &closing);
        CHECK(dv.Length() < 0.01f, "a 2 m/s closing is a tap and throws nothing (%.2f)", dv.Length());
        dv = CopKickCompute(Vec3(0, 13, 0), Vec3(0, 10, 0), Vec3(0, 4.7f, 0), &spin, &closing);
        CHECK(dv.Length() < 0.01f, "at the 3 m/s tap threshold the ramp still reads zero (%.2f)", dv.Length());
    }

    // ---- (n) THE 5x UPDATE: the kick fires LIVE in a chase, at the
    // vehicle the man is in, and the unit re-engages at batter pace on
    // the very next feeds - no re-path pause, no recovery
    {
        CopReset();
        ThreatNoteBoss(-1, false, Vec3(), false);
        host.freeze = false;
        float copS = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the kick cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        gCop.felony = 3000.0f; // batter: the flank aim, the full pace
        host.vehicle = 42;
        float manS = copS + 25.0f;
        {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            host.velocity = md * 35.0f;
            host.player = mp;
            CopTick(dt, mp);
            host.StepCar(dt);
        }
        CHECK(gCop.mode == COP_PURSUE, "the kick staging opens in a live pursuit");
        // the contact, staged exactly: the unit 4.5 m behind the man's
        // car (inside the 4.8 m band), rolling 30 against his 10 -
        // closing 20 down the road, square-on
        {
            Vec3 md;
            const Vec3 mp = road.PosAt(manS, &md);
            host.PlaceCar(mp - md * 4.5f, std::atan2(md.x, md.y) * 57.29578f);
            host.carVel = md * 30.0f;
            host.carSpeed = 30.0f;
            host.velocity = md * 10.0f;
            host.player = mp;
            const int kicks0 = host.kickVehCount;
            CopTick(dt, mp);
            host.StepCar(dt);
            CHECK(host.kickVehCount > kicks0 && host.lastKickVeh == 42,
                  "the contact threw the vehicle the man is in (veh %d)", host.lastKickVeh);
            CHECK(host.lastKickDv.Length() > 10.0f && host.lastKickDv.Length() < 13.0f,
                  "the throw is 0.6 of the 20 m/s closing (%.1f m/s)", host.lastKickDv.Length());
            CHECK(host.lastKickDv.Dot(md) > 0.9f * host.lastKickDv.Length(),
                  "the throw runs along the cop's own travel, down the road");
            CHECK(gCop.kickCount >= 1, "the unit's kick book counted the throw (%d)", gCop.kickCount);
            // the re-attack, graded as the law is written: no pause
            // and no recovery - the feed stays live on every tick,
            // the pursuit holds, and the batter pace shows inside the
            // window. (A tick spent in the overshoot's BACK zone feeds
            // the original's reverse at its own pace: that is the
            // loop's closing leg, not a pause, and the pin does not
            // tax it as one - the first 5x run's pin did, and read a
            // lawful reverse as a mercy.)
            bool reattack = true;
            float maxCruise = 0.0f;
            for (float t = 0.0f; t < 0.5f; t += dt) {
                manS += 10.0f * dt;
                Vec3 md2;
                const Vec3 mp2 = road.PosAt(manS, &md2);
                host.velocity = md2 * 10.0f;
                host.player = mp2;
                CopTick(dt, mp2);
                host.StepCar(dt);
                if (gCop.mode != COP_PURSUE || gCop.recoveryT > 0.0f || !gCop.feedLive)
                    reattack = false;
                if (gCop.feedCruise > maxCruise)
                    maxCruise = gCop.feedCruise;
            }
            CHECK(reattack && maxCruise >= 150.0f,
                  "after the throw he re-engages at once: feed live every tick, no recovery, batter pace in the window (max cruise %.0f)",
                  maxCruise);
        }
    }

    // ---- (o) THE ZERO-MERCY AMENDMENT: an attacking man is never
    // "lost". Staged at felony 3000 with the man 300 m out of every
    // sight gate, on foot, his fire landing on the unit every tick:
    // the position must hold and the pursuit must never stand down.
    // (The pin is only survivable because the 5x book prices his fire
    // at 135 a tick - at the old line he would have killed the unit
    // in ~4 s and the stand-down would be a death, not a mercy.)
    {
        CopReset();
        host.freeze = false;
        float copS = road.NearestDist(host.carPos) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copS, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CHECK(CopSummon(copAt, road.PosAt(copS + 25.0f, nullptr)), "the no-mercy cop stands up");
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true;
        gCop.spikeDone = true;
        gCop.felony = 3000.0f;
        host.vehicle = -1;
        host.velocity = Vec3();
        const Vec3 nearMan = road.PosAt(copS + 8.0f, nullptr);
        CopTick(dt, nearMan);
        host.StepCar(dt);
        CHECK(gCop.known && gCop.mode == COP_PURSUE, "seen up close first: known, and pursuing");
        const Vec3 farMan = road.PosAt(copS + 300.0f, nullptr);
        bool held = true;
        for (float t = 0.0f; t < 10.0f; t += dt) {
            host.struckShotQ = 5.0f; // his fire lands on the unit
            CopTick(dt, farMan);
            host.StepCar(dt);
            if (!gCop.known || gCop.mode != COP_PURSUE)
                held = false;
        }
        CHECK(held, "while his fire lands, position-lost never fires and the pursuit never stands down");
        CHECK(gCop.felony >= 3000.0f, "the felony does not bleed under his attack (%.0f)", gCop.felony);
    }

    // ---- (p) THE PLOUGH LAW's price split: his book is for the
    // prey's fight. A 20 m/s impact spike two hundred metres from the
    // man (traffic rammed aside, scenery struck at pace) bills the
    // token share; health lost in the same report rides the token
    // lane with it; the same world loss with NO impact in the report
    // is the fire case and keeps the anchor rate whole; the same
    // impact at the prey's side pays the full BCOLLIDE price. (The
    // fight price of a 20 m/s spike: (20-2.8)*340 capped at 1500,
    // times toughness 0.875 = 1312.5; the token is its 1/25 = 52.5.)
    {
        CopReset();
        host.freeze = false;
        float copSc = road.NearestDist(grove) + 30.0f;
        Vec3 dir;
        const Vec3 copAt = road.PosAt(copSc, &dir);
        host.PlaceCar(copAt, std::atan2(dir.x, dir.y) * 57.29578f);
        CHECK(CopSummon(copAt, road.PosAt(copSc + 25.0f, nullptr)), "the plough cop stands up");
        gCop.mode = COP_IDLE;
        gCop.felony = 0.0f;
        host.vehicle = -1;
        host.velocity = Vec3();
        const Vec3 farMan = road.PosAt(copSc + 200.0f, nullptr);
        host.player = farMan;
        auto bookTick = [&](const Vec3& man) {
            const float before = gCop.totalDamage;
            CopTick(dt, man);
            host.StepCar(dt);
            return gCop.totalDamage - before;
        };
        host.impactDvQ = 20.0f;
        const float sceneryImpact = bookTick(farMan);
        CHECK(sceneryImpact > 30.0f && sceneryImpact < 80.0f,
              "a scenery impact at pace bills the token, not the fight price (%.1f)", sceneryImpact);
        host.impactDvQ = 20.0f;
        host.struckWorldQ = 100.0f;
        const float sceneryBoth = bookTick(farMan);
        CHECK(sceneryBoth > 120.0f && sceneryBoth < 220.0f,
              "collision-filed world loss rides the token lane beside it (%.1f)", sceneryBoth);
        host.struckWorldQ = 100.0f;
        const float fireBill = bookTick(farMan);
        CHECK(fireBill > 2600.0f && fireBill < 2800.0f,
              "a smooth drain with no impact in it bills the anchor rate whole (%.0f)", fireBill);
        const Vec3 nearMan = copAt + dir * 4.0f;
        host.player = nearMan;
        host.impactDvQ = 20.0f;
        const float fightImpact = bookTick(nearMan);
        CHECK(fightImpact > 1200.0f && fightImpact < 1400.0f,
              "an impact at the prey's side pays the full fight price (%.1f)", fightImpact);
    }
    CopReset();
    SetHost(nullptr);
}

// ---------------------------------------------------------------------------
// Step 2's force host: many bodies, one per unit, each with the same
// honest little physics as piece 1's fake (including the deliberately
// INVERTED steering the spike calibration exists to catch).
struct ForceFakeHost : TestHost {
    struct Body {
        Vec3 pos, vel;
        float yaw = 0.0f, speed = 0.0f;
        float cmdGas = 0, cmdBrake = 0, cmdSteer = 0;
        bool cmdSiren = false;
        float display = 1000.0f;
        bool burned = false, driverDown = false;
        float qRam = 0, qShot = 0, qWorld = 0, qDriver = 0, qImpact = 0;
        // the feed: the last drive intent the core named for this body
        Vec3 feedPt;
        float feedCruise = 0.0f;
        bool feedLive = false;
        // items 36/38: this body is a bike; its driver is out on foot
        bool bike = false;
        bool driverOut = false;
    };
    std::map<int, Body> bodies;
    int nextId = 100;
    // item 13's staged wall: one segment the core's world probes can
    // read, off unless a sim stages it
    bool wallOn = false;
    Vec3 wallA, wallB;
    bool LineBlockedWorld(const Vec3& a, const Vec3& b) override {
        if (!wallOn)
            return false;
        auto cr = [](const Vec3& u, const Vec3& v) { return u.x * v.y - u.y * v.x; };
        const Vec3 r = b - a, s = wallB - wallA;
        const float d = cr(r, s);
        if (std::fabs(d) < 1e-9f)
            return false;
        const Vec3 qp = wallA - a;
        const float t = cr(qp, s) / d, u = cr(qp, r) / d;
        return t > 0.02f && t < 0.98f && u > 0.0f && u < 1.0f;
    }
    float lastSpawnDist = -1.0f; // from the staged player, at the last spawn
    std::vector<Vec3> spawnPts;  // every spawn point, in order - the
                                 // presence pin reads the ladder's fan
                                 // off it (bodies move after spawning;
                                 // the ledger does not)
    // item 24's staged traffic: civilian cars the core may read as
    // weapons; a sim fills the list, the verb serves it honestly
    std::vector<VehicleInfo> traffic;
    int bikeSpawns = 0;   // item 36: bike bodies this host has made
    int bailOrders = 0;   // item 38: bail verbs this host has taken
    bool lastBailOut = false;
    void NearbyVehicles(const Vec3& centre, float radius,
                        std::vector<VehicleInfo>& out) override {
        for (const VehicleInfo& v : traffic)
            if ((v.pos - centre).Length() <= radius)
                out.push_back(v);
    }
    int SpawnCopBody(const Vec3& at, float, const Vec3& face) override {
        Body b;
        b.pos = at;
        const Vec3 d = face - at;
        b.yaw = std::atan2(d.x, d.y);
        const int id = nextId++;
        bodies[id] = b;
        lastSpawnDist = Dist2D(at, player);
        spawnPts.push_back(at);
        return id;
    }
    // item 36: a bike body is a body in every way the fake grades -
    // the difference lives in the bridge (a real bike model and no
    // Chicago mesh), so here it is a body with the flag set
    int SpawnCopBikeBody(const Vec3& at, float, const Vec3& face) override {
        Body b;
        b.pos = at;
        const Vec3 d = face - at;
        b.yaw = std::atan2(d.x, d.y);
        b.bike = true;
        const int id = nextId++;
        bodies[id] = b;
        lastSpawnDist = Dist2D(at, player);
        spawnPts.push_back(at);
        ++bikeSpawns;
        return id;
    }
    // item 38: the bail verb moves the fake's driver word; the
    // report below reads it back, the way the bridge's will
    void CopBodyBail(int veh, bool out) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return;
        it->second.driverOut = out;
        ++bailOrders;
        lastBailOut = out;
    }
    bool CopBodyReport(int veh, CopBody* out) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return false;
        Body& b = it->second;
        out->ok = true;
        out->pos = b.pos;
        out->fwd = Vec3(std::sin(b.yaw), std::cos(b.yaw), 0.0f);
        out->vel = b.vel;
        out->health = 1.0f;
        out->gas = b.cmdGas;
        out->brake = b.cmdBrake;
        out->steer = b.cmdSteer;
        out->siren = b.cmdSiren;
        out->struckRam = b.qRam;
        out->struckShot = b.qShot;
        out->struckWorld = b.qWorld;
        out->struckDriver = b.qDriver;
        out->impactDv = b.qImpact;
        b.qRam = b.qShot = b.qWorld = b.qDriver = b.qImpact = 0.0f;
        out->mission = b.feedLive ? 9 : 0;
        out->cruise = (int)std::fmin(b.feedCruise, 127.0f); // the mirror
            // model's landing pair (the 5x update, as in the single fake)
        out->cruiseF = b.feedCruise;
        out->driverOut = b.driverOut; // item 38: the driver stands
        out->driverPos = b.driverOut ? b.pos + Vec3(2, 0, 0) : b.pos;
        return true;
    }
    void CopBodySetHealth(int veh, float book01) override {
        auto it = bodies.find(veh);
        if (it != bodies.end())
            it->second.display = book01 > 0.0f ? 380.0f + 620.0f * book01 : 90.0f;
    }
    void CopBodyRetire(int veh, bool burnOut) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return;
        it->second.burned = burnOut;
        it->second.driverDown = burnOut;
        bodies.erase(it);
    }
    void CopBodyDrive(int veh, float gas, float brake, float steer, bool siren) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return;
        it->second.cmdGas = gas;
        it->second.cmdBrake = brake;
        it->second.cmdSteer = steer;
        it->second.cmdSiren = siren;
    }
    void CopBodyDriveTarget(int veh, const Vec3& at, float cruise) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return;
        it->second.feedPt = at;
        it->second.feedCruise = cruise;
        it->second.feedLive = cruise > 0.05f;
    }
    void CopBodyPlace(int veh, const Vec3& at, float headingDeg) override {
        auto it = bodies.find(veh);
        if (it == bodies.end())
            return;
        it->second.pos = at;
        it->second.yaw = headingDeg * 0.0174533f;
        it->second.speed = 0.0f;
        it->second.vel = Vec3();
    }
    float PlayerVehicleImpact() override { return 0.0f; }
    Body* Find(int veh) {
        auto it = bodies.find(veh);
        return it == bodies.end() ? nullptr : &it->second;
    }
    void StepAll(float dt) {
        for (auto& kv : bodies)
            MirrorStep(kv.second.pos, kv.second.yaw, kv.second.speed, kv.second.vel,
                       kv.second.feedPt, kv.second.feedCruise, kv.second.feedLive, dt);
    }
};

static void TestCopForce() {
    ForceFakeHost host;
    SetHost(&host);
    WideStage();
    CopReset();
    CHECK(LoadVehicleGraphFromRepo(), "the force sims ride the real vehicle graph");
    const float dt = 0.02f;
    const Vec3 grove(2486.0f, -1654.0f, 13.5f);
    auto ladderLiving = []() {
        int n = 0;
        for (int s = 0; s < kMaxCopCars; ++s)
            if (gCops[s].veh >= 0 && gCops[s].mode != COP_NONE)
                ++n;
        return n;
    };
    auto stageAnchor = [&](const Vec3& at, const Vec3& man) {
        CopReset();
        host.bodies.clear();
        host.hasPlayer = true;
        host.player = man;
        host.vehicle = 42;
        host.velocity = Vec3();
        CHECK(CopSummon(at, man), "the force's anchor stands up");
        // the spike is piece 0's proof, not this step's: skip it the way
        // piece 1's chase sims do, calibration already the anchor's
        gCop.mode = COP_IDLE;
        gCop.steerCalDone = true;
        gCop.steerFlip = true; // this fake body is inverted, as calibrated
        gCop.spikeDone = true;
    };
    auto tick = [&](float seconds) {
        for (float t = 0.0f; t < seconds; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
        }
    };

    // ---- (i) the ladder: felony climbs 0->1->2->3 more units, decays
    // back down, and the surplus LEAVES - drives off to the ping-out
    // radius, never charges the man it is standing down from
    {
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        stageAnchor(grove, man);
        gCop.felony = 700.0f;
        tick(1.0f);
        CHECK(ladderLiving() == 1 && CopPursuingCount() == 1,
              "felony 700: the anchor alone pursues (living %d, pursuing %d)",
              ladderLiving(), CopPursuingCount());
        gCop.felony = 900.0f;
        tick(9.0f);
        CHECK(ladderLiving() == 2, "felony 900: the ladder sent a second unit (%d)", ladderLiving());
        gCop.felony = 1700.0f;
        tick(9.0f);
        CHECK(ladderLiving() == 3, "felony 1700: a third unit (%d)", ladderLiving());
        gCop.felony = 2500.0f;
        tick(9.0f);
        CHECK(ladderLiving() == 4, "felony 2500: the ladder is full at maxCopCars (%d)", ladderLiving());
        // decay: two units are surplus. Watch them leave: each must end
        // at the ping-out edge and never close on the man after leaving
        gCop.felony = 900.0f;
        float leaveStart[kMaxCopUnits], leaveMin[kMaxCopUnits], leaveMax[kMaxCopUnits];
        bool leaving[kMaxCopUnits] = {}, left[kMaxCopUnits] = {};
        for (int s = 0; s < kMaxCopUnits; ++s) {
            leaveStart[s] = -1.0f;
            leaveMin[s] = 1e9f;
            leaveMax[s] = -1.0f;
        }
        for (float t = 0.0f; t < 30.0f && ladderLiving() > 2; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
            for (int s = 0; s < kMaxCopUnits; ++s) {
                if (gCops[s].mode == COP_LEAVE) {
                    const float d = (gCops[s].pos - host.player).Length();
                    if (!leaving[s]) {
                        leaving[s] = true;
                        leaveStart[s] = d;
                    }
                    if (d < leaveMin[s])
                        leaveMin[s] = d;
                    if (d > leaveMax[s])
                        leaveMax[s] = d;
                } else if (leaving[s] && gCops[s].veh < 0) {
                    left[s] = true;
                }
            }
        }
        CHECK(ladderLiving() == 2, "felony back to 900: the surplus is gone (%d)", ladderLiving());
        int leftCount = 0;
        for (int s = 0; s < kMaxCopUnits; ++s) {
            if (!leaving[s])
                continue;
            ++leftCount;
            CHECK(leaveMax[s] >= 90.0f, "a leaver reached the ping-out edge (max %.0f m)", leaveMax[s]);
            CHECK(leaveMin[s] >= leaveStart[s] - 8.0f,
                  "a leaver never charged the man while leaving (start %.0f, closest %.0f)",
                  leaveStart[s], leaveMin[s]);
            CHECK(left[s], "a leaver actually pinged out, not parked in LEAVE");
        }
        CHECK(leftCount == 2, "exactly the two surplus units left (%d)", leftCount);
        gCop.felony = 200.0f;
        tick(30.0f);
        CHECK(ladderLiving() == 1 && gCop.mode == COP_IDLE,
              "felony 200: the anchor alone, stood down (living %d, mode %d)", ladderLiving(), (int)gCop.mode);
        CHECK(gThreat.ourUnitsLost == 0, "stand-downs and ping-outs are not losses (%d)", gThreat.ourUnitsLost);
    }

    // ---- (ii) a retired unit is replaced on the timer while the felony
    // holds; (vi) ourUnitsLost counts ONLY the player's own work
    {
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        stageAnchor(grove, man);
        gCop.felony = 1700.0f;
        for (float t = 0.0f; t < 20.0f && ladderLiving() < 3; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
        }
        CHECK(ladderLiving() == 3, "the force staged at three units (%d)", ladderLiving());
        int slotA = -1, slotB = -1;
        for (int s = 1; s < kMaxCopCars; ++s)
            if (gCops[s].veh >= 0 && gCops[s].mode != COP_NONE) {
                if (slotA < 0)
                    slotA = s;
                else if (slotB < 0)
                    slotB = s;
            }
        CHECK(slotA > 0 && slotB > 0, "two ladder units to spend (%d, %d)", slotA, slotB);
        // the player's gun spends the first: his book fills past its
        // line through the shot channel alone, so the loss is wholly
        // his. Fix round 3: the staging rides over several ticks
        // because one tick now bills at most one blast-class price
        // (8100) however much raw damage the fake host queues - that
        // cap is the law under test in block (j); the attribution
        // intent here is unchanged (shot channel = the player's work,
        // world = not). THE 5x UPDATE: the line is 135000, so the
        // staging needs seventeen capped ticks, not four - the loop
        // bound is 24 for margin. THE 39-PACKAGE restages the ending:
        // item 16's handover refills a fallen PURSUER's slot inside
        // the very tick he falls, so "the slot stands empty" is no
        // longer the retirement's signature - the loss count and the
        // vanished body are, and the refill itself is pinned as the
        // handover working. The loop breaks on the loss count itself
        // so the handover's spawn distance is read before any later
        // spawn can overwrite it.
        const int vehA = gCops[slotA].veh;
        const int vehB = gCops[slotB].veh;
        for (int i = 0; i < 24 && gThreat.ourUnitsLost == 0; ++i) {
            if (ForceFakeHost::Body* b = host.Find(vehA))
                b->qShot = 1200.0f;
            tick(dt);
        }
        CHECK(gThreat.ourUnitsLost == 1, "a player-caused retirement counted (%d)", gThreat.ourUnitsLost);
        CHECK(host.Find(vehA) == nullptr, "the shot-spent unit's body is gone");
        CHECK(gCops[slotA].veh >= 0 && gCops[slotA].totalDamage < 20000.0f,
              "item 16: the handover refilled the fallen pursuer's slot in his fall tick, book fresh");
        CHECK(host.lastSpawnDist >= 45.0f && host.lastSpawnDist <= 100.0f,
              "the handover replacement entered at range, not in the player's face (%.0f m)",
              host.lastSpawnDist);
        // the world spends the second: nothing of it is the player's
        for (int i = 0; i < 24 && host.Find(vehB) != nullptr; ++i) {
            if (ForceFakeHost::Body* b = host.Find(vehB))
                b->qWorld = 1200.0f;
            tick(dt);
        }
        CHECK(host.Find(vehB) == nullptr, "the world-spent unit retired");
        CHECK(gThreat.ourUnitsLost == 1, "a world-caused retirement did NOT count (%d)", gThreat.ourUnitsLost);
        // and the force stands whole again: handover refills plus the
        // spawn clock bring the ladder back to its want
        for (float t = 0.0f; t < 9.0f && ladderLiving() < 3; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
        }
        CHECK(ladderLiving() >= 3, "the force refilled after its losses (%d)", ladderLiving());
    }

    // ---- (iv) the roadblock: felony past 1318 arms the count; the block
    // stands across the road ahead of a moving player, asleep until he
    // closes; (iii) once it wakes, pursuers exceed the ladder's want and
    // the cutoff halves the far units' power - the nearest fights full
    {
        std::vector<Vec3> roadPts;
        CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(1690, -2280, 13), &roadPts, nullptr) &&
                  roadPts.size() > 4,
              "the roadblock sim rides the Grove-to-airport road");
        RoadWalker road;
        road.Build(roadPts);
        float manS = road.NearestDist(grove) + 60.0f;
        Vec3 md;
        const Vec3 man0 = road.PosAt(manS, &md);
        const float copS = manS - 30.0f;
        stageAnchor(road.PosAt(copS, nullptr), man0);
        gCop.felony = 1400.0f;
        bool placed = false, asleepOk = true, woke = false;
        Vec3 blk0, blk1, atPlayer, atDir;
        float tAll = 0.0f;
        for (; tAll < 90.0f && !woke; tAll += dt) {
            manS += 15.0f * dt;
            const Vec3 mp = road.PosAt(manS, &md);
            host.player = mp;
            host.velocity = md * 15.0f;
            CopTick(dt, mp);
            host.StepAll(dt);
            for (auto& kv : host.bodies)
                kv.second.pos.z = road.PosAt(road.NearestDist(kv.second.pos), nullptr).z;
            if (!placed && CopBlockCount() == kMaxBlockCars) {
                placed = true;
                blk0 = gCops[kMaxCopCars].pos;
                blk1 = gCops[kMaxCopCars + 1].pos;
                atPlayer = mp;
                atDir = md;
                CHECK(gCops[kMaxCopCars].mode == COP_BLOCK && gCops[kMaxCopCars + 1].mode == COP_BLOCK,
                      "the block cars stand asleep");
            } else if (placed && !woke) {
                const float d0 = (gCops[kMaxCopCars].pos - mp).Length();
                const float d1 = (gCops[kMaxCopCars + 1].pos - mp).Length();
                if (d0 > 50.0f && d1 > 50.0f) {
                    // still far: asleep means asleep - no gas, no pursuit
                    ForceFakeHost::Body* b0 = host.Find(gCops[kMaxCopCars].veh);
                    ForceFakeHost::Body* b1 = host.Find(gCops[kMaxCopCars + 1].veh);
                    if (gCops[kMaxCopCars].mode != COP_BLOCK || gCops[kMaxCopCars + 1].mode != COP_BLOCK ||
                        (b0 && b0->cmdGas != 0.0f) || (b1 && b1->cmdGas != 0.0f))
                        asleepOk = false;
                }
                if (gCops[kMaxCopCars].mode == COP_PURSUE && gCops[kMaxCopCars + 1].mode == COP_PURSUE)
                    woke = true;
            }
        }
        CHECK(placed, "felony 1400 armed the count and the block was placed");
        if (placed) {
            const Vec3 rel = blk0 - atPlayer;
            CHECK(rel.Dot(atDir) > 15.0f, "the block stands AHEAD of the moving player (%.0f m along his road)",
                  rel.Dot(atDir));
            const float bd = rel.Length();
            CHECK(bd >= 25.0f && bd <= 85.0f, "the block's distance is the fan's own (%.0f m)", bd);
            CHECK(std::fabs((blk0 - blk1).Length() - 4.6f) < 1.5f,
                  "the two cars stand across the lanes (%.1f m apart)", (blk0 - blk1).Length());
        }
        CHECK(asleepOk, "the block stayed asleep until the player closed in");
        CHECK(woke, "the block woke as the player closed - both units pursue");
        // (iii) the cutoff, staged exactly: the nearest pursuer 10 m
        // behind the man, the farthest 80 m behind him on the same road
        if (woke) {
            int nearSlot = -1, farSlot = -1;
            float nd = 1e18f, fd = -1.0f;
            for (int s = 0; s < kMaxCopUnits; ++s) {
                if (gCops[s].veh < 0 || gCops[s].mode != COP_PURSUE)
                    continue;
                const float d = (gCops[s].pos - host.player).Length();
                if (d < nd) {
                    nd = d;
                    nearSlot = s;
                }
                if (d > fd) {
                    fd = d;
                    farSlot = s;
                }
            }
            CHECK(nearSlot >= 0 && farSlot >= 0 && nearSlot != farSlot,
                  "four pursuers to rank (near %d, far %d)", nearSlot, farSlot);
            CHECK(CopPursuingCount() > CopWantedCount(),
                  "pursuers exceed the ladder's want (%d > %d)", CopPursuingCount(), CopWantedCount());
            if (nearSlot >= 0 && farSlot >= 0 && nearSlot != farSlot) {
                if (ForceFakeHost::Body* b = host.Find(gCops[nearSlot].veh)) {
                    b->pos = road.PosAt(manS - 10.0f, nullptr);
                    b->speed = 0.0f;
                }
                if (ForceFakeHost::Body* b = host.Find(gCops[farSlot].veh)) {
                    b->pos = road.PosAt(manS - 80.0f, nullptr);
                    b->speed = 0.0f;
                }
                const Vec3 mp = road.PosAt(manS, &md);
                host.player = mp;
                host.velocity = md * 15.0f;
                CopTick(dt, mp);
                host.StepAll(dt);
                CHECK(CopCutoffDistance() < 100.0f, "a cutoff line is in force (%.0f m)", CopCutoffDistance());
                CHECK(gCops[nearSlot].powerScale == 1.0f,
                      "the nearest pursuer fights at full power (%.2f)", gCops[nearSlot].powerScale);
                CHECK(gCops[farSlot].powerScale == 0.5f,
                      "the far pursuer is cut to the half scale (%.2f)", gCops[farSlot].powerScale);
                // THE 39-PACKAGE: the far unit may carry item 26's
                // counter-pick scale from this pursuit's own record -
                // the cutoff pin is about the CUTOFF's half, so it is
                // read divided by the unit's own pace scale
                CHECK(gCops[farSlot].desiredSpeed / gCops[farSlot].paceScale <= 134.5f,
                      "the far unit's desired speed rides the half scale of the 5x straight row (%.1f m/s at pace %.2f)",
                      gCops[farSlot].desiredSpeed, gCops[farSlot].paceScale);
                int full = 0;
                for (int s = 0; s < kMaxCopUnits; ++s)
                    if (gCops[s].veh >= 0 && gCops[s].mode == COP_PURSUE && gCops[s].powerScale == 1.0f)
                        ++full;
                CHECK(full == CopWantedCount(), "exactly the wanted number fight at full strength (%d)", full);
                // THE 5x UPDATE's second act: at the felony CAP the
                // discount is REMOVED - the same staged geometry, the
                // page at 4096, and the far unit fights at full fed
                // speed like the near one (the presence order: the
                // pressure closes from several streets at once)
                gCop.felony = 4096.0f;
                {
                    const Vec3 mp2 = road.PosAt(manS, &md);
                    host.player = mp2;
                    host.velocity = md * 15.0f;
                    CopTick(dt, mp2);
                    host.StepAll(dt);
                }
                CHECK(CopCutoffDistance() > 900.0f,
                      "at the felony cap no cutoff line is in force (%.0f m)", CopCutoffDistance());
                CHECK(gCops[farSlot].powerScale == 1.0f,
                      "at the felony cap the far pursuer fights at FULL power (%.2f)",
                      gCops[farSlot].powerScale);
                gCop.felony = 1400.0f; // hand the scene back as staged
            }
        }
    }

    // ---- (v) perf: four units, ONE shared field - the refresh counter
    // moves at the field's own cadence, not once per unit per tick
    {
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        stageAnchor(grove, man);
        gCop.felony = 2500.0f;
        for (float t = 0.0f; t < 30.0f && CopPursuingCount() < 4; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
        }
        CHECK(CopPursuingCount() == 4, "four units pursuing for the field proof (%d)", CopPursuingCount());
        const int before = VehicleFieldBuildCount();
        tick(7.5f);
        const int delta = VehicleFieldBuildCount() - before;
        CHECK(delta >= 4 && delta <= 14,
              "one shared field for the whole force: %d refreshes in 7.5 s (the 0.75 s cadence), not per-unit", delta);
    }

    // ---- (vi) the complete update's second roadblock row: four cars
    // in two rows - the front pair across the lanes at the block point
    // (piece 2's law, untouched), the second pair one car-length of
    // road behind it toward the player, staggered to the opposite
    // laterals and angled (their stand points sit OFF the front row's
    // line, back along the road, and inboard)
    {
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        stageAnchor(grove, man);
        gCop.felony = 1400.0f;
        Vec3 front[2], back[2];
        bool got = false;
        for (float t = 0.0f; t < 90.0f && !got; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
            if (CopBlockCount() == kMaxBlockCars) {
                front[0] = gCops[kMaxCopCars].pos;
                front[1] = gCops[kMaxCopCars + 1].pos;
                back[0] = gCops[kMaxCopCars + 2].pos;
                back[1] = gCops[kMaxCopCars + 3].pos;
                got = true;
            }
        }
        CHECK(got, "the block places all %d cars (two rows)", kMaxBlockCars);
        if (got) {
            const Vec3 fMid = (front[0] + front[1]) * 0.5f;
            const Vec3 bMid = (back[0] + back[1]) * 0.5f;
            const float rowGap = (fMid - bMid).Length();
            CHECK(rowGap > 3.0f && rowGap < 10.0f,
                  "the second row stands one car-length behind the first (%.1f m)", rowGap);
            const float backPair = (back[0] - back[1]).Length();
            CHECK(backPair > 2.0f && backPair < 4.2f,
                  "the second row's pair is staggered inboard (%.1f m apart)", backPair);
            const float toPlayer = (bMid - man).Length();
            const float frontToPlayer = (fMid - man).Length();
            CHECK(toPlayer < frontToPlayer,
                  "the angled row is the one the player meets first (%.0f m vs %.0f m)",
                  toPlayer, frontToPlayer);
        }
        CopDismiss(); // leave no standing block cars behind: the unit
                      // table is global across the suite, and a later
                      // test's fresh host reuses the same body ids
    }

    // ---- (vii) THE PRESENCE TUNE (the user's ابهت order): successive
    // ladder spawns spread around the ring instead of stacking on the
    // one best node. Staged against the stacking's best case: a
    // STATIONARY man, whose ring has one fixed best point - before
    // the tune every spawn took that same node and the force convoyed
    // in down one street
    {
        const Vec3 man(2496.0f, -1666.0f, 13.5f);
        stageAnchor(grove, man);
        host.velocity = Vec3();
        host.spawnPts.clear(); // the anchor's own spawn is not the fan
        gCop.felony = 4096.0f;
        for (float t = 0.0f; t < 20.0f && ladderLiving() < 4; t += dt) {
            CopTick(dt, host.player);
            host.StepAll(dt);
        }
        CHECK(ladderLiving() == 4, "the ladder filled for the spread proof (%d)", ladderLiving());
        CHECK(host.spawnPts.size() >= 3, "three ladder spawns on the ledger (%d)", (int)host.spawnPts.size());
        if (host.spawnPts.size() >= 3) {
            float maxPair = 0.0f;
            int distinct = 0;
            for (size_t i = 0; i < host.spawnPts.size(); ++i) {
                bool dup = false;
                for (size_t j = 0; j < i; ++j)
                    if ((host.spawnPts[i] - host.spawnPts[j]).Length() < 5.0f)
                        dup = true;
                if (!dup)
                    ++distinct;
                for (size_t j = i + 1; j < host.spawnPts.size(); ++j) {
                    const float d = (host.spawnPts[i] - host.spawnPts[j]).Length();
                    if (d > maxPair)
                        maxPair = d;
                }
            }
            CHECK(distinct >= 2, "the spawns did not all stack on one node (%d distinct points)", distinct);
            CHECK(maxPair >= 25.0f, "the fan spans the ring (widest pair %.0f m)", maxPair);
        }
    }

    // ---- (am) THE 39-PACKAGE, LAYER A (tactics, windows, the book's
    // new laws). The staging discipline throughout: bodies are PINNED
    // to staged offsets after every host step, so the geometry the
    // brain reads is the geometry the sim means - the brain thinks,
    // the sim holds the world still around its thinking.
    {
        const Vec3 man0(2496.0f, -1666.0f, 13.5f);
        // the formation stage rides the REAL road (a RoadWalker over
        // the Grove-to-airport route, the (iv) road): the first
        // staging drove the man in a straight line east and off the
        // graph within seconds - the front unit's field read failed
        // and its target fell back to the man's raw position, which
        // is the fake lying about the world, not the brain failing.
        // Pins also set the body's YAW to the travel direction: a
        // teleported body keeps its summon heading in this fake, and
        // a car sitting astern with a stale nose is a car mid-turn -
        // the core rightly reverses it out (ZONE_BACK), which is the
        // second conviction these pins carry
        std::vector<Vec3> amRoute;
        CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(1690, -2280, 13), &amRoute, nullptr) &&
                  amRoute.size() > 4,
              "the 39-package stage rides the Grove-to-airport road");
        RoadWalker amRoad;
        amRoad.Build(amRoute);
        float amS = 0.0f;
        Vec3 amDir(1, 0, 0);
        auto amMan = [&]() { return amRoad.PosAt(amS, &amDir); };
        auto amRight = [&]() { return Vec3(amDir.y, -amDir.x, 0); };
        auto amLeft = [&]() { return Vec3(-amDir.y, amDir.x, 0); };
        auto rLatOf = [&](const Vec3& p) { return (p - host.player).Dot(amRight()); };
        auto rFwdOf = [&](const Vec3& p) { return (p - host.player).Dot(amDir); };
        auto stageFormation = [&](float felony, int units) {
            amS = amRoad.NearestDist(grove) + 60.0f;
            const Vec3 mp = amMan();
            stageAnchor(amRoad.PosAt(amS - 30.0f, nullptr), mp);
            gCop.felony = felony;
            host.velocity = amDir * 15.0f;
            for (float t = 0.0f; t < 25.0f && ladderLiving() < units; t += dt) {
                amS += 15.0f * dt;
                const Vec3 p = amMan();
                host.player = p;
                host.velocity = amDir * 15.0f;
                CopTick(dt, p);
                host.StepAll(dt);
            }
        };
        auto roadPinTick = [&](const Vec3 offs[4], int units, float seconds,
                               const std::function<bool()>& done) {
            for (float t = 0.0f; t < seconds; t += dt) {
                amS += 15.0f * dt;
                const Vec3 p = amMan();
                host.player = p;
                host.velocity = amDir * 15.0f;
                CopTick(dt, p);
                if (done && done())
                    return true;
                host.StepAll(dt);
                for (int s = 0; s < units; ++s)
                    if (ForceFakeHost::Body* b = host.Find(gCops[s].veh)) {
                        b->pos = p + amDir * offs[s].x + amLeft() * offs[s].y;
                        b->vel = host.velocity;
                        b->yaw = std::atan2(amDir.x, amDir.y); // the fake yaw is radians (MirrorStep)
                    }
            }
            return false;
        };
        // the straight-line pin stage (single-unit sims whose reads
        // are zone/window arithmetic, in the graph-dense Grove blocks)
        auto pinTick = [&](const Vec3 offs[4], int units, float seconds,
                           const std::function<bool()>& done) {
            for (float t = 0.0f; t < seconds; t += dt) {
                host.player = host.player + host.velocity * dt;
                CopTick(dt, host.player);
                if (done && done())
                    return true;
                host.StepAll(dt);
                for (int s = 0; s < units; ++s)
                    if (ForceFakeHost::Body* b = host.Find(gCops[s].veh)) {
                        b->pos = host.player + offs[s];
                        b->vel = host.velocity;
                        if (host.velocity.Length() > 1.0f)
                            b->yaw = std::atan2(host.velocity.x, host.velocity.y);
                    }
            }
            return false;
        };

        // (am1) item 1, the roles: lead nearest, flanks at his doors,
        // the front unit ahead to brake-check - read off the units'
        // own targets after a tactics tick
        {
            const Vec3 offs[4] = {Vec3(-4, 0, 0), Vec3(-19, -9, 0), Vec3(-19, 9, 0), Vec3(19, 0, 0)};
            stageFormation(2500.0f, 4);
            CHECK(ladderLiving() == 4, "the roles stage stands at four units (%d)", ladderLiving());
            const bool assigned = roadPinTick(offs, 4, 3.5f, [&]() {
                return gCops[1].role == 1 && gCops[2].role == 2 && gCops[3].role == 3 &&
                       rFwdOf(gCops[3].target) > 5.0f; // and its aim is live,
            });                                 // not a cold-spawn fallback
            CHECK(assigned, "item 1: the tactics tick dealt the roles (r %d %d %d %d)",
                  gCops[0].role, gCops[1].role, gCops[2].role, gCops[3].role);
            CHECK(rLatOf(gCops[1].target) > 1.5f && rLatOf(gCops[1].target) < 6.0f,
                  "item 1: the right flank aims at his right door (lat %.1f)", rLatOf(gCops[1].target));
            CHECK(rLatOf(gCops[2].target) < -1.5f && rLatOf(gCops[2].target) > -6.0f,
                  "item 1: the left flank aims at his left door (lat %.1f)", rLatOf(gCops[2].target));
            CHECK(rFwdOf(gCops[3].target) > 5.0f,
                  "item 1: the front unit aims ahead of him, to brake-check (fwd %.1f)",
                  rFwdOf(gCops[3].target));
        }
        // (am1b) item 13: a wall on his right stands the right flank
        // down - the batter belongs on the open side
        {
            const Vec3 offs[4] = {Vec3(-4, 0, 0), Vec3(-19, -9, 0), Vec3(-19, 9, 0), Vec3(19, 0, 0)};
            stageFormation(2500.0f, 4);
            host.wallOn = true;
            const bool read = roadPinTick(offs, 4, 3.5f, [&]() {
                host.wallA = host.player + amRight() * 8.5f - amDir * 30.0f;
                host.wallB = host.player + amRight() * 8.5f + amDir * 30.0f;
                return gCops[2].role == 2 && gCops[1].role == 0;
            });
            host.wallOn = false;
            CHECK(read && gCops[1].role == 0,
                  "item 13: with his right walled, the right flank stands down (roles %d %d)",
                  gCops[1].role, gCops[2].role);
        }
        // (am2) item 11: two flank units close on opposite sides -
        // their aim collapses to his centre for the window
        {
            const Vec3 offs[4] = {Vec3(-4, 0, 0), Vec3(0, -6, 0), Vec3(0, 6, 0), Vec3()};
            stageFormation(1700.0f, 3);
            const bool collapsed = roadPinTick(offs, 3, 3.5f, [&]() {
                return gCops[1].role == 1 && gCops[2].role == 2 &&
                       std::fabs(rLatOf(gCops[1].target)) < 1.2f &&
                       std::fabs(rLatOf(gCops[2].target)) < 1.2f;
            });
            CHECK(collapsed, "item 11: opposite flanks close in - both aims sit on his centre (lat %.1f / %.1f)",
                  rLatOf(gCops[1].target), rLatOf(gCops[2].target));
        }
        // (am3) item 6, the PIT aim: at batter, close, the aim is his
        // rear quarter on the unit's own side - not the flank point,
        // not the tail point
        {
            amS = amRoad.NearestDist(grove) + 60.0f;
            {
                const Vec3 mp = amMan();
                stageAnchor(amRoad.PosAt(amS - 30.0f, nullptr), mp);
            }
            gCop.felony = 3000.0f;
            const Vec3 offs[4] = {Vec3(-4, 2.5f, 0), Vec3(), Vec3(), Vec3()};
            roadPinTick(offs, 1, 0.5f, nullptr);
            CHECK(rLatOf(gCop.target) < -0.4f && rLatOf(gCop.target) > -1.6f,
                  "item 6: the PIT aims at his rear quarter on the unit's side (lat %.2f)",
                  rLatOf(gCop.target));
            CHECK(rFwdOf(gCop.target) > -2.5f && rFwdOf(gCop.target) < 3.5f,
                  "item 6: the PIT point sits at his rear wheel, not his tail (fwd %.1f)",
                  rFwdOf(gCop.target));
        }
        // (am4) item 12: mid-swing, the pacing dies and the strike
        // floor rules the desire
        {
            stageAnchor(grove, man0);
            gCop.felony = 1400.0f;
            host.velocity = Vec3(15, 0, 0);
            const Vec3 offs[4] = {Vec3(-8, 0, 0), Vec3(), Vec3(), Vec3()};
            float calm = 0.0f;
            {
                int n = 0;
                for (float t = 0.0f; t < 0.6f; t += dt) {
                    host.player = host.player + host.velocity * dt;
                    CopTick(dt, host.player);
                    host.StepAll(dt);
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = host.player + offs[0];
                        b->vel = host.velocity;
                    }
                    if (t > 0.3f) {
                        calm += gCop.desiredSpeed;
                        ++n;
                    }
                }
                calm = n ? calm / n : 0.0f; // the steady paced desire, not
            }                              // one tick's zone transient
            float swung = 0.0f;
            for (float t = 0.0f; t < 0.5f; t += dt) {
                const float cs = std::cos(0.30f), sn = std::sin(0.30f);
                const Vec3 v = host.velocity;
                host.velocity = Vec3(v.x * cs - v.y * sn, v.x * sn + v.y * cs, 0);
                host.player = host.player + host.velocity * dt;
                CopTick(dt, host.player);
                if (gCop.desiredSpeed > swung)
                    swung = gCop.desiredSpeed;
                host.StepAll(dt);
                if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                    const Vec3 f = host.velocity * (1.0f / host.velocity.Length());
                    b->pos = host.player - f * 8.0f;
                    b->vel = host.velocity;
                    b->yaw = std::atan2(f.x, f.y);
                }
            }
            CHECK(swung >= 185.0f && swung > calm + 60.0f,
                  "item 12: the swing kills the pacing - desire %.0f over the calm %.0f", swung, calm);
        }
        // (am5) item 31: the crash window feeds the strike at batter
        // pace while he gathers himself up
        {
            stageAnchor(grove, man0);
            gCop.felony = 1400.0f;
            host.velocity = Vec3(25, 0, 0);
            const Vec3 offs[4] = {Vec3(-10, 0, 0), Vec3(), Vec3(), Vec3()};
            pinTick(offs, 1, 1.2f, nullptr);
            const float before = gCop.feedCruise;
            host.velocity = Vec3(2, 0, 0); // the wall he did not see
            float fed = 0.0f;
            for (float t = 0.0f; t < 1.2f; t += dt) {
                host.player = host.player + host.velocity * dt;
                CopTick(dt, host.player);
                if (gCop.feedCruise > fed)
                    fed = gCop.feedCruise;
                host.StepAll(dt);
                if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                    b->pos = host.player + Vec3(-10, 0, 0);
                    b->vel = host.velocity;
                    b->yaw = std::atan2(host.velocity.x, host.velocity.y);
                }
            }
            CHECK(before < 160.0f && fed >= 185.0f && fed > before + 40.0f,
                  "item 31: the crash opens the window - fed cruise %.0f over the paced %.0f",
                  fed, before);
        }
        // (am6) item 33: one blast-class tick on a unit and the
        // felony is the cap - the ladder never gets a say
        {
            stageAnchor(grove, man0);
            gCop.felony = 1000.0f;
            tick(0.5f);
            if (ForceFakeHost::Body* b = host.Find(gCop.veh))
                b->qShot = 100.0f; // 2700 of his points: a hard hit, no blast
            const float felonyBefore = gCop.felony;
            tick(dt);
            CHECK(gCop.felony > felonyBefore && gCop.felony < 4096.0f,
                  "item 33 control: a sub-blast hit notes its offence but does not jump the felony to the cap (%.0f)",
                  gCop.felony);
            if (ForceFakeHost::Body* b = host.Find(gCop.veh))
                b->qShot = 300.0f; // the full blast price in one tick
            tick(dt);
            CHECK(gCop.felony >= 4096.0f, "item 33: a blast-class hit - the felony is the cap at once (%.0f)",
                  gCop.felony);
        }
        // (am7) item 16's preference: the replacement enters on the
        // side the pressure was lost, inside the same band as always
        {
            const Vec3 offs[4] = {Vec3(15, 0, 0), Vec3(-30, 10, 0), Vec3(-30, -10, 0), Vec3()};
            stageFormation(1700.0f, 3);
            host.velocity = Vec3(); // the man stands; the ring is read plain
            pinTick(offs, 3, 0.3f, nullptr);
            const size_t spawnsBefore = host.spawnPts.size();
            for (int i = 0; i < 24 && gThreat.ourUnitsLost == 0; ++i) {
                if (ForceFakeHost::Body* b = host.Find(gCops[0].veh))
                    b->qShot = 1200.0f;
                CopTick(dt, host.player);
                host.StepAll(dt);
                // the fall must stay the staged east fall: every unit
                // is re-pinned to its formation spot after each step,
                // so the handover's preference is read against the
                // fall this sim means, not wherever the dying unit
                // happened to wander
                for (int s = 0; s < 3; ++s)
                    if (ForceFakeHost::Body* b = host.Find(gCops[s].veh)) {
                        b->pos = host.player + offs[s];
                        b->vel = Vec3();
                        const Vec3 toMan = offs[s] * -1.0f; // he faces the man
                        b->yaw = std::atan2(toMan.x, toMan.y);
                    }
            }
            CHECK(gThreat.ourUnitsLost == 1, "item 16 stage: the east unit fell to the man's gun");
            CHECK(host.spawnPts.size() > spawnsBefore &&
                      (host.spawnPts.back() - host.player).x > 10.0f,
                  "item 16: the replacement entered on the fallen unit's side (east), not across the ring");
            CHECK(host.lastSpawnDist >= 45.0f && host.lastSpawnDist <= 100.0f,
                  "item 16: and still at the band's range (%.0f m)", host.lastSpawnDist);
        }
        // (am8) items 26 + 8: the counter-pick reads the pursuit's
        // own record, and a book past 80% arms the last ram
        {
            const Vec3 offs[4] = {Vec3(-20, 0, 0), Vec3(-35, 8, 0), Vec3(), Vec3()};
            stageAnchor(grove, man0);
            gCop.felony = 1700.0f;
            host.velocity = Vec3(45, 0, 0); // the fastest him on record
            for (float t = 0.0f; t < 25.0f && ladderLiving() < 2; t += dt) {
                host.player = host.player + host.velocity * dt;
                CopTick(dt, host.player);
                host.StepAll(dt);
            }
            pinTick(offs, 2, 2.0f, nullptr); // 45 m/s, on the record
            host.velocity = Vec3(15, 0, 0);  // he cruises now
            const int veh1 = gCops[1].veh;
            for (int i = 0; i < 24 && host.Find(veh1) != nullptr; ++i) {
                if (ForceFakeHost::Body* b = host.Find(veh1))
                    b->qShot = 1200.0f;
                CopTick(dt, host.player);
                host.StepAll(dt);
            }
            CHECK(host.Find(veh1) == nullptr, "item 26 stage: the ladder unit fell");
            CHECK(gCops[1].paceScale >= 1.45f,
                  "item 26: the replacement is picked against his shown 45 m/s, not his cruising 15 (scale %.2f)",
                  gCops[1].paceScale);
            // item 8 rides the same stage: bill the anchor past 80%
            if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                for (int i = 0; i < 14; ++i) {
                    b->qShot = 300.0f;
                    CopTick(dt, host.player);
                    host.StepAll(dt);
                    if (ForceFakeHost::Body* b2 = host.Find(gCop.veh))
                        b = b2;
                }
            }
            CHECK(gCop.totalDamage > 0.8f * 135000.0f && gCop.totalDamage < 135000.0f,
                  "item 8 stage: the anchor's book stands past 80%%, unspent (%.0f)", gCop.totalDamage);
            CHECK(gCop.lastRamT > 0.0f, "item 8: the last ram is armed (%.1f s)", gCop.lastRamT);
            if (ForceFakeHost::Body* b = host.Find(gCop.veh))
                b->pos = host.player + Vec3(-12, 0, 0);
            CopTick(dt, host.player);
            CHECK(gCop.desiredSpeed >= 250.0f,
                  "item 8: the dying unit drives at the straight row (desire %.0f)", gCop.desiredSpeed);
        }
        // (am9) item 21: two minutes at the cap and the force's pace
        // itself has risen - the far unit's desire reads the heat
        {
            stageAnchor(grove, man0);
            gCop.felony = 4096.0f;
            host.velocity = Vec3(18, 0, 0);
            for (float t = 0.0f; t < 25.0f && ladderLiving() < 4; t += dt) {
                host.player = host.player + host.velocity * dt;
                CopTick(dt, host.player);
                host.StepAll(dt);
            }
            auto farDesire = [&](float seconds) {
                float best = 0.0f;
                for (float t = 0.0f; t < seconds; t += dt) {
                    host.player = host.player + host.velocity * dt;
                    CopTick(dt, host.player);
                    if (gCops[3].desiredSpeed > best)
                        best = gCops[3].desiredSpeed;
                    host.StepAll(dt);
                    if (ForceFakeHost::Body* b = host.Find(gCops[3].veh)) {
                        b->pos = host.player + Vec3(-150, 0, 0); // held far:
                        b->vel = host.velocity;                  // the far reading
                    }
                }
                return best;
            };
            const float cold = farDesire(2.0f);
            farDesire(128.0f); // the long cap-felony chase itself
            const float hot = farDesire(2.0f);
            CHECK(cold > 150.0f, "item 21 stage: the far desire reads at all (%.0f)", cold);
            CHECK(hot > cold * 1.08f,
                  "item 21: after two minutes at the cap the far desire carries the heat (%.0f -> %.0f)",
                  cold, hot);
        }
    }

    // ---- (an) THE 39-PACKAGE, LAYER B (blocks and the map)
    {
        std::vector<Vec3> bRoute;
        CHECK(VehicleRoute(Vec3(2480, -1660, 13), Vec3(1690, -2280, 13), &bRoute, nullptr) &&
                  bRoute.size() > 4,
              "the layer-B stage rides the Grove-to-airport road");
        RoadWalker bRoad;
        bRoad.Build(bRoute);
        // a block scenario: the anchor pursuing on the road, the man
        // walking it at speed v from road distance s0, until the
        // block stands
        auto stageBlockRun = [&](float felony, float s0, float v, float seconds) {
            float s = s0;
            Vec3 dir;
            const Vec3 mp = bRoad.PosAt(s, &dir);
            stageAnchor(bRoad.PosAt(s - 25.0f, nullptr), mp);
            gCop.felony = felony;
            host.player = mp;
            host.velocity = dir * v;
            for (float t = 0.0f; t < seconds && CopBlockCount() < kMaxBlockCars; t += dt) {
                s += v * dt;
                const Vec3 p = bRoad.PosAt(s, &dir);
                host.player = p;
                host.velocity = dir * v;
                CopTick(dt, p);
                host.StepAll(dt);
                for (auto& kv : host.bodies)
                    kv.second.pos.z = bRoad.PosAt(bRoad.NearestDist(kv.second.pos), nullptr).z;
            }
            return s;
        };

        // (an1) item 15: the block stands at his braking distance
        {
            const float s0 = bRoad.NearestDist(grove) + 60.0f;
            stageBlockRun(1400.0f, s0, 40.0f, 20.0f);
            CHECK(CopBlockCount() == kMaxBlockCars, "item 15 stage: the fast block stands");
            const float dFast = (gCops[kMaxCopCars].pos - host.player).Length();
            CHECK(dFast >= 95.0f,
                  "item 15: at 40 m/s the block stands at his braking distance (%.0f m)", dFast);
            stageBlockRun(1400.0f, s0, 15.0f, 20.0f);
            CHECK(CopBlockCount() == kMaxBlockCars, "item 15 stage: the slow block stands");
            const float dSlow = (gCops[kMaxCopCars].pos - host.player).Length();
            CHECK(dSlow >= 25.0f && dSlow <= 60.0f,
                  "item 15: at 15 m/s the block stands at the floor (%.0f m)", dSlow);
        }
        // (an2) item 2: the standing block creeps to the moved crossing
        {
            const float s0 = bRoad.NearestDist(grove) + 60.0f;
            float s = stageBlockRun(1400.0f, s0, 15.0f, 20.0f);
            CHECK(CopBlockCount() == kMaxBlockCars, "item 2 stage: the block stands");
            const Vec3 carBefore = gCops[kMaxCopCars].pos;
            // he falls back 40 m of road and opens the throttle: his
            // braking point (110 at 45 m/s) jumps ~35 m past the
            // standing line, while he stays inside the block's own
            // ping-out radius (93.75 m - the first staging fell back
            // 80 and the block was taken back before it could creep)
            s -= 40.0f;
            bool creeping = false;
            for (float t = 0.0f; t < 8.0f && !creeping; t += dt) {
                Vec3 dir;
                const Vec3 p = bRoad.PosAt(s, &dir);
                host.player = p;
                host.velocity = dir * 45.0f;
                CopTick(dt, p);
                host.StepAll(dt);
                for (int sl = kMaxCopCars; sl < kMaxCopUnits; ++sl)
                    if (gCops[sl].holding)
                        creeping = true;
            }
            CHECK(creeping, "item 2: the moved crossing sets the block creeping");
            bool arrived = false;
            float moved = 0.0f;
            for (float t = 0.0f; t < 10.0f && !arrived; t += dt) {
                // he holds his ground at speed: the crossing he
                // presents stays the one the creep was set by, so the
                // cars run one clean leg to the new line (a crawling
                // man re-aims the point every two seconds and the
                // arrival lands on an intermediate slot - the first
                // staging's 8 m read was that artifact)
                Vec3 dir;
                const Vec3 p = bRoad.PosAt(s, &dir);
                host.player = p;
                host.velocity = dir * 45.0f;
                CopTick(dt, p);
                host.StepAll(dt);
                for (auto& kv : host.bodies)
                    kv.second.pos.z = bRoad.PosAt(bRoad.NearestDist(kv.second.pos), nullptr).z;
                moved = (gCops[kMaxCopCars].pos - carBefore).Length();
                if (!gCops[kMaxCopCars].holding && moved > 8.0f)
                    arrived = true;
            }
            CHECK(arrived && moved > 15.0f && gCops[kMaxCopCars].mode == COP_BLOCK,
                  "item 2: the block car crept %.0f m to the new line and sleeps there, still a block",
                  moved);
        }
        // (an3) item 22: the airport run is read in advance
        {
            const int aptIdx = AtlasNearestPoi(Vec3(1690, -2280, 13), 15, false);
            CHECK(aptIdx >= 0, "item 22 stage: an airport POI stands at the road's end");
            AtlasPoiInfo apt;
            AtlasPoiGet(aptIdx, apt);
            float total = 0.0f;
            for (size_t i = 1; i < bRoute.size(); ++i)
                total += (bRoute[i] - bRoute[i - 1]).Length();
            stageBlockRun(3000.0f, total - 380.0f, 15.0f, 25.0f);
            CHECK(CopBlockCount() == kMaxBlockCars, "item 22 stage: the airport block stands");
            Vec3 doorRoad = apt.door;
            {
                const int dn = VehicleNearestNode(apt.door, 120.0f);
                Vec3 np;
                if (dn >= 0 && VehicleNodePos(dn, &np))
                    doorRoad = np;
            }
            float toDoor = -1.0f;
            std::vector<Vec3> r;
            CHECK(VehicleRoute(gCops[kMaxCopCars].pos, doorRoad, &r, &toDoor),
                  "item 22 stage: the block-to-door route reads");
            CHECK(toDoor >= 15.0f && toDoor <= 75.0f,
                  "item 22: the block stands on the approach, %.0f m of road before the door's road access",
                  toDoor);
        }
        // (an4) item 9: at the cap, a unit holds the rear exit
        {
            float s = bRoad.NearestDist(grove) + 60.0f;
            Vec3 dir;
            const Vec3 mp = bRoad.PosAt(s, &dir);
            stageAnchor(bRoad.PosAt(s - 25.0f, nullptr), mp);
            gCop.felony = 4096.0f;
            host.player = mp;
            host.velocity = dir * 15.0f;
            for (float t = 0.0f; t < 25.0f && ladderLiving() < 3; t += dt) {
                s += 15.0f * dt;
                const Vec3 p = bRoad.PosAt(s, &dir);
                host.player = p;
                host.velocity = dir * 15.0f;
                CopTick(dt, p);
                host.StepAll(dt);
            }
            CHECK(ladderLiving() == 3, "item 9 stage: three units up (%d)", ladderLiving());
            int guardSlot = -1;
            bool released = false;
            for (float t = 0.0f; t < 30.0f && !released; t += dt) {
                s += 15.0f * dt;
                const Vec3 p = bRoad.PosAt(s, &dir);
                host.player = p;
                host.velocity = dir * 15.0f;
                CopTick(dt, p);
                host.StepAll(dt);
                if (guardSlot < 0) {
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (gCops[sl].holding && (gCops[sl].holdPoint - p).Dot(dir) < -15.0f)
                            guardSlot = sl;
                } else if (!gCops[guardSlot].holding) {
                    released = true;
                }
            }
            CHECK(guardSlot >= 0, "item 9: a unit took the rear-exit post behind him");
            CHECK(released, "item 9: the rear watch stands down after its ten seconds");
        }
        // (an5) item 34: his road dies ahead - the second unit holds
        // the mouth of the dead end
        {
            int tip = -1, mouthNode = -1, preyNode = -1;
            Vec3 tipPos, mouthPos, preyPos2, preyHead;
            for (int n = 0; n < VehicleNodeCount() && tip < 0; ++n) {
                Vec3 np;
                if (!VehicleNodePos(n, &np) || (np - grove).Length() > 400.0f ||
                    VehicleNodeDegree(n) > 1)
                    continue;
                // walk the pocket back to its mouth, remembering the
                // node one step in from the tip (the prey's spot)
                int cur = n, prev = -1, mouthN = -1, firstStep = -1;
                float walked = 0.0f;
                Vec3 curPos = np, firstStepPos;
                for (int step = 0; step < 40; ++step) {
                    const int deg = VehicleNodeDegree(cur);
                    if (deg >= 3) {
                        mouthN = cur;
                        break;
                    }
                    int next = -1;
                    Vec3 nextPos;
                    for (int i = 0; i < deg; ++i) {
                        const int to = VehicleNodeEdgeTo(cur, i);
                        if (to < 0 || to == prev)
                            continue;
                        if (VehicleNodePos(to, &nextPos))
                            next = to;
                    }
                    if (next < 0)
                        break;
                    if (firstStep < 0) {
                        firstStep = next;
                        firstStepPos = nextPos;
                    }
                    walked += (nextPos - curPos).Length();
                    prev = cur;
                    cur = next;
                    curPos = nextPos;
                }
                if (mouthN >= 0 && walked >= 30.0f && firstStep >= 0) {
                    tip = n;
                    tipPos = np;
                    mouthNode = mouthN;
                    VehicleNodePos(mouthN, &mouthPos);
                    preyNode = firstStep;
                    preyPos2 = firstStepPos;
                    Vec3 h = tipPos - preyPos2;
                    h.z = 0.0f;
                    preyHead = h.Length() > 0.01f ? h * (1.0f / h.Length()) : Vec3(1, 0, 0);
                }
            }
            CHECK(tip >= 0, "item 34 stage: a dead end with a real pocket stands near Grove");
            if (tip >= 0) {
                stageAnchor(preyPos2 - preyHead * 30.0f, preyPos2);
                gCop.felony = 2000.0f;
                host.player = preyPos2;
                host.velocity = preyHead * 8.0f; // he is driving in,
                host.vehicle = 42;               // and stays staged so
                int slotFar = -1;                // the law can be read
                bool held = false;
                for (float t = 0.0f; t < 25.0f && !held; t += dt) {
                    host.player = preyPos2;
                    host.velocity = preyHead * 8.0f;
                    CopTick(dt, preyPos2);
                    host.StepAll(dt);
                    // pin the force: the anchor 10 m behind him, the
                    // next unit 25 m behind - near and second-near
                    for (int sl = 0; sl < kMaxCopCars; ++sl) {
                        if (gCops[sl].veh < 0)
                            continue;
                        if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                            const float back = sl == 0 ? 10.0f : 25.0f;
                            b->pos = preyPos2 - preyHead * back;
                            b->vel = preyHead * 8.0f;
                            b->yaw = std::atan2(preyHead.x, preyHead.y);
                            if (sl != 0)
                                slotFar = sl;
                        }
                    }
                    if (slotFar >= 0 && gCops[slotFar].holding &&
                        (gCops[slotFar].holdPoint - mouthPos).Length() < 12.0f)
                        held = true;
                }
                CHECK(held, "item 34: the second unit holds the mouth of his dead end");
            }
        }
        // (an6) item 27: he goes inside - the doors are camped
        {
            const char* famName = nullptr;
            Vec3 inside;
            bool staged = false;
            for (int i = 0; i < AtlasHeavenCount() && !staged; ++i) {
                const char* f = nullptr;
                Vec3 e;
                if (!AtlasHeavenGet(i, f, e))
                    continue;
                const char* f2 = nullptr;
                if (!AtlasHeavenFamAt(e, 4.0f, f2))
                    continue;
                // the camp stands on the family's street doors, so
                // the staged family must be one the atlas connects
                // heaven-to-doors by name (the first heaven in the
                // table, a club interior, has heavens but no door
                // range under its name - the law camps where the
                // data connects, and logs where it cannot)
                int first = 0, count = 0;
                if (AtlasDoorFamRange(f2, first, count) && count > 0) {
                    famName = f2;
                    inside = e;
                    staged = true;
                }
            }
            CHECK(staged, "item 27 stage: a heaven stands to hide in");
            if (staged) {
                stageAnchor(inside + Vec3(60, 0, 0), inside);
                host.vehicle = -1; // on foot, inside
                host.player = inside;
                host.velocity = Vec3();
                bool camped = false;
                for (float t = 0.0f; t < 4.0f && !camped; t += dt) {
                    CopTick(dt, inside);
                    host.StepAll(dt);
                    if (gCop.holding) {
                        int first = 0, count = 0;
                        if (AtlasDoorFamRange(famName, first, count))
                            for (int i = first; i < first + count; ++i) {
                                const char* f3 = nullptr;
                                Vec3 en, ex;
                                if (AtlasDoorGet(i, f3, en, ex) &&
                                    (gCop.holdPoint - en).Length() < 3.0f)
                                    camped = true;
                            }
                    }
                }
                CHECK(camped, "item 27: the anchor camps a door of the family he is inside");
                CHECK(gCop.known, "item 27: inside, his position is never lost");
                const Vec3 outside = inside + Vec3(300, 0, 0);
                host.player = outside;
                bool released = false;
                for (float t = 0.0f; t < 4.0f && !released; t += dt) {
                    CopTick(dt, outside);
                    host.StepAll(dt);
                    if (!gCop.holding)
                        released = true;
                }
                CHECK(released, "item 27: outside again, the camp stands down");
            }
        }
        // (an7) item 28: the stacked floors - the odd unit's far
        // descent starts at the prey's height. Graded as a DISTINCTION
        // (the shifted unit's aim rides his floor, the unshifted
        // unit's rides its own); if the graph's geometry will not
        // separate them here, the honest grade is the attempt itself
        {
            int upNode = -1;
            Vec3 upPos;
            for (int i = 0; i < VehicleNodeCount() && upNode < 0; ++i) {
                Vec3 a;
                if (!VehicleNodePos(i, &a))
                    continue;
                const int j = VehicleNearestNode(Vec3(a.x, a.y, a.z + 8.0f), 15.0f);
                if (j < 0 || j == i)
                    continue;
                Vec3 b;
                if (VehicleNodePos(j, &b) && b.z - a.z > 5.0f && b.z - a.z < 14.0f) {
                    upNode = j;
                    upPos = b;
                }
            }
            CHECK(upNode >= 0, "item 28 stage: a stacked pair stands in the graph");
            if (upNode >= 0) {
                stageAnchor(upPos + Vec3(150, 0, 0), upPos);
                gCop.felony = 2500.0f;
                host.player = upPos;
                host.velocity = Vec3();
                // a close pass first: the position must be KNOWN and
                // the pursuit live before the far readings mean anything
                for (float t = 0.0f; t < 25.0f && ladderLiving() < 2; t += dt) {
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh))
                        b->pos = upPos + Vec3(20, 0, 0);
                    CopTick(dt, upPos);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() == 2 && CopPursuingCount() >= 1,
                      "item 28 stage: the force is up over the stacked road");
                const Vec3 groundSpot = upPos + Vec3(70, 0, -8.0f);
                for (float t = 0.0f; t < 3.0f; t += dt) {
                    CopTick(dt, upPos);
                    host.StepAll(dt);
                    for (int sl = 0; sl < 2; ++sl)
                        if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                            b->pos = groundSpot + Vec3(0, sl * 6.0f, 0);
                            b->vel = Vec3();
                        }
                }
                CHECK(std::fabs(gCops[1].target.z - upPos.z) < 3.0f,
                      "item 28: the shifted unit's far aim rides the prey's floor (target z %.1f, prey z %.1f)",
                      gCops[1].target.z, upPos.z);
                // the unshifted unit's aim ALSO lands on the upper
                // road at this pair (its descent climbs within the
                // look-ahead) - the sim cannot separate the two
                // starts here, so the floor CHOICE is graded eye-only
                // in the report; what is pinned is the shifted half
                // of the force reading his floor directly
            }
        }

        // (ao) LAYER C - presence and shape: the force arrives as a
        // force and stays one
        {
            const Vec3 G(2480, -1660, 13); // Grove, on the graph
            // (ao1) item 23, the void pin: the graph is bidirectional
            {
                int edges = 0, oneWay = 0;
                for (int n = 0; n < VehicleNodeCount(); ++n) {
                    const int deg = VehicleNodeDegree(n);
                    for (int e = 0; e < deg; ++e) {
                        const int m = VehicleNodeEdgeTo(n, e);
                        if (m < 0)
                            continue;
                        ++edges;
                        bool back = false;
                        const int d2 = VehicleNodeDegree(m);
                        for (int e2 = 0; e2 < d2; ++e2)
                            if (VehicleNodeEdgeTo(m, e2) == n) {
                                back = true;
                                break;
                            }
                        if (!back)
                            ++oneWay;
                    }
                }
                CHECK(oneWay == 0,
                      "item 23: wrong-way needs no law - %d of %d directed edges lack a reverse",
                      oneWay, edges);
            }
            // (ao2) item 4: the ladder's last car at the top rungs is
            // the heavy one
            {
                host.player = G;
                host.velocity = Vec3();
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -25, 0), G);
                gCop.felony = 3000.0f;
                for (float t = 0.0f; t < 15.0f && ladderLiving() < 4; t += dt) {
                    host.player = G;
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() == 4, "item 4 stage: the ladder is full");
                int heavies = 0;
                bool linesOk = true;
                for (int sl = 0; sl < kMaxCopCars; ++sl) {
                    if (gCops[sl].veh < 0 || gCops[sl].mode == COP_NONE)
                        continue;
                    if (gCops[sl].heavyUnit) {
                        ++heavies;
                        if (gCops[sl].wreckLine != 202500.0f)
                            linesOk = false;
                    } else if (gCops[sl].wreckLine != 135000.0f)
                        linesOk = false; // SpawnUnit writes the
                                          // standing line at birth
                }
                CHECK(heavies == 1 && linesOk,
                      "item 4: exactly one heavy unit, his book line 202500, the rest the standing 135000 (heavies %d)",
                      heavies);
            }
            // (ao3) item 5: at batter, spawns alternate to his front
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3(0, 20, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -30, 0), G);
                gCop.felony = 4096.0f;
                const size_t base = host.spawnPts.size();
                for (float t = 0.0f; t < 15.0f && host.spawnPts.size() < base + 2; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, G);
                }
                CHECK(host.spawnPts.size() >= base + 2,
                      "item 5 stage: two batter spawns recorded (%d)",
                      (int)(host.spawnPts.size() - base));
                if (host.spawnPts.size() >= base + 2) {
                    const float y1 = host.spawnPts[base].y - G.y;
                    const float y2 = host.spawnPts[base + 1].y - G.y;
                    CHECK(y1 > 15.0f && y2 < -15.0f,
                          "item 5: at batter the spawns alternate front (%.0f) then behind (%.0f)",
                          y1, y2);
                }
            }
            // (ao4) items 3+20: a flight is written down, its exit
            // ambushed at once, and the junction remembered later
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3(0, 20, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -60, 0), G);
                gCop.felony = 3000.0f;
                for (float t = 0.0f; t < 12.0f && ladderLiving() < 3; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() >= 3 && gCop.known,
                      "item 3 stage: the force is up and holds his position");
                // freeze the force on an exact 55 m ring, facing him:
                // inside the hot sight cone (~64 m), outside the
                // ambush pick's 50 m floor - where they converged to
                // is the chase's business, not this law's staging
                Vec3 spots[kMaxCopCars];
                for (int sl = 0; sl < kMaxCopCars; ++sl) {
                    const float a = (float)sl * 2.1f;
                    spots[sl] = G + Vec3(std::sin(a), std::cos(a), 0.0f) * 55.0f;
                    if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                        b->pos = spots[sl];
                        b->vel = Vec3();
                        b->yaw = std::atan2(G.x - spots[sl].x, G.y - spots[sl].y);
                    }
                }
                const Vec3 farAway = G + Vec3(0, 400, 0);
                bool ambushed = false;
                for (float t = 0.0f; t < 9.0f && !ambushed; t += dt) {
                    host.player = farAway;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, farAway);
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                            b->pos = spots[sl];
                            b->vel = Vec3();
                        }
                    for (int sl = 0; sl < kMaxCopUnits; ++sl)
                        if (gCops[sl].holding &&
                            (gCops[sl].holdPoint - (G + Vec3(0, 45, 0))).Length() < 5.0f)
                            ambushed = true;
                }
                {
                    float bestHold = 1e9f;
                    int bestSlot = -1;
                    for (int sl = 0; sl < kMaxCopUnits; ++sl)
                        if (gCops[sl].holding) {
                            const float dd = (gCops[sl].holdPoint - (G + Vec3(0, 45, 0))).Length();
                            if (dd < bestHold) { bestHold = dd; bestSlot = sl; }
                        }
                    CHECK(!gCop.known && ambushed,
                          "item 20: the flight's exit is ambushed the moment the position is lost (known %d, nearest hold %.0f m from the exit, unit %d mode %d)",
                          (int)gCop.known, bestHold > 1e8f ? -1.0f : bestHold, bestSlot,
                          bestSlot >= 0 ? (int)gCops[bestSlot].mode : -1);
                }
                // the watch runs out; then he passes the written
                // junction again and the memory posts it anew
                for (float t = 0.0f; t < 7.0f; t += dt) {
                    host.player = farAway;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, farAway);
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                            b->pos = spots[sl];
                            b->vel = Vec3();
                        }
                }
                bool reposted = false;
                const Vec3 backNear = G + Vec3(10, 0, 0);
                for (float t = 0.0f; t < 4.0f && !reposted; t += dt) {
                    host.player = backNear;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, backNear);
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (ForceFakeHost::Body* b = host.Find(gCops[sl].veh)) {
                            b->pos = spots[sl];
                            b->vel = Vec3();
                        }
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (gCops[sl].holding &&
                            (gCops[sl].holdPoint - (G + Vec3(0, 45, 0))).Length() < 5.0f)
                            reposted = true;
                }
                CHECK(reposted,
                      "item 3: passing the written junction again posts its exit ambush anew");
            }
            // (ao5) item 7: the ring around a pinned man
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3();
                host.vehicle = -1;
                stageAnchor(G + Vec3(0, -25, 0), G);
                gCop.felony = 2000.0f;
                host.vehicle = -1; // on foot for the ring
                int settled = 0;
                for (float t = 0.0f; t < 12.0f && settled < 2; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                    settled = 0;
                    for (int sl = 0; sl < kMaxCopCars; ++sl) {
                        if (!gCops[sl].holding || !gCops[sl].holdArrived)
                            continue;
                        const float d = (gCops[sl].pos - G).Length();
                        if (d > 5.0f && d < 11.0f)
                            ++settled;
                    }
                }
                CHECK(settled >= 2,
                      "item 7: the ring settles - %d units hold the circle around him", settled);
                int ringSlots[4];
                int ringN = 0;
                for (int sl = 0; sl < kMaxCopCars && ringN < 4; ++sl)
                    if (gCops[sl].holding)
                        ringSlots[ringN++] = sl;
                bool released = false;
                Vec3 moved = G;
                for (float t = 0.0f; t < 3.0f && !released; t += dt) {
                    // he MOVES off - a man who merely stands somewhere
                    // else is a new centre and the ring rightly
                    // re-forms on him there (the first staging read
                    // exactly that re-forming as a failed release)
                    moved = moved + Vec3(10.0f * dt, 0, 0);
                    host.player = moved;
                    host.velocity = Vec3(10, 0, 0);
                    CopTick(dt, moved);
                    host.StepAll(dt);
                    released = true;
                    for (int i = 0; i < ringN; ++i)
                        if (gCops[ringSlots[i]].holding)
                            released = false;
                }
                CHECK(released, "item 7: he moves off the centre and the ring opens");
            }
            // (ao6) item 29: the shoreline while he swims
            {
                CopReset();
                host.sea = 13.0f; // the water stands at his height
                host.player = G;
                host.velocity = Vec3();
                host.vehicle = -1;
                stageAnchor(G + Vec3(0, -30, 0), G);
                gCop.felony = 2000.0f;
                host.vehicle = -1; // on foot, swimming
                bool posted = false;
                for (float t = 0.0f; t < 12.0f && !posted; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                    for (int sl = 0; sl < kMaxCopCars; ++sl)
                        if (gCops[sl].holding && gCops[sl].holdArrived &&
                            (gCops[sl].holdPoint - G).Length() < 55.0f)
                            posted = true;
                }
                CHECK(posted, "item 29: he swims and a unit holds the shoreline by him");
                int shoreSlots[3];
                int shoreN = 0;
                for (int sl = 0; sl < kMaxCopCars && shoreN < 3; ++sl)
                    if (gCops[sl].holding)
                        shoreSlots[shoreN++] = sl;
                host.sea = -1000.0f;
                bool down = false;
                Vec3 ashore = G;
                for (float t = 0.0f; t < 3.0f && !down; t += dt) {
                    // he climbs out and MOVES - a standing man ashore
                    // would be re-posted by the ring law in the same
                    // tick, which is that law working, not this one
                    // failing to stand down
                    ashore = ashore + Vec3(8.0f * dt, 0, 0);
                    host.player = ashore;
                    host.velocity = Vec3(8, 0, 0);
                    CopTick(dt, ashore);
                    host.StepAll(dt);
                    down = true;
                    for (int i = 0; i < shoreN; ++i)
                        if (gCops[shoreSlots[i]].holding)
                            down = false;
                }
                CHECK(down, "item 29: out of the water, the shoreline watch stands down");
            }
            // (ao7) item 18: off-road, the unit leaves the road too
            {
                CopReset();
                Vec3 Q;
                bool found = false;
                for (float x = 2350.0f; x <= 2650.0f && !found; x += 10.0f)
                    for (float y = -1850.0f; y <= -1550.0f && !found; y += 10.0f) {
                        const Vec3 cand(x, y, 13.0f);
                        if (VehicleNearestNode(cand, 25.0f) >= 0)
                            continue;
                        const int n = VehicleNearestNode(cand, 90.0f);
                        Vec3 np;
                        if (n < 0 || !VehicleNodePos(n, &np))
                            continue;
                        const float d = (np - cand).Length();
                        if (d < 50.0f || d > 90.0f)
                            continue;
                        Q = cand;
                        found = true;
                    }
                CHECK(found, "item 18 stage: an off-road field stands by the road");
                if (found) {
                    const int nn = VehicleNearestNode(Q, 90.0f);
                    Vec3 np;
                    VehicleNodePos(nn, &np);
                    host.player = Q;
                    host.velocity = Vec3();
                    host.vehicle = 42;
                    stageAnchor(np, Q);
                    gCop.felony = 2000.0f;
                    const float d0 = (gCop.pos - Q).Length();
                    float dEnd = d0;
                    for (float t = 0.0f; t < 4.0f; t += dt) {
                        host.player = Q;
                        host.velocity = Vec3();
                        CopTick(dt, Q);
                        host.StepAll(dt);
                        dEnd = (gCop.pos - Q).Length();
                    }
                    CHECK((gCop.target - Q).Length() < 6.0f && dEnd < d0 - 8.0f,
                          "item 18: off-road, the unit's aim is the man himself and it closes cross-country (%.0f -> %.0f m)",
                          d0, dEnd);
                }
            }
            // (ao8) item 24: the traffic car in the corridor is aimed
            // at as a weapon
            {
                CopReset();
                host.traffic.clear();
                Host::VehicleInfo tv;
                tv.veh = 777;
                tv.pos = G + Vec3(1, -9, 0);
                host.traffic.push_back(tv);
                host.player = G;
                host.velocity = Vec3(0, 5, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -18, 0), G);
                gCop.felony = 2000.0f;
                bool aimed = false;
                for (float t = 0.0f; t < 5.0f && !aimed; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 5, 0);
                    // the anchor is pinned in the corridor: a free
                    // anchor drives through the staged geometry in
                    // under a second, before the first tactics tick
                    // can read it (the first staging's miss)
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = G + Vec3(0, -16, 0);
                        b->vel = Vec3();
                        b->yaw = 0.0f;
                    }
                    CopTick(dt, G);
                    host.StepAll(dt);
                    if ((gCop.target - tv.pos).Length() < 1.5f)
                        aimed = true;
                }
                CHECK(aimed, "item 24: the corridor's traffic car becomes the unit's aim");
                host.traffic.clear();
            }
            // (ao9) item 25: the wounded friend is the helper's aim
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3(0, 8, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -20, 0), G);
                gCop.felony = 2000.0f;
                for (float t = 0.0f; t < 12.0f && ladderLiving() < 2; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 8, 0);
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() >= 2, "item 25 stage: two units are up");
                if (ladderLiving() >= 2) {
                    const Vec3 wSpot = G + Vec3(6, -10, 0);
                    gCops[1].totalDamage = 100000.0f; // past 60% of his
                        // book, short of the retirement line
                    bool aimed = false;
                    for (float t = 0.0f; t < 5.0f && !aimed; t += dt) {
                        host.player = G;
                        host.velocity = Vec3(0, 8, 0);
                        if (ForceFakeHost::Body* b = host.Find(gCops[1].veh)) {
                            b->pos = wSpot;
                            b->vel = Vec3();
                            b->yaw = 0.0f; // facing north
                        }
                        CopTick(dt, G);
                        host.StepAll(dt);
                        if (gCop.aimOverrideOn &&
                            (gCop.aimOverride - (wSpot + Vec3(0, -3.2f, 0))).Length() < 2.0f)
                            aimed = true;
                    }
                    CHECK(aimed,
                          "item 25: the sound unit is aimed at the wounded friend's rear quarter");
                }
            }
            // (ao10) item 30: a wreck in the line is part of the line
            {
                CopReset();
                std::vector<Vec3> route;
                float routeLen = 0.0f;
                VehicleRoute(Vec3(2480, -1660, 13), Vec3(1690, -2280, 13), &route, &routeLen);
                RoadWalker w;
                w.Build(route);
                const float s0 = 700.0f;
                Vec3 dir0;
                const Vec3 startP = w.PosAt(s0, &dir0);
                host.player = startP;
                host.velocity = dir0 * 15.0f;
                host.vehicle = 42;
                stageAnchor(w.PosAt(s0 - 25.0f, &dir0), startP);
                gCop.felony = 1200.0f;
                for (float t = 0.0f; t < 12.0f && ladderLiving() < 2; t += dt) {
                    host.player = startP;
                    host.velocity = dir0 * 15.0f;
                    CopTick(dt, startP);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() >= 2, "item 30 stage: a second unit is up");
                // burn it where the coming block will stand: the
                // fire happens 3.2..4.2 s into the run (48..63 m at
                // 15 m/s) and the fan crowns 86 m past the fire point
                // (ahead 35 x 1.6 + 30), so the point lands at
                // s0+134..149 - the wreck sits mid-window at +141
                // (two earlier stagings, +80 and +95, both priced the
                // point without its fan reach and were outrun)
                const Vec3 wreckSpot = w.PosAt(s0 + 141.0f, &dir0);
                if (ForceFakeHost::Body* b = host.Find(gCops[1].veh))
                    b->pos = wreckSpot;
                gCops[1].totalDamage = 200000.0f;
                CopTick(dt, startP);
                host.StepAll(dt);
                // dip and re-arm the block chain, then run at it
                gCop.felony = 1000.0f;
                CopTick(dt, startP);
                gCop.felony = 1400.0f;
                float s = s0;
                for (float t = 0.0f; t < 14.0f && CopBlockCount() == 0; t += dt) {
                    s += 15.0f * dt;
                    Vec3 dir;
                    const Vec3 p = w.PosAt(s, &dir);
                    host.player = p;
                    host.velocity = dir * 15.0f;
                    CopTick(dt, p);
                    host.StepAll(dt);
                }
                CHECK(CopBlockCount() == kMaxBlockCars - 1,
                      "item 30: the block over the wreck places one car fewer - the wreck holds its berth (cars %d, first block car %.0f m from the wreck)",
                      CopBlockCount(),
                      CopBlockCount() > 0 ? (gCops[kMaxCopCars].pos - wreckSpot).Length() : -1.0f);
            }
        }

        // (ap) LAYER D+E - the priced disable and the sound law
        {
            const Vec3 G(2480, -1660, 13);
            // (ap1) item 39: every big moment is announced, logged
            // and counted - the six kinds, staged one by one
            {
                stageAnchor(G + Vec3(0, -25, 0), G);
                host.vehicle = 42;
                gCop.felony = 700.0f; // the pursuit opens (wanted 1)
                for (float t = 0.0f; t < 1.5f; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                gCop.felony = 2500.0f; // the rungs climb, units join
                for (float t = 0.0f; t < 4.0f; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                host.vehicle = -1; // he bails out mid-pursuit
                for (float t = 0.0f; t < 1.0f; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                host.vehicle = 42;
                gCop.felony = 1000.0f; // dip and re-arm the block chain
                CopTick(dt, G);
                gCop.felony = 2500.0f;
                for (float t = 0.0f; t < 8.0f && CopBlockCount() == 0; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 15, 0);
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                if (ladderLiving() >= 2)
                    gCops[1].totalDamage = 200000.0f; // a unit goes down
                for (float t = 0.0f; t < 1.0f; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(CopRadioLinesFired() >= 5 && CopRadioEventMask() == 63,
                      "item 39: the radio speaks at every big moment - %d lines, kind mask %d (want 63: open|rung|bail|block|death|join)",
                      CopRadioLinesFired(), CopRadioEventMask());
                CHECK(Heard(SND_COP_RADIO_07) + Heard(SND_COP_RADIO_08) >= 1,
                      "item 39: the opening call is on the recording, not just in the count");
            }
            // (ap2) item 39/32: at night the force arrives dark and
            // announces itself at contact; a settled ring sleeps
            {
                host.dark = true;
                stageAnchor(G + Vec3(0, -60, 0), G);
                host.vehicle = 42;
                gCop.felony = 2000.0f;
                for (float t = 0.0f; t < 0.5f; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                ForceFakeHost::Body* b = host.Find(gCop.veh);
                CHECK(b && !b->cmdSiren,
                      "item 39/32: in the dark, closing from 60 m, the siren sleeps");
                bool heard = false;
                for (float t = 0.0f; t < 10.0f && !heard; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    CopTick(dt, G);
                    host.StepAll(dt);
                    b = host.Find(gCop.veh);
                    if (b && (b->pos - G).Length() < 16.0f) {
                        // one more breath of ticks at contact range
                        for (float u = 0.0f; u < 0.4f; u += dt) {
                            CopTick(dt, G);
                            host.StepAll(dt);
                        }
                        b = host.Find(gCop.veh);
                        heard = b && b->cmdSiren;
                    }
                }
                CHECK(heard, "item 39/32: at contact the siren wakes - the force announces itself");
                host.dark = false;
            }
            // (ap3) item 10: the disable is priced off the kicks -
            // a tyre per full-blooded hit, spaced, four in all
            {
                stageAnchor(G + Vec3(0, -25, 0), G);
                host.vehicle = 42;
                gCop.felony = 2000.0f;
                host.player = G;
                host.velocity = Vec3();
                for (float t = 0.0f; t < 3.0f && gCop.mode != COP_PURSUE; t += dt) {
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(gCop.mode == COP_PURSUE, "item 10 stage: the anchor pursues");
                const int pops0 = host.tyrePops;
                const int vehPops0 = host.vehTyrePops[42]; // (ap1)'s
                    // organic kicks may already have cost him tyres
                    // on this same host - the pin reads deltas
                auto parkFar = [&]() {
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = G + Vec3(0, -60, 0);
                        b->vel = Vec3();
                    }
                    CopTick(dt, G);
                    host.StepAll(dt);
                };
                auto kickOnce = [&]() {
                    parkFar(); // the separation re-arms the throw
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = G + Vec3(0, -4, 0);
                        b->vel = Vec3(0, 20, 0);
                        b->yaw = 0.0f;
                    }
                    CopTick(dt, G);
                    host.StepAll(dt);
                };
                auto waitOut = [&](float seconds) {
                    for (float t = 0.0f; t < seconds; t += dt)
                        parkFar();
                };
                kickOnce();
                CHECK(host.tyrePops == pops0 + 1,
                      "item 10: the first full-blooded kick costs him a tyre (%d)",
                      host.tyrePops - pops0);
                const int kicksBefore = gCop.kickCount;
                kickOnce();
                CHECK(gCop.kickCount > kicksBefore && host.tyrePops == pops0 + 1,
                      "item 10: a second kick inside six seconds throws him but pops nothing - the stages are spaced");
                waitOut(6.5f);
                kickOnce();
                CHECK(host.tyrePops == pops0 + 2, "item 10: past the spacing, the next kick buys the second tyre");
                waitOut(6.5f);
                kickOnce();
                waitOut(6.5f);
                kickOnce();
                CHECK(host.tyrePops == pops0 + 4, "item 10: four spaced kicks, four tyres (%d)",
                      host.tyrePops - pops0);
                waitOut(6.5f);
                kickOnce();
                CHECK(host.tyrePops == pops0 + 4 && host.vehTyrePops[42] - vehPops0 == host.tyrePops - pops0,
                      "item 10: a fifth stage does not exist - and every pop was on HIS car (pops %d, on-42 %d)",
                      host.tyrePops - pops0, host.vehTyrePops[42] - vehPops0);
            }
            // (aq) item 35: the ramp jump - the force drives at the
            // landing while he is in the air, and a kick inside the
            // touchdown window throws half again
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3(0, 18, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -25, 0), G);
                gCop.felony = 3000.0f;
                // the arc, scripted ballistically: vz = 8 - 9.8 t,
                // z = 8t - 4.9 t^2 over the launch height, touchdown
                // at t = 1.633, 29.4 m north of the launch
                auto arcAt = [&](float t, Vec3* pos, Vec3* vel) {
                    if (t < 1.633f) {
                        *pos = G + Vec3(0, 18.0f * t, 8.0f * t - 4.9f * t * t);
                        *vel = Vec3(0, 18.0f, 8.0f - 9.8f * t);
                    } else {
                        *pos = G + Vec3(0, 29.4f + 18.0f * (t - 1.633f), 0);
                        *vel = Vec3(0, 18.0f, 0);
                    }
                };
                Vec3 mp, mv;
                float aimErr = 1e9f;
                for (float t = 0.0f; t < 1.633f; t += dt) {
                    arcAt(t, &mp, &mv);
                    host.player = mp;
                    host.velocity = mv;
                    CopTick(dt, mp);
                    host.StepAll(dt);
                    if (t > 0.55f && t < 0.65f)
                        aimErr = (gCop.target - (G + Vec3(0, 29.4f, 0))).Length();
                }
                CHECK(aimErr < 7.0f,
                      "item 35: mid-flight the force aims at the landing, not the car in the air (err %.1f m)",
                      aimErr);
                auto stageKick = [&](const Vec3& preyPos) {
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = preyPos + Vec3(0, -4.5f, 0);
                        b->vel = Vec3(0, 38, 0);
                        b->yaw = 0.0f;
                    }
                };
                arcAt(1.633f, &mp, &mv);
                // one settle tick first: the touchdown registers in
                // the tracker, which runs after the kick's Pre pass -
                // staging the body in the same tick would kick a man
                // the book still reads as falling
                host.player = mp;
                host.velocity = mv;
                CopTick(dt, mp);
                host.StepAll(dt);
                mp = mp + mv * dt;
                stageKick(mp);
                const int kicks0 = host.kickVehCount;
                for (int i = 0; i < 25 && host.kickVehCount == kicks0; ++i) {
                    host.player = mp;
                    host.velocity = mv;
                    CopTick(dt, mp);
                    host.StepAll(dt);
                    mp = mp + mv * dt; // he rolls on at 18
                }
                const float landDv = host.lastKickDv.Length();
                CHECK(host.kickVehCount > kicks0 && landDv > 15.0f && landDv < 21.0f,
                      "item 35: the landing hit throws half again (dv %.1f, the plain throw is ~12)",
                      landDv);
                // the control: the same geometry a second later,
                // the window shut, is the plain throw again
                for (float t = 0.0f; t < 1.2f; t += dt) {
                    host.player = mp;
                    host.velocity = mv;
                    CopTick(dt, mp);
                    host.StepAll(dt);
                    mp = mp + mv * dt;
                }
                if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                    b->pos = mp + Vec3(0, -30, 0); // separation re-arms
                    b->vel = Vec3();
                }
                for (int i = 0; i < 10; ++i) {
                    host.player = mp;
                    host.velocity = mv;
                    CopTick(dt, mp);
                    host.StepAll(dt);
                    mp = mp + mv * dt;
                }
                stageKick(mp);
                const int kicks1 = host.kickVehCount;
                for (int i = 0; i < 25 && host.kickVehCount == kicks1; ++i) {
                    host.player = mp;
                    host.velocity = mv;
                    CopTick(dt, mp);
                    host.StepAll(dt);
                    mp = mp + mv * dt;
                }
                const float plainDv = host.lastKickDv.Length();
                CHECK(host.kickVehCount > kicks1 && plainDv > 9.0f && plainDv < 14.0f,
                      "item 35: outside the window the same kick is the plain throw (dv %.1f)",
                      plainDv);
            }
            // (ar) items 36+38: the motorcycle fields beside a full
            // ladder and cuts direct at range; a pinned on-foot prey
            // at the top rungs puts a driver out of his car, and the
            // prey mounting anything calls him back
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3(0, 20, 0);
                host.vehicle = 42;
                stageAnchor(G + Vec3(0, -25, 0), G);
                gCop.felony = 3000.0f;
                int bikeSlot = -1;
                for (float t = 0.0f; t < 20.0f && bikeSlot < 0; t += dt) {
                    host.player = G;
                    host.velocity = Vec3(0, 20, 0);
                    CopTick(dt, G);
                    host.StepAll(dt);
                    for (int sl = 0; sl < kMaxCopUnits; ++sl)
                        if (gCops[sl].bikeUnit && gCops[sl].veh >= 0)
                            bikeSlot = sl;
                }
                CHECK(bikeSlot >= 0 && host.bikeSpawns >= 1,
                      "item 36: at the top rungs a motorcycle cop fields beside the full ladder (slot %d)",
                      bikeSlot);
                if (bikeSlot >= 0) {
                    // the feed split at 80 m: the bike cuts at the
                    // man; the car rides the road field's descent
                    const Vec3 preyPos = G + Vec3(0, 200, 0);
                    host.player = preyPos;
                    host.velocity = Vec3(0, 20, 0);
                    if (ForceFakeHost::Body* b = host.Find(gCops[bikeSlot].veh)) {
                        b->pos = preyPos + Vec3(0, -80, 0);
                        b->vel = Vec3(0, 10, 0);
                        b->yaw = 0.0f;
                    }
                    if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                        b->pos = preyPos + Vec3(0, 80, 0);
                        b->vel = Vec3(0, 10, 0);
                        b->yaw = 0.0f;
                    }
                    CopTick(dt, preyPos);
                    const float bikeErr = (gCops[bikeSlot].target - preyPos).Length();
                    const float carErr = (gCop.target - preyPos).Length();
                    CHECK(bikeErr < 3.0f && carErr > 5.0f,
                          "item 36: at 80 m the bike's aim is the man himself (%.1f m) while the car rides the descent (%.1f m)",
                          bikeErr, carErr);
                }
            }
            {
                CopReset();
                host.player = G;
                host.velocity = Vec3();
                host.vehicle = -1;
                stageAnchor(G + Vec3(0, -20, 0), G);
                host.vehicle = -1; // stageAnchor seats him; this
                                   // staging's prey stands on foot
                gCop.felony = 3000.0f;
                for (float t = 0.0f; t < 8.0f && ladderLiving() < 2; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    host.vehicle = -1;
                    CopTick(dt, G);
                    host.StepAll(dt);
                }
                CHECK(ladderLiving() >= 2, "item 38 stage: two units are up");
                const int bail0 = host.bailOrders;
                bool bailed = false;
                for (float t = 0.0f; t < 6.0f && !bailed; t += dt) {
                    host.player = G;
                    host.velocity = Vec3();
                    host.vehicle = -1;
                    if (ForceFakeHost::Body* b = host.Find(gCops[1].veh)) {
                        b->pos = G + Vec3(10, 0, 0);
                        b->vel = Vec3();
                        b->yaw = -1.5708f; // facing the man
                    }
                    CopTick(dt, G);
                    host.StepAll(dt);
                    bailed = gCops[1].bailState == 1;
                }
                CHECK(bailed && host.bailOrders > bail0,
                      "item 38: a driver is ordered out against a pinned on-foot prey at the top rungs");
                CopTick(dt, G);
                host.StepAll(dt);
                CHECK(gCops[1].repDriverOut && !gCops[1].feedLive,
                      "item 38: the report reads the driver out and the car stands parked (feed live %d)",
                      (int)gCops[1].feedLive);
                host.vehicle = 42; // he mounts - everybody back in
                CopTick(dt, G);
                host.StepAll(dt);
                CHECK(gCops[1].bailState == 0 && !host.lastBailOut,
                      "item 38: the prey mounting calls the driver back to his car");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The /coptest machine's fake host: the force fake plus the decoys and
// the two stagings the machine needs to be honest in a fake world:
//  - the decoy car drives itself by the same MirrorStep law (its feed
//    is stored by DecoyCarDrive), the decoy ped just stands;
//  - LineBlocked models a city-block grid of building faces for the
//    recovery staging's PLAIN-oracle probes (streets every 24 m around
//    the Grove test ground): the wedge can be FOUND from anywhere.
//    Sight keeps the skip-veh oracle the other cop sims use - the grid
//    exists for staging, not for blinding the force;
//  - CopBodyPlace wedges for real: a placed body cannot advance past
//    the wall plane (its forward component is clamped at the plane)
//    until it has reversed 2 m clear - the one property of a wall the
//    recovery grade is about. Sideways and backward stay free, as
//    they are at a real wall.
struct CopTestFakeHost : ForceFakeHost {
    struct DecoyCar {
        bool up = false;
        Vec3 pos, vel;
        float yaw = 0.0f, speed = 0.0f;
        Vec3 drivePt;
        float cruise = 0.0f;
    };
    struct DecoyPed {
        bool up = false;
        Vec3 pos;
        float health = 100.0f;
        Vec3 kickVel; // the momentum channel's throw, integrated by
                      // StepTest with ground friction - the re-graded
                      // run-over bar grades the HIT off the
                      // displacement this produces, so the fake must
                      // carry a thrown man the way the game's does
    };
    DecoyCar decoyCar;
    DecoyPed decoyPed;
    int SpawnDecoyCar(const Vec3& at, float headingDeg) override {
        decoyCar = DecoyCar();
        decoyCar.up = true;
        decoyCar.pos = at;
        decoyCar.yaw = headingDeg * 0.0174533f;
        decoyCar.drivePt = at;
        return 5001;
    }
    void DecoyCarDrive(int, const Vec3& at, float cruiseMps) override {
        decoyCar.drivePt = at;
        decoyCar.cruise = cruiseMps;
    }
    bool DecoyReport(int, Vec3* pos, Vec3* vel) override {
        if (!decoyCar.up)
            return false;
        *pos = decoyCar.pos;
        *vel = decoyCar.vel;
        return true;
    }
    void RetireDecoy(int) override { decoyCar.up = false; }
    int SpawnDecoyPed(const Vec3& at) override {
        decoyPed = DecoyPed();
        decoyPed.up = true;
        decoyPed.pos = at;
        // the man stands at a house front on his roadside: one real
        // wall face through his spot, along the road. The recovery
        // phase's probes find THIS wall beside the man he stands
        // next to - the only building the fake keeps, after two
        // city grids lied about sight (an axis-aligned carpet of
        // houses cannot align with the graph's roads, so every
        // chord between two road users "crossed a house" and the
        // block phase's chase was blind from birth)
        wall = true;
        wallC = Vec3(at.x, at.y, at.z - 1.5f); // the base host's wall
            // test wants the wall's centre BELOW the probe line's
            // height (its z gate is strict) - the wall stands on the
            // ground, the probes run at body height
        wallDir = Vec3(1, 0, 0);
        wallHalf = 30.0f;
        return 6001;
    }
    bool DecoyPedReport(int, Vec3* pos, float* health) override {
        if (!decoyPed.up)
            return false;
        *pos = decoyPed.pos;
        *health = decoyPed.health;
        return true;
    }
    void RetireDecoyPed(int) override { decoyPed.up = false; }
    bool LineBlocked(const Vec3& a, const Vec3& b) override {
        // the one house front (set when the decoy ped is staged -
        // see SpawnDecoyPed) plus whatever the base host carries;
        // the ground is otherwise open, and sight runs as the roads
        // run
        return TestHost::LineBlocked(a, b);
    }
    bool LineBlockedSkipVeh(const Vec3& a, const Vec3& b, int veh) override {
        return TestHost::LineBlockedSkipVeh(a, b, veh);
    }
    // the wedge
    bool wedged = false;
    int wedgeVeh = -1;
    Vec3 wedgePos, wedgeDir;
    void CopBodyPlace(int veh, const Vec3& at, float headingDeg) override {
        ForceFakeHost::CopBodyPlace(veh, at, headingDeg);
        wedged = true;
        wedgeVeh = veh;
        wedgePos = at;
        wedgeDir = Vec3(std::sin(headingDeg * 0.0174533f), std::cos(headingDeg * 0.0174533f), 0.0f);
    }
    // the momentum channel on the decoys: the car's throw lands as
    // speed along its own nose plus a yaw shove (the fake's plain
    // reading of a launch); the ped's throw is a velocity he slides
    // out in StepTest below
    void KickVehicle(int veh, const Vec3& dv, float yawRate) override {
        TestHost::KickVehicle(veh, dv, yawRate);
        if (decoyCar.up && veh == 5001) {
            const Vec3 f(std::sin(decoyCar.yaw), std::cos(decoyCar.yaw), 0.0f);
            decoyCar.speed += dv.x * f.x + dv.y * f.y;
            decoyCar.yaw += yawRate * 0.3f;
        }
    }
    void KickPed(int ped, const Vec3& dv) override {
        TestHost::KickPed(ped, dv);
        if (decoyPed.up && ped == 6001)
            decoyPed.kickVel = decoyPed.kickVel + dv;
    }
    void StepTest(float dt) {
        StepAll(dt);
        if (decoyCar.up)
            MirrorStep(decoyCar.pos, decoyCar.yaw, decoyCar.speed, decoyCar.vel,
                       decoyCar.drivePt, decoyCar.cruise, decoyCar.cruise > 0.05f, dt);
        if (decoyPed.up && decoyPed.kickVel.Length() > 0.05f) {
            // a thrown man slides: ground friction at ~1.8/s of decay
            // (a 20 m/s throw carries him ~11 m before he stops - the
            // game's own slide is its business; the fake's is a plain
            // exponential, enough to grade displacement honestly)
            decoyPed.pos = decoyPed.pos + decoyPed.kickVel * dt;
            decoyPed.kickVel = decoyPed.kickVel * std::fmax(0.0f, 1.0f - 1.8f * dt);
        }
        if (wedged) {
            Body* b = Find(wedgeVeh);
            if (!b) {
                wedged = false;
            } else {
                Vec3 rel = b->pos - wedgePos;
                rel.z = 0.0f;
                const float fwdD = rel.Dot(wedgeDir);
                Vec3 side = rel - wedgeDir * fwdD;
                if (fwdD < -2.0f) {
                    wedged = false; // backed clear: the wall lets go
                } else if (fwdD > 0.3f) {
                    b->pos = wedgePos + wedgeDir * 0.3f + side;
                    b->pos.z = wedgePos.z;
                    if (b->speed > 0.0f)
                        b->speed = 0.0f;
                    b->vel = Vec3(std::sin(b->yaw), std::cos(b->yaw), 0.0f) * b->speed;
                }
            }
        }
    }
};

// The whole /coptest machine, run end to end against the fake: the
// sim plays the user's part where the machine asks (it peppers the
// anchor once the decoys are down, takes a car when a chase is on,
// bails when told by circumstance) and then grades the GRADER off its
// own sheet: every counted scenario must pass in the fake world, the
// run must finish, and the cleanup must be real (no bodies, no
// decoys, the felony page handed back at its pre-run value).
static void TestCopTest() {
    CopTestFakeHost host;
    SetHost(&host);
    WideStage();
    CopDismiss(); // sanitize the global unit table against this fresh
                  // host: an earlier test may have left units standing
                  // whose body ids this host reuses for new bodies
                  // (their stale reports poisoned a whole run once)
    CopReset();
    ThreatNoteBoss(-1, false, Vec3(), false);
    CHECK(LoadVehicleGraphFromRepo(), "the coptest sim rides the real vehicle graph");
    const float dt = 0.02f;
    const Vec3 stand(2486.0f, -1654.0f, 13.5f);
    host.hasPlayer = true;
    host.player = stand;
    host.vehicle = -1;
    host.velocity = Vec3();
    CopForceFelony(333.0f); // a page with a past: cleanup must hand it back
    CHECK(CopTestStart(), "the self-test starts");
    bool bailed = false, droveCar = false, sawDecoy = false, peppered = false;
    float driveT = 0.0f;
    float t = 0.0f;
    for (; t < 1200.0f && CopTestRunning(); t += dt) {
        // the user's part, by circumstance alone. The peppering waits
        // until the decoy acts are over (a decoy has been up and is
        // down again) and the anchor is not yet pursuing: that is
        // player phase 1's stage exactly (a fresh anchor, calibrating
        // or stood by, the user told to shoot it). An earlier
        // choreography peppered whenever the decoys were down, which
        // included the opening SPIKE, and shot the felony to 2819
        // under the spike route (the machine's own log convicted it:
        // "the spike is over" at felony 2819)
        if (host.decoyCar.up || host.decoyPed.up)
            sawDecoy = true;
        const bool decoysDown = !host.decoyCar.up && !host.decoyPed.up;
        if (sawDecoy && decoysDown && gCop.veh >= 0 && gCop.mode != COP_PURSUE &&
            gCop.mode != COP_BLOCK && !droveCar) {
            // player phase 1's ask: his hand on the anchor - one host
            // point a tick through the shot channel until it lands
            if (ForceFakeHost::Body* b = host.Find(gCop.veh)) {
                b->qShot = 1.0f;
                peppered = true;
            }
        }
        // he takes a car only for a chase that is on HIM - which the
        // sim knows because it did the peppering that opened it. An
        // earlier gate keyed on felony + any pursuit, and fired at
        // the recovery->block seam (recovery's forced 4096, pursuers
        // still warm from the ped), sending the "player" 1200 m east
        // through the whole block phase
        if (peppered && decoysDown && gThreat.felony >= 659.0f && CopPursuingCount() > 0 && !droveCar) {
            droveCar = true;
            host.vehicle = 42; // he takes a car and drives
        }
        if (droveCar && !bailed) {
            driveT += dt;
            host.velocity = Vec3(20.0f, 0.0f, 0.0f);
            host.player = host.player + host.velocity * dt;
            int ps = -1;
            float best = 1e9f;
            for (int s = 0; s < kMaxCopUnits; ++s) {
                if (gCops[s].veh < 0 || gCops[s].mode != COP_PURSUE)
                    continue;
                const float d = (gCops[s].pos - host.player).Length();
                if (d < best) {
                    best = d;
                    ps = s;
                }
            }
            // he bails when the chase is on him - after he has actually
            // DRIVEN a few seconds. The first cut bailed on the very
            // tick he took the car (the anchor was already on top of
            // him from phase 1), so the bail phase never once saw him
            // in a car: no user teleports in and out in one frame.
            if (driveT > 3.0f && ps >= 0 && best < 45.0f) {
                bailed = true; // he gets out and stays on foot
                host.vehicle = -1;
                host.velocity = Vec3();
            }
        }
        CopTick(dt, host.player);
        host.StepTest(dt);
    }
    CHECK(!CopTestRunning(), "the self-test finished its run (%.0f s of sim)", t);
    const CopTestSheet sheet = CopTestSheetForTest();
    CHECK(sheet.counted >= 11, "the sheet counted %d scenarios (spike..soak, roadblock may skip)", sheet.counted);
    CHECK(sheet.passed == sheet.counted, "every counted scenario passed in the fake world (%d of %d)",
          sheet.passed, sheet.counted);
    static const char* const kMustPass[] = { "spike", "arrival", "ram", "siren", "runover",
                                             "ring", "recovery", "player-felony", "bailout", "radio", "soak" };
    for (const char* name : kMustPass) {
        bool pass = false, counted = false;
        const bool found = CopTestVerdictForTest(name, &pass, &counted);
        CHECK(found && counted && pass, "coptest sheet: %s passed", name);
    }
    {
        bool pass = false, counted = false;
        const bool found = CopTestVerdictForTest("roadblock", &pass, &counted);
        CHECK(!found || !counted || pass, "coptest sheet: the roadblock passed or honestly skipped");
    }
    CHECK(CopUnitCount() == 0, "cleanup: the force stood down whole (%d units left)", CopUnitCount());
    CHECK(!host.decoyCar.up && !host.decoyPed.up, "cleanup: both decoys retired");
    CHECK(std::fabs(gThreat.felony - 333.0f) < 1.5f,
          "cleanup: the felony page was handed back at its pre-run value (%.0f)", gThreat.felony);
    CopReset();
    SetHost(nullptr);
}

// piece 3, stage 2: the cop's OWN body - the converter's real output in
// the repo (driver2/cop_car.bin, baked from the user's disc by
// tools/convert_driver2_cop.py - never a synthetic bin) must load whole
// in the core loader, measure out to his own dimensions, refuse broken
// files, and draw his body + four wheels through the sink. The pins are
// his data: 128 verts / 252 tris off CHICAGO.LEV's model 0, the bbox the
// LCF's colBox (129, 88, 382) foretold, the wheel centres his LCF
// placements give through the draw law.
static const char* CopCarBinPath() {
    static const char* paths[] = { "driver2/cop_car.bin", "../driver2/cop_car.bin",
                                   "../../driver2/cop_car.bin", "../../../driver2/cop_car.bin" };
    for (const char* p : paths) {
        FILE* f = std::fopen(p, "rb");
        if (f) {
            std::fclose(f);
            return p;
        }
    }
    return nullptr;
}

static std::vector<unsigned char> ReadWholeFile(const char* p) {
    FILE* f = std::fopen(p, "rb");
    if (!f)
        return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes((size_t)std::max<long>(n, 0));
    const bool ok = n > 0 && std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    return ok ? bytes : std::vector<unsigned char>();
}

static void TestCopCarModel() {
    TestHost host;
    SetHost(&host);
    const char* path = CopCarBinPath();
    CHECK(path != nullptr, "driver2/cop_car.bin is in the repo");
    if (!path)
        return;
    std::vector<unsigned char> bytes = ReadWholeFile(path);
    CHECK(bytes.size() == 16488, "the shipped cop_car.bin is the converter's 16488 bytes (got %d)",
          (int)bytes.size());

    UnloadCopCarModel();
    CHECK(LoadCopCarModel(bytes.data(), bytes.size()), "the real cop_car.bin loads in the core loader");
    CHECK(CopCarModelReady(), "and the cop body reports itself ready");
    CHECK(CopCarTextureCount() == 16, "his body names 16 textures incl. the flat card (got %d)",
          CopCarTextureCount());
    CHECK(CopCarBodyTris() == 252, "his body is 252 triangles (got %d)", CopCarBodyTris());
    Vec3 lo, hi;
    CHECK(CopCarBodyExtents(&lo, &hi), "the body measures out");
    // fix round 3: at the convicted geometry factor (153.6 units = 1 m)
    // the disc's own units (x +-132, y +-377, z +-88) are a real sedan:
    // 1.72 m wide, 4.91 m long, 1.15 m tall. At the old 256 factor this
    // same check pinned the 0.6x body the eye test rejected.
    CHECK(std::fabs(lo.x + 0.8594f) < 0.01f && std::fabs(hi.x - 0.8594f) < 0.01f &&
              std::fabs(lo.y + 2.4544f) < 0.01f && std::fabs(hi.y - 2.4544f) < 0.01f &&
              std::fabs(lo.z + 0.5729f) < 0.01f && std::fabs(hi.z - 0.5729f) < 0.01f,
          "his dimensions are the disc's own: x +-0.859, y +-2.454, z +-0.573 (got %.3f..%.3f, %.3f..%.3f, %.3f..%.3f)",
          lo.x, hi.x, lo.y, hi.y, lo.z, hi.z);

    // broken files are refused, whole and loudly
    CHECK(!LoadCopCarModel(bytes.data(), bytes.size() / 2), "a truncated bin is refused");
    CHECK(!CopCarModelReady(), "and leaves the body unloaded");
    std::vector<unsigned char> bad = bytes;
    bad[0] = 'X';
    CHECK(!LoadCopCarModel(bad.data(), bad.size()), "a foreign bin is refused");
    CHECK(LoadCopCarModel(bytes.data(), bytes.size()), "the real bin loads back");

    // his wheel centres, from his own LCF through the draw law
    // (DrawCarWheels, fix round 3): side = (17 - vx) / (-17 - vx) by
    // wheelnum half - the LCF vx is not the centre - forward = the
    // post-FixCarCos vz, height = the measured 64 units. At the old
    // law this check pinned centres at -vx outright: the tyres sat
    // ~0.07 m outside the body's slab, the eye test's "glued on".
    Vec3 centers[4];
    CopWheelCenters(centers);
    CHECK(std::fabs(centers[0].x - 0.6836f) < 0.005f && std::fabs(centers[0].y - 1.4453f) < 0.005f &&
              std::fabs(centers[2].x + 0.6901f) < 0.005f && std::fabs(centers[2].y - 1.4453f) < 0.005f,
          "the front axle sits at his LCF placements (got %.3f/%.3f and %.3f/%.3f)",
          centers[0].x, centers[0].y, centers[2].x, centers[2].y);
    CHECK(std::fabs(centers[1].y + 1.3477f) < 0.005f && std::fabs(centers[3].y + 1.3477f) < 0.005f,
          "the rear axle sits at his LCF placements (got %.3f, %.3f)", centers[1].y, centers[3].y);
    CHECK(centers[0].z == centers[1].z && centers[1].z == centers[2].z && centers[2].z == centers[3].z,
          "all four centres share the measured arch height");
    CHECK(std::fabs(centers[0].z + 64.0f / 153.6f) < 0.005f,
          "the centres ride 64 of his units under his origin (z %.3f)", centers[0].z);

    // the draw: body + four wheels through the sink, at the identity pose
    RecordSink sink;
    SetModelSink(&sink);
    Pose pose; // identity: X right, Y forward, Z up, origin at his origin
    sink.Reset();
    DrawCopCar(pose, 0.0f, centers, 0.0f, 0.0f, false, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] == 252 + 4 * 16, "the draw emits his 252 body tris + 4x16 wheel tris (%d)",
          sink.quads[0]);
    CHECK(sink.lo.z < -0.84f && sink.lo.z > -0.89f,
          "the wheels reach the ground line his arch measures (lo z %.3f)", sink.lo.z);
    CHECK(std::fabs(sink.hi.z - 0.5729f) < 0.01f, "the light bar is the highest point (hi z %.3f)", sink.hi.z);
    CHECK(std::fabs(sink.hi.y - 2.4544f) < 0.01f && std::fabs(sink.lo.y + 2.4544f) < 0.01f,
          "nose and tail are his own length (y %.3f..%.3f)", sink.lo.y, sink.hi.y);

    // dented to his book's end and spun a quarter turn: same count, sane box
    sink.Reset();
    DrawCopCar(pose, 1.0f, centers, 0.3f, 1.5708f, true, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] == 252 + 4 * 16, "dent + steer + spin + fast wheels draw the same count (%d)",
          sink.quads[0]);
    // the envelope is his own at the convicted factor: body x +-0.859
    // plus a dent's reach, z from the wheels' contact patch (-0.866)
    // to the roof (0.573) plus the same dent reach - a vert outside
    // this box has flown off, anything inside is his own geometry
    CHECK(sink.lo.x > -1.05f && sink.hi.x < 1.05f && sink.lo.z > -0.95f && sink.hi.z < 0.75f,
          "the dented, spun draw stays inside his own envelope (x %.2f..%.2f, z %.2f..%.2f)",
          sink.lo.x, sink.hi.x, sink.lo.z, sink.hi.z);

    // unloaded: nothing is drawn - there is no fallback body in the core
    UnloadCopCarModel();
    sink.Reset();
    DrawCopCar(pose, 0.0f, centers, 0.0f, 0.0f, false, 1.0f, 1.0f, 1.0f, 1.0f);
    CHECK(sink.quads[0] == 0, "without cop_car.bin nothing of him is drawn (%d)", sink.quads[0]);
    CHECK(LoadCopCarModel(bytes.data(), bytes.size()), "the body loads back for the bridge");

    // piece 3, stage 3: his voice in the sound table - appended after
    // the boss's block, every earlier row untouched (the table's own
    // append-only law, pinned at both seams)
    CHECK(SND_COP_SIREN == SND_BOSS_USP + 1 && SND_COP_RADIO_17 == SND_COUNT - 1,
          "his sound events append after the boss's, nothing renumbered");
    CHECK(kSoundEvents[SND_BOSS_USP].first == 530 && kSoundEvents[SND_BOSS_USP].count == 1 &&
              std::strcmp(kSoundFiles[530].file, "boss_usp.ogg") == 0,
          "the boss's last row is untouched");
    CHECK(kSoundEvents[SND_COP_SIREN].first == 531 && kSoundEvents[SND_COP_SIREN].count == 1 &&
              std::strcmp(kSoundFiles[531].file, "cop_siren.ogg") == 0,
          "his siren is file row 531");
    CHECK(kSoundEvents[SND_COP_ENGINE_REVS].first == 532 &&
              std::strcmp(kSoundFiles[532].file, "cop_engine_revs.ogg") == 0 &&
              kSoundEvents[SND_COP_ENGINE_IDLE].first == 533 &&
              std::strcmp(kSoundFiles[533].file, "cop_engine_idle.ogg") == 0,
          "his engine layers are file rows 532/533");
    CHECK(kSoundEvents[SND_COP_RADIO_17].first == 545 &&
              std::strcmp(kSoundFiles[545].file, "cop_radio_17.ogg") == 0,
          "his last radio phrase is file row 545");
}

int main(int argc, char** argv) {
    if (argc > 1)
        return InspectWorldFile(argv[1]);
    printf("blocks %d, items %d, shaped %d, shapeless %d, cooking %d\n", NUM_BLOCKS - 1, ITEM_END - FIRST_ITEM, kNumShaped,
           kNumShapeless, kNumCooking);
    const uint16_t P = ID_OAK_PLANKS, S = ID_STICK, C = ID_COBBLESTONE, I = ID_IRON_INGOT, G = ID_GUNPOWDER;
    Expect("log->planks", Craft(2, 2, { ID_OAK_LOG, 0, 0, 0 }), ID_OAK_PLANKS, 4);
    Expect("birch log->planks", Craft(3, 3, { 0, 0, 0, 0, ID_BIRCH_LOG, 0, 0, 0, 0 }), ID_BIRCH_PLANKS, 4);
    Expect("sticks", Craft(2, 2, { P, 0, P, 0 }), ID_STICK, 4);
    Expect("sticks (right column)", Craft(2, 2, { 0, P, 0, P }), ID_STICK, 4);
    Expect("crafting table", Craft(2, 2, { P, P, P, P }), ID_CRAFTING_TABLE, 1);
    Expect("wooden pickaxe", Craft(3, 3, { P, P, P, 0, S, 0, 0, S, 0 }), ID_WOODEN_PICKAXE, 1);
    Expect("stone pickaxe", Craft(3, 3, { C, C, C, 0, S, 0, 0, S, 0 }), ID_STONE_PICKAXE, 1);
    Expect("stone axe", Craft(3, 3, { C, C, 0, C, S, 0, 0, S, 0 }), ID_STONE_AXE, 1);
    Expect("stone axe mirrored", Craft(3, 3, { 0, C, C, 0, S, C, 0, S, 0 }), ID_STONE_AXE, 1);
    Expect("iron sword", Craft(3, 3, { 0, I, 0, 0, I, 0, 0, S, 0 }), ID_IRON_SWORD, 1);
    Expect("diamond sword", Craft(3, 3, { ID_DIAMOND, 0, 0, ID_DIAMOND, 0, 0, S, 0, 0 }), ID_DIAMOND_SWORD, 1);
    Expect("furnace", Craft(3, 3, { C, C, C, C, 0, C, C, C, C }), ID_FURNACE, 1);
    Expect("chest", Craft(3, 3, { P, P, P, P, 0, P, P, P, P }), ID_CHEST, 1);
    Expect("bow", Craft(3, 3, { 0, S, ID_STRING, S, 0, ID_STRING, 0, S, ID_STRING }), ID_BOW, 1);
    Expect("arrow", Craft(3, 3, { ID_FLINT, 0, 0, S, 0, 0, ID_FEATHER, 0, 0 }), ID_ARROW, 4);
    Expect("tnt", Craft(3, 3, { G, ID_SAND, G, ID_SAND, G, ID_SAND, G, ID_SAND, G }), ID_TNT, 1);
    Expect("flint and steel", Craft(2, 2, { I, ID_FLINT, 0, 0 }), ID_FLINT_AND_STEEL, 1);
    Expect("firework rocket", Craft(2, 2, { G, ID_PAPER, 0, 0 }), ID_FIREWORK_ROCKET, 3);
    Expect("iron block", Craft(3, 3, { I, I, I, I, I, I, I, I, I }), ID_IRON_BLOCK, 1);
    Expect("iron block -> ingots", Craft(2, 2, { ID_IRON_BLOCK, 0, 0, 0 }), ID_IRON_INGOT, 9);
    Expect("bread", Craft(3, 3, { ID_WHEAT, ID_WHEAT, ID_WHEAT, 0, 0, 0, 0, 0, 0 }), ID_BREAD, 1);
    Expect("garbage", Craft(2, 2, { ID_DIRT, ID_STONE, 0, 0 }), 0, 0);
    Expect("empty", Craft(2, 2, { 0, 0, 0, 0 }), 0, 0);

    int t = 0;
    CHECK(CookResult(COOK_SMELTING, ID_RAW_IRON, &t) == ID_IRON_INGOT && t == 200, "smelt raw iron (t=%d)", t);
    CHECK(CookResult(COOK_BLASTING, ID_RAW_IRON, &t) == ID_IRON_INGOT && t == 100, "blast raw iron (t=%d)", t);
    CHECK(CookResult(COOK_SMELTING, ID_COBBLESTONE) == ID_STONE, "smelt cobblestone");
    CHECK(CookResult(COOK_SMELTING, ID_SAND) == ID_GLASS, "smelt sand");
    CHECK(CookResult(COOK_SMELTING, ID_OAK_LOG) == ID_CHARCOAL, "smelt log");
    CHECK(CookResult(COOK_SMOKING, ID_BEEF) == ID_COOKED_BEEF, "smoke beef");
    CHECK(CookResult(COOK_SMELTING, ID_DIRT) == 0, "dirt must not smelt");

    CHECK(FuelTicks(ID_COAL) == 1600, "coal fuel");
    CHECK(FuelTicks(ID_OAK_PLANKS) == 300, "planks fuel %d", FuelTicks(ID_OAK_PLANKS));
    CHECK(FuelTicks(ID_STONE) == 0, "stone fuel");
    CHECK(MaxStack(ID_DIAMOND_SWORD) == 1 && MaxStack(ID_ENDER_PEARL) == 16 && MaxStack(ID_STONE) == 64, "stack sizes");
    CHECK(Item(ID_DIAMOND_SWORD).tool == TOOL_SWORD && Item(ID_DIAMOND_SWORD).damage == 7.0f, "diamond sword stats");
    CHECK(Item(ID_ELYTRA).armorSlot == 2 && Item(ID_ELYTRA).special == SP_ELYTRA, "elytra");
    CHECK(Item(ID_BOW).special == SP_BOW && Item(ID_ARROW).special == SP_ARROW, "bow/arrow");
    CHECK(Item(ID_FLINT_AND_STEEL).special == SP_IGNITER && Item(ID_FIREWORK_ROCKET).special == SP_FIREWORK,
          "igniter/firework");
    CHECK(Block(ID_STONE).requiresTool && Block(ID_STONE).tool == TOOL_PICKAXE, "stone needs pickaxe");
    CHECK(!Block(ID_DIRT).requiresTool && Block(ID_DIRT).tool == TOOL_SHOVEL, "dirt");
    CHECK(Block(ID_DIAMOND_ORE).tier == TIER_IRON, "diamond ore tier %d", Block(ID_DIAMOND_ORE).tier);
    CHECK(Block(ID_OAK_LOG).shape == SHAPE_COLUMN && Block(ID_FURNACE).shape == SHAPE_FACING, "shapes");
    CHECK(Block(ID_GLASS).render == RENDER_CUTOUT && Block(ID_WHITE_STAINED_GLASS).render == RENDER_TRANSLUCENT &&
              Block(ID_STONE).render == RENDER_OPAQUE,
          "render types");
    // facing: the front tile follows the facing
    for (int f = 0; f < 4; ++f) {
        static const int faces[4] = { FACE_NORTH, FACE_EAST, FACE_SOUTH, FACE_WEST };
        int fronts = 0;
        for (int k = 0; k < 4; ++k)
            if (BlockFaceTile(ID_FURNACE, faces[k], f) == Block(ID_FURNACE).tex[FACE_NORTH])
                fronts += (k == f) ? 1 : 10;
        CHECK(fronts == 1, "furnace facing %d front count %d", f, fronts);
    }
    CHECK(BlockFaceTile(ID_FURNACE, FACE_NORTH, META_LIT) == Block(ID_FURNACE).texFrontLit, "lit furnace front");
    CHECK(BlockFaceTile(ID_OAK_LOG, FACE_EAST, 1) == Block(ID_OAK_LOG).tex[FACE_TOP], "log axis X end");
    CHECK(BlockFaceTile(ID_OAK_LOG, FACE_TOP, 1) == Block(ID_OAK_LOG).tex[FACE_EAST], "log axis X side on top");

    size_t total = 0;
    for (int c = 0; c < CAT_COUNT; ++c) {
        printf("creative tab %d: %d items\n", c, (int)CreativeItems(c).size());
        total += CreativeItems(c).size();
    }
    size_t fluids = 0; // water and lava only come from buckets; a bed's head and a door's upper half only with their other half
    for (int b = 1; b < NUM_BLOCKS; ++b)
        if (IsFluidBlock(b) || IsFireBlock(b) || Block(b).shape == SHAPE_BED_HEAD || Block(b).shape == SHAPE_DOOR_UPPER)
            ++fluids;
    CHECK(total == (size_t)(NUM_BLOCKS - 1 - fluids + ITEM_END - FIRST_ITEM), "creative tabs cover everything (%d vs %d)",
          (int)total, (int)(NUM_BLOCKS - 1 - fluids + ITEM_END - FIRST_ITEM));
    {
        auto at = [](int tab, uint16_t id) {
            const std::vector<uint16_t>& v = CreativeItems(tab);
            return (int)(std::find(v.begin(), v.end(), id) - v.begin());
        };
        CHECK(CreativeItems(CAT_BUILDING)[0] == ID_OAK_LOG && at(CAT_BUILDING, ID_OAK_PLANKS) < at(CAT_BUILDING, ID_OAK_STAIRS) &&
                  at(CAT_BUILDING, ID_OAK_STAIRS) < at(CAT_BUILDING, ID_SPRUCE_LOG) && at(CAT_BUILDING, ID_STONE) < at(CAT_BUILDING, ID_STONE_STAIRS) &&
                  at(CAT_BUILDING, ID_STONE_STAIRS) < at(CAT_BUILDING, ID_COBBLESTONE),
              "the building blocks as in Minecraft: wood by wood type, then stone, each with its stairs and slabs");
        CHECK(at(CAT_COLORED, ID_WHITE_WOOL) + 1 == at(CAT_COLORED, ID_LIGHT_GRAY_WOOL) && at(CAT_NATURAL, ID_GRASS_BLOCK) == 0 &&
                  at(CAT_TOOLS, ID_WOODEN_SHOVEL) + 1 == at(CAT_TOOLS, ID_WOODEN_PICKAXE) && at(CAT_FOOD, ID_APPLE) == 0,
              "colours in Minecraft's order; the natural tab starts with grass; tools by material");
        CHECK(at(CAT_REDSTONE, ID_REDSTONE) < (int)CreativeItems(CAT_REDSTONE).size() &&
                  at(CAT_SPAWN_EGGS, ID_WARDEN_SPAWN_EGG) < (int)CreativeItems(CAT_SPAWN_EGGS).size(),
              "redstone and spawn eggs have their tabs");
    }

    // inventory behaviour
    PlayerInventory inv;
    ItemStack s;
    s.id = ID_STONE;
    s.count = 64;
    for (int i = 0; i < 36; ++i)
        CHECK(inv.Add(s) == 0, "add stack %d", i);
    CHECK(inv.Add(s) == 64, "full inventory must reject");
    CHECK(inv.CountOf(ID_STONE) == 36 * 64, "count");
    CHECK(inv.Remove(ID_STONE, 100) && inv.CountOf(ID_STONE) == 36 * 64 - 100, "remove");

    // the soldier exists only with his real model file: load it once, up front, so
    // every soldier test below meets the same world as the game (SoldierBinPath is
    // defined further up, with TestSoldierModel)
    if (LoadSoldierBinFromRepo())
        printf("soldier model loaded from %s\n", SoldierBinPath());
    else
        printf("WARNING: cs_soldier.bin not found from here - the soldier tests will fail loudly\n");

    TestPhysics();
    TestBlockRules();
    TestSurvival();
    TestEntities();
    TestInteract();
    TestMobs();
    TestNewMobs();
    TestSoldier();
    TestCreeper();
    TestCombat();
    TestFishing();
    TestVillagers();
    TestPlayerHealth();
    TestHands();
    TestScreens();
    TestHud();
    TestBlockMesh();
    TestShapes();
    TestBeds();
    TestDoors();
    TestWarden();
    TestLanguage();
    TestBodyYaw();
    TestChat();
    TestDebugScreen();
    TestModels();
    TestSoldierRealAsset();
    TestSoldierModel();
    TestSoldierAimFix();
    TestBoss();
    TestSmartPack();
    TestCsRules();
    TestBossFixRound();
    TestBossAggroLaw();
    TestBossStateChurn();
    TestBossDriver();
    TestBossJetpack();
    TestBossReanchor();
    TestBossDriverHop();
    TestBossDriverStartup();
    TestBossTracker();
    TestBossTravelModes();
    TestBossTenacity();
    TestBossOffPath();
    TestBossCoreJump();
    TestCop();
    TestCopForce();
    TestCopTest();
    TestCopCarModel();
    TestAtlas();
    TestSoldierPoseBounds();
    TestSoldierTransitionFuzz();
    TestBossDeathReset();
    TestCleanSA();
    TestPlayerAnim();
    TestRenderers();
    TestSave();
    TestControls();
    TestGameState();

    printf(gFail ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", gFail);
    return gFail;
}
