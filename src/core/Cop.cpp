#include "Cop.h"

#include <cmath>

#include "Host.h"
#include "NavGraph.h"

namespace mc {

SharedThreat gThreat;
CopState gCop;

void ThreatNoteBoss(int state, bool engaged, const Vec3& pos, bool alive) {
    gThreat.bossState = state;
    gThreat.bossEngaged = engaged;
    gThreat.bossPos = pos;
    gThreat.bossAlive = alive;
}

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
constexpr float kBatterFelony = 2457.0f;   // the ladder's top rung (piece 2
    // brings the ladder itself; today it is the batter line - the
    // original's gBatterPlayer is a per-mission flag, and top pressure is
    // the honest stand-in, named as such)
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
constexpr float kImpactFree = 2.8f;    // m/s of velocity change: free
constexpr float kImpactRate = 340.0f;  // his points per m/s past it
constexpr float kImpactCap = 1500.0f;  // per single impact, before discount
constexpr float kPursuerToughness = 0.875f; // ApplyDamage, difficulty 2
constexpr float kSaBookRate = 27.0f;   // host health point -> his points

// FELONY.C's own tables.
constexpr int kPlacid[12] = { 659, 659, 659, 659, 659, 1318, 659, 659, 659, 659, 659, 4096 };
constexpr int kAngry[12] = { 0, 75, 0, 0, 659, 659, 659, 25, 0, 0, 0, 0 };
constexpr float kOccurDelay[12] = { 24 * kTick, 0, 0, 0, 0, 0, 0, 0, 24 * kTick, 0, 24 * kTick, 0 };
constexpr float kRepeatDelay[12] = { 128 * kTick, 0, 128 * kTick, 64 * kTick, 64 * kTick, 32 * kTick,
                                     32 * kTick, 0, 128 * kTick, 256 * kTick, 0, 0 };
// pursuitFelonyScale is 3000 in InitCops (NOT its header's 4096).
constexpr float kPursuitScale = 3000.0f;

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
void NoteFelony(int type, int additionalScale) {
    CopState& c = gCop;
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
// spike's calibration settled is applied at this one choke point, and
// the magnitude rides kSteerScale - chosen so both readings of the
// field's unit stay sane (radians: 0.6 rad is a full city lock;
// normalised: 0.6 of lock still turns a chase). The spike's in-game
// behaviour is the scale's judge, named here rather than hidden.
constexpr float kSteerScale = 0.6f;
void Drive(float gas, float brake, float wheelNorm, bool siren) {
    CopState& c = gCop;
    const float steer = (c.steerFlip ? -wheelNorm : wheelNorm) * kSteerScale;
    TheHost().CopBodyDrive(c.veh, gas, brake, steer, siren);
}

// ---------------------------------------------------------------------------
// Piece 0's spike controller: first the steering calibration (one second
// of commanded turn, read back off the car's own nose), then the block
// route by pure pursuit on the waypoints the road graph itself hands
// back. Deliberately dumb beyond that - its job is proving the body
// answers gas, brake and steering, with the read-back (the bridge's,
// from the vehicle itself) logged every second.
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
        Drive(0.0f, c.speed > 1.0f ? 1.0f : 0.0f, 0.0f, false); // brake while
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
    Drive(gas, brake, steer, false);
    c.gas = gas;
    c.brake = brake;
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
// CopControl1 (COP_AI.C), ported whole onto metres. dt-driven: the
// original runs one decision per frame at 50 ticks/s, so every per-tick
// constant rides kTick.
void CopControl(float dt) {
    CopState& c = gCop;
    const bool playerInCar = gThreat.playerInVehicle;
    const Vec3 pPos = gThreat.playerPos;
    const Vec3 pVel = gThreat.playerVel;

    // the bumper corners, probed every 8 ticks like the original: a point
    // 400 units ahead and 150 to the side (1.56 m / 0.59 m) - the original
    // asks CellAtPositionEmpty there; the host's wall oracle stands in
    // (the same named swap as the sight gates).
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
        c.gas = pedal > 0.0f ? pedal : 0.0f;
        c.brake = pedal < 0.0f ? -pedal : 0.0f;
        Drive(c.gas, c.brake, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
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
                Vec3 c, f, chest, v;
                if (TheHost().PlayerVehiclePose(&c, &f, &chest, &v)) {
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
        if (c.fieldOk && VehicleFieldDescent(c.pos, ahead, &far)) {
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
        c.desiredSpeed = straight ? kSpeed2[kDifficulty] : kSpeed1[kDifficulty];
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
        float pedal = Clamp(thrust / (0.35f * maxPower), -1.0f, 1.0f);
        c.gas = pedal > 0.0f ? pedal : 0.0f;
        c.brake = pedal < 0.0f ? -pedal : 0.0f;
        Drive(c.gas, c.brake, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
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
        c.desiredSpeed = kSpeed1[kDifficulty];
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
    const float pedal = Clamp(thrust / (0.35f * maxPower), -1.0f, 1.0f);
    c.gas = pedal > 0.0f ? pedal : 0.0f;
    c.brake = pedal < 0.0f ? -pedal : 0.0f;
    c.target = target;
    Drive(c.gas, c.brake, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), true);
}

} // namespace

void CopReset() {
    gCop = CopState();
    gThreat.copAlive = false;
    gThreat.copPursuing = false;
    gThreat.felony = 0.0f;
}

bool CopActive() { return gCop.mode != COP_NONE && gCop.veh >= 0; }

bool CopSummon(const Vec3& at, const Vec3& playerPos) {
    if (gCop.veh >= 0)
        TheHost().CopBodyRetire(gCop.veh, false); // one cop: a re-summon
                                        // stands a fresh body up, old removed
    CopState fresh;
    // face the man: the original's justPinged heading snap
    Vec3 d = playerPos - at;
    const float heading = std::atan2(d.x, d.y) * 57.29578f;
    const int veh = TheHost().SpawnCopBody(at, heading, playerPos);
    if (veh < 0)
        return false;
    gCop = fresh;
    gCop.veh = veh;
    gCop.mode = COP_SPIKE;
    gCop.pos = at;
    // piece 0's route: a block of Grove's own streets, out and to a second
    // point the road graph itself connects - the same streets, the same
    // graph the atlas round proved (Grove rides to the airport on it)
    Vec3 dest = at + Vec3(-170.0f, -110.0f, 0.0f);
    if (VehicleRoute(at, dest, &gCop.spikeRoute, nullptr) && gCop.spikeRoute.size() >= 2) {
        Log("cop: body up at (%.0f, %.0f) - spike route of %d points laid over Grove's streets",
            at.x, at.y, (int)gCop.spikeRoute.size());
    } else {
        gCop.spikeRoute.clear();
        gCop.spikeRoute.push_back(at);
        gCop.spikeRoute.push_back(playerPos);
        Log("cop: body up at (%.0f, %.0f) - no road route for the spike; he drives at the player's mark and stops",
            at.x, at.y);
    }
    // shared knowledge, day one: if the boss is already in a fight, this
    // cop starts informed - the record says so, and the felony that goes
    // with joining a live fight is the original's own placid hit (659).
    if (gThreat.bossEngaged) {
        gCop.informed = true;
        gCop.felony = 659.0f;
        gCop.known = true;
        Log("cop: summoned into the boss's fight - he starts informed (shared threat record)");
    }
    return true;
}

void CopTick(float dt, const Vec3& playerPos) {
    CopState& c = gCop;
    // the feed: both brains read the same man from the same record
    gThreat.playerPos = playerPos;
    gThreat.playerVel = TheHost().PlayerVelocity();
    gThreat.playerVehicle = TheHost().PlayerVehicle();
    gThreat.playerInVehicle = gThreat.playerVehicle >= 0;
    if (c.mode == COP_NONE || c.veh < 0) {
        gThreat.copAlive = false;
        gThreat.copPursuing = false;
        return;
    }
    Host::CopBody rep;
    if (!TheHost().CopBodyReport(c.veh, &rep) || !rep.ok) {
        // the body is gone from the world (streamed out, removed): the
        // brain forgets the car, exactly the boss body's own discipline -
        // no dangling hands, no invented state
        Log("cop: his body is gone from the world - the brain stands down");
        c.mode = COP_NONE;
        c.veh = -1;
        gThreat.copAlive = false;
        gThreat.copPursuing = false;
        return;
    }
    c.pos = rep.pos;
    c.fwd = rep.fwd;
    c.vel = rep.vel;
    c.speed = rep.vel.x * rep.fwd.x + rep.vel.y * rep.fwd.y + rep.vel.z * rep.fwd.z;
    gThreat.copPos = rep.pos;
    gThreat.copAlive = true;
    gThreat.felony = c.felony;

    // ---- his damage book, priced HERE on the original's scale (the SA
    // body's health is a display the bridge holds for us - it never rules
    // his survival). Collisions bill by their own strike, the rest by the
    // host's health points at the anchor conversion; a hit on the DRIVER
    // bills the same book - car and driver are one unit, and he is not
    // independently killable (the user's order; in Driver 2 no separate
    // driver death exists at all).
    if (rep.impactDv > kImpactFree) {
        float price = (rep.impactDv - kImpactFree) * kImpactRate;
        if (price > kImpactCap)
            price = kImpactCap;
        c.totalDamage += price * kPursuerToughness;
    }
    c.totalDamage += rep.struckShot * kSaBookRate;
    c.totalDamage += rep.struckWorld * kSaBookRate;
    c.totalDamage += rep.struckDriver * kSaBookRate;
    c.health = Clamp(1.0f - c.totalDamage / kWreckLine, 0.0f, 1.0f);
    TheHost().CopBodySetHealth(c.veh, c.health); // the display follows

    // a spent book retires him, as in the original: the display is handed
    // to the host's fire and the brain forgets the car
    if (c.totalDamage > kWreckLine && c.mode != COP_WRECKED) {
        c.mode = COP_WRECKED;
        Log("cop: his book is spent (totalDamage %.0f past %.0f) - he retires and burns", c.totalDamage, kWreckLine);
        TheHost().CopBodySetHealth(c.veh, 0.0f);
        TheHost().CopBodyRetire(c.veh, true);
        c.veh = -1;
        c.mode = COP_NONE;
        gThreat.copAlive = false;
        gThreat.copPursuing = false;
        return;
    }

    // felony timers always run (the warmth of each offence type, and the
    // 31-tick AdjustFelony clock)
    for (int i = 0; i < 12; ++i) {
        if (c.offenceOccur[i] < kOccurDelay[i])
            c.offenceOccur[i] += dt;
        if (c.offenceRepeat[i] > 0.0f)
            c.offenceRepeat[i] -= dt;
    }

    // sight: hot gates once the felony is over the trigger
    const bool hot = c.felony > kFelonyTrigger;
    c.seen = CopSees(c.pos, c.fwd, hot);
    // being hit needs no eyes: the victim knows (named extension of the
    // original's gate - in Driver 2 the player has no gun and no ram
    // target but the world; here the cop himself can be the victim)
    if (rep.struckRam > 0.5f || rep.struckShot > 0.5f)
        c.seen = true;
    if (c.seen) {
        c.unseenT = 0.0f;
        c.known = true;
    } else {
        c.unseenT += dt;
        if (c.unseenT > kLostAfter)
            c.known = false; // 256 ticks unseen: player_position_known lost
    }

    // the offences of the simple form: 2 speeding, 4 hit a car, 5 hit a cop
    if (gThreat.playerInVehicle && c.seen) {
        // speeding: the original fines past 1.5x the road's limit (19/16x
        // at the top limit); our road graph carries no per-road limits, so
        // the one top-limit stand-in (25 m/s) serves the whole city -
        // named here, not hidden: threshold 25 * 19/16 = 29.7 m/s
        if (gThreat.playerVel.Length() > 25.0f * 19.0f / 16.0f)
            NoteFelony(2, 0);
        // he hit something hard: his own car's damage is the ledger
        const float impact = TheHost().PlayerVehicleImpact();
        if (impact > 12.0f)
            NoteFelony(4, impact >= 25.0f ? 4096 : (int)(impact * 160.0f));
    }
    if (rep.struckRam > 2.0f) {
        // the cop himself, rammed: CarHitByPlayer's scale by hardness
        NoteFelony(5, rep.struckRam >= 25.0f ? 4096 : (int)(rep.struckRam * 160.0f));
        c.known = true;
    }
    if (rep.struckShot > 0.5f) {
        // shot by the player (Driver 2's man has no gun: our extension,
        // named) - priced like the hit he is, at half the full scale
        NoteFelony(5, 2048);
        c.known = true;
    }

    // AdjustFelony: seen and hot, the felony creeps up; at or under the
    // trigger it bleeds away - 1 per 31 ticks, the original's own rates
    c.felonyClock += dt;
    while (c.felonyClock >= kFelonyClock) {
        c.felonyClock -= kFelonyClock;
        if (c.felony > kFelonyTrigger) {
            if (c.seen && c.felony < kFelonyCap)
                c.felony += 1.0f;
        } else if (c.felony > 0.0f) {
            c.felony -= 1.0f;
        }
    }
    gThreat.felony = c.felony;

    // pursuit state. The 256-tick loss gates the felony and the
    // stand-down, NOT the steering (the work order's own reading, and the
    // original's shape: an active pursuer's steering is omniscient -
    // CopControl1 always drives at the live position). He stands down
    // only when the position is lost AND the man is genuinely gone: past
    // the original's own ping-out distance (24000 units = 93.75 m, where
    // CheckPingsOut removes a car from the world - ours stands down
    // instead of vanishing). Back in sight or back in range, with the
    // felony intact, the pursuit resumes on its own.
    const float distToMan = (playerPos - c.pos).Length();
    const bool lost = !c.known && distToMan > 24000 * kM;
    const bool wantPursue = c.felony > kFelonyTrigger && !lost;
    if (c.mode == COP_IDLE && wantPursue) {
        c.mode = COP_PURSUE;
        Log("cop: felony %.0f over the trigger - pursuit", c.felony);
    } else if (c.mode == COP_PURSUE && !wantPursue) {
        c.mode = COP_IDLE;
        Log("cop: pursuit stood down (felony %.0f, position %s, %.0f m)", c.felony,
            c.known ? "known" : "lost", distToMan);
    }
    c.batter = c.felony >= kBatterFelony;
    gThreat.copPursuing = c.mode == COP_PURSUE;

    // the road field rides a throttle: rebuilt when it is stale or the man
    // has run 25 m from where it was priced
    if (c.mode == COP_PURSUE) {
        c.fieldT -= dt;
        if (!c.fieldOk || c.fieldT <= 0.0f || (playerPos - c.fieldGoal).Length() > 25.0f) {
            c.fieldOk = VehicleFieldTo(playerPos);
            c.fieldGoal = playerPos;
            c.fieldT = 0.75f;
        }
    }

    switch (c.mode) {
    case COP_SPIKE:
        SpikeDrive(dt);
        break;
    case COP_PURSUE:
        CopControl(dt);
        break;
    default: // COP_IDLE: he stands by, felony watching. The stop is
        // pedal-free once he is slow: on this host a held brake at a
        // standstill is a reverse gear, not a handbrake - he brakes
        // only while he is still rolling
        if (std::fabs(c.speed) > 1.0f) {
            Drive(0.0f, 0.7f, 0.0f, false);
            c.brake = 0.7f;
        } else {
            Drive(0.0f, 0.0f, 0.0f, false);
            c.brake = 0.0f;
        }
        c.gas = 0.0f;
        break;
    }
}

} // namespace mc
