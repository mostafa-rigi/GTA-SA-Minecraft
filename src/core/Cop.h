#pragma once
// The Driver 2 cop, living in San Andreas by the house formula: his brain
// in this core, his body a real car of the host's (src/gta/GtaCop.cpp),
// the city ours. The brain is a port of REDRIVER 2's CopControl1 +
// FELONY.C - read from the decompiled PS1 code, not reconstructed from
// behaviour; every rescale from the original's units is documented in
// Cop.cpp and in DRIVER2-STEP1-fa.md / DRIVER2-STEP2-fa.md. Piece 3
// (DRIVER2-STEP3-fa.md) brought the rest of him from the user's own
// disc: his CHICAGO.LCF constants as the control profile below, his own
// body drawn over the host car (CopCarModel), and his own voice
// (VOICES2.BLK). What still is not his: the host car's physics under
// the body, and the collision the host resolves - stated, never
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
// The complete update: two ROWS of two, as the original places them -
// the second row staggered behind the first, angled (piece 2 shipped one
// row and named this follow-up in its report).
constexpr int kMaxBlockCars = 4;
// The 39-package's one cap change, made openly: item 36's
// motorcycle is a ninth body the complete update's force never
// contemplated, and it cannot steal a roadblock berth (items
// 9/15/22/30 grade those four) nor a ladder slot (item 4's heavy
// owns the last one). The force is nine now - the cost is one more
// body on the host, named in the step report; every loop that
// means the BLOCK's berths still stops at kMaxCopCars +
// kMaxBlockCars, and only the hunting scans read all nine.
constexpr int kMaxBikeUnits = 1;
constexpr int kMaxCopUnits = kMaxCopCars + kMaxBlockCars + kMaxBikeUnits;

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
                               // (BCOLLIDE.C pricing): retires past the
                               // line below (135000 since the 5x update
                               // deepened it; the prices that fill it
                               // did not move)
    float wreckLine = 0.0f;    // this unit's retirement line: 0 reads
                               // as the standing 135000; the 39-package's
                               // heavy top-rung unit (item 4) carries
                               // 1.5x it in this field, set at spawn
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
    int recoverCount = 0;      // the complete update: how many recoveries
                               // this unit has begun since it stood up -
                               // /coptest grades recovery partly on this
                               // (a completion, not a strategy spam), and
                               // the unit line can name a spammer
    Vec3 recoveryFrom;         // where the running recovery began: the
                               // completion is judged against it (moved
                               // >= 1.5 m by the expiry = he is out)
    float fairStartT = 0.0f;   // fix round 5: the stuck witness's fair
                               // start - seconds after a fresh drive
                               // order (a spawn, a pursuit entry, a
                               // recovery just spent) during which the
                               // witness does not accumulate. The boss's
                               // statue round taught the same lesson for
                               // legs: a native drive channel has a
                               // start-up ramp, and a witness that reads
                               // raw stillness inside it convicts a car
                               // that has barely been told to move.
    // THE ROOT STEP (2026-11-08): the autopilot feed is RETIRED -
    // there is no feedPoint/feedCruise/feedLive any more. The brain's
    // wheel/pedal words (wheel, gas, brake above) are the drive, whole
    // and alone; Drive() sends nothing beside them. What the feed
    // carried that was intent rather than translation is re-expressed
    // in Cop.cpp where it lived (the batter pace floor in the side
    // zones, the far walk's length as a steering aim, the brake-to-
    // post hold) and each translation is named in DRIVER2-STEP3-fa.md.
    float cmdSteer = 0.0f;     // the steer word Drive() last sent, as
                               // sent (after the session flip and
                               // kSteerScale) - the unit line prints
                               // it beside the body's own read-back, so
                               // a session can convict the CHANNEL
                               // (commanded vs landed) from numbers
    // the last report's drive-state read-backs: the pedal fields as
    // the body itself answered (repSteer/repGas/repBrake - under direct
    // drive these are the channel's receipt, read back from the car,
    // never echoed from the command), and the autopilot's mission and
    // cruise as the body answered them - which under direct drive must
    // read NONE/0 while the car drives: a moving car on mission 0 is
    // the no-middleman receipt, printed on the same unit line
    float repSteer = 0.0f, repGas = 0.0f, repBrake = 0.0f;
    // the channel repair's third reading (2026-11-08): the words the
    // bridge's ProcessAI replacement last stamped at the desk physics
    // reads, and its stamp count on this body - the unit line prints
    // them beside cmd and body, so one session separates all three
    // stories (what the core ordered, what the channel stamped, what
    // the fields held when the tick looked)
    float repHookSteer = 0.0f, repHookGas = 0.0f, repHookBrake = 0.0f;
    int repHookStamps = 0;
    int repMission = -1;
    int repCruise = 0;
    float repCruiseF = 0.0f;
    bool repSiren = false;
    // the momentum channel's bookkeeping (the 5x update): one throw
    // per approach - the latch re-arms only when the centres have
    // separated past the contact band + 3 m, so a single pass can
    // never machine-gun throws; the count is the channel's receipt,
    // graded by the sims and printed on the unit line
    bool kickArmed = true;
    int kickCount = 0;
    // ---- the 39-package's per-unit state (items named at each field)
    int role = 0;              // item 1: the tactics tick's assignment -
                               // 0 lead, 1/2 the two flanks, 3 the front
                               // brake-check; read by CopControl's aim
    bool bikeUnit = false;     // item 36: this body is the motorcycle
    bool heavyUnit = false;    // item 4: the ladder's top rung made
                               // heavy (book line and throw, by law)
    float paceScale = 1.0f;    // item 26: the counter-pick - a spawn
                               // priced off the prey's measured top
                               // speed this pursuit (1.0 until measured)
    // the HOLD: one mechanism under the ring (7), the ambush (20),
    // the exit camp (27), the shore line (29), the dead-end mouth
    // (34), the rear-exit guard (9) and the block creep (2). While
    // holding, the unit drives to holdPoint and stands there, engine
    // on, instead of running CopControl; holdArrived latches inside
    // 2.2 m of the point. The LAWS that set and clear holds live in
    // Cop.cpp's tactics tick; the drive itself is HoldDrive.
    Vec3 holdPoint;
    bool holding = false;
    bool holdArrived = false;
    // the post's own still-clock (HoldControl's bail): the point the
    // clock is serving and how long the unit has stood still short of
    // it. It is the post's clock, not the pursuit's stuckT - a unit
    // handed a post while its pursuit stuck-clock was already hot
    // must not give the post back in the same tick he takes it.
    Vec3 postPoint;
    float postStillT = 0.0f;
    // item 38: the bail-out. 0 = the driver sits in his car (the
    // standing state); 1 = the core has ordered him out and the bridge
    // fights him on foot. bailT is his time out; the report's own
    // words (driverOut/driverPos) are stashed beside the other
    // read-backs so the unit line can print them.
    int bailState = 0;
    float bailT = 0.0f;
    bool repDriverOut = false;
    Vec3 repDriverPos;
    bool sirenCmd = false;     // item 39: the sound law's word for this
                               // unit this tick - CopControl's drives
                               // pass it instead of a literal true, so
                               // stealth, ring-stillness and the parked
                               // bail car can all sleep the siren
    float lastRamT = 0.0f;     // item 8: the dying unit's last-ram
                               // window, seconds left; lastRamDone once
                               // spent (one per unit per life)
    bool lastRamDone = false;
    // item 24/25: a one-tick aim override - the traffic car in his
    // corridor, or the wounded friend he is shoving - set by the
    // tactics tick, consumed by CopControl's target choice
    Vec3 aimOverride;
    bool aimOverrideOn = false;
    float wallSide = 0.0f;     // item 13: the probe cadence's reading of
                               // which side of the prey is walled
                               // (-1/+1 = a wall within 9 m on that
                               // side of his line, 0 = both open)
    float unitLogT = 0.0f;     // the per-second unit line's clock
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
// Stand the whole force down: every body retired (no burn-out), the
// books forgotten. /coptest's cleanup uses it; so does a plain re-summon
// path if one ever needs it.
void CopDismiss();
// ---- /coptest support (the complete update). While a prey override is
// armed, the force hunts the DECOY, not the player: CopTick fills the
// threat record from the decoy's reports (a car decoy: in-vehicle true;
// a ped decoy: the man himself, on foot), and every internal use of the
// tick's player position follows the prey. The offences that read the
// real player's own car (his impact ledger, his wheel offences) stay
// silent under an override - the test prices felony directly through
// CopForceFelony. -1 disarms (the player is the prey again, the default).
void CopPreyOverride(int decoyVeh, int decoyPed);
void CopForceFelony(float felony);
// Seed the position-known flag once (a dispatcher's radio call: "the
// car was reported at..."). Staged /coptest phases use it at their
// start - detection itself is graded by the player phases, and after
// the seed the ordinary sight/lost laws rule untouched.
void CopKnownSeed();
// The force's books, for tests and the log: living units (any slot with
// a body), units in pursuit, roadblock cars placed (asleep or awake),
// the ladder's wanted count this tick, and the cutoff distance the
// manager priced (1e9 = no cutoff in force).
int CopUnitCount();
int CopPursuingCount();
int CopBlockCount();
int CopWantedCount();
float CopCutoffDistance();
// Item 39's receipts, for the suite and the /coptest sheet: how many
// radio lines the core has fired since the last reset, and the bitmask
// of event kinds among them (bit per kind, in the firing function's
// own order) - his live complaint was "they never talked", so the
// talk is counted, not asserted.
int CopRadioLinesFired();
int CopRadioEventMask();

// ---------------------------------------------------------------------------
// Piece 3: his OWN constants, decoded from the disc - CHICAGO.LCF's
// CAR_COSMETICS row for external model 0, the cop (the row is the cop's
// by the rebuild's own identifications: CarHasSiren in GAMESND.C returns
// nonzero exactly when a resident slot holds model 0, MapCarIndexToBank
// maps model 0 to the cop's engine bank, and ProcessCosmeticsLump in
// COSMETIC.C indexes the LCF offset table by external model id).
// Decoded by tools/decode_lcf.py from the user's own disc image; the
// full row is in driver2-assets/chicago-lcf.json. FixCarCos (HANDLING.C)
// shifts wheelDisp/cPoints vz by -( (235 + -194 - 14) / 2 ) = -13 at
// load and touches nothing else without cheats, so the wheel placements
// below are the POST-fix values the game itself drives with.
struct CopProfileData {
    int powerRatio;   // 4096 - his full-throttle thrust coefficient,
                      // 12-bit fixed point: exactly 1.0, the reference
                      // car of his city (the field runs 2666..4500)
    int traction;     // 4096 - his friction multiplier, 1.0
    int susCoeff;     // 4096 - suspension coefficient, 1.0
    int mass;         // 4096 - his mass unit, 1.0
    int wheelSize;    // 49 - his wheel size (CARS.C scales the drawn
                      // wheel model by wheelSize * 14142 / 10000)
    int frictionScaleRatio; // 68 - handling type 4, the hardest
                      // difficulty's cop type (InitCopState, COP_AI.C):
                      // WHEELFORCES.C applies it as frontFS/rearFS
                      // scale * 68 / 32 on top of traction / 4096
    float wheelDisp[4][3]; // his wheel placements, metres at the
                      // convicted 153.6 units = 1 m (fix round 3: the
                      // geometry factor - his length 754 u -> 4.9 m and
                      // wheelbase 429 u -> 2.8 m convict it; 256 was
                      // the speed rescale's constant, not geometry's),
                      // POST-FixCarCos: (x, y, z) per wheel in the
                      // LCF's own order [0..3]
    float colBox[3];  // his collision cage half-extents (129, 88, 382)
    float cog[3];     // his centre of gravity (0, -95, 0) - the data's
                      // own values: FixCarCos shifts wheelDisp/cPoints
                      // and touches nothing else, cog included (piece
                      // 3's "+13" here was the wheel shift's mirror,
                      // not data; nothing consumed it)
};
const CopProfileData& CopProfile();
// powerRatio as a gain on the pedal: in his physics the delivered
// thrust is the coefficient times the demand (HANDLING.C's player path:
// thrust = FIXEDH(powerRatio * 4915) at full pedal; the AI's thrust cap
// in CopControl1 is written in the same thrust units). The pre-LCF
// normaliser silently assumed the 4096 car; now it is his data.
float CopProfilePowerGain(); // powerRatio / 4096
// His total tyre friction scale: (68 / 32) * (traction / 4096) = 2.125.
// Carried as his data - see Cop.cpp for why it has no second expression
// on this host (the difficulty's pace row already carries the step).
float CopProfileGrip();
// The one wired consumer: the pursuit drive law's pedal, gained by his
// own power coefficient and clamped. Both thrust sites ride it.
float CopProfilePedal(float pedal);
// Test hook: pretend the LCF row carried another powerRatio, to pin the
// gain's law in the sims. -1 restores the decoded row. Nothing else in
// the build calls this.
void CopProfileOverrideForTest(int powerRatio);

// The momentum channel's arithmetic (the 5x update), exposed for the
// sims - the live path in CopTick calls this same function, so the
// pins grade the shipping law, not a copy of it. Given the cop's
// velocity, the prey's velocity and the prey centre's offset from the
// cop's centre (all m/s and m), returns the velocity the prey gains:
// along the cop's own travel direction, sized at the transfer fraction
// of the closing speed, ramped in between the tap threshold and full
// contact, and capped at the channel's stated cap. *spinOut receives
// the yaw rate (rad/s) a flank hit adds, signed by which side of the
// cop's line of travel the prey's centre sits on (zero square-on);
// *closingOut receives the closing speed the price was read off. A
// contact under the tap threshold - or a cop barely moving - returns
// the zero vector: parked taps stay taps.
Vec3 CopKickCompute(const Vec3& copVel, const Vec3& preyVel, const Vec3& rel,
                    float* spinOut, float* closingOut);

} // namespace mc
