#pragma once
// The Driver 2 cop, living in San Andreas by the house formula: his brain
// in this core, his body a real car of the host's (src/gta/GtaCop.cpp),
// the city ours. The brain is a port of REDRIVER 2's CopControl1 +
// FELONY.C - read from the decompiled PS1 code, not reconstructed from
// behaviour; every rescale from the original's units is documented in
// Cop.cpp and in DRIVER2-STEP1-fa.md / DRIVER2-STEP2-fa.md. His model's
// own physics constants (powerRatio and friends) live in the original
// disc's CHICAGO.LCF; what drives him today is the original's handling
// TYPE and control law on the host car's own physics - stated, never
// dressed up.
//
// Step 2 grows the one cop into the force the original manages: a ladder
// of units (ControlNumberOfCops), the distance cutoff, and roadblocks
// (CreateRoadblock / PassiveCopTasks). The shape follows the original's
// own bookkeeping: the FELONY belongs to the PLAYER, not to any cop -
// FELONY.C keeps it on the player's car record - so here the felony page
// (level, clocks, offence timers, sight/known, the shared road field's
// throttle, the session steering calibration) lives on slot 0's state as
// the system page, and every unit reads it. What is per-unit is only the
// body, the damage book and the CopControl1 working state.

#include "Core.h"

namespace mc {

// -------------------------------------------------------------------
// The shared threat record: ONE player, two hunters. Both brains read and
// write this every tick; it is the whole of their teamwork (the user's
// law: whoever sees him, the other knows - «یه تیم، نه دو غریبه»).
// Fields, exactly:
//   playerPos / playerVel / playerInVehicle / playerVehicle - the feed:
//     the player's true position and velocity as the host reports them,
//     refreshed by the cop's tick (the boss reads the same host answers in
//     his own tick; this record is where they MEET).
//   felony / copPursuing / copPos / copAlive - written by the cop force's
//     tick: the felony level, whether any unit pursues, where the force's
//     lead car is.
//   bossState / bossEngaged / bossPos / bossAlive - written by the boss's
//     tick (MobsTick): his state number (Mob::bossState), whether he is in
//     combat, where he stands. A cop summoned while bossEngaged starts
//     INFORMED - he does not need his own offence to know a fight is on.
//   ourUnitsLost - OUR units the player has put out of the fight, counted
//     at the moment of retirement and only when the player's own damage
//     did the work (see ThreatNoteOurUnitLost). The replacement step's
//     shoot-order law (the user's, 2026-11-07: two of ours killed, or one
//     killed and he keeps firing) will read this counter; step 2 only
//     lays it and keeps it honest.
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
    int ourUnitsLost = 0;
};
extern SharedThreat gThreat;
// The boss's tick publishes his side here (called from MobsTick).
void ThreatNoteBoss(int state, bool engaged, const Vec3& pos, bool alive);
// One of OUR units was put out of the fight BY THE PLAYER: a cop whose
// book the player's damage spent (counted in CopTick at his retirement),
// and - in the replacement step, not yet - the boss himself when he
// falls. Nothing else calls this: a ping-out, a stand-down, a body the
// world streamed away - none of those are losses to the player.
void ThreatNoteOurUnitLost();

enum CopMode {
    COP_NONE = 0,   // no body in this slot
    COP_SPIKE,      // piece 0: the test controller drives the block route (slot 0 only)
    COP_IDLE,       // body stands by, handbrake on, felony watching
    COP_PURSUE,     // felony over the trigger: CopControl1 is driving
    COP_BLOCK,      // step 2: asleep across a road ahead of the player (roadblock)
    COP_LEAVE,      // step 2: stood down by the ladder - driving off to ping out
    COP_WRECKED     // totalDamage passed the original's line: he retires
};

// The ladder's own cap (maxCopCars, MISSION.C's default and the hardest
// missions' too): felony buys at most this many ladder units. Roadblock
// cars are NOT ladder units - in the original they ride the civilian
// pool with a cop flag until they wake - so they get their own slots
// behind the ladder's.
constexpr int kMaxCopCars = 4;
constexpr int kMaxBlockCars = 2;
constexpr int kMaxCopUnits = kMaxCopCars + kMaxBlockCars;

struct CopState {
    CopMode mode = COP_NONE;
    int veh = -1;              // the host's number for his car
    // the body's last report (the bridge reads these back from the car)
    Vec3 pos, fwd{ 0, 1, 0 }, vel;
    float speed = 0.0f;        // signed, m/s along his nose
    float prevSpeed = 0.0f;    // last tick's speed: the stuck witness
    float health = 1.0f;       // his BOOK's fraction left, 0..1 (not the
                               // host body's health - the bridge displays
                               // the book on the body, the book rules)
    float totalDamage = 0.0f;  // his damage book, the original's own scale
                               // (BCOLLIDE.C pricing): retires past 27000
    float playerBookDamage = 0.0f; // of that book, the part the PLAYER
                               // priced (his gunfire at the anchor, and an
                               // impact the same report ties to his ram):
                               // a retirement is "lost to the player" when
                               // this is at least half the book
    // ---- the system page (SLOT 0 ONLY - the felony is the player's, as
    // in FELONY.C; the other slots' copies of these fields are never read)
    float felony = 0.0f;
    float felonyClock = 0.0f;  // the 31-tick AdjustFelony clock, in seconds
    float offenceOccur[12] = {};   // occurrence warm-up timers (seconds)
    float offenceRepeat[12] = {};  // re-occurrence delays (seconds)
    bool seen = false;         // any unit's eyes on the player this tick
    float unseenT = 0.0f;      // seconds no sight: 256 ticks ≈ 5.12 s = lost
    bool known = false;        // player_position_known, the original's flag
    // CopControl1's working state (per unit)
    Vec3 target;               // this tick's aim point (tests read it)
    bool closePursuit = false; // FindCost < 2800: he is on the man's tail
    bool batter = false;       // felony at the ladder's top: flank, not tail
    bool informed = false;     // summoned into the boss's fight (shared record)
    float desiredSpeed = 0.0f; // m/s, signed
    float powerScale = 1.0f;   // the cutoff's hand on this unit: 1.0 inside
                               // the cutoff distance, 0.5 beyond it (the
                               // original's cutOffPowerScale 2048 of 4096)
    float wheel = 0.0f;        // the original's units: -512..512 of full lock
    float gas = 0.0f, brake = 0.0f; // the command this tick, 0..1
    float stuckT = 0.0f;
    float recoveryT = 0.0f;
    int recoverStrategy = 0;   // the original's rotating 8-pattern index
    bool frontLClear = true, frontRClear = true; // bumper-corner probes
    float probeT = 0.0f;
    // step 2: the ladder's leaving (stood down, driving off to ping out)
    bool blockUnit = false;    // a roadblock car, not a ladder unit
    std::vector<Vec3> leaveRoute;
    size_t leaveIdx = 0;
    // piece 0's spike controller (slot 0 only)
    std::vector<Vec3> spikeRoute;
    size_t spikeIdx = 0;
    bool spikeDone = false;    // the route ran out: the body answered (or not)
    float spikeLogT = 0.0f;
    // the steering sign is the one fact the SDK headers do not state:
    // which way m_fSteerAngle turns the car. The spike retires it in its
    // first second - command +0.6, read the turn off the vehicle's own
    // forward vector, and set the session's flip so that from then on a
    // positive command from this brain means a right turn, as CopControl1
    // means it. Until calibration lands, commands go out unflipped.
    // (Session state: it lives on slot 0's page and every unit reads it -
    // the sign is a fact about the host, not about one car.)
    bool steerFlip = false;
    bool steerCalDone = false;
    float calT = 0.0f;
    Vec3 calFwd{ 0, 1, 0 };
    float spikeLatch = 0.0f;   // the committed turn direction while the
                               // waypoint sits dead behind (the pure-
                               // pursuit error wraps at +-pi there and
                               // would bang the wheel lock-to-lock)
    // the far mode's field throttle (system page: ONE field for the force)
    float fieldT = 0.0f;
    Vec3 fieldGoal;
    bool fieldOk = false;
};
// All units. Slot 0 is the summoned anchor (and carries the system page);
// slots 1..kMaxCopCars-1 are the ladder's; the last kMaxBlockCars slots
// are roadblock cars.
extern CopState gCops[kMaxCopUnits];
// The piece-1 name for slot 0 - the summoned cop, the tests' cop.
extern CopState& gCop;

// Forget the bodies and the felony (a new session tore the world down).
void CopReset();
// /summon cop: stand the anchor body up near `at`, facing the player. The
// first drive is piece 0's spike route; the felony system is live from
// the same tick, and the ladder answers it from there. Returns false
// when the host could not make a body.
bool CopSummon(const Vec3& at, const Vec3& playerPos);
void CopTick(float dt, const Vec3& playerPos);
bool CopActive();
// The force's books, for tests and the log: living units (any slot with
// a body), units in pursuit, roadblock cars placed (asleep or awake),
// the ladder's wanted count this tick, and the cutoff distance the
// manager priced (1e9 = no cutoff in force).
int CopUnitCount();
int CopPursuingCount();
int CopBlockCount();
int CopWantedCount();
float CopCutoffDistance();

} // namespace mc
