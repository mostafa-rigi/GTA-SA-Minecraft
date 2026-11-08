#include "GtaCop.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "CCopPed.h"
#include "CDecisionMakerTypes.h"
#include "CPools.h"
#include "eCarMission.h" // the autopilot's mission words (fix round 4)
#include "CPlayerPed.h"
#include "CTask.h"
#include "CTaskComplexKillPedOnFoot.h" // item 38: the bailed driver's fight (the soldier's own task class)
#include "eWeaponType.h"                 // item 38: his pistol's word
#include "CVehicle.h"
#include "CAutomobile.h" // the census: the car's drive-state fields
#include "CBike.h"       // the census: the bike's own layout of them
#include "CWorld.h"
#include "Patch.h" // plugin::patch::SetPointer: the channel repair's
                   // vtable claim (the SDK's own _override idiom,
                   // Patch.h:159, with the page protection done)
#include "common.h"
#include "extensions/ScriptCommands.h"

#include "CCamera.h"   // TheCamera: CopRender's distance/frustum courtesy
#include "Cop.h"
#include "CopDiff.h" // the full-diff step: the region law over raw bodies
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
constexpr int kCopBikeModel = 523;  // SA's HPV-1000 police bike (item 36):
                                    // the host's own bike IS the body - no
                                    // Chicago mesh is drawn over it

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
    bool bike = false;     // item 36: this body is the host's police
                           // bike - it stays visible and CopRender
                           // draws nothing over it
    bool bailOut = false;  // item 38: the driver is out of this body,
                           // fighting on foot (the core ordered it;
                           // the report reads it back)
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
    // THE CHANNEL REPAIR (2026-11-08): what the ProcessAI replacement
    // last stamped on this body at the desk physics reads, and how
    // many frames it has stamped - the report carries them to the
    // core as the channel's own receipt (hook* beside cmd and body on
    // the unit line; the /coptest direct act grades the count's
    // advance). Written only inside the hook, during the game's own
    // processing; read by the Report after it.
    float stampGas = 0.0f, stampBrake = 0.0f, stampSteer = 0.0f;
    unsigned stampCount = 0;
    // THE ROOT STEP (2026-11-08): the autopilot mirror's state is
    // GONE - apFeed/apTarget/apCruise/apArmed/apWritten/apWriteAge/
    // apRescueLogT and the whole arming/rescue machine they served.
    // The brain's pedal words are the drive now (ApplyCommand below),
    // and the only autopilot word this bridge ever speaks again is
    // MISSION_NONE, asserted every frame so no game system can claim
    // the body. The history stands in DRIVER2-STEP3-fa.md: every
    // convicted defect of the earlier rounds (the crawl, the frozen
    // units, the block-player missions, the orbiting ring) was born
    // in the translation between the brain and that middleman.
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

// ---- the /coptest diff act's TWIN (the full-diff step): a second
// cop-model body stood up by the anchor's own spawn/seat path - the
// same model 596, the same CCopPed driver kind, the same pool
// proofs, the same seat warp - but registered NOWHERE the force
// keeps its bodies: not in sBodies (so CopBodyPre/Post never touch
// it, ApplyCommand never writes it and the claimed desk's
// FindBodyByPointer never finds it - its ProcessAI forwards to the
// original untouched), and never given a mission by us beyond the
// autopilot arm its drive verb writes. He is the control animal:
// what the game itself does with a body built exactly like ours.
// One twin at a time is all the act stages.
struct TwinBodyState {
    bool used = false;
    CVehicle* veh = nullptr;
    int vehRef = -1;
    CPed* driver = nullptr;
    int driverRef = -1;
};
TwinBodyState sTwin;
CVehicle* LiveTwinVeh(const TwinBodyState& t) {
    if (t.vehRef < 0 || !CPools::ms_pVehiclePool)
        return nullptr;
    CVehicle* v = CPools::ms_pVehiclePool->GetAtRef(t.vehRef);
    return v == t.veh ? v : nullptr;
}
CPed* LiveTwinDriver(const TwinBodyState& t) {
    if (t.driverRef < 0 || !CPools::ms_pPedPool)
        return nullptr;
    CPed* p = CPools::ms_pPedPool->GetAtRef(t.driverRef);
    return p == t.driver ? p : nullptr;
}

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

// =====================================================================
// THE CHANNEL REPAIR (2026-11-08): the door physics actually reads.
//
// THE LIVE CONVICTION it answers (session 14:42-14:45, build
// f6b626ef): the core commanded steer -0.60 / gas 1.00 / brake 0.00;
// the fields ApplyCommand writes DID hold those words microseconds
// after the write (the spike's fresh read-back: gas 1.00, steer
// -0.60); and the car never moved - speed 0.0, position frozen, spike
// FAIL at 0.1 m/s, soak freeze 28.2 s, sheet 0/12. The unit line's
// tick-start report read the SAME fields as gas 0.00 / brake
// 0.50-1.00. Two readings, one field set, opposite stories - so the
// fields are RECOMPUTED inside the game's own frame processing,
// between our gameProcessEvent writes (Events.h: SA's
// gameProcessEvent is PRIORITY_AFTER at the CGame::Process call site
// 0x53E981 - after the whole frame's processing, physics included).
//
// WHOSE hand recomputes them - the SDK evidence, file by file:
// - CVehicle.h:240-243, VALIDATE_OFFSETs at 575-578: m_fSteerAngle
//   0x494, m_fGasPedal 0x49C, m_fBreakPedal 0x4A0 - the fields both
//   witnesses read; there are not two pedal sets on the vehicle.
// - CVehicle.h:341 + CVehicle.cpp:75-79: ProcessControlInputs(player
//   Num) is the PLAYER's writer of those fields (vtable slot 24,
//   CPad-fed). An AI driver has no pad path to his car - candidate
//   (c), writing the driver's pad words, has no reader on an AI car.
// - CAutomobile.h:141 + CAutomobile.cpp:18-22: CAutomobile::
//   ProcessAI(uint&) - vtable slot 66 - is the AI car's control
//   writer: it computes the pedal words from the car's CAutoPilot
//   (CAutoPilot.h: mission, cruise, destination) inside the
//   vehicle's own control pass (CVehicle.h:214 places ProcessControl
//   as the car's per-frame control entry), which runs BEFORE physics
//   in the same processing. CBike.cpp:9-11: the bike's ProcessAI is
//   the same slot 66 on its own table.
// - The control experiment our own sheet runs every session: the
//   /coptest DECOY car - same warp seating, same absence of any
//   drive task on its driver (CTaskComplexCarDrive exists nowhere
//   in this SDK copy; the warp is CTaskSimpleCarSetPedInAsDriver) -
//   DRIVES, because GtaDecoyCarDrive arms its autopilot. The cop,
//   mission forced NONE, does not. The gate is not a task; it is
//   ProcessAI's output, and at mission NONE that output is the
//   hold-still stomp the unit line read: gas 0, brake 0.50-1.00
//   (it brakes a stopped car the way a traffic driver holds one).
//   Candidate (b) - keep a genuine drive task and overwrite after
//   it - collapses: the task's words would still land in the
//   autopilot, and ProcessAI would still speak after them; and the
//   class to build the task from is absent from the SDK in-tree.
//
// THE DOOR (candidate (a), at its sharpest point): claim slot 66
// itself. The class vtable entry for CAutomobile::ProcessAI (and
// CBike::ProcessAI for the bike body) is replaced - once, lazily, at
// the first spawn of each kind, the table read off the fresh body
// itself so no table address is ever hardcoded - with the function
// below, following the SDK's own vtable idiom (Patch.h:159's
// _override macro; the write itself goes through patch::SetPointer
// so the page protection is handled, and the landing is read back
// and logged). For a body of OURS the replacement stamps the core's
// stored command onto the fields at that desk: nothing recomputes
// the words afterwards, and physics - later in the same processing
// - reads ours. THE MISSING-LINK CLOSE (the block at CopAIFn::
// ProcessAI below) changed HOW the stamp is reached: the original
// runs first and the stamp lands after it, because the channel
// build's live session proved the desk alone is not the whole
// story - the original maintains state besides the pedal words.
// For every other vehicle the replacement forwards to the saved
// original untouched. A body whose core has given no order yet
// forwards too: the channel opens with the first command, and
// before it the body is simply parked.
// The patch is never lifted: session resets forget bodies, and a
// table whose every call forwards is the game's own behaviour.
// =====================================================================
constexpr int kProcessAISlot = 66; // CAutomobile.cpp:21 / CBike.cpp:11
void** sCarVtable = nullptr;       // read off our first car body
void** sBikeVtable = nullptr;      // read off our first bike body
void* sCarAIOrig = nullptr;        // the slot's saved originals
void* sBikeAIOrig = nullptr;
bool sCarChannelDone = false;      // claim attempted (once, either way)
bool sBikeChannelDone = false;

CopBodyState* FindBodyByPointer(CVehicle* veh) {
    if (!veh)
        return nullptr;
    for (int i = 0; i < kMaxCopUnits; ++i) {
        CopBodyState& b = sBodies[i];
        // the cheap compare first; the pool confirmation second -
        // a pointer the world has recycled must never match a body
        // whose car is gone (the retire paths forget the body, and
        // this is the second lock on the same door)
        if (b.used && b.veh == veh && LiveVeh(b) == veh)
            return &b;
    }
    return nullptr;
}

// THE STATE CENSUS (the missing-link step, 2026-11-08): every
// drive-state field that plausibly gates or shapes drive force,
// read off the body into the report's census block. The offsets are
// the SDK's VALIDATE_OFFSETs, cited per field; the typed members
// carry them, so no raw offset is ever written here. Car and bike
// keep the same facts at different addresses (their own headers'
// layouts); the shared CVehicle/CPhysical/CAutoPilot fields lead.
//   gear        CVehicle::m_nCurrentGear            0x4B4
//   gearChange  CVehicle::m_fGearChangeCount        0x4B8
//   rawSteer    CVehicle::m_fRawSteerAngle          0x58C
//   steer2      CVehicle::m_f2ndSteerAngle          0x498
//   fakePhysics CPhysical::m_nFakePhysics           0xB8
//   tempAction  CAutoPilot::m_nTempAction           +0x2B
//   ctrlFlags   CAutoPilot::m_nCarCtrlFlags         +0x4B
//   switched    CAutoPilot::m_nTimeSwitchedToRealPhysics +0x20
//   revs        CAutomobile::m_fGasPedalAudioRevs   0x964
//               CBike::m_fGasPedalAudioRevs         0x808
//   contact / onGround / driveGround
//               CAutomobile 0x960 / 0x961 / 0x962
//               CBike       0x804 / 0x805 / 0x806
//   wheelState  CAutomobile::m_wheelState           0x968
//               CBike::m_wheelState                 0x80C
//   susp        CAutomobile::m_fWheelsSuspensionCompression 0x7D4
//               CBike::m_aWheelSuspensionHeights    0x760
//   wheelSpeed  CAutomobile::m_fWheelSpeed          0x848
//               CBike::m_aWheelAngularVelocity      0x758
//   brakeCount  CAutomobile::m_fBrakeCount          0x804
//               CBike::m_fBrakeCount                0x740
//   burnout     CAutomobile::m_bDoingBurnout        0x86A
//               CBike::m_nBrakesOn                  0x7A8
// The velocity half of the census already rides the report (vel).
void FillCensus(CVehicle* v, bool bike, Host::CopBody::Census& cen) {
    cen.gear = (int)v->m_nCurrentGear;
    cen.gearChange = v->m_fGearChangeCount;
    cen.rawSteer = v->m_fRawSteerAngle;
    cen.steer2 = v->m_f2ndSteerAngle;
    cen.fakePhysics = (int)v->m_nFakePhysics;
    cen.tempAction = (int)v->m_autoPilot.m_nTempAction;
    cen.ctrlFlags = (int)v->m_autoPilot.m_nCarCtrlFlags;
    cen.switched = v->m_autoPilot.m_nTimeSwitchedToRealPhysics;
    if (bike) {
        CBike* bk = static_cast<CBike*>(v);
        cen.revs = bk->m_fGasPedalAudioRevs;
        cen.contact = (int)bk->m_nNumContactWheels;
        cen.onGround = (int)bk->m_nNumWheelsOnGround;
        cen.driveGround = (int)bk->m_nNumDriveWheelsOnGroundLastFrame;
        for (int w = 0; w < 2; ++w) {
            cen.wheelState[w] = (int)bk->m_wheelState[w];
            cen.susp[w] = bk->m_aWheelSuspensionHeights[w];
            cen.wheelSpeed[w] = bk->m_aWheelAngularVelocity[w];
        }
        cen.brakeCount = bk->m_fBrakeCount;
        cen.burnout = (int)bk->m_nBrakesOn;
    } else {
        CAutomobile* aut = static_cast<CAutomobile*>(v);
        cen.revs = aut->m_fGasPedalAudioRevs;
        cen.contact = (int)aut->m_nNumContactWheels;
        cen.onGround = (int)aut->m_nWheelsOnGround;
        cen.driveGround = (int)aut->m_nNumDriveWheelsOnGroundLastFrame;
        for (int w = 0; w < 4; ++w) {
            cen.wheelState[w] = (int)aut->m_wheelState[w];
            cen.susp[w] = aut->m_fWheelsSuspensionCompression[w];
            cen.wheelSpeed[w] = aut->m_fWheelSpeed[w];
        }
        cen.brakeCount = aut->m_fBrakeCount;
        cen.burnout = (int)aut->m_bDoingBurnout;
    }
}

// the stamp itself: the drive surface of ApplyCommand, written at
// the desk physics reads - the pedal fields, the siren flag, and
// (once he sits) the parked-car switches open and the mission NONE,
// so no game system can arm a destination under the brain's pedals.
// The health display is NOT stamped here: its measure-and-hold pair
// lives in Pre/Post and keeps its own cadence.
void StampDrive(CVehicle* veh, CopBodyState& b) {
    veh->m_fGasPedal = b.gas;
    veh->m_fBreakPedal = b.brake;
    veh->m_fSteerAngle = b.steer;
    // the input-side steer word rides the stamp (the missing-link
    // step): m_fRawSteerAngle (CVehicle.h:314, VALIDATE_OFFSET
    // 0x58C - "AKA m_fSteeringLeftRight or fSteer") is the field the
    // PLAYER's channel writes (ProcessControlInputs), and the
    // census now prints it for our body and for a host-driven
    // decoy alike - if the AI path never reads it, the decoy's
    // reads as dead as ours and this write is a harmless belt; if
    // anything between the desk and the wheels converts from it,
    // this is the word that was missing, and the stamp is where it
    // must land (after the original, per the close below)
    veh->m_fRawSteerAngle = b.steer;
    veh->bSirenOrAlarm = b.siren;
    // the handbrake is cleared from the FIRST stamp on every body
    // (the missing-link step's secondary defect: on the channel
    // build the clear was seated-gated, and the later spawns' first
    // reports read handbrake 1 - their drivers had not landed yet,
    // so the parked car's switch stood set under their first
    // orders). The stamp is the body's first word of control; the
    // switch opens with it, seated or not
    veh->bIsHandbrakeOn = false;
    // THE FOURTH WORD (2026-11-08): clear the park latch. The
    // missing-link session's census (17:29, shape-(b) build
    // 39d43e9e) convicted the one field that never moved while
    // everything else answered our words: m_fBrakeCount
    // (CAutomobile 0x804, CBike 0x740 - "the brake integrator" of
    // the census table above) sat pinned at exactly 20.00, its
    // cap, for a full minute of gas 1.00 at brake pedal 0.00 -
    // gear 1, the rev counter climbing 0.50 -> 1.00, and the
    // wheel speeds dead at -0.0 on every wheel. A value held at
    // its cap against a zero brake pedal is not decaying physics
    // residue; something tops it up each frame on a body whose
    // mission reads NONE, and the only per-frame writer we run
    // on this body with a stand-still brief is the original at
    // this very desk, speaking just before us under shape (b).
    // While the latch stands, physics lets no drive force reach
    // the wheels - the engine revs against a parked car. So the
    // stamp clears it, in this same call, after the original:
    // whenever the stored command carries DRIVE INTENT. Intent
    // in the core's own pedal language is: gas above the
    // threshold (forward), or a brake word while the body is at
    // rest or already rolling backward (on this host a held
    // brake at a standstill IS the reverse gear - StandDrive's
    // law in the core, and the recovery's extraction rides the
    // same field out of a wedged stop). The threshold is 0.05:
    // five times under the smallest deliberate drive word the
    // core ever sends (the pursuit blend's gas floors at 0.25;
    // deliberate brake words are 0.6-class) and above the idle
    // defaults (gas 0.0 - and this stamp only runs under
    // haveCmd, so the stored brake default never reaches here).
    // The exception is the HOLD: a command with no drive intent
    // (gassing nowhere, braking only while rolling in to slow,
    // or the standstill words) leaves the latch alone - a unit
    // the core is braking onto a post and holding there WANTS
    // the park latch; there it works for us, not against us.
    // If a writer after this desk in the pass re-tops the latch,
    // the census shows bc back at 20.00 under drive orders next
    // session and names that instead - the clear stands where
    // the evidence puts the top-up: before physics, after the
    // original, at the desk physics reads.
    {
        const CVector& mv = veh->m_vecMoveSpeed;
        const float spd2 = mv.x * mv.x + mv.y * mv.y;
        const bool driveIntent =
            b.gas > 0.05f || (b.brake > 0.05f && spd2 <= 0.25f);
        if (driveIntent) {
            if (b.bike)
                static_cast<CBike*>(veh)->m_fBrakeCount = 0.0f;
            else
                static_cast<CAutomobile*>(veh)->m_fBrakeCount = 0.0f;
        }
    }
    if (b.seated) {
        veh->bEngineOn = true;
        veh->m_autoPilot.m_nCarMission = MISSION_NONE;
    }
    b.stampGas = b.gas;
    b.stampBrake = b.brake;
    b.stampSteer = b.steer;
    ++b.stampCount;
}

// the replacement, in the SDK's _override shape: a member function
// whose address takes the slot. __single_inheritance pins the
// member pointer to the single word the macro's cast assumes, and
// the static_assert at the claim proves it on this compiler.
struct __single_inheritance CopAIFn {
    bool ProcessAI(unsigned int& arg0);
};

// =====================================================================
// THE MISSING LINK (2026-11-08): the desk claimed, the car frozen.
//
// THE LIVE CONVICTION it answers (session 16:47, channel build
// ced434f7): both desks CLAIMED, the stamps advancing ~65/s, and at
// the desk cmd == body == hook exactly (steer -0.60, gas 1.00,
// brake 0.00), mission NONE, engine 1, driver-in 1, the anchor's
// handbrake 0 - and the car never moved (spike FAIL at avg 0.0,
// displacement 0; the only nonzero speeds were external pushes).
// So replacing ProcessAI removed more than its pedal words: the
// original maintains STATE besides the three fields - some link
// between the desk and wheel force that only it supplies. The
// prime suspect CLASS going in (named as a class, not a verdict)
// was transmission/gear state; the census above is the instrument
// built to convict the specific field in one session, by diffing
// this body against a host-driven decoy field by field.
//
// THE CLOSE - shape (b): at the desk, run the ORIGINAL first for
// our bodies, then stamp ONLY the pedal/steer words over its
// outputs, inside the same call. Whatever state machinery the
// original runs - gear-adjacent autopilot state, temp actions, the
// rev counter, the stuck logic - lives again, byte for byte the
// game's own; what physics reads as the drive words are still
// ours, because the stamp lands after the original returns and
// nothing runs after us in that call before physics. Shape (a) -
// maintain the missing state ourselves in the replacement - is
// REJECTED on the evidence in-tree: the SDK carries no body for
// ProcessAI (CAutomobile.cpp in-tree is an address trampoline,
// "Converted from void CAutomobile::ProcessAI(uint &) 0x0"), so
// its full write set is not enumerable from anything we can
// inspect; (a) would have us re-implement an unverifiable subset
// and call it the link, and even a census-diffed field is not
// proof it is the ONLY maintained state (the census covers the
// fields we thought to print). (b) needs no enumeration.
//
// (b)'s safety, point by point, off the same evidence:
// - DRIVING INTENT: the original's intent is a function of the
//   autopilot, and the autopilot is held at MISSION_NONE for our
//   bodies (StampDrive and ApplyCommand both force it, and the
//   original in this call reads the mission the last stamp left:
//   NONE). At NONE its computed pedal outputs are the hold-still
//   words the 14:44 unit line read (gas 0, brake 0.50-1.00) - and
//   those are exactly what the stamp overwrites, in this call,
//   after it speaks.
// - HANDBRAKE: a parked traffic car at NONE may sit on its
//   handbrake inside the original's run; the stamp clears the
//   switch after it, in the same call (and now from the first
//   stamp, seated or not - the secondary defect above).
// - VELOCITY: the original has no direct velocity or force
//   writes to hide: in the SDK it exists only as the control
//   writer, and the force path from the control fields to the
//   wheels is physics-side (cTransmission::
//   CalculateDriveAcceleration + the wheel-pair processing). If a
//   build of the game proved otherwise, the census's velocity
//   neighbour (the report's vel, beside every census line) is
//   where it would show - a stamped car moving WITHOUT our words
//   would convict this paragraph, and the sheet's direct act
//   grades motion against our words every run.
// - GEAR: CalculateDriveAcceleration's own signature
//   (cTransmission.h:32) takes gasPedal and currrentGear BY
//   REFERENCE from its physics-side caller - the gear is selected
//   at physics time off the FINAL pedal fields, i.e. off the
//   stamp, not off the original's pre-stamp outputs.
// The census stays as the standing witness: if any part of this
// reasoning is wrong on the real host, the per-second anchor/decoy
// pair names the field it is wrong about.
// =====================================================================
bool CopAIFn::ProcessAI(unsigned int& arg0) {
    CVehicle* veh = reinterpret_cast<CVehicle*>(this);
    CopBodyState* b = FindBodyByPointer(veh);
    // the original, resolved the way the forwarding path below
    // resolves it (the same cast idiom the SDK's own wrappers use)
    void** vt = *reinterpret_cast<void***>(veh);
    void* orig = vt == sBikeVtable ? sBikeAIOrig : sCarAIOrig;
    if (b && b->haveCmd) {
        // THE CLOSE (shape (b), the block above): the original
        // runs FIRST - its state machinery lives - and the stamp
        // lands after it inside the same call, so the pedal/steer
        // words physics reads are the core's and no state the
        // original maintains is missing. With no original saved
        // (a claim that failed), the stamp alone stands, as it did
        // on the channel build - the stamps' silence or the census
        // grades the difference either way
        if (orig)
            ((bool(__thiscall*)(CVehicle*, unsigned int&))orig)(veh, arg0);
        StampDrive(veh, *b);
        return true;
    }
    // not ours (or ours before its first order): the game's own AI
    // drives him, byte for byte as unpatched
    if (!orig)
        return true;
    return ((bool(__thiscall*)(CVehicle*, unsigned int&))orig)(veh, arg0);
}

// claim one class table's slot 66, once, off a live body of the kind
void EnsureChannel(CVehicle* veh, bool bike) {
    bool& done = bike ? sBikeChannelDone : sCarChannelDone;
    if (done)
        return;
    done = true; // attempted once: a failure is logged once and the
                 // stamps' silence grades it, never retried per spawn
    void** vt = *reinterpret_cast<void***>(veh);
    void*& orig = bike ? sBikeAIOrig : sCarAIOrig;
    void**& savedVt = bike ? sBikeVtable : sCarVtable;
    orig = vt[kProcessAISlot];
    savedVt = vt;
    auto fn = &CopAIFn::ProcessAI;
    static_assert(sizeof(fn) == sizeof(void*),
                  "the ProcessAI replacement must be a one-word member pointer");
    void* hook = *reinterpret_cast<void**>(&fn);
    patch::SetPointer((uintptr_t)&vt[kProcessAISlot], hook);
    const bool landed = vt[kProcessAISlot] == hook;
    Log("cop channel: the %s ProcessAI desk (vtable slot %d) %s - original %p saved;"
        " the brain stamps at the desk physics reads from here",
        bike ? "bike" : "car", kProcessAISlot,
        landed ? "is CLAIMED" : "COULD NOT BE CLAIMED - the direct channel stays shut",
        orig);
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
    veh->m_fRawSteerAngle = b.steer; // the input-side steer word
        // rides every assert, as it rides the stamp (StampDrive
        // names the reasoning and the census that grades it)
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
    // the parked car's other switch (fix round 4): a car the game
    // counts as parked sits on its handbrake (CVehicle.h:155).
    // THE MISSING-LINK STEP moves this clear out of the seated
    // block below: on the channel build the seated gate left the
    // later spawns' first reports reading handbrake 1 (their
    // drivers had not landed when their first orders stamped), so
    // the switch now opens with the body's first asserted command
    // - the same law StampDrive follows at the desk
    veh->bIsHandbrakeOn = false;
    if (b.seated) {
        veh->bEngineOn = true;
        // THE ROOT STEP (2026-11-08): the autopilot mirror is REMOVED
        // - the whole fix-round-4/complete-update machine that armed
        // GOTOCOORDS_STRAIGHT missions with the brain's aim point and
        // cruise, refreshed them on a cadence, and rescued them when
        // foreign orders overwrote them. The convictions that built
        // it were real (the pedal fields ARE recomputed by ProcessAI
        // while an autopilot order owns the car), but the user's live
        // session of 2026-11-08 convicted the mirror itself as the
        // last-metres disease: the brain's decisions read right all
        // session while the bodies orbited their feed points forever
        // - a traffic autopilot's native goal is arrive-and-stop, so
        // a point fed THROUGH the target buys overshoot, loop and
        // re-approach without end, and the ring built on it never
        // settled once. The direct law is the opposite shape: the
        // core's steer/gas/brake words (written at the top of this
        // function) are the drive, and this bridge's remaining
        // autopilot duty is to keep every other hand OFF the car -
        // the mission is forced to NONE every frame while the driver
        // sits in it, so no game behaviour (the block-player family
        // claimed siren-flagged cars in the 02:00 session) can arm a
        // destination under the brain's pedals.
        // THE CHANNEL REPAIR (2026-11-08) - what these gameProcessEvent
        // writes are, and are not: the root step claimed this order
        // made the pedal writes "the frame's last word" and the next
        // frame's physics would read them first. The 14:44 live
        // session convicted that claim whole: gas 1.00 stood on the
        // fields microseconds after this write while the car never
        // moved, because the game's ProcessAI recomputes the fields
        // INSIDE the frame's processing, before physics - after our
        // write, before any reader that matters. These writes stay
        // (they cost nothing, they keep the siren/engine switches
        // fresh between control passes, and the Report's tick-start
        // read-back remains a witness), but they are NOT the channel
        // anymore: the channel is the ProcessAI replacement above
        // (StampDrive), which writes the same stored words at the
        // desk physics reads, inside the processing itself.
        veh->m_autoPilot.m_nCarMission = MISSION_NONE;
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
    sTwin = TwinBodyState();
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
                // (the join callout that used to sound here is gone:
                // since the 39-package the CORE is the only radio
                // voice - it fires the join line at the ladder spawn
                // itself, gained and logged; two voices saying the
                // same moment was the double-talk the user heard)
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
        const bool shown = CopBodyFilesReady() && !b.bike;
        veh->bIsVisible = !shown;
        if (CPed* driver = LiveDriver(b)) {
            // a bailed-out driver is a man fighting in the street:
            // he is shown even while his car wears the drawn body
            driver->bIsVisible = !shown || b.bailOut;
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
        // (his radio on the pursuit's first moment used to sound
        // here; since the 39-package the CORE fires the opening line
        // itself - gained, logged and counted - and this site keeps
        // only the edge bookkeeping)
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
        if (!b.used || b.bike)
            continue; // item 36: no Chicago mesh over a bike - the
                      // host's own bike and rider are the body
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

// the one standing-up, two bodies: the car (the Chicago-bodied
// cruiser the force has always worn) and, for item 36, the host's
// own police bike - same pool discipline, same driver, same seat
// warp, same proofs; only the vehicle model and the body's own
// flags differ (a bike is never hidden and never drawn over)
int GtaSpawnCopBodyKind(const Vec3& at, float headingDeg, const Vec3& face, bool bike) {
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
    s.bike = bike;
    const int vehModel = bike ? kCopBikeModel : kCopCarModel;
    Command<Commands::REQUEST_MODEL>(vehModel);
    Command<Commands::REQUEST_MODEL>(kCopDriverModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(vehModel) ||
        !Command<Commands::HAS_MODEL_LOADED>(kCopDriverModel)) {
        s = CopBodyState();
        return -1;
    }
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    Log("cop spawn: step 1 - models in, creating %s %d at (%.0f, %.0f)", bike ? "bike" : "car", vehModel, at.x, at.y);
    int handle = 0;
    Command<Commands::CREATE_CAR>(vehModel, at.x, at.y, gz + 0.6f, &handle);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(vehModel);
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
    // THE CHANNEL REPAIR: with the body's identity proven, claim the
    // desk this body is driven from - the class table's ProcessAI
    // slot, once per kind, read off this very body (EnsureChannel
    // logs the claim and its read-back). From the next processing
    // pass, this car's control writer is ours.
    EnsureChannel(veh, bike);

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

int GtaSpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face) {
    return GtaSpawnCopBodyKind(at, headingDeg, face, false);
}

int GtaSpawnCopBikeBody(const Vec3& at, float headingDeg, const Vec3& face) {
    return GtaSpawnCopBodyKind(at, headingDeg, face, true);
}

// item 38: the bail order. Out: the seated driver is warped to his
// own door (the game's script warp, GtaMining's surface), given his
// pistol and the soldier's own fight task against the player, with
// his senses opened for the fight's duration; Pre keeps measuring
// and topping him wherever he stands, so the one-unit book never
// notices he left the seat. Back: the fight is cleared, the pistol
// taken, the senses closed, and the same warp that seated him at
// spawn puts him back. The core owns when; this owns how.
void GtaCopBodyBail(int veh, bool out) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    CopBodyState& b = *bp;
    CVehicle* v = LiveVeh(b);
    CPed* driver = LiveDriver(b);
    if (!v || !driver || !v->m_matrix) {
        Log("cop bail: body %d cannot answer a bail order (vehicle %p, driver %p)", veh, (void*)v, (void*)driver);
        return;
    }
    if (out && !b.bailOut) {
        CPlayerPed* player = FindPlayerPed();
        if (!player) {
            Log("cop bail: no player to fight - the order waits");
            return;
        }
        const CMatrix& cm = *v->m_matrix;
        const CVector doorP = cm.pos + cm.right * -0.9f + cm.up * 0.6f;
        Command<Commands::WARP_CHAR_FROM_CAR_TO_COORD>(CPools::GetPedRef(driver), doorP.x, doorP.y, doorP.z);
        Command<Commands::GIVE_WEAPON_TO_CHAR>(CPools::GetPedRef(driver), (int)WEAPONTYPE_PISTOL, 9999);
        Command<Commands::SET_CURRENT_CHAR_WEAPON>(CPools::GetPedRef(driver), (int)WEAPONTYPE_PISTOL);
        if (driver->m_pIntelligence) {
            driver->m_pIntelligence->SetSeeingRange(60.0f);
            driver->m_pIntelligence->SetHearingRange(60.0f);
            auto* fight = new CTaskComplexKillPedOnFoot(player, -1, 0, 0, 100, 0);
            driver->m_pIntelligence->m_TaskMgr.SetTask(fight, TASK_PRIMARY_PRIMARY, false);
        }
        b.bailOut = true;
        Log("cop bail: the driver of car %d is out and fighting on foot", veh);
    } else if (!out && b.bailOut) {
        driver->ClearWeapons();
        if (driver->m_pIntelligence) {
            driver->m_pIntelligence->SetSeeingRange(0.0f);
            driver->m_pIntelligence->SetHearingRange(0.0f);
        }
        Command<Commands::TASK_WARP_CHAR_INTO_CAR_AS_DRIVER>(CPools::GetPedRef(driver),
                                                             CPools::GetVehicleRef(v));
        b.bailOut = false;
        Log("cop bail: the driver of car %d is warped back into his seat", veh);
    }
}

// item 24's traffic: the host's own vehicles around a point, by the
// pool's own enumeration. Cop bodies, the decoys and the player's
// vehicle are not traffic and never appear; the number handed back
// is the pool ref, the same number the kick verbs trade in.
void GtaNearbyVehicles(const Vec3& centre, float radius, std::vector<Host::VehicleInfo>& out) {
    auto* pool = CPools::ms_pVehiclePool;
    if (!pool)
        return;
    CVehicle* mine[kMaxCopUnits];
    int nMine = 0;
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (sBodies[i].used)
            if (CVehicle* v = LiveVeh(sBodies[i]))
                mine[nMine++] = v;
    CVehicle* decoys[5];
    int nDecoys = 0;
    for (int i = 0; i < 2; ++i) {
        if (sDecoyCars[i].used) {
            if (CVehicle* v = LiveDecoyVeh(sDecoyCars[i]))
                decoys[nDecoys++] = v;
        }
    }
    // the diff act's twin is a staged body too, not traffic (the
    // plough law must never price a throw at the control animal)
    if (sTwin.used) {
        if (CVehicle* v = LiveTwinVeh(sTwin))
            decoys[nDecoys++] = v;
    }
    CVehicle* pv = FindPlayerVehicle();
    const float r2 = radius * radius;
    for (int i = 0; i < pool->m_nSize; ++i) {
        CVehicle* v = pool->GetAt(i);
        if (!v || v == pv)
            continue;
        bool own = false;
        for (int j = 0; j < nMine; ++j)
            own = own || mine[j] == v;
        for (int j = 0; j < nDecoys; ++j)
            own = own || decoys[j] == v;
        if (own || !v->m_matrix)
            continue;
        const CVector& vp = v->m_matrix->pos;
        const float dx = vp.x - centre.x, dy = vp.y - centre.y, dz = vp.z - centre.z;
        if (dx * dx + dy * dy + dz * dz > r2)
            continue;
        Host::VehicleInfo info;
        info.veh = pool->GetRef(v);
        info.pos = Vec3(vp.x, vp.y, vp.z);
        info.vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
        out.push_back(info);
    }
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
    // instead of leaving the next round to guess. THE ROOT STEP: the
    // mission and cruise below are now the CHANNEL'S RECEIPT - under
    // direct drive the mission must read NONE (0) and the cruise 0 on
    // every report while the body drives; any other value means a
    // game system claimed the car under the brain's pedals, and the
    // /coptest direct phase grades exactly that witness.
    out->engineOn = v->bEngineOn;
    out->handbrake = v->bIsHandbrakeOn;
    out->engineBroken = v->bEngineBroken;
    out->status = (int)v->m_nStatus;
    out->mission = (int)v->m_autoPilot.m_nCarMission;
    out->cruise = (int)v->m_autoPilot.m_nCruiseSpeed;
    out->cruiseF = v->m_autoPilot.m_fMaxTrafficSpeed; // the float's
        // landing, read back beside the char's (the 5x update: the
        // pair is the clipping's receipt)
    // THE CHANNEL REPAIR's receipt: what the ProcessAI replacement
    // last stamped on this body at the desk physics reads, and its
    // stamp count - beside the tick-start field read-back above,
    // whose timing the 14:44 session convicted (it reads the fields
    // after a full frame of game processing; the stamp is taken
    // inside that processing, at the last writer's desk)
    out->hookGas = s.stampGas;
    out->hookBrake = s.stampBrake;
    out->hookSteer = s.stampSteer;
    out->hookStamps = (int)s.stampCount;
    // the state census (the missing-link step): this body's whole
    // drive-state block, read with the rest of the report - the
    // fields FillCensus names, in the shape the decoy's census
    // below answers in, so the log's per-second pair diffs cleanly
    FillCensus(v, s.bike, out->census);
    if (CPed* drv = LiveDriver(s))
        out->driverInVehicle = drv->bInVehicle;
    // item 38's read-back: out of the car, and where he stands -
    // read off the ped himself, the way every other word here is
    out->driverOut = s.bailOut;
    if (CPed* drv = LiveDriver(s)) {
        const CVector dp = drv->GetPosition();
        out->driverPos = Vec3(dp.x, dp.y, dp.z);
    }
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
            " | engine %d handbrake %d broken %d status %d mission %d cruise %d driver-in %d"
            " | channel stamps %d - the body answers",
            s.vehRef, out->gas, out->brake, out->steer,
            out->engineOn ? 1 : 0, out->handbrake ? 1 : 0, out->engineBroken ? 1 : 0,
            out->status, out->mission, out->cruise, out->driverInVehicle ? 1 : 0,
            out->hookStamps);
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
    // THE ROOT STEP: RETIRED, kept only so the host interface links.
    // The feed this verb carried (a destination + cruise for the
    // traffic autopilot) has no consumer left: the core drives the
    // pedals directly through GtaCopBodyDrive, and nothing here may
    // arm the middleman again. The arguments are deliberately unused.
    (void)veh;
    (void)at;
    (void)cruiseMps;
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
            // autopilot is left at NONE before the fire takes him, so
            // the wreck the city keeps is a wreck, not a driverless
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
// outright - the same basis write the spawn proves, then the body's
// impact bookkeeping forgotten so the staging itself is not priced
// as a crash. Nothing in play calls this; it exists so the self-test
// can put a unit nose-to-wall exactly, instead of hoping a chase ends
// in a wedge.
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
    ap.m_nCruiseSpeed = (char)Clamp(cruiseMps, 0.0f, 127.0f); // the
        // narrow char saturates at 127, the float beside it carries
        // the true ask (the 5x update's receipt pair). NOTE: this
        // decoy is a civilian and keeps the game's autopilot on
        // purpose - the root step retired the middleman for COP
        // bodies only
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

// the decoy's state census (the missing-link step): the cop
// report's shape read off a HOST-DRIVEN body - pedals, mission and
// the whole census block - so the /coptest pursuit act can print
// this car's drive state beside our stamped anchor's, per second.
// The decoy is an ordinary automobile (never a bike), and its
// autopilot is ARMED by GtaDecoyCarDrive: this is what a moving
// car's fields look like while the game's own ProcessAI drives it
bool GtaDecoyCensus(int veh, Host::CopBody* out) {
    DecoyCarState* dp = FindDecoyCar(veh);
    if (!dp || !out)
        return false;
    CVehicle* v = LiveDecoyVeh(*dp);
    if (!v || !v->m_matrix)
        return false;
    *out = Host::CopBody();
    out->ok = true;
    out->pos = Vec3(v->m_matrix->pos.x, v->m_matrix->pos.y, v->m_matrix->pos.z);
    out->fwd = Vec3(v->m_matrix->up.x, v->m_matrix->up.y, v->m_matrix->up.z);
    out->vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
    out->gas = v->m_fGasPedal;
    out->brake = v->m_fBreakPedal;
    out->steer = v->m_fSteerAngle;
    out->siren = v->bSirenOrAlarm;
    out->engineOn = v->bEngineOn;
    out->handbrake = v->bIsHandbrakeOn;
    out->engineBroken = v->bEngineBroken;
    out->status = (int)v->m_nStatus;
    out->mission = (int)v->m_autoPilot.m_nCarMission;
    out->cruise = (int)v->m_autoPilot.m_nCruiseSpeed;
    out->cruiseF = v->m_autoPilot.m_fMaxTrafficSpeed;
    FillCensus(v, false, out->census);
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

// ---- the diff act's twin (the full-diff step) --------------------
// The anchor's own spawn path, walked a second time for a body the
// force will never own: same model, same driver kind, same proofs,
// same seat warp (GtaSpawnCopBodyKind is the text this mirrors - the
// differences are the point and they are exactly three: no sBodies
// registration, no channel claim, no book). What the game then does
// with him - his autopilot armed by GtaCopTwinDrive alone - is the
// control reading the anchor is diffed against.
int GtaSpawnCopTwin(const Vec3& at, float headingDeg, const Vec3& face) {
    (void)headingDeg; // the face point decides the nose, as at the anchor's spawn
    if (sTwin.used) {
        Log("cop twin: a twin already stands - spawn refused");
        return -1;
    }
    TwinBodyState& s = sTwin;
    s = TwinBodyState();
    s.used = true;
    Command<Commands::REQUEST_MODEL>(kCopCarModel);
    Command<Commands::REQUEST_MODEL>(kCopDriverModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(kCopCarModel) ||
        !Command<Commands::HAS_MODEL_LOADED>(kCopDriverModel)) {
        s = TwinBodyState();
        return -1;
    }
    bool found = false;
    float gz = CWorld::FindGroundZFor3DCoord(at.x, at.y, at.z + 2.0f, &found, nullptr);
    if (!found)
        gz = at.z;
    int handle = 0;
    Command<Commands::CREATE_CAR>(kCopCarModel, at.x, at.y, gz + 0.6f, &handle);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kCopCarModel);
    // the handle is a pool REF, as at every spawn in this file:
    // resolve through GetAtRef and prove the identity round-trips
    auto* vpool = CPools::ms_pVehiclePool;
    CVehicle* veh = (vpool && handle > 0) ? vpool->GetAtRef(handle) : nullptr;
    if (!veh) {
        Log("cop twin: CREATE_CAR handle %d resolves to no vehicle - spawn refused", handle);
        s = TwinBodyState();
        return -1;
    }
    const int vidx = vpool->GetIndex(veh);
    if (vidx < 0 || vidx >= vpool->m_nSize || vpool->IsFreeSlotAtIndex(vidx) ||
        vpool->GetRef(veh) != handle) {
        Log("cop twin: handle %d resolved but the pool identity does not round-trip (slot %d) - spawn refused",
            handle, vidx);
        s = TwinBodyState();
        return -1;
    }
    if (!veh->m_matrix)
        veh->AllocateMatrix();
    if (!veh->m_matrix) {
        Log("cop twin: the new car has no matrix and AllocateMatrix gave none - spawn refused");
        CWorld::Remove(veh);
        delete veh;
        s = TwinBodyState();
        return -1;
    }
    Vec3 d = face - at;
    d.z = 0.0f;
    const float dl = d.Length();
    Vec3 fwd = dl > 0.01f ? d * (1.0f / dl) : Vec3(0, 1, 0);
    CMatrix& m = *veh->m_matrix;
    m.right = CVector(fwd.y, -fwd.x, 0.0f);
    m.up = CVector(fwd.x, fwd.y, 0.0f);
    m.at = CVector(0.0f, 0.0f, 1.0f);
    m.pos = CVector(at.x, at.y, gz + 0.6f);
    veh->UpdateRwMatrix();
    const CVector& back = veh->m_matrix->pos;
    if (std::fabs(back.x - at.x) > 0.01f || std::fabs(back.y - at.y) > 0.01f ||
        std::fabs(back.z - (gz + 0.6f)) > 0.01f) {
        Log("cop twin: the basis write does not read back (got %.2f, %.2f, %.2f) - spawn refused",
            back.x, back.y, back.z);
        CWorld::Remove(veh);
        delete veh;
        s = TwinBodyState();
        return -1;
    }
    s.veh = veh;
    s.vehRef = vpool->GetRef(veh);
    if (LiveTwinVeh(s) != veh) {
        Log("cop twin: the stored ref does not resolve back to the new car - spawn refused");
        CWorld::Remove(veh);
        delete veh;
        s = TwinBodyState();
        return -1;
    }
    // NO channel claim here, on purpose: the desk claim belongs to
    // the force's bodies. This car's ProcessAI must stay the game's
    // own - if the claim already happened (the anchor spawned
    // first, as the act stages it), the replacement forwards him
    // to the saved original because he is in no body table; if it
    // has not, his table is simply unpatched. Either way he is
    // driven by the original alone.
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool) {
        CWorld::Remove(veh);
        delete veh;
        s = TwinBodyState();
        return -1;
    }
    CCopPed* cop = new CCopPed(COP_TYPE_CITYCOP);
    CPed* driver = cop;
    if (!driver) {
        CWorld::Remove(veh);
        delete veh;
        s = TwinBodyState();
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
        Log("cop twin: the driver did not land in the ped pool (index %d) - spawn refused", idx);
        s = TwinBodyState();
        return -1;
    }
    if (driver->m_pIntelligence) {
        driver->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        driver->m_pIntelligence->SetSeeingRange(0.0f);
        driver->m_pIntelligence->SetHearingRange(0.0f);
    }
    driver->m_fMaxHealth = 1000.0f;
    driver->m_fHealth = 1000.0f;
    driver->bExplosionProof = true;
    driver->bFireProof = true;
    const CVector door(at.x - fwd.y * 1.3f, at.y + fwd.x * 1.3f, gz + 1.0f);
    if (driver->m_matrix) {
        driver->m_matrix->pos = door;
        driver->UpdateRwMatrix();
    } else {
        driver->SetPosn(door);
    }
    Command<Commands::TASK_WARP_CHAR_INTO_CAR_AS_DRIVER>(CPools::GetPedRef(driver),
                                                         CPools::GetVehicleRef(veh));
    s.driver = driver;
    s.driverRef = ppool->GetRef(driver);
    PedSkinsExcludePed(driver);
    Log("cop twin: car %d up at (%.0f, %.0f) with its driver - host-driven, no brain, no desk",
        s.vehRef, at.x, at.y);
    return s.vehRef;
}

void GtaCopTwinDrive(int veh, const Vec3& at, float cruiseMps) {
    if (!sTwin.used || sTwin.vehRef != veh)
        return;
    CVehicle* v = LiveTwinVeh(sTwin);
    if (!v)
        return;
    // the game's own autopilot, written plainly every call - the
    // decoy's recipe byte for byte (the v7-era feed's shape, which
    // provably drove host cars): a straight GOTO, the cruise pair,
    // engine on, handbrake off. Nothing else on this body is ever
    // written by us: no pedals, no mission forcing, no stamps
    CAutoPilot& ap = v->m_autoPilot;
    ap.m_nCarMission = MISSION_GOTOCOORDS_STRAIGHT;
    ap.m_nCarDrivingStyle = DRIVINGSTYLE_AVOID_CARS;
    ap.m_vecDestinationCoors = CVector(at.x, at.y, at.z);
    ap.m_nCruiseSpeed = (char)Clamp(cruiseMps, 0.0f, 127.0f);
    ap.m_fMaxTrafficSpeed = cruiseMps;
    v->bEngineOn = true;
    v->bIsHandbrakeOn = false;
}

bool GtaCopTwinReport(int veh, Vec3* pos, Vec3* vel) {
    if (!sTwin.used || sTwin.vehRef != veh || !pos || !vel)
        return false;
    CVehicle* v = LiveTwinVeh(sTwin);
    if (!v || !v->m_matrix)
        return false;
    *pos = Vec3(v->m_matrix->pos.x, v->m_matrix->pos.y, v->m_matrix->pos.z);
    *vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
    return true;
}

void GtaRetireCopTwin(int veh) {
    if (!sTwin.used || sTwin.vehRef != veh)
        return;
    TwinBodyState& s = sTwin;
    if (CPed* driver = LiveTwinDriver(s)) {
        CWorld::Remove(driver);
        delete driver;
    }
    if (CVehicle* v = LiveTwinVeh(s)) {
        CWorld::Remove(v);
        delete v;
    }
    s = TwinBodyState();
}

// ---- the full diff + adoption (the full-diff step) ---------------
// One call, inside one frame callback: snapshot both bodies whole,
// diff them through the core engine (CopDiff: the region law over
// the SDK's own layout), print the diff to the log, and land the
// adoption - the twin's drive-state words onto the anchor, nothing
// else, nowhere else. The twin is never written. Guards first, and
// every refusal is a printed line, not a silence.
bool GtaCopBodyDiffAdopt(int anchorVeh, int twinVeh, Host::DiffAdopt* out) {
    if (!out)
        return false;
    *out = Host::DiffAdopt();
    CopBodyState* ab = FindBody(anchorVeh);
    if (!ab) {
        Log("cop diff: anchor %d is not a body of ours - refused", anchorVeh);
        return false;
    }
    if (ab->bike) {
        Log("cop diff: anchor %d is the bike - the diff is a car instrument, refused", anchorVeh);
        return false;
    }
    CVehicle* anchor = LiveVeh(*ab);
    if (!sTwin.used || sTwin.vehRef != twinVeh) {
        Log("cop diff: twin %d does not resolve - refused", twinVeh);
        return false;
    }
    CVehicle* twin = LiveTwinVeh(sTwin);
    if (!anchor || !twin) {
        Log("cop diff: a body no longer resolves (anchor %p, twin %p) - refused",
            (void*)anchor, (void*)twin);
        return false;
    }
    // the same-class proof: the diff is only meaningful over one
    // layout - both bodies must carry the same vtable word (both
    // are our own 596 spawns, and this is the mechanical check)
    static_assert(sizeof(CAutomobile) == kCopBodyExtent,
                  "the diff extent is the SDK's CAutomobile size");
    if (*reinterpret_cast<void**>(anchor) != *reinterpret_cast<void**>(twin)) {
        Log("cop diff: the two bodies are not the same class (vtables differ) - refused");
        return false;
    }
    // snapshots: the whole extent, taken now; the adoption below
    // writes from the twin SNAPSHOT, so the list the log prints and
    // the words that land are one and the same reading
    static unsigned char snapA[kCopBodyExtent];
    static unsigned char snapT[kCopBodyExtent];
    std::memcpy(snapA, anchor, kCopBodyExtent);
    std::memcpy(snapT, twin, kCopBodyExtent);
    static CopDiffWord words[kCopBodyWords];
    int totalDiff = 0, adoptable = 0;
    const int filled = CopBodyDiffWords(snapA, snapT, kCopBodyExtent,
                                        (unsigned)(uintptr_t)anchor,
                                        (unsigned)(uintptr_t)twin,
                                        words, (int)kCopBodyWords,
                                        &totalDiff, &adoptable);
    out->wordsTotal = (int)kCopBodyWords;
    out->wordsDiffer = totalDiff;
    for (int i = 0; i < filled; ++i)
        if (words[i].cls >= 0 && words[i].cls < CD_COUNT)
            ++out->classDiffer[words[i].cls];
    Log("cop diff: anchor %d vs twin %d - %d of %d words differ; classes:"
        " drive %d, header %d, structural %d, pointer %d, autopilot %d,"
        " identity %d, health %d, contact %d; adoptable %d",
        anchorVeh, twinVeh, totalDiff, (int)kCopBodyWords,
        out->classDiffer[CD_DRIVE], out->classDiffer[CD_HEADER],
        out->classDiffer[CD_STRUCT], out->classDiffer[CD_POINTER],
        out->classDiffer[CD_AUTOPILOT], out->classDiffer[CD_IDENTITY],
        out->classDiffer[CD_HEALTH], out->classDiffer[CD_CONTACT],
        adoptable);
    // the print: every differing word the name table knows gets its
    // own line whatever its class (the famous fields are the ones a
    // reader hunts first); every ADOPTABLE word gets its own line
    // (the list is the deliverable); the unnamed rest group into
    // runs of consecutive offsets of one class. Values print as hex,
    // plus a float reading when the bits make a sane one.
    auto valStr = [](unsigned w, char* buf, size_t n) {
        float f;
        std::memcpy(&f, &w, 4);
        if (f == f && std::fabs(f) > 1e-6f && std::fabs(f) < 1e7f)
            std::snprintf(buf, n, "%08X (%g)", w, f);
        else
            std::snprintf(buf, n, "%08X", w);
    };
    int adoptLines = 0;
    for (int i = 0; i < filled; ++i) {
        const CopDiffWord& w = words[i];
        const char* name = CopBodyWordName(w.offset);
        if (!w.adoptable && !name)
            continue;
        if (w.adoptable && !name && adoptLines >= 240)
            continue; // the cap: the summary line below carries the count
        char va[40], vt[40];
        valStr(w.anchor, va, sizeof va);
        valStr(w.twin, vt, sizeof vt);
        Log("cop diff: +0x%03X %-30s anchor %-18s twin %-18s %s [%s]",
            w.offset, name ? name : "", va, vt,
            w.adoptable ? "-> ADOPT" : "kept", CopBodyClassName(w.cls));
        if (w.adoptable && !name)
            ++adoptLines;
    }
    for (int i = 0; i < filled;) {
        const CopDiffWord& w0 = words[i];
        if (w0.adoptable || CopBodyWordName(w0.offset)) {
            ++i;
            continue;
        }
        unsigned lo = w0.offset, hi = w0.offset;
        const int cls = w0.cls;
        int n = 0;
        while (i < filled && !words[i].adoptable &&
               !CopBodyWordName(words[i].offset) && words[i].cls == cls &&
               words[i].offset <= hi + 4) {
            hi = words[i].offset;
            ++n;
            ++i;
        }
        Log("cop diff: +0x%03X..+0x%03X %s (%d words) - not adopted",
            lo, hi, CopBodyClassName(cls), n);
    }
    // the adoption: the engine writes the twin snapshot's word at
    // every adoptable offset, onto the live anchor, in this call
    out->adopted = CopBodyAdoptWords(reinterpret_cast<unsigned char*>(anchor),
                                     snapT, words, filled,
                                     out->adoptedOffsets, 96);
    out->adoptedListed = out->adopted < 96 ? out->adopted : 96;
    out->ok = true;
    Log("cop diff: adoption landed - %d words of the twin's drive state now stand on the anchor",
        out->adopted);
    return true;
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
