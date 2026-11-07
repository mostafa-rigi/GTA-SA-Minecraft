#include "Cop.h"

#include <cmath>

#include "Host.h"
#include "NavGraph.h"

namespace mc {

SharedThreat gThreat;
CopState gCops[kMaxCopUnits];
CopState& gCop = gCops[0];

void ThreatNoteBoss(int state, bool engaged, const Vec3& pos, bool alive) {
    gThreat.bossState = state;
    gThreat.bossEngaged = engaged;
    gThreat.bossPos = pos;
    gThreat.bossAlive = alive;
}

void ThreatNoteOurUnitLost() { ++gThreat.ourUnitsLost; }

namespace {

// ---------------------------------------------------------------------------
// The unit bridges. REDRIVER 2 counts in PS1 units: 256 world units = 1 m,
// a logic tick is one frame (50/s on PAL), and its speed scale is anchored
// here on the civilian road limits the game itself uses (56/97/138 in
// CIV_AI.C): our top civilian limit stands at 25 m/s, so one internal speed
// unit = 25/138 m/s. Everything below is the original's number passed
// through those two bridges - no value was tuned by feel.
constexpr float kM = 1.0f / 256.0f;       // world unit -> metre
constexpr float kV = 25.0f / 138.0f;      // speed unit -> m/s
constexpr float kTick = 1.0f / 50.0f;     // one logic tick, seconds

// CopControl1's desired speeds by difficulty (COP_AI.C): speed1 is the
// cornering pace, speed2 the straight-road pace. The HARDEST row (2)
// ships - the user's own choice, 2026-11-07, «آره سخت ترین بهترینه»:
// speed1 217 units = 39.3 m/s, speed2 294 units = 53.3 m/s, and the
// thrust scales ride the same row ((difficulty+8) base, +(difficulty+4)
// on a straight). «خودش» is the ceiling too: nothing above his own top.
constexpr int kDifficulty = 2;
constexpr float kSpeed1[3] = { 203 * kV, 210 * kV, 217 * kV };
constexpr float kSpeed2[3] = { 238 * kV, 266 * kV, 294 * kV };
constexpr float kSideSpeed = 110 * kV;    // 19.9 m/s, the side-zone crawl
constexpr float kBackSpeed = -70 * kV;    // -12.7 m/s, overshoot reverse
constexpr float kBlockedSpeed = -120 * kV; // -21.7 m/s, both corners shut
constexpr float kRecoverSpeed = 200 * kV; // 36.2 m/s desired in recovery

// FindCost's gates, in metres: close pursuit under 2800 units (10.9 m),
// the CarTail intercept computed under 8400 (32.8 m), the anti-rear-end
// pacing blend under 4096 (16 m).
constexpr float kCloseCost = 2800 * kM;
constexpr float kTailWindow = 8400 * kM;
constexpr float kPaceCost = 4096 * kM;

// The felony scale (FELONY.C): the pursuit trigger and the cap are the
// original's own integers; the clock is its 31 ticks.
constexpr float kFelonyTrigger = 658.0f;
constexpr float kFelonyCap = 4096.0f;
constexpr float kFelonyClock = 31 * kTick; // 0.62 s per +1/-1 step
constexpr float kBatterFelony = 2457.0f;   // the ladder's top rung - the
    // original's gBatterPlayer is a per-mission flag, and top pressure is
    // the honest stand-in, named as such (piece 1's reading, kept)
constexpr float kLostAfter = 256 * kTick;  // 5.12 s unseen = position lost
constexpr float kWreckLine = 27000.0f;     // the original's retirement line
    // (COP_AI.C: totalDamage > 27000 && !immortal - the dying sequence)
// His damage book's prices (BCOLLIDE.C's ApplyDamage / DamageCar3D, the
// user's order 2026-11-07: survival law = HIS, display = the city's):
// - collisions price off the impact's own velocity change, on the shape
//   of DamageCar3D's curve: under its strike threshold nothing bills at
//   all (light taps are free), the price climbs with the strike and is
//   capped per impact, and a pursuer at the hardest difficulty pays the
//   original's own discount (ApplyDamage: x2 for a non-player car, then
//   x7/16 at gCopDifficultyLevel 2 - net x0.875: his toughness IS his).
// - everything the host prices in its own health points (bullets, blast,
//   fire, hits on the driver) converts at one anchor: 27000 of his
//   points == the host body's own 1000-point life, 27 per point. An RPG
//   that would take ~300 host points off the body bills ~8100 of his:
//   expensive, and three-plus direct hits from retiring him - never one.
//   FIX ROUND 3 (2026-11-07): the "~300 host points" was the design's
//   assumption, and the host does not honour it - a point-blank blast
//   strips the body 1000 -> 0 (or below) inside one frame, so the raw
//   per-tick delta billed the WHOLE book at once (the eye test's log
//   reads the proof: one retirement at 27503 of his points, billed by a
//   single event). The anchor now rules as a CAP, not a hope: one tick
//   (one report) can bill at most one blast-class price - 300 host
//   points' worth, 8100 - across the shot, world and driver channels
//   together. Honest traffic never reaches it (a bullet's per-tick
//   delta is a few host points; fire drains a fraction of a point a
//   tick and accumulates uncapped over time), so bullets stay cheap,
//   fire stays a priced drain, and no single frame can counterfeit a
//   whole life. Collisions keep their own BCOLLIDE-shaped channel
//   (impactPrice, capped per impact) beside this one.
constexpr float kImpactFree = 2.8f;    // m/s of velocity change: free
constexpr float kImpactRate = 340.0f;  // his points per m/s past it
constexpr float kImpactCap = 1500.0f;  // per single impact, before discount
constexpr float kPursuerToughness = 0.875f; // ApplyDamage, difficulty 2
constexpr float kSaBookRate = 27.0f;   // host health point -> his points
constexpr float kBlastPrice = 8100.0f; // one blast-class event: 300 host
                                       // points at the anchor - the most
                                       // one tick's health-delta channels
                                       // may bill, all of them together

// FELONY.C's own tables.
constexpr int kPlacid[12] = { 659, 659, 659, 659, 659, 1318, 659, 659, 659, 659, 659, 4096 };
constexpr int kAngry[12] = { 0, 75, 0, 0, 659, 659, 659, 25, 0, 0, 0, 0 };
constexpr float kOccurDelay[12] = { 24 * kTick, 0, 0, 0, 0, 0, 0, 0, 24 * kTick, 0, 24 * kTick, 0 };
constexpr float kRepeatDelay[12] = { 128 * kTick, 0, 128 * kTick, 64 * kTick, 64 * kTick, 32 * kTick,
                                     32 * kTick, 0, 128 * kTick, 256 * kTick, 0, 0 };
// pursuitFelonyScale is 3000 in InitCops (NOT its header's 4096).
constexpr float kPursuitScale = 3000.0f;

// ---- step 2: the ladder, the cutoff, the roadblock (COP_AI.C / CIV_AI.C)
// ControlNumberOfCops counts the felony past the trigger ladder built in
// InitCopData: {0, 819, 1638, 2457, 32767} - felony >= a rung wants one
// more car, capped by maxCopCars (4). gMinimumCops is per-mission data
// (0 in the free-roam data this step stands in for).
constexpr float kLadderRungs[4] = { 0.0f, 819.0f, 1638.0f, 2457.0f };
// The spawn timer: gCopRespawnTime 320 ticks (InitCops, normal game),
// with ControlNumberOfCops' own felony shrink wrapped around it:
// respawnTime * (4096 - felony*autoRespawnScaleLimit)/4096 ticks must
// pass. The rebuild never assigns autoRespawnScaleLimit in code - it was
// initial data in the original's COP_DATA block - so the port adopts
// 2048 (the delay halves by the felony cap) as a NAMED stand-in, the
// same epistemic status as the batter line above.
constexpr float kRespawnTicks = 320.0f;
constexpr float kRespawnScale = 2048.0f / 4096.0f;
// The cutoff (ControlNumberOfCops + its use in CopControl1): when more
// pursuers are live than the ladder wants, cutOffDistance is set to the
// wanted-th nearest pursuer's distance, and a pursuer beyond it reads
// his desired speed through cutOffPowerScale = 2048 of 4096 - one half.
constexpr float kCutoffScale = 2048.0f / 4096.0f;
// CheckPingOut's own radius: a car farther than 24000 units (93.75 m)
// from the player's spool centre leaves the world.
constexpr float kPingOut = 24000 * kM;
// Ladder spawns enter at range, the traffic spawner's shape (PingInCivCar
// seats a car on a road spool position the player is not looking at):
// ours is the road-graph node nearest a ring point 70 m out, accepted
// inside 50..95 m - inside this host's body streaming, outside the near
// field, never in the player's face.
constexpr float kSpawnRing = 70.0f;
// The roadblock chain (NoteFelony / MAIN.C's StepSim / CreateRoadblock):
// felony crossing 1318 (0x526) arms roadblockCount with
// roadblockDelayDiff[difficulty] + (Random & 0xFF) - the hardest row is
// 800 ticks; the count runs down while no block cars stand; at zero,
// with the pursuit live and felony >= 1319 (0x527), CreateRoadblock
// scans a fan ahead of the player (to ~16384 units = 64 m) for the first
// straight-or-curved road segment at least 6 car-lengths long and lays
// sleeping cars across its lanes.
constexpr float kBlockArmFelony = 1318.0f;
constexpr float kBlockFireFelony = 1319.0f;
constexpr float kBlockDelayTicks[3] = { 1500.0f, 1000.0f, 800.0f };
constexpr float kBlockAhead = 48.0f;  // where on the found road the block
                                      // stands, metres ahead of the player
// A block car wakes the way PassiveCopTasks converts any flagged car:
// felony over the trigger and the player's position known. The original
// checks nothing about distance - its block cars sit in the traffic flow
// the player is about to meet - so ours wakes inside 45 m, the honest
// SA reading of "about to meet", named here rather than hidden.
constexpr float kBlockWakeDist = 45.0f;

// ---- piece 3: his own constants, decoded from the disc -------------
// CHICAGO.LCF, CAR_COSMETICS row of external model 0 (offset 52 in the
// file; the table's offsets are [52, 288, 524, ...] - one 236-byte row
// per model, back to back). The row is the cop's by the rebuild's own
// identifications (see Cop.h). What each number does in HIS physics,
// and what becomes of it here:
// - powerRatio 4096: his full-throttle thrust coefficient, 12-bit
//   fixed point 1.0 - the reference car of his city (the weakest
//   Chicago car carries 2666, the strongest 4500). WIRED: it gains the
//   pursuit pedal (CopProfilePedal below), the same place the
//   coefficient sits in his physics - between the driver's demand and
//   the delivered thrust. Before this piece the normaliser silently
//   assumed a 4096 car; the assumption is now his decoded data, and a
//   different row would change his pull by exactly its ratio.
// - traction 4096 (1.0) and the handling type's frictionScaleRatio 68:
//   his total tyre friction scale is (68/32) x (4096/4096) = 2.125
//   (WHEELFORCES.C multiplies the friction scales by both). CARRIED,
//   NOT WIRED: the difficulty row that selects type 4 is the same row
//   that selects speed1/speed2, and the pace half of that step already
//   rides the desired speeds above - applying the grip half a second
//   time as a speed multiplier would double-count his own difficulty
//   and break the "within his numbers" ceiling. On a host whose car
//   physics we do not integrate, his grip is data, named here like
//   piece 1's handling note named its own limit.
// - susCoeff 4096, mass 4096, twistRate (224, 224, 1120 in the raw
//   row): his suspension, his collision mass, his body-roll rates -
//   all properties of HIS integrator, which the host's car replaces.
//   Carried in the decode (driver2-assets/chicago-lcf.json), not
//   re-expressed here; the host resolves suspension and collisions.
// - wheelSize 49 and wheelDisp: WIRED in the body (piece 3's drawn
//   mesh puts his wheels where his LCF says, at his size - CARS.C's
//   own placement law), so they live in the profile below.
// - colBox (129, 88, 382) and the 12 cPoints: his collision cage.
//   Carried as measurement; collision stays the host car's (the body
//   step states what the eye sees vs what collides).
// the geometry factor (fix round 3): his metres are his units over
// 153.6 - convicted from his own length and wheelbase, cross-checked
// on the cone and the road tile in his level data (DRIVER2-STEP3-fa.md
// carries the whole derivation). The 256 that stood here before was
// the brain's speed-rescale constant wearing geometry's clothes; it
// shrank every carried length to 0.6x.
constexpr float kHisUnitsPerMetre = 153.6f;
const CopProfileData kCopProfile = {
    4096, 4096, 4096, 4096, 49, 68,
    { { 122.0f / kHisUnitsPerMetre, -30.0f / kHisUnitsPerMetre, 222.0f / kHisUnitsPerMetre },
      { 125.0f / kHisUnitsPerMetre, -30.0f / kHisUnitsPerMetre, -207.0f / kHisUnitsPerMetre },
      { -123.0f / kHisUnitsPerMetre, -30.0f / kHisUnitsPerMetre, 222.0f / kHisUnitsPerMetre },
      { -126.0f / kHisUnitsPerMetre, -30.0f / kHisUnitsPerMetre, -207.0f / kHisUnitsPerMetre } },
    { 129.0f / kHisUnitsPerMetre, 88.0f / kHisUnitsPerMetre, 382.0f / kHisUnitsPerMetre },
    { 0.0f, -95.0f / kHisUnitsPerMetre, 0.0f },
};
int sProfilePowerOverride = -1; // the sims' hook; -1 = the decoded row

// The system page: slot 0's felony-side fields. In the original the
// felony lives on the player's own record; here slot 0 carries the page
// for the whole force (see Cop.h).
CopState& Page() { return gCops[0]; }

bool UnitLiving(const CopState& c) { return c.veh >= 0 && c.mode != COP_NONE; }

// The manager's books (ControlNumberOfCops / StepSim state).
bool sForce = false;        // a summon stood the force up this session
float sRespawnT = 0.0f;     // the ladder's spawn clock, seconds
int sWanted = 0;            // the ladder's wanted count this tick
float sCutoff = 1e9f;       // the cutoff distance in force (1e9: none)
bool sBlockWasLow = true;   // the arm latch: felony was at/under 1318
bool sBlockTimerOn = false;
float sBlockTimer = 0.0f;   // the armed roadblock count, seconds

float BlockDelay() {
    return (kBlockDelayTicks[kDifficulty] + (float)(McsaRand() & 0xFF)) * kTick;
}

int LadderWanted(float felony) {
    int wanted = 0;
    for (int i = 0; i < 4; ++i)
        if (felony >= kLadderRungs[i])
            ++wanted;
    if (wanted > kMaxCopCars)
        wanted = kMaxCopCars;
    return wanted;
}

// The sight gates (UpdateCopSightData): placid / hot, front range and
// half-angle, peripheral range; a height gap past 1500 units (5.9 m)
// blinds. Ours read through the host's own wall oracle (LineBlocked)
// instead of the original's cell PVS - the shape is his, the eyes are the
// host's, and that swap is named here rather than hidden.
bool CopSees(const Vec3& copPos, const Vec3& copFwd, bool hot) {
    const Vec3 to = gThreat.playerPos - copPos;
    const float dist = to.Length();
    if (std::fabs(to.z) > 1500 * kM)
        return false;
    const float front = (hot ? 16320 : 7820) * kM;
    const float periph = (hot ? 5440 : 2720) * kM;
    if (dist > front)
        return false;
    if (dist <= periph)
        return !TheHost().LineBlocked(copPos, gThreat.playerPos);
    Vec3 flat = to;
    flat.z = 0.0f;
    const float len = flat.Length();
    if (len < 0.001f)
        return true;
    flat = flat * (1.0f / len);
    Vec3 f = copFwd;
    f.z = 0.0f;
    const float fl = f.Length();
    if (fl < 0.001f)
        return false;
    f = f * (1.0f / fl);
    const float cosHalf = std::cos((hot ? 1024 : 512) * (6.2831853f / 4096.0f));
    if (flat.Dot(f) < cosHalf)
        return false;
    return !TheHost().LineBlocked(copPos, gThreat.playerPos);
}

// FELONY.C's NoteFelony, whole: occurrence warm-up and re-occurrence delay
// gate the note; the placid value lands whole below the trigger, the angry
// value rides the pursuit scale above it; an additional scale rides on top.
// The felony and its timers are the system page's, whoever earned them.
void NoteFelony(int type, int additionalScale) {
    CopState& c = Page();
    if (type < 0 || type > 11)
        return;
    if (c.offenceOccur[type] < kOccurDelay[type])
        return; // the offence type has not warmed up yet
    if (c.offenceRepeat[type] > 0.0f)
        return; // too soon after the last note of this type
    if (!c.seen && type != 11)
        return; // CopsCanSeePlayer, the original's own gate
    float add;
    if (c.felony <= kFelonyTrigger)
        add = (float)kPlacid[type];
    else
        add = (float)kAngry[type] * kPursuitScale / 4096.0f;
    c.felony += add + additionalScale * kPursuitScale / 4096.0f;
    if (c.felony > kFelonyCap)
        c.felony = kFelonyCap;
    c.offenceRepeat[type] = kRepeatDelay[type];
}

// The steering wheel's own rate limit, out of CopControl1: the desired
// angle is approached at 200 units per tick, never snapped.
float WheelToward(float wheel, float desired, float dt) {
    const float step = 200.0f * (dt / kTick);
    float d = desired - wheel;
    if (d > step)
        d = step;
    if (d < -step)
        d = -step;
    return wheel + d;
}

// Every drive out of this brain passes here: the steering sign the
// spike's calibration settled (a fact about the host, kept on the system
// page) is applied at this one choke point, and the magnitude rides
// kSteerScale - chosen so both readings of the field's unit stay sane
// (radians: 0.6 rad is a full city lock; normalised: 0.6 of lock still
// turns a chase). The spike's in-game behaviour is the scale's judge,
// named here rather than hidden.
constexpr float kSteerScale = 0.6f;
void Drive(CopState& c, float gas, float brake, float wheelNorm, bool siren) {
    const float steer = (Page().steerFlip ? -wheelNorm : wheelNorm) * kSteerScale;
    TheHost().CopBodyDrive(c.veh, gas, brake, steer, siren);
    c.gas = gas;
    c.brake = brake;
}

// Standing still, for IDLE and BLOCK alike: on this host a held brake at
// a standstill is a reverse gear, not a handbrake - he brakes only while
// he is still rolling.
void StandDrive(CopState& c) {
    if (std::fabs(c.speed) > 1.0f)
        Drive(c, 0.0f, 0.7f, 0.0f, false);
    else
        Drive(c, 0.0f, 0.0f, 0.0f, false);
}

// ---------------------------------------------------------------------------
// Piece 0's spike controller (slot 0 only): first the steering calibration
// (one second of commanded turn, read back off the car's own nose), then
// the block route by pure pursuit on the waypoints the road graph itself
// hands back. Deliberately dumb beyond that - its job is proving the body
// answers gas, brake and steering, with the read-back (the bridge's, from
// the vehicle itself) logged every second.
void SpikeDrive(float dt) {
    CopState& c = gCop;
    if (!c.steerCalDone) {
        if (c.calT == 0.0f)
            c.calFwd = c.fwd;
        c.calT += dt;
        const float gas = c.speed < 3.0f ? 0.5f : 0.0f;
        TheHost().CopBodyDrive(c.veh, gas, 0.0f, 0.6f * kSteerScale, false);
        c.gas = gas;
        // the cross product of the nose then and now is sin(yawThen -
        // yawNow): NEGATIVE when the car turned RIGHT on the map (yaw
        // grows rightward in this frame), positive when it turned left.
        // The brain's convention is +command = right turn; if the raw
        // +0.6 turned him left, the session flips every command.
        const float cross = c.calFwd.x * c.fwd.y - c.calFwd.y * c.fwd.x;
        if ((c.calT > 0.5f && std::fabs(cross) > 0.02f) || c.calT > 3.0f) {
            c.steerCalDone = true;
            c.steerFlip = cross > 0.02f;
            Log("cop spike: steer calibration - command +0.6 turned the car %s (cross %.3f); session flip %s",
                cross < -0.02f ? "RIGHT" : cross > 0.02f ? "LEFT" : "NOT MEASURABLY",
                cross, c.steerFlip ? "ON" : "off");
        }
        return;
    }
    if (c.spikeIdx >= c.spikeRoute.size()) {
        Drive(c, 0.0f, c.speed > 1.0f ? 1.0f : 0.0f, 0.0f, false); // brake while
            // rolling only - a held brake at rest is reverse on this host
        if (!c.spikeDone) {
            c.spikeDone = true;
            Log("cop spike: route finished - final read-back: pos (%.1f, %.1f, %.1f), speed %.1f m/s. "
                "The body answered the core's pedals for the whole route.",
                c.pos.x, c.pos.y, c.pos.z, c.speed);
            c.mode = COP_IDLE;
        }
        return;
    }
    const Vec3 wp = c.spikeRoute[c.spikeIdx];
    Vec3 to = wp - c.pos;
    to.z = 0.0f;
    const float dist = to.Length();
    if (dist < 7.0f) {
        ++c.spikeIdx;
        return;
    }
    to = to * (1.0f / dist);
    const float cross = c.fwd.x * to.y - c.fwd.y * to.x;
    const float dot = c.fwd.x * to.x + c.fwd.y * to.y;
    const float err = std::atan2(cross, dot);
    // in the rear hemisphere the angle error wraps at +-pi and its sign
    // is noise: commit to a full-lock turn toward the side the point is
    // on (the cross product's sign, which never wraps) and hold it until
    // the point is well into the front hemisphere (dot > 0.45), where
    // the proportional law is continuous again
    float steer;
    if (c.spikeLatch != 0.0f) {
        steer = c.spikeLatch;
        if (dot > 0.45f)
            c.spikeLatch = 0.0f;
    } else if (dot < 0.0f) {
        c.spikeLatch = cross >= 0.0f ? -1.0f : 1.0f;
        steer = c.spikeLatch;
    } else {
        steer = Clamp(-err * 1.4f, -1.0f, 1.0f);
    }
    // a city pace, easing off as the point nears or the turn grows: a
    // cruiser's turn radius at full pace is wider than a waypoint's
    // capture circle, and an orbiting spike proves nothing
    float want = 11.0f;
    if (dist < 25.0f || std::fabs(err) > 0.7f)
        want = 6.0f;
    float gas = 0.0f, brake = 0.0f;
    if (c.speed < want)
        gas = Clamp((want - c.speed) * 0.2f, 0.25f, 1.0f);
    else
        brake = Clamp((c.speed - want) * 0.2f, 0.0f, 0.6f);
    Drive(c, gas, brake, steer, false);
    c.spikeLogT -= dt;
    if (c.spikeLogT <= 0.0f) {
        c.spikeLogT = 1.0f;
        Host::CopBody rep;
        if (TheHost().CopBodyReport(c.veh, &rep) && rep.ok)
            Log("cop spike: pos (%.1f, %.1f, %.1f) speed %.1f m/s | read-back gas %.2f brake %.2f steer %.2f",
                rep.pos.x, rep.pos.y, rep.pos.z,
                rep.vel.x * rep.fwd.x + rep.vel.y * rep.fwd.y + rep.vel.z * rep.fwd.z,
                rep.gas, rep.brake, rep.steer);
    }
}

// ---------------------------------------------------------------------------
// The leaving drive (step 2): a stood-down unit drives AWAY from the man
// along the road graph until CheckPingOut's own radius takes him out of
// the world. He does not charge, does not chase, does not stop to fight:
// the route's far end is 140 m past him on the line away from the player,
// and if the graph cannot lay it he drives the straight line off.
void LeaveDrive(CopState& c, float dt, const Vec3& playerPos) {
    if ((c.pos - playerPos).Length() > kPingOut) {
        Log("cop: the stood-down unit pinged out at %.0f m - the world takes him back",
            (c.pos - playerPos).Length());
        TheHost().CopBodyRetire(c.veh, false);
        c.veh = -1;
        c.mode = COP_NONE;
        return;
    }
    if (c.leaveIdx >= c.leaveRoute.size()) {
        c.leaveRoute.clear();
        Vec3 away = c.pos - playerPos;
        away.z = 0.0f;
        const float al = away.Length();
        away = al > 0.5f ? away * (1.0f / al) : Vec3(1, 0, 0);
        const Vec3 dest = c.pos + away * 140.0f;
        // the first leg is his own ground, straight off the man: a stood-
        // down unit was just pacing on top of him, and a road route laid
        // from that spot can cross the man's position on its way out -
        // he leaves AWAY first, then joins the road. (This step's sims
        // caught the crossing; the push leg is the fix.)
        const Vec3 push = c.pos + away * 25.0f;
        c.leaveRoute.push_back(push);
        std::vector<Vec3> road;
        if (VehicleRoute(push, dest, &road, nullptr) && road.size() >= 2) {
            for (size_t i = 0; i < road.size(); ++i)
                c.leaveRoute.push_back(road[i]);
        } else {
            c.leaveRoute.push_back(dest);
        }
        c.leaveIdx = 0;
    }
    const Vec3 wp = c.leaveRoute[c.leaveIdx];
    Vec3 to = wp - c.pos;
    to.z = 0.0f;
    const float dist = to.Length();
    if (dist < 7.0f) {
        ++c.leaveIdx;
        return;
    }
    to = to * (1.0f / dist);
    const float cross = c.fwd.x * to.y - c.fwd.y * to.x;
    const float dot = c.fwd.x * to.x + c.fwd.y * to.y;
    const float err = std::atan2(cross, dot);
    const float steer = dot < 0.0f ? (cross >= 0.0f ? -1.0f : 1.0f)
                                   : Clamp(-err * 1.4f, -1.0f, 1.0f);
    float gas = 0.0f, brake = 0.0f;
    const float want = dist < 25.0f || std::fabs(err) > 0.7f ? 6.0f : 11.0f;
    if (c.speed < want)
        gas = Clamp((want - c.speed) * 0.2f, 0.25f, 1.0f);
    else
        brake = Clamp((c.speed - want) * 0.2f, 0.0f, 0.6f);
    Drive(c, gas, brake, steer, false);
    c.desiredSpeed = want;
}

// ---------------------------------------------------------------------------
// CopControl1 (COP_AI.C), ported whole onto metres, one instance per
// unit. dt-driven: the original runs one decision per frame at 50
// ticks/s, so every per-tick constant rides kTick. Step 2 changes two
// things only, both the original's own: the road field is the force's
// ONE shared field (PATHFIND.C keeps a single map), read through the
// system page's throttle; and the desired speed passes the cutoff's
// scale - a unit beyond the manager's cutOffDistance reads his tables
// at cutOffPowerScale, one half, exactly where the original applies it
// (the front-zone desire, before the pacing blend).
void CopControl(CopState& c, float dt) {
    const bool playerInCar = gThreat.playerInVehicle;
    const Vec3 pPos = gThreat.playerPos;
    const Vec3 pVel = gThreat.playerVel;

    // the bumper corners, probed every 8 ticks like the original: a point
    // 400 units ahead and 150 to the side (1.56 m / 0.59 m) - the original
    // asks CellAtPositionEmpty there; the host's wall oracle stands in
    // (the same named swap as the sight gates). With a force of them the
    // probes are staggered by slot so a tick never pays for all at once.
    c.probeT -= dt;
    if (c.probeT <= 0.0f) {
        c.probeT = 8 * kTick;
        Vec3 right(c.fwd.y, -c.fwd.x, 0.0f); // his TRUE right: (fwd.y, -fwd.x) in this frame - facing north it points east
        const float rl = right.Length();
        right = rl > 0.001f ? right * (1.0f / rl) : Vec3(1, 0, 0);
        const Vec3 base = c.pos + c.fwd * (400 * kM);
        c.frontLClear = !TheHost().LineBlocked(c.pos, base - right * (150 * kM));
        c.frontRClear = !TheHost().LineBlocked(c.pos, base + right * (150 * kM));
    }

    // the recovery runs its own clock first (the original checks it before
    // anything else steers): reverse, on the rotating strategy's wheel.
    if (c.recoveryT > 0.0f) {
        c.recoveryT -= dt;
        const float strat = (float)(((c.recoverStrategy * 5) & 6) - 3) * 128.0f;
        c.wheel = WheelToward(c.wheel, strat, dt);
        // the direction was chosen ONCE, when the recovery started
        // (opposite the way he was stuck facing); re-deriving it from
        // the live speed each tick cancels the manoeuvre the moment
        // the reverse bites - the original holds it for the 22 ticks
        const float err = c.desiredSpeed - c.speed;
        float pedal = Clamp(err / (50 * kV), -1.0f, 1.0f);
        const float g = pedal > 0.0f ? pedal : 0.0f;
        const float b = pedal < 0.0f ? -pedal : 0.0f;
        Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
        return;
    }

    // stuck: wanting to move and not moving, ten ticks of it (the
    // original: hd.speed < 6 with a nonzero desire, stuckTimer > 10).
    // One witness the original gets for free from its own physics and
    // we must state: a car accelerating hard toward its desire is not
    // stuck, even while its speed is still inside the band - without
    // the gaining test, the swing out of a recovery (which ends at
    // several m/s of reverse) re-fires the clock mid-acceleration and
    // he cycles recovery-to-recovery forever, never leaving the spot.
    {
        const float gaining = (c.speed - c.prevSpeed) *
                              (c.desiredSpeed >= 0.0f ? 1.0f : -1.0f);
        if (std::fabs(c.speed) < 6 * kV && std::fabs(c.desiredSpeed) > 6 * kV &&
            gaining < 2.0f * dt)
            c.stuckT += dt;
        else
            c.stuckT = 0.0f;
        c.prevSpeed = c.speed;
    }
    if (c.stuckT > 10 * kTick) {
        c.stuckT = 0.0f;
        c.recoveryT = 22 * kTick; // the original's 22-tick recovery
        c.recoverStrategy = (c.recoverStrategy + 1) & 7;
        c.desiredSpeed = c.speed >= 0.0f ? -kRecoverSpeed : kRecoverSpeed;
        Log("cop: stuck - reverse recovery, strategy %d", c.recoverStrategy);
        return;
    }

    // FindCost: the distance to where the man will be after one tick of
    // relative motion - the original's chase metric, sqrt and all.
    const Vec3 dv = pVel - c.vel;
    Vec3 lead = pPos + dv * kTick; // dv/256 in units == one tick in metres
    const float cost = (lead - c.pos).Length();
    c.closePursuit = cost < kCloseCost;

    // the intercept: how many frames of HIS velocity close the gap (the
    // original's dd, clamped to 60 frames, shortened a quarter when he is
    // not being battered), then the CarTail point itself.
    Vec3 target;
    if (cost < kTailWindow || c.batter) {
        const Vec3 rel = pPos - c.pos;
        const float vv = dv.x * dv.x + dv.y * dv.y + dv.z * dv.z;
        float frames = 0.0f;
        if (vv > 0.01f) {
            frames = -(rel.x * dv.x + rel.y * dv.y + rel.z * dv.z) / vv / kTick;
            frames = Clamp(frames, 0.0f, 60.0f);
            if (!c.batter)
                frames = frames * 3.0f / 4.0f;
        }
        const Vec3 intercept = pPos + pVel * (frames * kTick);
        // his nose: a moving car's nose follows its velocity on the road;
        // standing still, the host's own pose answers (the tail only needs
        // a direction to stand behind - at a standstill any error is a
        // metre, inside the pacing blend's own slack)
        Vec3 pFwd(0, 1, 0);
        {
            Vec3 flat = pVel;
            flat.z = 0.0f;
            if (flat.Length() > 2.0f) {
                pFwd = flat * (1.0f / flat.Length());
            } else if (playerInCar) {
                Vec3 cc, f, chest, v;
                if (TheHost().PlayerVehiclePose(&cc, &f, &chest, &v)) {
                    f.z = 0.0f;
                    if (f.Length() > 0.01f)
                        pFwd = f * (1.0f / f.Length());
                }
            }
        }
        const Vec3 pRight(pFwd.y, -pFwd.x, 0.0f);
        if (c.batter)
            target = intercept + pRight * (5.0f * 0.5f); // his flank: 5 units,
                // the matrix arithmetic of the original lands at ~2.5 m
        else if (!playerInCar)
            target = intercept; // SA adaptation, the user's order
                // (2026-11-07): the hunt binds to the MAN, never to his
                // car - bailed out on foot, there is no tail to stand
                // behind, so he drives at the man himself and blocks his
                // path with the car. In Driver 2 the man is never out of
                // a car and FELONY.C's gates all read the car; the
                // player-binding is ours, named as such in the step report
        else
            target = intercept - pFwd * 2.5f; // the CarTail: behind his rear
    } else {
        // FAR: WibbleDownTheRoad - walk the field's descent from where he
        // stands by his speed and 680 units of road, and aim at the point
        // it reaches; a straight descent earns the faster table.
        const float ahead = std::fabs(c.speed) * (10.0f / (kV / kM)) + 680 * kM; // his
            // speed x10 ticks of road, +680 units: in metres, |v|*0.216 + 2.66
        Vec3 far, farther;
        bool straight = false;
        if (Page().fieldOk && VehicleFieldDescent(c.pos, ahead, &far)) {
            target = far;
            if (VehicleFieldDescent(c.pos, ahead * 2.2f, &farther)) {
                Vec3 a = far - c.pos;
                Vec3 b = farther - far;
                a.z = 0.0f;
                b.z = 0.0f;
                if (a.Length() > 0.5f && b.Length() > 0.5f)
                    straight = a.Dot(b) / (a.Length() * b.Length()) > 0.95f;
            }
        } else {
            target = pPos; // no road field (graph absent): the original's
                           // own fallback aims at the man himself
        }
        c.desiredSpeed = (straight ? kSpeed2[kDifficulty] : kSpeed1[kDifficulty]) * c.powerScale;
        // (pathStraight is consumed by the speed choice and the thrust
        // bonus below; it is recomputed with the target every far tick)
        c.target = target;
        // steer and thrust for the far target ride the shared tail below
        const Vec3 toF = target - c.pos;
        Vec3 right(c.fwd.y, -c.fwd.x, 0.0f); // his TRUE right: (fwd.y, -fwd.x) in this frame - facing north it points east
        const float f = toF.x * c.fwd.x + toF.y * c.fwd.y;
        const float l = toF.x * right.x + toF.y * right.y;
        const float latVel = c.vel.x * right.x + c.vel.y * right.y;
        const float n = l - latVel * 0.000154f; // the slide trim, rescaled
        float wantWheel;
        if (f > 0.5f)
            wantWheel = Clamp(n * 512.0f / f, -512.0f, 512.0f);
        else
            wantWheel = n >= 0.0f ? 512.0f : -512.0f;
        c.wheel = WheelToward(c.wheel, wantWheel, dt);
        const float err = c.desiredSpeed - c.speed;
        float maxPower = (kDifficulty + 8) * 1024.0f;
        if (straight)
            maxPower += (kDifficulty + 4) * 1024.0f;
        float thrust;
        if (err < -50 * kV)
            thrust = -maxPower;
        else if (err > 50 * kV)
            thrust = maxPower;
        else
            thrust = err / kV * maxPower / 50.0f;
        // the original's low-speed softening and 4WD/0.7 shaping fold into
        // the pedal as one normalised curve (documented in the step report:
        // SA's pedal surface has no torque split to express them in)
        if (c.speed < 48 * kV)
            thrust *= (c.speed / kV + 80.0f) / 128.0f;
        float pedal = CopProfilePedal(Clamp(thrust / (0.35f * maxPower), -1.0f, 1.0f));
        const float g = pedal > 0.0f ? pedal : 0.0f;
        const float b = pedal < 0.0f ? -pedal : 0.0f;
        Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
        return;
    }

    // CLOSE: the four zones, in the car's own frame. t/n in the original
    // are world units along and across the nose; the ratios are scale-free
    // and port as-is, the speeds ride kV.
    const Vec3 toT = target - c.pos;
    Vec3 right(c.fwd.y, -c.fwd.x, 0.0f); // his TRUE right: (fwd.y, -fwd.x) in this frame - facing north it points east
    const float rl = right.Length();
    right = rl > 0.001f ? right * (1.0f / rl) : Vec3(1, 0, 0);
    const float t = toT.x * c.fwd.x + toT.y * c.fwd.y;
    const float latVel = c.vel.x * right.x + c.vel.y * right.y;
    const float n = (toT.x * right.x + toT.y * right.y) - latVel * 0.000154f;
    enum { ZONE_FRONT, ZONE_BACK, ZONE_LEFT, ZONE_RIGHT } zone = ZONE_FRONT;
    if (n <= 0.0f) {
        if (-2.0f * n >= t || !c.frontLClear) {
            zone = ZONE_LEFT;
            if (-n < 4.0f * t && c.closePursuit)
                zone = ZONE_BACK;
        }
    } else {
        if (2.0f * n >= t || !c.frontRClear) {
            zone = ZONE_RIGHT;
            if (n < 4.0f * t && c.closePursuit)
                zone = ZONE_BACK;
        }
    }
    float wantWheel;
    if (zone == ZONE_FRONT) {
        c.desiredSpeed = kSpeed1[kDifficulty] * c.powerScale;
        // the anti-rear-end pacing: close and not battering, blend the
        // desire toward just under the man's own speed. Car-to-car only:
        // on foot the man gets the full pressure speed (the same user
        // order - a swap or a bail-out cleanses nothing)
        if (cost < kPaceCost && !c.batter && playerInCar) {
            const float pSpeed = pVel.Length();
            const float w = cost / kPaceCost;
            const float paced = std::fmax(pSpeed - 20 * kV, 20 * kV);
            c.desiredSpeed = w * c.desiredSpeed + (1.0f - w) * paced;
            if (c.desiredSpeed < 0.0f)
                c.desiredSpeed = 0.0f;
        }
        wantWheel = t > 0.5f ? Clamp(n * 512.0f / t, -512.0f, 512.0f)
                             : (n >= 0.0f ? 512.0f : -512.0f);
    } else if (zone == ZONE_BACK) {
        c.desiredSpeed = kBackSpeed;
        wantWheel = ((n < 0.0f) != (c.speed < 0.0f)) ? -284.0f : 284.0f;
    } else {
        c.desiredSpeed = kSideSpeed;
        wantWheel = (zone == ZONE_LEFT ? -512.0f : 512.0f);
        if (((n < 0.0f) != (c.speed < 0.0f)))
            wantWheel = -wantWheel; // the original flips the lock by the
                                    // sign of travel (its steerDif^speed)
        if (!c.frontLClear && !c.frontRClear)
            c.desiredSpeed = kBlockedSpeed;
    }
    c.wheel = WheelToward(c.wheel, wantWheel, dt);
    // thrust: proportional inside the ±50-unit band, full outside it
    const float err = c.desiredSpeed - c.speed;
    const float maxPower = (kDifficulty + 8) * 1024.0f;
    float thrust;
    if (err < -50 * kV)
        thrust = -maxPower;
    else if (err > 50 * kV)
        thrust = maxPower;
    else
        thrust = err / kV * maxPower / 50.0f;
    if (c.speed < 48 * kV)
        thrust *= (c.speed / kV + 80.0f) / 128.0f;
    const float pedal = CopProfilePedal(Clamp(thrust / (0.35f * maxPower), -1.0f, 1.0f));
    const float g = pedal > 0.0f ? pedal : 0.0f;
    const float b = pedal < 0.0f ? -pedal : 0.0f;
    c.target = target;
    Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
}

// ---------------------------------------------------------------------------
// Spawning units. A ladder unit enters at range on the road graph (the
// traffic spawner's shape); a roadblock unit is placed by the block
// builder below. Slot 0 carries the system page: if the ladder ever
// replaces the anchor's BODY, the page is lifted out and laid back - the
// felony is the player's and does not die with one car.
struct PageSnapshot {
    float felony, felonyClock, offenceOccur[12], offenceRepeat[12];
    bool seen, known, informed, steerFlip, steerCalDone, spikeDone;
    float unseenT, fieldT;
    Vec3 fieldGoal;
    bool fieldOk;
};
void SnapshotPage(PageSnapshot* s) {
    const CopState& c = Page();
    s->felony = c.felony;
    s->felonyClock = c.felonyClock;
    for (int i = 0; i < 12; ++i) {
        s->offenceOccur[i] = c.offenceOccur[i];
        s->offenceRepeat[i] = c.offenceRepeat[i];
    }
    s->seen = c.seen;
    s->known = c.known;
    s->informed = c.informed;
    s->steerFlip = c.steerFlip;
    s->steerCalDone = c.steerCalDone;
    s->spikeDone = c.spikeDone;
    s->unseenT = c.unseenT;
    s->fieldT = c.fieldT;
    s->fieldGoal = c.fieldGoal;
    s->fieldOk = c.fieldOk;
}
void RestorePage(const PageSnapshot& s) {
    CopState& c = Page();
    c.felony = s.felony;
    c.felonyClock = s.felonyClock;
    for (int i = 0; i < 12; ++i) {
        c.offenceOccur[i] = s.offenceOccur[i];
        c.offenceRepeat[i] = s.offenceRepeat[i];
    }
    c.seen = s.seen;
    c.known = s.known;
    c.informed = s.informed;
    c.steerFlip = s.steerFlip;
    c.steerCalDone = s.steerCalDone;
    c.spikeDone = s.spikeDone;
    c.unseenT = s.unseenT;
    c.fieldT = s.fieldT;
    c.fieldGoal = s.fieldGoal;
    c.fieldOk = s.fieldOk;
}

bool SpawnUnit(int slot, const Vec3& at, const Vec3& face, bool block) {
    PageSnapshot snap;
    const bool keepPage = slot == 0;
    if (keepPage)
        SnapshotPage(&snap);
    Vec3 d = face - at;
    const float heading = std::atan2(d.x, d.y) * 57.29578f;
    const int veh = TheHost().SpawnCopBody(at, heading, face);
    if (veh < 0)
        return false;
    gCops[slot] = CopState();
    CopState& c = gCops[slot];
    c.veh = veh;
    c.pos = at;
    c.blockUnit = block;
    c.mode = block ? COP_BLOCK : COP_IDLE;
    c.probeT = (float)(slot % 4) * 2.0f * kTick; // the probes stagger by slot
    if (keepPage)
        RestorePage(snap);
    return true;
}

// A ladder spawn point: the road node nearest a ring point 70 m out,
// accepted in the 50..95 m band. The fan of ring points is walked in
// full; the first in-band node wins, with a preference for the half of
// the ring behind the player's motion (the spawner seats cars where the
// player is not looking - ours approximates the looking with his vel).
bool LadderSpawnPoint(const Vec3& playerPos, Vec3* out) {
    Vec3 back(0, -1, 0);
    {
        Vec3 v = gThreat.playerVel;
        v.z = 0.0f;
        if (v.Length() > 2.0f)
            back = v * (-1.0f / v.Length());
    }
    int bestNode = -1;
    float bestScore = -1e18f;
    for (int i = 0; i < 12; ++i) {
        const float a = (float)i * (6.2831853f / 12.0f);
        const Vec3 ring = playerPos + Vec3(std::sin(a), std::cos(a), 0.0f) * kSpawnRing;
        const int node = VehicleNearestNode(ring, 45.0f);
        if (node < 0)
            continue;
        Vec3 np;
        if (!VehicleNodePos(node, &np))
            continue;
        const float d = (np - playerPos).Length();
        if (d < 50.0f || d > 95.0f)
            continue;
        Vec3 dirv = np - playerPos;
        dirv.z = 0.0f;
        const float dl = dirv.Length();
        const float behind = dl > 0.5f ? (dirv * (1.0f / dl)).Dot(back) : 0.0f;
        const float score = behind * 100.0f - std::fabs(d - kSpawnRing);
        if (score > bestScore) {
            bestScore = score;
            bestNode = node;
            *out = np;
        }
    }
    if (bestNode >= 0)
        return true;
    // no in-band node anywhere on the ring (a sparse corner of the map):
    // the nearest node to the point straight behind him, whatever its
    // distance - the ladder would rather have its car than its band
    const Vec3 behindPt = playerPos + back * kSpawnRing;
    const int node = VehicleNearestNode(behindPt, 120.0f);
    return node >= 0 && VehicleNodePos(node, out);
}

// ---------------------------------------------------------------------------
// CreateRoadblock on our road graph: the original fans angles ahead of
// the player to ~64 m looking for the first road segment; the graph's
// honest equivalent is a fan of ROUTES - for each fan direction, lay the
// road route to a point 64 m out and read the road the player is
// actually on. The first fan whose road runs at least ~35 m takes the
// block: two cars across it, noses turned across the lanes, asleep.
bool PlaceRoadblock(const Vec3& playerPos) {
    Vec3 pDir(0, 1, 0);
    {
        Vec3 v = gThreat.playerVel;
        v.z = 0.0f;
        if (v.Length() > 2.0f) {
            pDir = v * (1.0f / v.Length());
        } else {
            Vec3 cc, f, chest, vv;
            if (gThreat.playerInVehicle && TheHost().PlayerVehiclePose(&cc, &f, &chest, &vv)) {
                f.z = 0.0f;
                if (f.Length() > 0.01f)
                    pDir = f * (1.0f / f.Length());
            }
        }
    }
    static const float kFanDeg[9] = { 0, 10, -10, 20, -20, 35, -35, 55, -55 };
    for (int fi = 0; fi < 9; ++fi) {
        const float a = kFanDeg[fi] * 0.0174533f;
        const float ca = std::cos(a), sa = std::sin(a);
        const Vec3 dir(pDir.x * ca - pDir.y * sa, pDir.x * sa + pDir.y * ca, 0.0f);
        const Vec3 dest = playerPos + dir * 64.0f;
        std::vector<Vec3> route;
        float roadLen = 0.0f;
        if (!VehicleRoute(playerPos, dest, &route, &roadLen) || route.size() < 2 || roadLen < 35.0f)
            continue;
        // the block point: kBlockAhead metres of road along the route
        float covered = 0.0f;
        Vec3 at = route.back();
        Vec3 segDir = pDir;
        for (size_t i = 1; i < route.size(); ++i) {
            Vec3 seg = route[i] - route[i - 1];
            seg.z = 0.0f;
            const float sl = seg.Length();
            if (covered + sl >= kBlockAhead) {
                const float t = sl > 0.001f ? (kBlockAhead - covered) / sl : 0.0f;
                at = route[i - 1] + (route[i] - route[i - 1]) * t;
                if (sl > 0.001f)
                    segDir = seg * (1.0f / sl);
                break;
            }
            covered += sl;
            if (sl > 0.001f)
                segDir = seg * (1.0f / sl);
            at = route[i];
        }
        const Vec3 lateral(segDir.y, -segDir.x, 0.0f); // across the lanes
        int placed = 0;
        for (int slot = kMaxCopCars; slot < kMaxCopUnits && placed < kMaxBlockCars; ++slot) {
            if (UnitLiving(gCops[slot]))
                continue;
            const float side = placed == 0 ? 2.3f : -2.3f;
            const Vec3 carAt = at + lateral * side;
            // the nose turns across the road, toward the far side: the
            // two cars meet nose-outward across the lanes between them
            const Vec3 face = carAt - lateral * (side > 0.0f ? 5.0f : -5.0f);
            if (SpawnUnit(slot, carAt, face, true)) {
                ++placed;
                Log("cop: roadblock car asleep across the road at (%.0f, %.0f)", carAt.x, carAt.y);
            }
        }
        if (placed > 0) {
            Log("cop: ROADBLOCK placed %.0f m ahead of the player (%d car%s)",
                kBlockAhead, placed, placed > 1 ? "s" : "");
            return true;
        }
        return false; // the road was found but no block slot is free
    }
    Log("cop: roadblock wanted but no road ahead of the player took it");
    return false;
}

} // namespace

void CopReset() {
    for (int i = 0; i < kMaxCopUnits; ++i)
        gCops[i] = CopState();
    sForce = false;
    sRespawnT = 0.0f;
    sWanted = 0;
    sCutoff = 1e9f;
    sBlockWasLow = true;
    sBlockTimerOn = false;
    sBlockTimer = 0.0f;
    gThreat.copAlive = false;
    gThreat.copPursuing = false;
    gThreat.felony = 0.0f;
    gThreat.ourUnitsLost = 0;
}

bool CopActive() { return gCop.mode != COP_NONE && gCop.veh >= 0; }

int CopUnitCount() {
    int n = 0;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (UnitLiving(gCops[i]))
            ++n;
    return n;
}
int CopPursuingCount() {
    int n = 0;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (UnitLiving(gCops[i]) && gCops[i].mode == COP_PURSUE)
            ++n;
    return n;
}
int CopBlockCount() {
    int n = 0;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (UnitLiving(gCops[i]) && gCops[i].blockUnit)
            ++n;
    return n;
}
int CopWantedCount() { return sWanted; }
float CopCutoffDistance() { return sCutoff; }

bool CopSummon(const Vec3& at, const Vec3& playerPos) {
    // a re-summon clears the field first (every old body removed through
    // the bridge, none burned), then the force stands up fresh
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (gCops[i].veh >= 0)
            TheHost().CopBodyRetire(gCops[i].veh, false);
    CopReset();
    const int veh = TheHost().SpawnCopBody(at, 0.0f, playerPos);
    if (veh < 0)
        return false;
    sForce = true;
    CopState& c = gCop;
    c.veh = veh;
    c.mode = COP_SPIKE;
    c.pos = at;
    // piece 0's route: a block of Grove's own streets, out and to a second
    // point the road graph itself connects - the same streets, the same
    // graph the atlas round proved (Grove rides to the airport on it)
    Vec3 dest = at + Vec3(-170.0f, -110.0f, 0.0f);
    if (VehicleRoute(at, dest, &c.spikeRoute, nullptr) && c.spikeRoute.size() >= 2) {
        Log("cop: body up at (%.0f, %.0f) - spike route of %d points laid over Grove's streets",
            at.x, at.y, (int)c.spikeRoute.size());
    } else {
        c.spikeRoute.clear();
        c.spikeRoute.push_back(at);
        c.spikeRoute.push_back(playerPos);
        Log("cop: body up at (%.0f, %.0f) - no road route for the spike; he drives at the player's mark and stops",
            at.x, at.y);
    }
    // shared knowledge, day one: if the boss is already in a fight, this
    // cop starts informed - the record says so, and the felony that goes
    // with joining a live fight is the original's own placid hit (659).
    if (gThreat.bossEngaged) {
        c.informed = true;
        c.felony = 659.0f;
        c.known = true;
        Log("cop: summoned into the boss's fight - he starts informed (shared threat record)");
    }
    return true;
}

void CopTick(float dt, const Vec3& playerPos) {
    CopState& page = Page();

    // the shared record's player page, refreshed every tick (piece 1's
    // feed, unchanged: both brains read the same man from the same record)
    gThreat.playerPos = playerPos;
    gThreat.playerVel = TheHost().PlayerVelocity();
    gThreat.playerVehicle = TheHost().PlayerVehicle();
    gThreat.playerInVehicle = gThreat.playerVehicle >= 0;

    if (!sForce) {
        gThreat.copAlive = false;
        gThreat.copPursuing = false;
        gThreat.felony = page.felony;
        return;
    }

    // the offence clocks run even in peace (FELONY.C's own timers)
    for (int i = 0; i < 12; ++i) {
        if (page.offenceOccur[i] < kOccurDelay[i])
            page.offenceOccur[i] += dt;
        if (page.offenceRepeat[i] > 0.0f)
            page.offenceRepeat[i] -= dt;
    }

    // ---- every living unit reports; every report prices its own book
    float struckRamU[kMaxCopUnits] = {};
    float struckShotU[kMaxCopUnits] = {};
    bool anySeen = false;
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (c.veh < 0 || c.mode == COP_NONE)
            continue;
        Host::CopBody rep;
        if (!TheHost().CopBodyReport(c.veh, &rep) || !rep.ok) {
            Log("cop: a unit's body is gone from the world - his brain stands down with it");
            c.mode = COP_NONE;
            c.veh = -1;
            continue;
        }
        c.pos = rep.pos;
        c.fwd = rep.fwd;
        c.vel = rep.vel;
        c.speed = rep.vel.x * rep.fwd.x + rep.vel.y * rep.fwd.y + rep.vel.z * rep.fwd.z;
        // his damage book (the collision price is BCOLLIDE.C's shape; the
        // host-priced channels convert at the one anchor). The struck
        // channels bill the book whoever dealt them; the ledger of what
        // the PLAYER dealt rides playerBookDamage, and that split is what
        // the replacement step's ourUnitsLost count reads at retirement.
        float impactPrice = 0.0f;
        if (rep.impactDv > kImpactFree) {
            impactPrice = std::fmin((rep.impactDv - kImpactFree) * kImpactRate, kImpactCap) *
                          kPursuerToughness;
            c.totalDamage += impactPrice;
        }
        // the health-delta channels, blast-capped as one sum (fix round
        // 3, see kBlastPrice): a frame that strips the whole body bills
        // one blast, never the whole book. The player's share of the
        // delta (shot + driver channels) scales by the same fraction
        // the cap leaves standing, so the ourUnitsLost attribution at
        // retirement reads the billed book, not the raw host numbers.
        const float rawDeltaPts =
            (rep.struckShot + rep.struckWorld + rep.struckDriver) * kSaBookRate;
        const float deltaBill = std::fmin(rawDeltaPts, kBlastPrice);
        c.totalDamage += deltaBill;
        float playerDeltaPts = (rep.struckShot + rep.struckDriver) * kSaBookRate;
        if (rawDeltaPts > deltaBill && rawDeltaPts > 0.0f)
            playerDeltaPts *= deltaBill / rawDeltaPts;
        c.playerBookDamage += playerDeltaPts +
                              (rep.struckRam > 0.5f ? impactPrice : 0.0f);
        c.health = Clamp(1.0f - c.totalDamage / kWreckLine, 0.0f, 1.0f);
        // the book is DISPLAYED on the body (step 1's display band): the
        // host shows smoke and fire off its own health, his law decides
        // when he is out - the body never enters the host's burn band
        // while his book lives
        {
            float shown = c.health;
            if (shown > 0.02f)
                shown = 0.38f + 0.62f * shown;
            TheHost().CopBodySetHealth(c.veh, shown);
        }
        if (c.totalDamage > kWreckLine && c.mode != COP_WRECKED) {
            c.mode = COP_WRECKED;
            Log("cop: a unit's book is spent (%.0f of his points) - he retires and burns as one unit, car and driver together",
                c.totalDamage);
            if (c.playerBookDamage * 2.0f >= c.totalDamage) {
                ThreatNoteOurUnitLost();
                Log("cop: that retirement was the player's work - ourUnitsLost = %d (the replacement step's counter)",
                    gThreat.ourUnitsLost);
            }
            TheHost().CopBodySetHealth(c.veh, 0.0f);
            TheHost().CopBodyRetire(c.veh, true);
            c.veh = -1;
            if (!c.blockUnit)
                c.mode = COP_NONE;
            else
                c.mode = COP_NONE; // a spent block car is just gone; the
                                   // block count re-arms off the live slots
            continue;
        }
        struckRamU[slot] = rep.struckRam;
        struckShotU[slot] = rep.struckShot;
        // the victim of a hit knows - the original's felony note for
        // being struck never asks for his eyes; the unit's own sight
        // feeds the page's seen flag, whoever of them saw the man
        bool sees = CopSees(c.pos, c.fwd, page.felony > kFelonyTrigger) ||
                    rep.struckRam > 0.5f || rep.struckShot > 0.5f;
        if (sees)
            anySeen = true;
    }

    // the page's sight bookkeeping (ControlCopDetection's shape, piece
    // 1's reading kept exactly): seen holds the position; 256 unseen
    // ticks lose it, whatever the felony - the loss gates the pursuit,
    // not the felony itself, which waits above the trigger. Under the
    // SA adaptation an informed summon starts with the position known.
    page.seen = anySeen;
    if (anySeen) {
        page.unseenT = 0.0f;
        page.known = true;
    } else if (page.known) {
        page.unseenT += dt;
        if (page.unseenT > kLostAfter) {
            page.known = false;
            Log("cop: position lost (256 unseen ticks)");
        }
    }

    // ---- felony notes, piece 1's forms kept whole: speeding is type 2
    // (the original fines past 19/16 of the road's limit at the top
    // limit; our graph carries no per-road limits, so the one stand-in
    // serves the city - threshold 25 * 19/16 = 29.7 m/s); hitting cars
    // is type 4 off the player's own damage ledger; a cop struck by the
    // player's car is type 5 on CarHitByPlayer's scale; being shot at
    // is our named extension (Driver 2's man has no gun), priced at
    // half the full scale. The victim of a hit knows the position.
    if (gThreat.playerInVehicle && anySeen) {
        if (gThreat.playerVel.Length() > 25.0f * 19.0f / 16.0f)
            NoteFelony(2, 0);
        const float impact = TheHost().PlayerVehicleImpact();
        if (impact > 12.0f)
            NoteFelony(4, impact >= 25.0f ? 4096 : (int)(impact * 160.0f));
    }
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        if (struckRamU[slot] > 2.0f) {
            page.known = true;
            NoteFelony(5, struckRamU[slot] >= 25.0f ? 4096 : (int)(struckRamU[slot] * 160.0f));
        }
        if (struckShotU[slot] > 0.5f) {
            page.known = true;
            NoteFelony(5, 2048);
        }
    }

    // AdjustFelony (FELONY.C), piece 1's exact reading kept: SEEN and
    // hot, the felony creeps up 1 per 31 ticks; at or below the trigger
    // it bleeds 1 per 31 ticks; above the trigger unseen it does NOT
    // bleed (the felony waits). "Seen" is the page's: any unit's eyes,
    // or any unit struck, this tick.
    page.felonyClock += dt;
    while (page.felonyClock >= kFelonyClock) {
        page.felonyClock -= kFelonyClock;
        if (page.felony > kFelonyTrigger) {
            if (anySeen && page.felony < kFelonyCap)
                page.felony += 1.0f;
        } else if (page.felony > 0.0f) {
            page.felony -= 1.0f;
        }
    }
    gThreat.felony = page.felony;

    // ---- the manager: pursuit transitions, the ladder, the cutoff
    const bool wantPursue = page.felony > kFelonyTrigger && page.known;
    int livingLadder = 0;
    for (int slot = 0; slot < kMaxCopCars; ++slot)
        if (UnitLiving(gCops[slot]))
            ++livingLadder;
    sWanted = LadderWanted(page.felony);

    // a summon's spike runs to its end whatever the felony does (piece 0's
    // proof is not the ladder's to cancel); afterwards slot 0 is a unit.
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (!UnitLiving(c))
            continue;
        c.batter = page.felony >= kBatterFelony;
        if (c.mode == COP_SPIKE || c.mode == COP_LEAVE)
            continue;
        if (c.mode == COP_BLOCK) {
            // PassiveCopTasks' conversion, on our wake terms: felony hot,
            // position known, and the player closing on the block - or
            // the block car itself struck, which wakes it on the spot
            const float d = (c.pos - playerPos).Length();
            if ((wantPursue && d < kBlockWakeDist) || struckRamU[slot] > 0.5f ||
                struckShotU[slot] > 0.5f) {
                c.mode = COP_PURSUE;
                Log("cop: the roadblock wakes - its unit is in the pursuit now, book and brain his own");
            } else if (d > kPingOut) {
                // CheckPingOut takes an unmet block car back, quietly
                TheHost().CopBodyRetire(c.veh, false);
                c.veh = -1;
                c.mode = COP_NONE;
                Log("cop: the unmet roadblock pinged out at %.0f m", d);
                continue;
            } else {
                continue; // still asleep
            }
        }
        if (wantPursue && (c.mode == COP_IDLE || c.mode == COP_PURSUE)) {
            if (c.mode != COP_PURSUE)
                Log("cop: %s - felony %.0f over the trigger, position known", slot == 0 ? "the pursuit opens" : "a unit joins the pursuit", page.felony);
            c.mode = COP_PURSUE;
        } else if (!wantPursue && c.mode == COP_PURSUE) {
            // a block-woken unit with the case gone leaves like the rest;
            // a plain unit stands down where it stands
            if (c.blockUnit) {
                c.mode = COP_LEAVE;
                c.leaveRoute.clear();
                c.leaveIdx = 0;
                Log("cop: the case is over - the block unit drives off");
            } else {
                c.mode = COP_IDLE;
                Log("cop: standing down (felony %.0f, position %s)", page.felony,
                    page.known ? "still known" : "lost");
            }
        } else if (page.felony <= kFelonyTrigger && c.blockUnit && c.mode == COP_IDLE) {
            c.mode = COP_LEAVE;
            c.leaveRoute.clear();
            c.leaveIdx = 0;
        }
    }

    // the ladder: count the force, the way ControlNumberOfCops counts
    // cars - it does not re-rank them every tick. A surplus among the
    // non-leaving units is stood down, the farthest first, and a unit
    // told to leave KEEPS leaving (he does not suicide-charge, and he
    // does not churn) until the world takes him at the ping-out edge or
    // the ladder genuinely wants everyone again (wanted >= living).
    // Per-tick rank re-evaluation was tried first in this step and
    // churned stand-down/turn-back cycles in the sims - the commitment
    // below is the fix, and the sims pin it.
    {
        int nonLeaving = 0;
        for (int slot = 0; slot < kMaxCopCars; ++slot)
            if (UnitLiving(gCops[slot]) && gCops[slot].mode != COP_LEAVE)
                ++nonLeaving;
        if (sWanted >= livingLadder) {
            for (int slot = 1; slot < kMaxCopCars; ++slot) {
                CopState& c = gCops[slot];
                if (UnitLiving(c) && c.mode == COP_LEAVE) {
                    c.mode = COP_IDLE; // the felony rose again under him
                    Log("cop: the ladder wants him again - the leaving unit turns back");
                }
            }
        } else {
            int excess = nonLeaving - sWanted;
            while (excess-- > 0) {
                int farSlot = -1;
                float farDist = -1.0f;
                for (int slot = 1; slot < kMaxCopCars; ++slot) {
                    CopState& c = gCops[slot];
                    if (!UnitLiving(c) || c.mode == COP_LEAVE || c.mode == COP_SPIKE)
                        continue; // the anchor never leaves; the spike is proof
                    const float d = (c.pos - playerPos).Length();
                    if (d > farDist) {
                        farDist = d;
                        farSlot = slot;
                    }
                }
                if (farSlot < 0)
                    break;
                CopState& c = gCops[farSlot];
                c.mode = COP_LEAVE;
                c.leaveRoute.clear();
                c.leaveIdx = 0;
                Log("cop: the ladder wants %d - a surplus unit stands down and drives off", sWanted);
            }
        }
    }

    // the cutoff: more pursuers live than the ladder wants (a woken
    // roadblock is how that happens) - the wanted-th nearest pursuer's
    // distance becomes the line, and pursuers beyond it read their
    // desire at the half scale. Nobody beyond: everyone at full strength.
    {
        float pDist[kMaxCopUnits];
        int nP = 0;
        for (int slot = 0; slot < kMaxCopUnits; ++slot)
            if (UnitLiving(gCops[slot]) && gCops[slot].mode == COP_PURSUE)
                pDist[nP++] = (gCops[slot].pos - playerPos).Length();
        for (int i = 1; i < nP; ++i) {
            const float d = pDist[i];
            int j = i - 1;
            while (j >= 0 && pDist[j] > d) {
                pDist[j + 1] = pDist[j];
                --j;
            }
            pDist[j + 1] = d;
        }
        if (nP > sWanted && sWanted >= 1)
            sCutoff = pDist[sWanted - 1];
        else
            sCutoff = 1e9f;
        for (int slot = 0; slot < kMaxCopUnits; ++slot) {
            CopState& c = gCops[slot];
            if (UnitLiving(c) && c.mode == COP_PURSUE)
                c.powerScale = (c.pos - playerPos).Length() > sCutoff ? kCutoffScale : 1.0f;
            else
                c.powerScale = 1.0f;
        }
    }

    // the spawn clock (ControlNumberOfCops): while the ladder is short,
    // the delay shrinks with the felony - 320 ticks at felony 0, half
    // that at the cap, through the named stand-in scale. When the clock
    // runs out and a ladder slot is free, the unit enters at range.
    if (livingLadder < sWanted) {
        sRespawnT += dt;
        const float delayTicks = kRespawnTicks * (1.0f - page.felony * kRespawnScale / kFelonyCap);
        if (sRespawnT >= delayTicks * kTick) {
            sRespawnT = 0.0f;
            for (int slot = 0; slot < kMaxCopCars; ++slot) {
                if (UnitLiving(gCops[slot]))
                    continue;
                Vec3 at;
                if (LadderSpawnPoint(playerPos, &at)) {
                    if (SpawnUnit(slot, at, playerPos, false)) {
                        Log("cop: the ladder sends another unit - he enters at %.0f m on the road (felony %.0f, wanted %d)",
                            (at - playerPos).Length(), page.felony, sWanted);
                        ++livingLadder;
                    }
                } else {
                    Log("cop: the ladder wants a unit but no road position took him this tick");
                }
                break;
            }
        }
    } else {
        sRespawnT = 0.0f;
    }

    // ---- the roadblock chain (NoteFelony's latch + MAIN.C's count)
    if (page.felony <= kBlockArmFelony) {
        sBlockWasLow = true;
    } else if (sBlockWasLow) {
        sBlockWasLow = false;
        sBlockTimer = BlockDelay();
        sBlockTimerOn = true;
        Log("cop: felony crossed 1318 - the roadblock count is armed (%.1f s)",
            sBlockTimer);
    }
    if (CopBlockCount() > 0) {
        sBlockTimer = BlockDelay(); // while block cars stand, the count
                                    // is held at the delay (MAIN.C)
    } else if (sBlockTimerOn) {
        sBlockTimer -= dt;
        if (sBlockTimer <= 0.0f) {
            bool pursuing = false;
            for (int slot = 0; slot < kMaxCopUnits; ++slot)
                if (UnitLiving(gCops[slot]) && gCops[slot].mode == COP_PURSUE)
                    pursuing = true;
            if (pursuing && page.felony >= kBlockFireFelony)
                PlaceRoadblock(playerPos);
            sBlockTimer = BlockDelay();
        }
    }

    // ---- the shared road field, refreshed ONCE for the whole force
    // (the original keeps one field for every cop; the throttle is the
    // page's, and no unit rebuilds it alone - the perf law of this step)
    bool anyWillPursue = false;
    for (int slot = 0; slot < kMaxCopUnits; ++slot)
        if (UnitLiving(gCops[slot]) && gCops[slot].mode == COP_PURSUE)
            anyWillPursue = true;
    if (anyWillPursue) {
        page.fieldT -= dt;
        if (page.fieldT <= 0.0f || (page.fieldGoal - playerPos).Length() > 25.0f) {
            page.fieldT = 0.75f;
            page.fieldGoal = playerPos;
            page.fieldOk = VehicleFieldTo(playerPos);
        }
    }

    // ---- drive every unit by its mode
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (!UnitLiving(c))
            continue;
        switch (c.mode) {
        case COP_SPIKE:
            SpikeDrive(dt);
            break;
        case COP_PURSUE:
            CopControl(c, dt);
            break;
        case COP_LEAVE:
            LeaveDrive(c, dt, playerPos);
            break;
        case COP_IDLE:
        case COP_BLOCK:
            StandDrive(c);
            break;
        default:
            break;
        }
    }

    // ---- the shared record's cop page: the anchor leads it while he
    // lives; with him gone, the nearest living unit speaks for the force
    bool alive = false, pursuing = false;
    float leadDist = 1e18f;
    Vec3 leadPos = page.pos;
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        const CopState& c = gCops[slot];
        if (!UnitLiving(c))
            continue;
        alive = true;
        if (c.mode == COP_PURSUE)
            pursuing = true;
        const float d = (c.pos - playerPos).Length();
        if (d < leadDist) {
            leadDist = d;
            leadPos = c.pos;
        }
    }
    if (UnitLiving(gCops[0]))
        leadPos = gCops[0].pos;
    gThreat.copAlive = alive;
    gThreat.copPursuing = pursuing;
    gThreat.copPos = leadPos;
}

// ---- piece 3: the decoded profile's accessors -------------------------
const CopProfileData& CopProfile() { return kCopProfile; }

float CopProfilePowerGain() {
    const int pr = sProfilePowerOverride >= 0 ? sProfilePowerOverride
                                              : kCopProfile.powerRatio;
    return (float)pr / 4096.0f;
}

float CopProfileGrip() {
    return (kCopProfile.frictionScaleRatio / 32.0f) *
           (kCopProfile.traction / 4096.0f);
}

float CopProfilePedal(float pedal) {
    return Clamp(pedal * CopProfilePowerGain(), -1.0f, 1.0f);
}

void CopProfileOverrideForTest(int powerRatio) {
    sProfilePowerOverride = powerRatio;
}

} // namespace mc
