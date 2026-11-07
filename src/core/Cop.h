#pragma once
// The Driver 2 cop, living in San Andreas by the house formula: his brain
// in this core, his body a real car of the host's (src/gta/GtaCop.cpp),
// the city ours. The brain is a port of REDRIVER 2's CopControl1 +
// FELONY.C - read from the decompiled PS1 code, not reconstructed from
// behaviour; every rescale from the original's units is documented in
// Cop.cpp and in DRIVER2-STEP1-fa.md. His model's own physics constants
// (powerRatio and friends) live in the original disc's CHICAGO.LCF and do
// not exist yet: what drives him today is the original's handling TYPE and
// control law on the host car's own physics - stated, never dressed up.

#include "Core.h"

namespace mc {

// -------------------------------------------------------------------
// The shared threat record: ONE player, two hunters. Both brains read and
// write this every tick; it is the whole of their teamwork in step 1 (the
// user's law: whoever sees him, the other knows - «یه تیم، نه دو غریبه»).
// Fields, exactly:
//   playerPos / playerVel / playerInVehicle / playerVehicle - the feed:
//     the player's true position and velocity as the host reports them,
//     refreshed by the cop's tick (the boss reads the same host answers in
//     his own tick; this record is where they MEET).
//   felony / copPursuing / copPos / copAlive - written by the cop's tick:
//     his felony level, whether his pursuit is live, where his car is.
//   bossState / bossEngaged / bossPos / bossAlive - written by the boss's
//     tick (MobsTick): his state number (Mob::bossState), whether he is in
//     combat, where he stands. A cop summoned while bossEngaged starts
//     INFORMED - he does not need his own offence to know a fight is on.
struct SharedThreat {
    Vec3 playerPos;
    Vec3 playerVel;
    bool playerInVehicle = false;
    int playerVehicle = -1;
    float felony = 0.0f;
    bool copPursuing = false;
    Vec3 copPos;
    bool copAlive = false;
    int bossState = -1;
    bool bossEngaged = false;
    Vec3 bossPos;
    bool bossAlive = false;
};
extern SharedThreat gThreat;
// The boss's tick publishes his side here (called from MobsTick).
void ThreatNoteBoss(int state, bool engaged, const Vec3& pos, bool alive);

enum CopMode {
    COP_NONE = 0,   // no body, no brain: /summon cop has not run (or he retired)
    COP_SPIKE,      // piece 0: the test controller drives the block route
    COP_IDLE,       // body stands by, handbrake on, felony watching
    COP_PURSUE,     // felony over the trigger: CopControl1 is driving
    COP_WRECKED     // totalDamage passed the original's line: he retires
};

struct CopState {
    CopMode mode = COP_NONE;
    int veh = -1;              // the host's number for his car
    // the body's last report (the bridge reads these back from the car)
    Vec3 pos, fwd{ 0, 1, 0 }, vel;
    float speed = 0.0f;        // signed, m/s along his nose
    float health = 1.0f;       // 0..1
    float totalDamage = 0.0f;  // the original's scale: retires past 27000
    // the felony system (FELONY.C's own numbers, 0..4096)
    float felony = 0.0f;
    float felonyClock = 0.0f;  // the 31-tick AdjustFelony clock, in seconds
    float offenceOccur[12] = {};   // occurrence warm-up timers (seconds)
    float offenceRepeat[12] = {};  // re-occurrence delays (seconds)
    bool seen = false;         // a cop's eyes on the player this tick
    float unseenT = 0.0f;      // seconds no sight: 256 ticks ≈ 5.12 s = lost
    bool known = false;        // player_position_known, the original's flag
    // CopControl1's working state
    Vec3 target;               // this tick's aim point (tests read it)
    bool closePursuit = false; // FindCost < 2800: he is on the man's tail
    bool batter = false;       // felony at the ladder's top: flank, not tail
    bool informed = false;     // summoned into the boss's fight (shared record)
    float desiredSpeed = 0.0f; // m/s, signed
    float wheel = 0.0f;        // the original's units: -512..512 of full lock
    float gas = 0.0f, brake = 0.0f; // the command this tick, 0..1
    float stuckT = 0.0f;
    float recoveryT = 0.0f;
    int recoverStrategy = 0;   // the original's rotating 8-pattern index
    bool frontLClear = true, frontRClear = true; // bumper-corner probes
    float probeT = 0.0f;
    // piece 0's spike controller
    std::vector<Vec3> spikeRoute;
    size_t spikeIdx = 0;
    bool spikeDone = false;    // the route ran out: the body answered (or not)
    float spikeLogT = 0.0f;
    // the far mode's field throttle
    float fieldT = 0.0f;
    Vec3 fieldGoal;
    bool fieldOk = false;
};
extern CopState gCop;

// Forget the body and the felony (a new session tore the world down).
void CopReset();
// /summon cop: stand his body up near `at`, facing the player. The first
// drive is piece 0's spike route; the felony system is live from the same
// tick. Returns false when the host could not make a body.
bool CopSummon(const Vec3& at, const Vec3& playerPos);
void CopTick(float dt, const Vec3& playerPos);
bool CopActive();

} // namespace mc
