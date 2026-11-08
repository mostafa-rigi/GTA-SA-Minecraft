#include "GtaCop.h"

#include <cmath>
#include <cstdlib>

#include "CCopPed.h"
#include "CDecisionMakerTypes.h"
#include "CPools.h"
#include "eCarMission.h" // the autopilot's mission words (fix round 4)
#include "CPlayerPed.h"
#include "CTask.h"
#include "CVehicle.h"
#include "CWorld.h"
#include "common.h"
#include "extensions/ScriptCommands.h"

#include "CCamera.h"   // TheCamera: CopRender's distance/frustum courtesy
#include "Cop.h"
#include "CopCarModel.h" // DrawCopCar / CopWheelCenters: his own body (piece 3)
#include "Core.h"
#include "GtaMobs.h"   // shared bridge utilities (the shot clock is the
                       // force's own since fix round 5 - see sCopShotAge)
#include "PedSkins.h"  // PedSkinsExcludePed
#include "Render3D.h"  // AddShadow
#include "Sound.h"     // SetPosLoop / PlaySfx: his own siren, engine and radio (piece 3)
#include "Textures.h"  // CopBodyFilesReady

using namespace plugin;

namespace mc {
namespace {

constexpr unsigned char kCreatedByMission = 2;
constexpr int kCopCarModel = 596;   // SA's LSPD cruiser: the stand-in body
constexpr int kCopDriverModel = 280; // SA's LSPD officer: the stand-in driver

// Step 2: one body state per cop unit, keyed by the vehicle's pool ref -
// piece 1's single state, pluralised. The discipline is unchanged: a
// stored pointer is only ever touched after the pool itself confirms the
// slot still holds it, and every operation finds ITS body by ref first.
struct CopBodyState {
    bool used = false;
    CVehicle* veh = nullptr;
    int vehRef = -1;
    CPed* driver = nullptr;
    int driverRef = -1;
    bool seated = false;
    float seatT = 0.0f;      // seconds since spawn; the seat warp's budget
    bool seatRetried = false;
    float seatLogT = 0.0f;   // the seat watch's per-second diagnostic clock
    // his survival book lives in the core; this bridge only DISPLAYS it:
    // book01 in (0,1] shows as 380..1000 host points (damage stages ride
    // the host's own bands; the fire band under ~300 is never entered
    // while his book lives), and every loss is measured against the last
    // displayed value before Post re-asserts it
    float book01 = 1.0f;
    float lastSetHealth = 1000.0f;
    float struckRam = 0.0f;  // priced since the last report, consumed by it
    float struckShot = 0.0f;
    float struckWorld = 0.0f;
    // the one-unit law: the driver is not a separate life. Hits on him
    // are measured here and billed to the cop's book by the core; his
    // health is topped back up every frame (the boss body's discipline)
    float driverHitAcc = 0.0f;
    // the hardest impact since the last report, as velocity change
    Vec3 prevVel;
    bool prevVelOk = false;
    float impactPeak = 0.0f;
    // the core's last command, re-asserted by Post every frame
    float gas = 0.0f, brake = 1.0f, steer = 0.0f;
    bool siren = false;
    bool haveCmd = false;
    // fix round 4: the core's drive intent, mirrored into the car's own
    // autopilot by ApplyCommand - the feed (what the brain last named)
    // and the writing (what the autopilot last heard, so a standing
    // order is not re-issued every frame and the game's own route
    // bookkeeping in the same struct is left alone between changes)
    bool apFeed = false;
    Vec3 apTarget;
    float apCruise = 0.0f;
    bool apArmed = false;
    Vec3 apWritten;
    float apWrittenCruise = -1.0f;
    float apWriteAge = 1e9f; // fix round 5: seconds since the mirror
        // last wrote the autopilot - the refresh cadence's clock,
        // advanced by CopBodyPre; a live mission is re-written at most
        // ~3 times a second, so ProcessAI's own route bookkeeping is
        // not reset under it faster than it can drive
    float apRescueLogT = 1e9f; // the complete update: seconds since the
        // mirror last LOGGED a rescue (a dropped or foreign mission
        // found under a live feed) - the rescue itself is never
        // throttled, only its voice is (once a second per body)
    // piece 3's sound state: the pursuit edge (for his radio) and the
    // engine layer currently sounding (his idle/revs swap rides it)
    bool sirenPrev = false;
    int engineEv = -1;
    // piece 3's drawn-body state: the wheels' spin angle (accumulated
    // in Pre off the car's own travel) and his speed, for the render
    float wheelSpin = 0.0f;
    float speedMps = 0.0f;
    // round 2's spawn instrumentation: the first report that answers
    // for this body says so once in the log, so a summon's story reads
    // end to end (spawn steps, seating in Pre, first report here)
    bool reportedOnce = false;
};
CopBodyState sBodies[kMaxCopUnits];

// the player's own vehicle damage ledger (Pre keeps it) - it belongs to
// the player, not to any one body, so it stands apart from the bodies
CVehicle* sPlayerVeh = nullptr;
float sPlayerVehHealth = 0.0f;
float sPlayerImpactPeak = 0.0f;

// the cop force's OWN shot clock (fix round 5): seconds since the
// player's selected weapon was in its firing state, kept by CopBodyPre
// every frame. The report's shot attribution used to read the BOSS
// relay's clock (BridgePlayerShotAge) - a byproduct of BossBodyDuties,
// which runs only while a soldier's body exists. In a cop-only session
// that clock never ticks off its 99 s rest value, so every bullet the
// player put into a cop car aged out as "the world's" damage: the book
// billed, the felony heard nothing, and the page printed "position
// lost" over a car being shot to death at point blank (the 2026-11-08
// session's conviction; the earlier session climbed because the man
// was IN a car ramming, and the ram channel never needed the clock).
// The force keeps its own clock now, read off the player's weapon the
// same way - two lines, no borrowed byproducts.
float sCopShotAge = 99.0f;

// ---- the /coptest decoys (the complete update): plain host bodies,
// owned by no brain - a civilian car with an ordinary contained driver
// for the game's own autopilot to drive, and a plain man standing where
// he was put. The same pool discipline as the cop bodies: a stored
// pointer is touched only after the pool confirms the slot still holds
// it, resolved through refs and nothing else.
struct DecoyCarState {
    bool used = false;
    CVehicle* veh = nullptr;
    int vehRef = -1;
    CPed* driver = nullptr;
    int driverRef = -1;
};
DecoyCarState sDecoyCars[2];
struct DecoyPedState {
    bool used = false;
    CPed* ped = nullptr;
    int pedRef = -1;
};
DecoyPedState sDecoyPeds[2];
constexpr int kDecoyCarModel = 445; // an Admiral: an ordinary sedan
constexpr int kDecoyManModel = 7;   // an ordinary man

DecoyCarState* FindDecoyCar(int vehRef) {
    if (vehRef < 0)
        return nullptr;
    for (int i = 0; i < 2; ++i)
        if (sDecoyCars[i].used && sDecoyCars[i].vehRef == vehRef)
            return &sDecoyCars[i];
    return nullptr;
}
DecoyPedState* FindDecoyPed(int pedRef) {
    if (pedRef < 0)
        return nullptr;
    for (int i = 0; i < 2; ++i)
        if (sDecoyPeds[i].used && sDecoyPeds[i].pedRef == pedRef)
            return &sDecoyPeds[i];
    return nullptr;
}
CVehicle* LiveDecoyVeh(const DecoyCarState& d) {
    if (d.vehRef < 0 || !CPools::ms_pVehiclePool)
        return nullptr;
    CVehicle* v = CPools::ms_pVehiclePool->GetAtRef(d.vehRef);
    return v == d.veh ? v : nullptr;
}
CPed* LiveDecoyDriver(const DecoyCarState& d) {
    if (d.driverRef < 0 || !CPools::ms_pPedPool)
        return nullptr;
    CPed* p = CPools::ms_pPedPool->GetAtRef(d.driverRef);
    return p == d.driver ? p : nullptr;
}
CPed* LiveDecoyPed(const DecoyPedState& d) {
    if (d.pedRef < 0 || !CPools::ms_pPedPool)
        return nullptr;
    CPed* p = CPools::ms_pPedPool->GetAtRef(d.pedRef);
    return p == d.ped ? p : nullptr;
}

CopBodyState* FindBody(int vehRef) {
    if (vehRef < 0)
        return nullptr;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (sBodies[i].used && sBodies[i].vehRef == vehRef)
            return &sBodies[i];
    return nullptr;
}

// Pool-ref discipline, the boss body's own law: a stored pointer is only
// ever touched after the pool itself confirms the slot still holds it.
CVehicle* LiveVeh(const CopBodyState& b) {
    if (b.vehRef < 0 || !CPools::ms_pVehiclePool)
        return nullptr;
    CVehicle* v = CPools::ms_pVehiclePool->GetAtRef(b.vehRef);
    return v == b.veh ? v : nullptr;
}
CPed* LiveDriver(const CopBodyState& b) {
    if (b.driverRef < 0 || !CPools::ms_pPedPool)
        return nullptr;
    CPed* p = CPools::ms_pPedPool->GetAtRef(b.driverRef);
    return p == b.driver ? p : nullptr;
}

// The display band, with blast headroom (fix round 3): the input is
// the core's shown fraction (0.38 at a spent book .. 1.0 at a full
// one; 0 is the wreck order). The floor is unchanged - 380 host
// points, never the host's burn band while his book lives - but the
// ceiling is no longer the host's own 1000: a point-blank blast
// strips ~1000 host points inside one frame, and at a 1000 ceiling
// the host would explode the body in the very damage stack that
// dealt it, before any re-assert could run, while his book still
// lived. The band now runs to 2500, so no single frame can zero the
// body out from under the book; billing reads deltas against the
// last set value in the same points, so no price changes. Named
// cost: the host's own smoke stages key off its thresholds and sit
// higher on the book now - with his own body drawn over the host,
// the dent blend is his damage display, not the host's smoke.
float DisplayHealth(float shown) {
    if (shown <= 0.0f)
        return 90.0f;
    const float frac = Clamp((shown - 0.38f) / 0.62f, 0.0f, 1.0f);
    return 380.0f + 2120.0f * frac;
}

// piece 3: silence one body's two loops (siren = slot*2, engine =
// slot*2+1, the ids CopBodyPost drives)
void StopBodyLoops(int slot) {
    static const Vec3 kNowhere;
    SetPosLoop(slot * 2, SND_COP_SIREN, false, kNowhere, 0.0f, 1.0f, 1.0f);
    SetPosLoop(slot * 2 + 1, SND_COP_ENGINE_IDLE, false, kNowhere, 0.0f, 1.0f, 1.0f);
}

void ApplyCommand(CVehicle* veh, CopBodyState& b) {
    // The control surface, verified against the SDK headers in-tree:
    // m_fGasPedal 0x49C, m_fBreakPedal right behind it, m_fSteerAngle
    // 0x494 (CVehicle.h VALIDATE_OFFSETs), the siren as the vehicle's own
    // flag (bSirenOrAlarm) that the game's own ProcessSirenAndHorn reads.
    veh->m_fGasPedal = b.gas;
    veh->m_fBreakPedal = b.brake;
    veh->m_fSteerAngle = b.steer;
    veh->bSirenOrAlarm = b.siren;
    // THE COMPLETE UPDATE removes a write this function used to make:
    // veh->bIsLawEnforcer = b.siren. The header's own words for that
    // flag (CVehicle.h:150) are "is this guy chasing the player at the
    // moment" - it is the marker by which the game's OWN police
    // behaviours (its block-player autopilot family among them) claim
    // a car. Asserting it on a siren unit handed that unit's autopilot
    // to the game's block-player logic: the 02:00 session's bodies read
    // missions 4/5/6 (BLOCKPLAYER_FARAWAY / _CLOSE / _HANDBRAKESTOP,
    // eCarMission.h) - values this bridge never writes - on siren
    // units only, never on stood-down ones, and an IDLE unit with its
    // siren latched kept driving a block mission at the player on its
    // own with the core's book saying it stood down. The siren flag
    // above (sound and light) is untouched; the law-enforcer flag is
    // simply never ours to assert again.
    if (b.seated) {
        veh->bEngineOn = true;
        // the parked car's other switch (fix round 4): a car the game
        // counts as parked sits on its handbrake (CVehicle.h:155). The
        // engine flag beside it has been written since piece 0 and the
        // seated, gassed, motionless sessions proved it is not the
        // missing link on its own - the handbrake is cleared with it
        // so no parked-car switch is left set under a driving order.
        veh->bIsHandbrakeOn = false;
        // THE AUTOPILOT MIRROR (fix round 4) - the round's conviction,
        // in short: on this host the pedal fields above are not an
        // input an AI-driven car is steered by. The SDK headers name
        // exactly two producers of a car's controls: ProcessControl-
        // Inputs(playerNum) - the player's pad path (CVehicle.h:341) -
        // and CAutomobile::ProcessAI (CAutomobile.h:141), whose input
        // is the vehicle's own CAutoPilot: mission, destination,
        // cruise (CAutoPilot.h), written in the game's own design by a
        // car-drive task (TASK_COMPLEX_CAR_DRIVE exists in eTaskType.h;
        // the class itself is not in the SDK - the same wall the boss's
        // hybrid met for the go-to task). Our driver is a contained
        // ped whose only task ever was the seat warp, so his car's
        // autopilot has sat at its CREATE_CAR rest state (MISSION_NONE)
        // since birth - and every value read back outside the game's
        // processing window showed our writes, while inside it the car
        // was a parked car: engine housekeeping, handbrake, no drive.
        // The mirror therefore puts the brain's own intent where this
        // host reads intent from: a straight-line goto at the brain's
        // current aim point, at the brain's desired speed, refreshed
        // only when the order materially changes (a fresh destination
        // every frame would churn the autopilot's own route state).
        // The pedals above keep being written exactly as before - if
        // the automobile honours them after all, nothing is lost; if
        // it honours only its autopilot, the brain's order is finally
        // on the channel that moves the car. A stop, a reverse or a
        // stand-down (no throttle, or a crawl's cruise) disarms the
        // mission back to NONE, so the mirror never drives a unit the
        // brain has told to stand.
        CAutoPilot& ap = veh->m_autoPilot;
        // THE COMPLETE UPDATE's arming law: the feed alone arms the
        // mirror - a named feed with a cruise above the crawl and a
        // living book. Round 5's extra gate (the pedal gas > 0.05) is
        // REMOVED on round 4's own conviction: the pedals are OUTPUTS
        // ProcessAI recomputes, and they read ~0 exactly while the
        // autopilot eases off in a close chase - so the old contract
        // disarmed live pursuits mid-chase, silently, at the very
        // ranges the ram lives at (the 02:00 session: units in pursuit
        // at felony 4096, speed 0.0, body mission 0 = NONE, frozen for
        // tens of seconds while the core believed it was feeding). The
        // core's half of the contract is its new feed law: a live feed
        // always carries a positive cruise, so this gate can rest on
        // the feed alone.
        const bool wantArm = b.apFeed && b.apCruise > 1.0f && b.book01 > 0.0f;
        if (wantArm) {
            const float dx = b.apTarget.x - b.apWritten.x;
            const float dy = b.apTarget.y - b.apWritten.y;
            // the refresh discipline (fix round 5): a destination is
            // re-written only when it truly changed (3 m of ground, or
            // a new pace) AND the cadence allows it (~0.3 s since the
            // last write) - except the arm itself, which lands at
            // once. Round 4's 2 m gate re-wrote the destination every
            // few frames of a pursuit; every write is a new destination
            // for ProcessAI's route bookkeeping in the same struct,
            // and a car whose route is replanned faster than it can
            // drive never settles into its pace.
            // THE COMPLETE UPDATE adds the RESCUE on top of it: the
            // mission is read back LIVE, and when it is not one of ours
            // (ours: GOTOCOORDS, the routing form ProcessAI itself
            // settles our STRAIGHT order into, or GOTOCOORDS_STRAIGHT)
            // the order is re-written IMMEDIATELY - no material gate,
            // no cadence. A dropped mission (NONE) or a foreign one
            // (the block-player family the game's own behaviours write
            // into flagged cars) under a live feed is not a refresh
            // detail; it is the unit being stolen mid-pursuit, and the
            // 02:00 session's frozen units sat exactly there: material
            // gates don't move for a car standing still, so a static
            // target never re-armed them. Each rescue is logged (the
            // write never is throttled; its voice is, at 1/s per body).
            const int liveMission = (int)ap.m_nCarMission;
            const bool ours = liveMission == MISSION_GOTOCOORDS ||
                              liveMission == MISSION_GOTOCOORDS_STRAIGHT;
            const bool dropped = !ours;
            const bool needWrite = !b.apArmed || dropped ||
                ((dx * dx + dy * dy > 9.0f ||
                  std::fabs(b.apCruise - b.apWrittenCruise) > 1.0f) &&
                 b.apWriteAge >= 0.3f);
            if (needWrite) {
                const bool rescue = dropped && (b.apArmed || liveMission != MISSION_NONE);
                ap.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
                // the driving style (fix round 5): a CREATE_CAR
                // autopilot rests at DRIVINGSTYLE_STOP_FOR_CARS - it
                // brakes to a stop for cars and peds in its corridor.
                // For this force the obstacle in the corridor is
                // usually the TARGET: the man himself, standing in
                // front of the spike car shooting it, or the car he
                // flees in. The game's own header names the style a
                // chase runs on - PLOUGH_THROUGH - and the original's
                // cop earns the name (his batter geometry rams the
                // flank; traffic niceties are not in his book). The
                // style is written with the mission, on the arm.
                ap.m_nCarDrivingStyle = DRIVINGSTYLE_PLOUGH_THROUGH;
                ap.m_vecDestinationCoors =
                    CVector(b.apTarget.x, b.apTarget.y, b.apTarget.z);
                // the cruise char and the traffic-speed cap are the
                // autopilot's own pace words; the brain's m/s is the
                // closest honest reading of both (traffic cruise
                // settings live in the same range) - the read-back in
                // the Report prints what landed, and the spike's speed
                // lines are the pace's judge.
                // THE 5x UPDATE: the char is a NARROW field - at the
                // new pace rows the brain's ask (up to 266) cannot
                // fit it, so it saturates at 127, the most a signed
                // char holds (the safe reading under either signedness
                // the SDK header allows), while the float beside it
                // carries the true ask whole. Neither is dressed as
                // the other: the Report reads both back and the unit
                // line prints the pair, so what ProcessAI was actually
                // handed is measured every second, not assumed.
                ap.m_nCruiseSpeed = (char)Clamp(b.apCruise, 0.0f, 127.0f);
                ap.m_fMaxTrafficSpeed = b.apCruise;
                b.apWritten = b.apTarget;
                b.apWrittenCruise = b.apCruise;
                b.apWriteAge = 0.0f;
                b.apArmed = true;
                if (rescue && b.apRescueLogT >= 1.0f) {
                    b.apRescueLogT = 0.0f;
                    Log("cop mirror: unit %d re-armed - the autopilot read mission %d under a live feed "
                        "(dropped or foreign order); the brain's goto is written again",
                        b.vehRef, liveMission);
                }
            }
        } else if (b.apArmed) {
            ap.m_nCarMission = MISSION_NONE;
            b.apArmed = false;
            b.apWrittenCruise = -1.0f;
        }
    }
    // the displayed health is re-asserted with the pedals: the host's own
    // damage may have bitten into it this frame, and the core's Report
    // (which runs before Post) is where that bite was measured and priced
    if (b.book01 > 0.0f)
        veh->m_fHealth = DisplayHealth(b.book01);
}

} // namespace

void CopSessionReset() {
    // the world is gone (or going): forget every body and the ledger
    StopPosLoops();
    for (int i = 0; i < kMaxCopUnits; ++i)
        sBodies[i] = CopBodyState();
    for (int i = 0; i < 2; ++i) {
        sDecoyCars[i] = DecoyCarState();
        sDecoyPeds[i] = DecoyPedState();
    }
    sPlayerVeh = nullptr;
    sPlayerVehHealth = 0.0f;
    sPlayerImpactPeak = 0.0f;
    sCopShotAge = 99.0f;
}

// the drawn wheel's radius: CARS.C's rewrite scales the ring by
// wheelSize * 14142 / 10000 = 69 of his units, at the convicted
// geometry factor of 153.6 units = 1 m (fix round 3)
constexpr float kCopWheelRadiusM = 69.0f / 153.6f;
// DrawCarWheels swaps CLEANWHEEL for FASTWHEEL when the wheel's angular
// increment passes 400 of 4096 per 1/50 s tick: 400/4096 of a turn per
// tick is ~30.7 rad/s, which at his wheel radius is ~13.8 m/s of road
constexpr float kCopFastWheelMps = 13.8f;
// his drawn body's lift over the host car's origin. The drawn side
// of the subtraction is exact from the baked bin (driver2-assets'
// measure): wheel centres at z = -0.4167 m in his frame, wheel radius
// 0.4492 m (diameter 0.8984) - his contact patch sits 0.866 m under
// his origin, and the wheel spin pivots on the mesh's own axle, so no
// part of a float can hide in the spin groups. The host side is the
// 596's origin height over ITS patch at rest, and fix round 5
// corrects round 3's number: the 0.53 m it used was asserted in a
// comment with no measurement behind it anywhere in the record. The
// measurement the record does hold is the spawn/settle behaviour -
// GtaSpawnCopBody places the car at FindGroundZ + 0.6, and across the
// eye sessions every settled report reads the car at the height it
// was placed (23.7 at the Grove spawn, at the log's 0.1 m print
// resolution): a body whose rest height were lower settles DOWN from
// its placed height within a frame or two, and no report ever shows
// the step. The host origin therefore rides 0.60 m over its patch,
// with the print resolution as the honest tolerance (+-0.05 m). The
// lift is 0.866 - 0.60 = 0.27 m - round 3 overshot by ~7 cm, which
// is the "floats a little" the user's eye reported. The eye at a stop
// on flat street owns the last centimetres; this is the constant it
// would tune.
constexpr float kCopBodyLiftM = 0.27f;

void CopBodyPre(float dt) {
    // every body's driver, every frame (the one-unit law): what he lost
    // since the last frame is measured for his unit's book, and his
    // health is held back up - he goes down only when his unit retires
    for (int i = 0; i < kMaxCopUnits; ++i) {
        CopBodyState& b = sBodies[i];
        if (!b.used)
            continue;
        b.apWriteAge += dt; // the mirror's refresh cadence clock
            // (fix round 5), read in ApplyCommand
        b.apRescueLogT += dt; // the rescue log's throttle clock (the
            // complete update), read in ApplyCommand
        // his drawn wheels turn with his own travel (piece 3): signed
        // speed along the nose off the vehicle itself, spun at his
        // wheel radius - forward travel decreases the angle, the
        // convention DrawCopCar documents
        if (CVehicle* veh = LiveVeh(b)) {
            const float fwdSpeed = (veh->m_vecMoveSpeed.x * veh->m_matrix->up.x +
                                    veh->m_vecMoveSpeed.y * veh->m_matrix->up.y +
                                    veh->m_vecMoveSpeed.z * veh->m_matrix->up.z) * 50.0f;
            b.speedMps = Vec3(veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y,
                              veh->m_vecMoveSpeed.z).Length() * 50.0f;
            b.wheelSpin -= fwdSpeed * dt / kCopWheelRadiusM;
        }
        if (CPed* driver = LiveDriver(b)) {
            if (driver->m_fHealth < 1000.0f)
                b.driverHitAcc += 1000.0f - driver->m_fHealth;
            driver->m_fHealth = 1000.0f;
        }
        // seating: the seat warp is the game's own task (fix round 3 -
        // the animated enter task never completed once in six units;
        // the conviction and the trade are in DRIVER2-STEP3-fa.md).
        // Watch the warp land by the round-trip the spike verdict
        // reads: his ped in the car, the car's driver him.
        CVehicle* veh = LiveVeh(b);
        CPed* driver = LiveDriver(b);
        if (veh && driver && !b.seated) {
            b.seatT += dt;
            if (driver->m_pVehicle == veh && veh->m_pDriver == driver) {
                b.seated = true;
                Log("cop body: a driver is seated - the pedals are the core's from here");
                // a unit seating while another already pursues is a
                // reinforcement joining a live chase - COP_AI.C's own
                // join callouts are phrases 10/11 (rnd % 2 + 10), spoken
                // from the joining car
                for (int j = 0; j < kMaxCopUnits; ++j) {
                    if (j != i && sBodies[j].used && sBodies[j].siren) {
                        const Vec3 at(veh->m_matrix->pos.x, veh->m_matrix->pos.y, veh->m_matrix->pos.z);
                        PlaySfx((rand() % 2) ? SND_COP_RADIO_10 : SND_COP_RADIO_11, &at, 1.0f, 1.0f);
                        break;
                    }
                }
            } else if (b.seatT > 20.0f) {
                // said out loud, in the log the spike verdict is read from:
                // if this line ever prints, field control has no driver.
                // Round 3 adds the discipline the first night lacked:
                // a body that never seats does not stay in service
                // driverless - it is taken out whole (the core sees
                // the report fail and stands the unit down), never
                // left standing in the street as a parked shell.
                Log("cop body: A DRIVER NEVER SAT (20 s) - field control has no driver to work through");
                const int ref = b.vehRef;
                GtaCopBodyRetire(ref, false);
                Log("cop body: the driverless body was taken out of service");
                continue;
            } else {
                // the watch names its reason once a second while it
                // waits: how far the ped stands from his door, and
                // what his primary task currently is - a stall after
                // this round reads its own cause off the log
                b.seatLogT += dt;
                if (b.seatT > 8.0f && !b.seatRetried) {
                    b.seatRetried = true;
                    Command<Commands::TASK_WARP_CHAR_INTO_CAR_AS_DRIVER>(
                        CPools::GetPedRef(driver), CPools::GetVehicleRef(veh));
                    Log("cop body: a driver had not sat in 8 s - the seat warp was issued once more");
                }
                if (b.seatLogT >= 1.0f) {
                    b.seatLogT = 0.0f;
                    const CMatrix& cm = *veh->m_matrix;
                    const CVector doorP = cm.pos + cm.right * -0.9f + cm.up * 0.6f;
                    const CVector dp = driver->GetPosition();
                    const float dx = dp.x - doorP.x, dy = dp.y - doorP.y, dz = dp.z - doorP.z;
                    CTask* task = nullptr;
                    if (driver->m_pIntelligence)
                        task = driver->m_pIntelligence->m_TaskMgr.m_aPrimaryTasks[TASK_PRIMARY_PRIMARY];
                    Log("cop body: seat watch - driver %.1f m from the door of car %d, primary task %p (type %d)",
                        std::sqrt(dx * dx + dy * dy + dz * dz), b.vehRef,
                        (void*)task, task ? (int)task->GetId() : -1);
                }
            }
        }
    }
    // the force's own shot clock (fix round 5): the player's selected
    // weapon in its firing state means his gun spoke this frame -
    // the report's attribution reads the age off this clock, which
    // CopBodyPre keeps whether or not any other system is awake
    if (CPlayerPed* player = FindPlayerPed()) {
        CWeapon& w = player->m_aWeapons[player->m_nSelectedWepSlot];
        sCopShotAge = (w.m_nState == WEAPONSTATE_FIRING) ? 0.0f : sCopShotAge + dt;
    }
    // the player's own car, priced per frame: the felony system's ears
    // for "he hit something" are his own car's health steps, peaked so a
    // hard hit is still on the books when the core asks once per tick
    CVehicle* pv = FindPlayerVehicle();
    if (pv != sPlayerVeh) {
        sPlayerVeh = pv;
        sPlayerVehHealth = pv ? pv->m_fHealth : 0.0f;
    } else if (pv) {
        const float lost = sPlayerVehHealth - pv->m_fHealth;
        if (lost > sPlayerImpactPeak)
            sPlayerImpactPeak = lost;
        if (pv->m_fHealth > sPlayerVehHealth || lost > 0.0f)
            sPlayerVehHealth = pv->m_fHealth; // repairs re-baseline; a
                // slow grind keeps stepping down from the last reading
    }
}

void CopBodyPost() {
    for (int i = 0; i < kMaxCopUnits; ++i) {
        CopBodyState& b = sBodies[i];
        if (!b.used)
            continue;
        CVehicle* veh = LiveVeh(b);
        if (b.haveCmd && veh)
            ApplyCommand(veh, b);
        if (!veh) {
            StopBodyLoops(i); // the world took the body: no frozen siren
            continue;
        }
        // piece 3: while his whole body set is loaded, the host car and
        // its driver are not seen - his own body (CopRender, in the
        // render pass) wears the unit. Written every frame, so a file
        // set that completes late, or a body the game re-shows, heals
        // itself either way; with any file missing both stay visible
        // and the force drives on the named stand-in.
        const bool shown = CopBodyFilesReady();
        veh->bIsVisible = !shown;
        if (CPed* driver = LiveDriver(b)) {
            driver->bIsVisible = !shown;
            // Pre's measure-and-hold, mirrored after the game's frame:
            // a hit that lands between Pre and Post is measured and
            // topped up in the same frame instead of the next. The
            // measure consumes the drop it sees (health returns to
            // 1000 at each measure), so the pair never double-counts.
            if (driver->m_fHealth < 1000.0f)
                b.driverHitAcc += 1000.0f - driver->m_fHealth;
            driver->m_fHealth = 1000.0f;
        }
        // ---- piece 3: his own voice, driven off the same state the
        // pedals are. The siren is his pursuit flag (the core sets it
        // exactly while CopControl1 drives), looped positionally from
        // his car; the engine is his two layers from lump 4 - idle
        // under 1.2 m/s, revs over 2.5 m/s (a hysteresis band between,
        // so the swap cannot flap), the pitch riding speed and the
        // core's own throttle command. The original crossfades its
        // layers by revs; a threshold swap with a continuous pitch is
        // what this surface honestly holds, and it is named as such.
        const Vec3 pos(veh->m_matrix->pos.x, veh->m_matrix->pos.y, veh->m_matrix->pos.z);
        SetPosLoop(i * 2, SND_COP_SIREN, b.siren, pos, 0.9f, 1.0f, 60.0f);
        const float speed = Vec3(veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y,
                                 veh->m_vecMoveSpeed.z).Length() * 50.0f;
        if (b.engineEv != (int)SND_COP_ENGINE_REVS && speed > 2.5f)
            b.engineEv = (int)SND_COP_ENGINE_REVS;
        else if (b.engineEv != (int)SND_COP_ENGINE_IDLE && speed < 1.2f)
            b.engineEv = (int)SND_COP_ENGINE_IDLE;
        if (b.engineEv < 0)
            b.engineEv = (int)SND_COP_ENGINE_IDLE;
        const bool revs = b.engineEv == (int)SND_COP_ENGINE_REVS;
        const float pitch = revs ? 0.75f + (speed < 40.0f ? speed : 40.0f) / 40.0f * 0.8f + b.gas * 0.10f
                                 : 0.90f + b.gas * 0.25f;
        SetPosLoop(i * 2 + 1, (SoundEvent)b.engineEv, b.seated && b.book01 > 0.0f,
                   pos, 0.55f, pitch, 40.0f);
        // his radio on the pursuit's first moment: FELONY.C speaks
        // phrases 7/8 ((rnd & 1) + 7) as a chase opens
        if (b.siren && !b.sirenPrev)
            PlaySfx((rand() % 2) ? SND_COP_RADIO_07 : SND_COP_RADIO_08, &pos, 1.0f, 1.0f);
        b.sirenPrev = b.siren;
    }
}

void CopRender(float light) {
    if (!CopBodyFilesReady())
        return; // no body set: the host cars stay visible (CopBodyPost)
    Vec3 centers[4];
    CopWheelCenters(centers);
    const CVector cam = TheCamera.GetPosition();
    // the drawn wheels' yaw off the vehicle's own steer field, through
    // the spike's calibration: the core's Drive writes the field as
    // (flip ? -norm : norm) for a brain whose +norm is a right turn, and
    // DrawCopCar's +steer is a yaw toward his left - so the raw field
    // passes unchanged when the session flipped, and negated when it
    // did not. Until the calibration lands (the first summon's first
    // second) the unflipped reading is the standing guess.
    const float steerSign = gCops[0].steerFlip ? 1.0f : -1.0f;
    for (int i = 0; i < kMaxCopUnits; ++i) {
        CopBodyState& b = sBodies[i];
        if (!b.used)
            continue;
        CVehicle* veh = LiveVeh(b);
        if (!veh)
            continue;
        const CMatrix& m = *veh->m_matrix;
        CVector d = m.pos - cam;
        if (d.x * d.x + d.y * d.y + d.z * d.z > 120.0f * 120.0f)
            continue; // MobsRender's own distance courtesy
        if (!TheCamera.IsSphereVisible(m.pos, 2.7f))
            continue;
        // the pose is the vehicle's own matrix, read in the core's car
        // frame: the matrix's "up" slot is the car's forward, its "at"
        // slot is the car's up (the spawn's own basis write proves the
        // layout). His origin rides kCopBodyLiftM over the vehicle's
        // origin along the car's own up, so his contact patch meets
        // the host's ground line (the constant's derivation stands at
        // its definition); the eye test owns the last centimetres and
        // this is the constant it would tune.
        Pose pose;
        pose.X = Vec3(m.right.x, m.right.y, m.right.z);
        pose.Y = Vec3(m.up.x, m.up.y, m.up.z);
        pose.Z = Vec3(m.at.x, m.at.y, m.at.z);
        pose.o = Vec3(m.pos.x, m.pos.y, m.pos.z) + pose.Z * kCopBodyLiftM;
        DrawCopCar(pose, 1.0f - b.book01, centers, steerSign * veh->m_fSteerAngle,
                   b.wheelSpin, b.speedMps > kCopFastWheelMps, light, 1.0f, 1.0f, 1.0f);
        // his shadow: hiding the 596 takes the game's own blob with it,
        // so ours stands in (the mob shadow's own mechanism), sized to
        // the true body now - a 4.9 m car's blob, not the 0.6x one's
        bool found = false;
        const float gz = CWorld::FindGroundZFor3DCoord(m.pos.x, m.pos.y, m.pos.z + 2.0f, &found, nullptr);
        if (found)
            AddShadow(CVector(m.pos.x, m.pos.y, gz), 2.5f, 0.55f);
    }
}

int GtaSpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face) {
    (void)headingDeg; // the face point decides the nose, like the original's
                      // justPinged: a fresh cop starts facing his man
    // step 2: a spawn takes a FREE body slot - the other units' bodies
    // are not this spawn's to forget (piece 1's entry reset is gone)
    CopBodyState* slot = nullptr;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (!sBodies[i].used) {
            slot = &sBodies[i];
            break;
        }
    if (!slot) {
        Log("cop body: no free body slot - spawn refused");
        return -1;
    }
    CopBodyState& s = *slot;
    s = CopBodyState();
    s.used = true;
    Command<Commands::REQUEST_MODEL>(kCopCarModel);
    Command<Commands::REQUEST_MODEL>(kCopDriverModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(kCopCarModel) ||
        !Command<Commands::HAS_MODEL_LOADED>(kCopDriverModel)) {
        s = CopBodyState();
        return -1;
    }
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    Log("cop spawn: step 1 - models in, creating car %d at (%.0f, %.0f)", kCopCarModel, at.x, at.y);
    int handle = 0;
    Command<Commands::CREATE_CAR>(kCopCarModel, at.x, at.y, gz + 0.6f, &handle);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kCopCarModel);
    Log("cop spawn: step 2 - CREATE_CAR returned handle %d", handle);
    // the handle is a pool REF, not a slot index - the second-summon
    // crash of 2026-11-07 was born here. CREATE_CAR is the game's own
    // script opcode and hands back what CPool::GetRef builds:
    // (slot << 8) + the slot's generation byte (CPool.h, in-tree).
    // Round 1 fed that number to GetAt, which expects a raw index and
    // checks NOTHING: it read the generation map out of bounds and
    // returned the pool base plus handle car-sized strides - a wild
    // pointer into unrelated memory. Whatever sat at +0x14 there was
    // the crash lottery: a clean zero the first night (the guard read
    // it as "no matrix yet"), junk the second (the guard passed and
    // the basis write stored through junk). The validated translation
    // is the pool's own GetAtRef - the same verb every stored body in
    // this file is resolved by (LiveVeh above) - and the identity
    // round-trips below prove the pointer is the car the opcode made:
    // its slot holds it, the slot is not free, and its own GetRef
    // reproduces the handle byte for byte. Anything less refuses.
    auto* vpool = CPools::ms_pVehiclePool;
    CVehicle* veh = (vpool && handle > 0) ? vpool->GetAtRef(handle) : nullptr;
    if (!veh) {
        Log("cop body: CREATE_CAR handle %d resolves to no vehicle - spawn refused", handle);
        s = CopBodyState();
        return -1;
    }
    const int vidx = vpool->GetIndex(veh);
    if (vidx < 0 || vidx >= vpool->m_nSize || vpool->IsFreeSlotAtIndex(vidx) ||
        vpool->GetRef(veh) != handle) {
        Log("cop body: handle %d resolved but the pool identity does not round-trip (slot %d) - spawn refused",
            handle, vidx);
        s = CopBodyState();
        return -1;
    }
    Log("cop spawn: step 3 - handle %d is the car at pool slot %d, identity round-trips", handle, vidx);
    // turn his nose to the man
    Vec3 d = face - at;
    d.z = 0.0f;
    const float dl = d.Length();
    Vec3 fwd = dl > 0.01f ? d * (1.0f / dl) : Vec3(0, 1, 0);
    // the matrix is not this code's to assume yet: CPlaceable::m_matrix
    // (the pointer at +0x14, per the SDK header in-tree) is allocated
    // lazily by the game, and a car fresh from CREATE_CAR still lives on
    // its simple transform - the SDK's own GetPosition falls back to
    // m_placement for exactly that state. Writing the basis through the
    // pointer as it stood was the first-summon crash of 2026-11-07
    // (fault at the [veh+0x14] read, this very block - the first time
    // this spawn ever ran in the game). Round 1's law stands - allocate
    // it the game's own way, and if even that leaves none, refuse the
    // spawn like the pool-landing refusal further down: every later
    // touch of this body (Pre's wheel spin, the Report, Post's sound
    // position, CopRender) reads the same pointer, so a body without a
    // matrix cannot enter service. What round 2 added is that the law
    // now runs on the car's OWN pointer, proven above - on the wild
    // pointer the old GetAt produced, this very block was reading and
    // writing somebody else's memory, and no guard here could see it.
    Log("cop spawn: step 4 - matrix %s on the fresh car", veh->m_matrix ? "present" : "absent");
    if (!veh->m_matrix)
        veh->AllocateMatrix();
    if (!veh->m_matrix) {
        Log("cop body: the new car has no matrix and AllocateMatrix gave none - spawn refused");
        s = CopBodyState();
        return -1;
    }
    Log("cop spawn: step 4 - matrix ready");
    CMatrix& m = *veh->m_matrix;
    m.right = CVector(fwd.y, -fwd.x, 0.0f);
    m.up = CVector(fwd.x, fwd.y, 0.0f);
    m.at = CVector(0.0f, 0.0f, 1.0f);
    m.pos = CVector(at.x, at.y, gz + 0.6f);
    veh->UpdateRwMatrix();
    // the write must prove itself before the body is anyone's: read the
    // position back through the same pointer and compare - a spawn that
    // cannot show its own nose on the ground it chose does not enter
    // service (the retire path's own teardown verbs take the car back).
    const CVector& back = veh->m_matrix->pos;
    if (std::fabs(back.x - at.x) > 0.01f || std::fabs(back.y - at.y) > 0.01f ||
        std::fabs(back.z - (gz + 0.6f)) > 0.01f) {
        Log("cop body: the basis write does not read back (got %.2f, %.2f, %.2f) - spawn refused",
            back.x, back.y, back.z);
        CWorld::Remove(veh);
        delete veh;
        s = CopBodyState();
        return -1;
    }
    Log("cop spawn: step 5 - nose written toward the man, position read back at (%.0f, %.0f)", back.x, back.y);
    s.veh = veh;
    s.vehRef = vpool->GetRef(veh);
    // and the identity the whole force will steer by: the ref the core
    // is about to be handed must resolve back to this very car through
    // the pool discipline every later touch uses (LiveVeh)
    if (LiveVeh(s) != veh) {
        Log("cop body: the stored ref does not resolve back to the new car - spawn refused");
        CWorld::Remove(veh);
        delete veh;
        s = CopBodyState();
        return -1;
    }
    s.book01 = 1.0f;
    s.lastSetHealth = veh->m_fHealth;

    // the driver: the game's own cop kind, pool-built and contained from
    // birth - the boss body's discipline, word for word (PED_EMPTY, senses
    // zeroed, mission-created so population never takes him)
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool) {
        CWorld::Remove(veh);
        delete veh;
        s = CopBodyState();
        return -1;
    }
    CCopPed* cop = new CCopPed(COP_TYPE_CITYCOP);
    CPed* driver = cop;
    if (!driver) {
        CWorld::Remove(veh);
        delete veh;
        s = CopBodyState();
        return -1;
    }
    driver->SetModelIndex(kCopDriverModel);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kCopDriverModel);
    driver->SetCharCreatedBy(kCreatedByMission);
    driver->ClearWeapons();
    CWorld::Add(driver);
    const int idx = ppool->GetIndex(driver);
    if (idx < 0 || idx >= ppool->m_nSize || ppool->GetAt(idx) != driver || ppool->IsFreeSlotAtIndex(idx)) {
        CWorld::Remove(driver);
        delete driver;
        CWorld::Remove(veh);
        delete veh;
        Log("cop body: the driver did not land in the ped pool (index %d) - spawn refused", idx);
        s = CopBodyState();
        return -1;
    }
    Log("cop spawn: step 6 - driver built and pool-landed at slot %d", idx);
    if (driver->m_pIntelligence) {
        driver->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        driver->m_pIntelligence->SetSeeingRange(0.0f);
        driver->m_pIntelligence->SetHearingRange(0.0f);
    }
    driver->m_fMaxHealth = 1000.0f; // held at the top by Pre, like the
    driver->m_fHealth = 1000.0f;    // boss's body: he is the unit's face,
                                    // not a life of his own
    // the two channels that could kill him inside a single frame -
    // before any top-up of ours can run - are proofed shut (fix round
    // 3): a blast and a fire. Both bill the book through the car's own
    // channels anyway (the blast through the shot/world delta and the
    // impact price, fire through the world drain), so nothing priced
    // is lost; what is lost is only the double-count through his body
    // and the chance of a separate ped death. Bullets and melee still
    // land on him and bill through the driver channel, headshots
    // included - the one-unit law keeps its priced residue.
    driver->bExplosionProof = true;
    driver->bFireProof = true;
    // stand him at his own door for the frame before the seat warp
    // lands (the warp does not care where he stands; the door is for
    // the eye and for the seat watch's distance reading)
    const CVector door(at.x - fwd.y * 1.3f, at.y + fwd.x * 1.3f, gz + 1.0f);
    // the same lazy-matrix law, one door down: nothing between the
    // driver's construction and here allocates his matrix either, so
    // the door placement writes through it only when it exists, and
    // otherwise goes through the SDK's null-safe SetPosn (the boss
    // body's own teleport verb) - his simple transform holds the
    // position until the game gives him a matrix of its own.
    if (driver->m_matrix) {
        driver->m_matrix->pos = door;
        driver->UpdateRwMatrix();
    } else {
        driver->SetPosn(door);
    }
    // SEATING (fix round 3) - the animated enter, convicted: piece 3
    // seated him with CTaskComplexEnterCarAsDriver, the game's own
    // walk-to-the-door task, and in the 2026-11-07 eye test six of six
    // drivers never sat - two issuances each, twenty seconds each,
    // standing a metre from the door on flat ground. Every ingredient
    // on our side of the call checks out (the header's ctor takes the
    // vehicle alone, exactly as called; the same ped construction,
    // PED_EMPTY and SetTask form run the boss body's tasks in the
    // same build; nothing in Pre fights the task; the car is a fresh
    // script car of the kind missions task chars into daily) - the
    // stall lives inside the task's own line-up phase, which has no
    // body in the SDK to inspect or repair. So the spawn takes the
    // game's OTHER own door into the seat: the script warp
    // TASK_WARP_CHAR_INTO_CAR_AS_DRIVER, the verb missions seat chars
    // with when the seat must simply happen, on the same script
    // surface this codebase already works (WARP_CHAR_FROM_CAR_TO_COORD
    // in GtaMining.cpp). THE TRADE, stated plainly: the walk-to-the-
    // door animation is given up for certainty - he is simply IN the
    // seat when the warp's task lands, a frame or two after this call.
    // (He is invisible under his drawn body anyway; nobody loses a
    // show.) The seating is then PROVEN by Pre's watch - the m_pDriver
    // / m_pVehicle round-trip - and a body that still has not seated
    // in 20 s is taken out of service whole, never left driverless.
    Command<Commands::TASK_WARP_CHAR_INTO_CAR_AS_DRIVER>(CPools::GetPedRef(driver),
                                                         CPools::GetVehicleRef(veh));
    s.driver = driver;
    s.driverRef = ppool->GetRef(driver);
    PedSkinsExcludePed(driver);
    Log("cop spawn: step 7 - seat warp issued; the seating itself is watched in Pre");
    Log("cop body: car %d and driver up at (%.0f, %.0f)", s.vehRef, at.x, at.y);
    return s.vehRef;
}

bool GtaCopBodyReport(int veh, Host::CopBody* out) {
    CopBodyState* bp = FindBody(veh);
    if (!bp || !out)
        return false;
    CopBodyState& s = *bp;
    CVehicle* v = LiveVeh(s);
    if (!v)
        return false;
    // note: no driver-death check - there is none. The driver is held by
    // Pre and goes down only with the unit (the one-unit law).
    out->ok = true;
    out->pos = Vec3(v->m_matrix->pos.x, v->m_matrix->pos.y, v->m_matrix->pos.z);
    out->fwd = Vec3(v->m_matrix->up.x, v->m_matrix->up.y, v->m_matrix->up.z);
    out->vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
    out->health = v->m_fHealth / 2500.0f; // the DISPLAY's fraction of
                                          // its band (380..2500 since
                                          // fix round 3); the book
                                          // itself lives in the core
    // READ BACK from the vehicle itself - never the stored command. If the
    // game's own processing overwrote a pedal between our two asserts,
    // this is the number that says so.
    out->gas = v->m_fGasPedal;
    out->brake = v->m_fBreakPedal;
    out->steer = v->m_fSteerAngle;
    out->siren = v->bSirenOrAlarm;
    // the drive state, read back the same way (fix round 4): the
    // switches on the automobile's road to the wheels, so a body that
    // answers the pedals but never moves names its blocker in the log
    // instead of leaving the next round to guess. The mission and
    // cruise are the autopilot's own words as they stand NOW - what
    // the mirror wrote, or the rest state it never left.
    out->engineOn = v->bEngineOn;
    out->handbrake = v->bIsHandbrakeOn;
    out->engineBroken = v->bEngineBroken;
    out->status = (int)v->m_nStatus;
    out->mission = (int)v->m_autoPilot.m_nCarMission;
    out->cruise = (int)v->m_autoPilot.m_nCruiseSpeed;
    out->cruiseF = v->m_autoPilot.m_fMaxTrafficSpeed; // the float's
        // landing, read back beside the char's (the 5x update: the
        // pair is the clipping's receipt)
    if (CPed* drv = LiveDriver(s))
        out->driverInVehicle = drv->bInVehicle;
    // the impact channel: the car's own velocity change since the last
    // report - the collision signature the core prices his book by (his
    // own acceleration is ~0.15 m/s a frame; a strike stands out whole)
    if (s.prevVelOk) {
        const float dv = (out->vel - s.prevVel).Length();
        if (dv > s.impactPeak)
            s.impactPeak = dv;
    }
    s.prevVel = out->vel;
    s.prevVelOk = true;
    out->impactDv = s.impactPeak;
    s.impactPeak = 0.0f;
    // the health the display lost since it was last set, split by how it
    // arrived: his car close (his own vehicle within 6 m) prices a ram
    // for the felony; the player's gun fresh (< 1 s on the force's OWN
    // shot clock, fix round 5 - it used to read the boss relay's clock,
    // which sleeps at 99 s whenever no soldier stands in the world, and
    // silently re-filed every on-foot shooting as world damage) prices
    // a shot; the residue is the world's (walls, fire)
    const float lost = s.book01 > 0.0f ? s.lastSetHealth - v->m_fHealth : 0.0f;
    if (lost > 0.0f) {
        CPlayerPed* player = FindPlayerPed();
        CVehicle* pv = FindPlayerVehicle();
        bool ram = false;
        if (player && pv) {
            const float dx = pv->m_matrix->pos.x - v->m_matrix->pos.x;
            const float dy = pv->m_matrix->pos.y - v->m_matrix->pos.y;
            ram = dx * dx + dy * dy < 36.0f;
        }
        if (ram)
            s.struckRam += lost;
        else if (sCopShotAge < 1.0f)
            s.struckShot += lost;
        else
            s.struckWorld += lost;
    }
    out->struckRam = s.struckRam;
    out->struckShot = s.struckShot;
    out->struckWorld = s.struckWorld;
    out->struckDriver = s.driverHitAcc;
    s.struckRam = 0.0f;
    s.struckShot = 0.0f;
    s.struckWorld = 0.0f;
    s.driverHitAcc = 0.0f;
    if (!s.reportedOnce) {
        s.reportedOnce = true;
        Log("cop spawn: first report for car %d read back gas %.2f brake %.2f steer %.2f"
            " | engine %d handbrake %d broken %d status %d mission %d cruise %d driver-in %d - the body answers",
            s.vehRef, out->gas, out->brake, out->steer,
            out->engineOn ? 1 : 0, out->handbrake ? 1 : 0, out->engineBroken ? 1 : 0,
            out->status, out->mission, out->cruise, out->driverInVehicle ? 1 : 0);
    }
    return true;
}

void GtaCopBodySetHealth(int veh, float book01) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    bp->book01 = book01;
    if (CVehicle* v = LiveVeh(*bp)) {
        v->m_fHealth = DisplayHealth(book01);
        bp->lastSetHealth = v->m_fHealth;
    }
}

void GtaCopBodyDrive(int veh, float gas, float brake, float steer, bool siren) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    bp->gas = gas;
    bp->brake = brake;
    bp->steer = steer;
    bp->siren = siren;
    bp->haveCmd = true;
    if (CVehicle* v = LiveVeh(*bp))
        ApplyCommand(v, *bp); // now; Post asserts it again after the game
}

void GtaCopBodyDriveTarget(int veh, const Vec3& at, float cruiseMps) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    // only the feed is stored here: the mirror itself is written in
    // ApplyCommand, beside the pedals, so the Drive-time write and
    // Post's re-assert carry the autopilot order in the same pass
    bp->apFeed = true;
    bp->apTarget = at;
    bp->apCruise = cruiseMps;
}

void GtaCopBodyRetire(int veh, bool burnOut) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    StopBodyLoops((int)(bp - sBodies)); // his voice goes with the body
    CopBodyState& s = *bp;
    CVehicle* v = LiveVeh(s);
    CPed* driver = LiveDriver(s);
    if (burnOut) {
        // his book is spent: the unit retires AS A CAR (fix round 3).
        // The old path zeroed the driver's health - he died a ped's
        // death in his seat: blood out of the car and a corpse for the
        // city, while the eye expects a machine's end. Now the driver
        // leaves the world silently (removed, never killed - no death,
        // no blood, no body) and the car alone is handed to the fire
        // at burn-band health: the host's own fire takes it, it
        // explodes as a car explodes, and the burnt wreck STAYS in the
        // world - nothing of ours deletes it; the city's own cleanup
        // owns it from here, like any burnt car on the street. What
        // the eye sees after this fix: fire, a car's explosion, a
        // blackened wreck that remains - and no blood anywhere.
        // ONLY this body is forgotten - the force drives on.
        if (driver) {
            CWorld::Remove(driver);
            delete driver;
        }
        if (v) {
            // no stale orders on a handed-over body (fix round 4): the
            // autopilot mirror is disarmed before the fire takes him,
            // so the wreck the city keeps is a wreck, not a driverless
            // car still holding a mission
            v->m_autoPilot.m_nCarMission = MISSION_NONE;
            v->m_fHealth = 90.0f;
        }
        s = CopBodyState();
        return;
    }
    if (driver) {
        CWorld::Remove(driver);
        delete driver;
    }
    if (v) {
        CWorld::Remove(v);
        delete v;
    }
    s = CopBodyState();
}

float GtaPlayerVehicleImpact() {
    const float peak = sPlayerImpactPeak;
    sPlayerImpactPeak = 0.0f;
    return peak;
}

// Test staging only (/coptest's recovery wedge): move a cop body
// outright - the same basis write the spawn proves, then the mirror's
// bookkeeping forgotten so the next feed re-arms from clean. Nothing in
// play calls this; it exists so the self-test can put a unit nose-to-
// wall exactly, instead of hoping a chase ends in a wedge.
void GtaCopBodyPlace(int veh, const Vec3& at, float headingDeg) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    CVehicle* v = LiveVeh(*bp);
    if (!v || !v->m_matrix)
        return;
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    const float h = headingDeg * 0.0174533f;
    const CVector fwd(std::sin(h), std::cos(h), 0.0f);
    CMatrix& m = *v->m_matrix;
    m.right = CVector(fwd.y, -fwd.x, 0.0f);
    m.up = CVector(fwd.x, fwd.y, 0.0f);
    m.at = CVector(0.0f, 0.0f, 1.0f);
    m.pos = CVector(at.x, at.y, gz + 0.6f);
    v->UpdateRwMatrix();
    v->m_vecMoveSpeed = CVector(0.0f, 0.0f, 0.0f);
    v->m_vecTurnSpeed = CVector(0.0f, 0.0f, 0.0f);
    if (CPed* driver = LiveDriver(*bp)) {
        if (driver->m_matrix) {
            driver->m_matrix->pos = m.pos;
            driver->UpdateRwMatrix();
        } else {
            driver->SetPosn(m.pos);
        }
    }
    bp->apArmed = false;
    bp->apFeed = false;
    bp->apWrittenCruise = -1.0f;
    bp->prevVelOk = false;
    bp->impactPeak = 0.0f;
    Log("cop body: unit %d placed by the test at (%.0f, %.0f), facing %.0f deg",
        veh, at.x, at.y, headingDeg);
}

// ---- the decoys' bodies ----------------------------------------------
int GtaSpawnDecoyCar(const Vec3& at, float headingDeg) {
    DecoyCarState* slot = nullptr;
    for (int i = 0; i < 2; ++i)
        if (!sDecoyCars[i].used) {
            slot = &sDecoyCars[i];
            break;
        }
    if (!slot)
        return -1;
    DecoyCarState& s = *slot;
    s = DecoyCarState();
    s.used = true;
    Command<Commands::REQUEST_MODEL>(kDecoyCarModel);
    Command<Commands::REQUEST_MODEL>(kDecoyManModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(kDecoyCarModel) ||
        !Command<Commands::HAS_MODEL_LOADED>(kDecoyManModel)) {
        s = DecoyCarState();
        return -1;
    }
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    int handle = 0;
    Command<Commands::CREATE_CAR>(kDecoyCarModel, at.x, at.y, gz + 0.6f, &handle);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kDecoyCarModel);
    // CREATE_CAR hands back a pool REF (the cop spawn's round-1 crash
    // lesson): resolve only through GetAtRef and prove the round-trip
    auto* vpool = CPools::ms_pVehiclePool;
    CVehicle* veh = (vpool && handle > 0) ? vpool->GetAtRef(handle) : nullptr;
    if (!veh || vpool->GetRef(veh) != handle) {
        Log("coptest decoy: CREATE_CAR handle %d does not resolve - spawn refused", handle);
        s = DecoyCarState();
        return -1;
    }
    if (!veh->m_matrix)
        veh->AllocateMatrix();
    if (!veh->m_matrix) {
        CWorld::Remove(veh);
        delete veh;
        s = DecoyCarState();
        return -1;
    }
    const float h = headingDeg * 0.0174533f;
    const CVector fwd(std::sin(h), std::cos(h), 0.0f);
    CMatrix& m = *veh->m_matrix;
    m.right = CVector(fwd.y, -fwd.x, 0.0f);
    m.up = CVector(fwd.x, fwd.y, 0.0f);
    m.at = CVector(0.0f, 0.0f, 1.0f);
    m.pos = CVector(at.x, at.y, gz + 0.6f);
    veh->UpdateRwMatrix();
    s.veh = veh;
    s.vehRef = vpool->GetRef(veh);
    // the driver: an ordinary man of the game's, contained the cop
    // driver's way (mission-created, senses zeroed, seat warp) - he has
    // no brain of ours; the car's autopilot is his whole instruction
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool) {
        CWorld::Remove(veh);
        delete veh;
        s = DecoyCarState();
        return -1;
    }
    CPed* driver = new CPed(PED_TYPE_CIVMALE);
    if (!driver) {
        CWorld::Remove(veh);
        delete veh;
        s = DecoyCarState();
        return -1;
    }
    driver->SetModelIndex(kDecoyManModel);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kDecoyManModel);
    driver->SetCharCreatedBy(kCreatedByMission);
    driver->ClearWeapons();
    CWorld::Add(driver);
    const int idx = ppool->GetIndex(driver);
    if (idx < 0 || idx >= ppool->m_nSize || ppool->GetAt(idx) != driver ||
        ppool->IsFreeSlotAtIndex(idx)) {
        CWorld::Remove(driver);
        delete driver;
        CWorld::Remove(veh);
        delete veh;
        s = DecoyCarState();
        return -1;
    }
    if (driver->m_pIntelligence) {
        driver->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        driver->m_pIntelligence->SetSeeingRange(0.0f);
        driver->m_pIntelligence->SetHearingRange(0.0f);
    }
    if (driver->m_matrix)
        driver->m_matrix->pos = CVector(at.x, at.y, gz + 1.0f);
    else
        driver->SetPosn(CVector(at.x, at.y, gz + 1.0f));
    Command<Commands::TASK_WARP_CHAR_INTO_CAR_AS_DRIVER>(CPools::GetPedRef(driver),
                                                         CPools::GetVehicleRef(veh));
    s.driver = driver;
    s.driverRef = ppool->GetRef(driver);
    Log("coptest decoy: car %d up at (%.0f, %.0f) with its driver", s.vehRef, at.x, at.y);
    return s.vehRef;
}

void GtaDecoyCarDrive(int veh, const Vec3& at, float cruiseMps) {
    DecoyCarState* dp = FindDecoyCar(veh);
    if (!dp)
        return;
    CVehicle* v = LiveDecoyVeh(*dp);
    if (!v)
        return;
    // the game's own autopilot, written plainly every call: the core
    // re-issues as its route look-ahead advances. A civilian's style -
    // AVOID_CARS: he is prey, not a second ram
    CAutoPilot& ap = v->m_autoPilot;
    ap.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
    ap.m_nCarDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
    ap.m_vecDestinationCoors = CVector(at.x, at.y, at.z);
    ap.m_nCruiseSpeed = (char)Clamp(cruiseMps, 0.0f, 127.0f); // the same
        // saturation law as the cop mirror's (the 5x update)
    ap.m_fMaxTrafficSpeed = cruiseMps;
    v->bEngineOn = true;
    v->bIsHandbrakeOn = false;
}

bool GtaDecoyReport(int veh, Vec3* pos, Vec3* vel) {
    DecoyCarState* dp = FindDecoyCar(veh);
    if (!dp || !pos || !vel)
        return false;
    CVehicle* v = LiveDecoyVeh(*dp);
    if (!v || !v->m_matrix)
        return false;
    *pos = Vec3(v->m_matrix->pos.x, v->m_matrix->pos.y, v->m_matrix->pos.z);
    *vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
    return true;
}

void GtaRetireDecoy(int veh) {
    DecoyCarState* dp = FindDecoyCar(veh);
    if (!dp)
        return;
    DecoyCarState& s = *dp;
    if (CPed* driver = LiveDecoyDriver(s)) {
        CWorld::Remove(driver);
        delete driver;
    }
    if (CVehicle* v = LiveDecoyVeh(s)) {
        CWorld::Remove(v);
        delete v;
    }
    s = DecoyCarState();
}

int GtaSpawnDecoyPed(const Vec3& at) {
    DecoyPedState* slot = nullptr;
    for (int i = 0; i < 2; ++i)
        if (!sDecoyPeds[i].used) {
            slot = &sDecoyPeds[i];
            break;
        }
    if (!slot)
        return -1;
    DecoyPedState& s = *slot;
    s = DecoyPedState();
    s.used = true;
    Command<Commands::REQUEST_MODEL>(kDecoyManModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(kDecoyManModel)) {
        s = DecoyPedState();
        return -1;
    }
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool) {
        s = DecoyPedState();
        return -1;
    }
    CPed* man = new CPed(PED_TYPE_CIVMALE);
    if (!man) {
        s = DecoyPedState();
        return -1;
    }
    man->SetModelIndex(kDecoyManModel);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kDecoyManModel);
    man->SetCharCreatedBy(kCreatedByMission);
    man->ClearWeapons();
    CWorld::Add(man);
    const int idx = ppool->GetIndex(man);
    if (idx < 0 || idx >= ppool->m_nSize || ppool->GetAt(idx) != man ||
        ppool->IsFreeSlotAtIndex(idx)) {
        CWorld::Remove(man);
        delete man;
        s = DecoyPedState();
        return -1;
    }
    if (man->m_pIntelligence) {
        man->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        man->m_pIntelligence->SetSeeingRange(0.0f);
        man->m_pIntelligence->SetHearingRange(0.0f);
    }
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    if (man->m_matrix) {
        man->m_matrix->pos = CVector(at.x, at.y, gz + 1.0f);
        man->UpdateRwMatrix();
    } else {
        man->SetPosn(CVector(at.x, at.y, gz + 1.0f));
    }
    s.ped = man;
    s.pedRef = ppool->GetRef(man);
    Log("coptest decoy: a man stands at (%.0f, %.0f)", at.x, at.y);
    return s.pedRef;
}

bool GtaDecoyPedReport(int ped, Vec3* pos, float* health) {
    DecoyPedState* dp = FindDecoyPed(ped);
    if (!dp || !pos || !health)
        return false;
    CPed* man = LiveDecoyPed(*dp);
    if (!man)
        return false;
    const CVector p = man->GetPosition();
    *pos = Vec3(p.x, p.y, p.z);
    *health = man->m_fHealth;
    return true;
}

void GtaRetireDecoyPed(int ped) {
    DecoyPedState* dp = FindDecoyPed(ped);
    if (!dp)
        return;
    DecoyPedState& s = *dp;
    if (CPed* man = LiveDecoyPed(s)) {
        CWorld::Remove(man);
        delete man;
    }
    s = DecoyPedState();
}

// ---- the momentum channel's hands (the 5x update): the core prices
// the throw and names its size; these two only land it on the body.
// Resolution follows this file's one discipline: a decoy is found in
// its own table and proven live through its pool; any other vehicle
// number is a pool ref and is touched only after GetAtRef resolves it
// AND its own GetRef reproduces the number (the round-1 crash lesson -
// a handle is never an index). The velocity fields are per-frame at
// this host's 50 fps (the Report's x50 read-back is the same fact
// read the other way), so the core's m/s divides by 50 on the way in;
// the turn speed beside it is the same per-frame family and takes the
// yaw rate on the same scale - the spin's exact felt size is the
// game's own integration, the linear throw is the graded channel.
void GtaKickVehicle(int veh, const Vec3& dv, float yawRate) {
    CVehicle* v = nullptr;
    if (DecoyCarState* dc = FindDecoyCar(veh))
        v = LiveDecoyVeh(*dc);
    if (!v) {
        auto* vpool = CPools::ms_pVehiclePool;
        if (vpool && veh > 0) {
            CVehicle* cand = vpool->GetAtRef(veh);
            if (cand && vpool->GetRef(cand) == veh)
                v = cand;
        }
    }
    if (!v)
        return;
    v->m_vecMoveSpeed += CVector(dv.x / 50.0f, dv.y / 50.0f, dv.z / 50.0f);
    v->m_vecTurnSpeed.z += yawRate / 50.0f;
}

void GtaKickPed(int ped, const Vec3& dv) {
    CPed* man = nullptr;
    if (ped == 0)
        man = FindPlayerPed();
    else if (DecoyPedState* dp = FindDecoyPed(ped))
        man = LiveDecoyPed(*dp);
    if (!man)
        return;
    // a man on his feet given a move speed is the host's own thrown
    // body from here - slide, stumble or fall is its physics' word,
    // the channel only guarantees the velocity lands whole
    man->m_vecMoveSpeed += CVector(dv.x / 50.0f, dv.y / 50.0f, dv.z / 50.0f);
}

} // namespace mc
