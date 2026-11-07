#include "GtaCop.h"

#include <cmath>

#include "CCopPed.h"
#include "CDecisionMakerTypes.h"
#include "CPools.h"
#include "CPlayerPed.h"
#include "CTaskComplexEnterCarAsDriver.h"
#include "CVehicle.h"
#include "CWorld.h"
#include "common.h"
#include "extensions/ScriptCommands.h"

#include "Cop.h"
#include "Core.h"
#include "GtaMobs.h"   // BridgePlayerShotAge
#include "PedSkins.h"  // PedSkinsExcludePed

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
    float seatT = 0.0f;      // seconds since spawn; the enter task's budget
    bool seatRetried = false;
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
};
CopBodyState sBodies[kMaxCopUnits];

// the player's own vehicle damage ledger (Pre keeps it) - it belongs to
// the player, not to any one body, so it stands apart from the bodies
CVehicle* sPlayerVeh = nullptr;
float sPlayerVehHealth = 0.0f;
float sPlayerImpactPeak = 0.0f;

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

float DisplayHealth(float book01) { return book01 > 0.0f ? 380.0f + 620.0f * book01 : 90.0f; }

void ApplyCommand(CVehicle* veh, const CopBodyState& b) {
    // The control surface, verified against the SDK headers in-tree:
    // m_fGasPedal 0x49C, m_fBreakPedal right behind it, m_fSteerAngle
    // 0x494 (CVehicle.h VALIDATE_OFFSETs), the siren as the vehicle's own
    // flag (bSirenOrAlarm) that the game's own ProcessSirenAndHorn reads.
    veh->m_fGasPedal = b.gas;
    veh->m_fBreakPedal = b.brake;
    veh->m_fSteerAngle = b.steer;
    veh->bSirenOrAlarm = b.siren;
    veh->bIsLawEnforcer = b.siren; // he is, while he pursues
    if (b.seated)
        veh->bEngineOn = true;
    // the displayed health is re-asserted with the pedals: the host's own
    // damage may have bitten into it this frame, and the core's Report
    // (which runs before Post) is where that bite was measured and priced
    if (b.book01 > 0.0f)
        veh->m_fHealth = DisplayHealth(b.book01);
}

} // namespace

void CopSessionReset() {
    // the world is gone (or going): forget every body and the ledger
    for (int i = 0; i < kMaxCopUnits; ++i)
        sBodies[i] = CopBodyState();
    sPlayerVeh = nullptr;
    sPlayerVehHealth = 0.0f;
    sPlayerImpactPeak = 0.0f;
}

void CopBodyPre(float dt) {
    // every body's driver, every frame (the one-unit law): what he lost
    // since the last frame is measured for his unit's book, and his
    // health is held back up - he goes down only when his unit retires
    for (int i = 0; i < kMaxCopUnits; ++i) {
        CopBodyState& b = sBodies[i];
        if (!b.used)
            continue;
        if (CPed* driver = LiveDriver(b)) {
            if (driver->m_fHealth < 1000.0f)
                b.driverHitAcc += 1000.0f - driver->m_fHealth;
            driver->m_fHealth = 1000.0f;
        }
        // seating: the enter task is the game's own; watch it land
        CVehicle* veh = LiveVeh(b);
        CPed* driver = LiveDriver(b);
        if (veh && driver && !b.seated) {
            b.seatT += dt;
            if (driver->m_pVehicle == veh && veh->m_pDriver == driver) {
                b.seated = true;
                Log("cop body: a driver is seated - the pedals are the core's from here");
            } else if (b.seatT > 8.0f && !b.seatRetried) {
                b.seatRetried = true;
                driver->m_pIntelligence->m_TaskMgr.SetTask(new CTaskComplexEnterCarAsDriver(veh),
                                                           TASK_PRIMARY_PRIMARY, false);
                Log("cop body: a driver had not sat in 8 s - the enter task was issued once more");
            } else if (b.seatT > 20.0f) {
                // said out loud, in the log the spike verdict is read from:
                // if this line ever prints, field control has no driver
                Log("cop body: A DRIVER NEVER SAT (20 s) - field control has no driver to work through");
                b.seatT = -1e9f; // say it once
            }
        }
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
        if (!b.used || !b.haveCmd)
            continue;
        if (CVehicle* veh = LiveVeh(b))
            ApplyCommand(veh, b);
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
    int handle = 0;
    Command<Commands::CREATE_CAR>(kCopCarModel, at.x, at.y, gz + 0.6f, &handle);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kCopCarModel);
    auto* vpool = CPools::ms_pVehiclePool;
    CVehicle* veh = vpool ? vpool->GetAt(handle) : nullptr;
    if (!veh) {
        Log("cop body: CREATE_CAR made no vehicle - spawn refused");
        s = CopBodyState();
        return -1;
    }
    // turn his nose to the man
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
    s.veh = veh;
    s.vehRef = vpool->GetRef(veh);
    s.book01 = 1.0f;
    s.lastSetHealth = veh->m_fHealth;

    // the driver: the game's own cop kind, pool-built and contained from
    // birth - the boss body's discipline, word for word (PED_EMPTY, senses
    // zeroed, mission-created so population never takes him)
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool) {
        s = CopBodyState();
        return -1;
    }
    CCopPed* cop = new CCopPed(COP_TYPE_CITYCOP);
    CPed* driver = cop;
    if (!driver) {
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
        Log("cop body: the driver did not land in the ped pool (index %d) - spawn refused", idx);
        s = CopBodyState();
        return -1;
    }
    if (driver->m_pIntelligence) {
        driver->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        driver->m_pIntelligence->SetSeeingRange(0.0f);
        driver->m_pIntelligence->SetHearingRange(0.0f);
    }
    driver->m_fMaxHealth = 1000.0f; // held at the top by Pre, like the
    driver->m_fHealth = 1000.0f;    // boss's body: he is the unit's face,
                                    // not a life of his own
    // stand him at his own door and give him the game's own enter task -
    // the one door the SDK opens into a car (the drive task itself is not
    // in the SDK, and no task is ever built from raw addresses here)
    const CVector door(at.x - fwd.y * 1.3f, at.y + fwd.x * 1.3f, gz + 1.0f);
    driver->m_matrix->pos = door;
    driver->UpdateRwMatrix();
    driver->m_pIntelligence->m_TaskMgr.SetTask(new CTaskComplexEnterCarAsDriver(veh),
                                               TASK_PRIMARY_PRIMARY, false);
    s.driver = driver;
    s.driverRef = ppool->GetRef(driver);
    PedSkinsExcludePed(driver);
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
    out->health = v->m_fHealth / 1000.0f; // the DISPLAY's fraction; the
                                          // book itself lives in the core
    // READ BACK from the vehicle itself - never the stored command. If the
    // game's own processing overwrote a pedal between our two asserts,
    // this is the number that says so.
    out->gas = v->m_fGasPedal;
    out->brake = v->m_fBreakPedal;
    out->steer = v->m_fSteerAngle;
    out->siren = v->bSirenOrAlarm;
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
    // for the felony; the player's gun fresh (< 1 s, the boss relay's own
    // clock) prices a shot; the residue is the world's (walls, fire)
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
        else if (BridgePlayerShotAge() < 1.0f)
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

void GtaCopBodyRetire(int veh, bool burnOut) {
    CopBodyState* bp = FindBody(veh);
    if (!bp)
        return;
    CopBodyState& s = *bp;
    CVehicle* v = LiveVeh(s);
    CPed* driver = LiveDriver(s);
    if (burnOut) {
        // his book is spent: the unit goes down together - the driver
        // slumps in his seat by the game's own hand (health zeroed, the
        // corpse is the city's to clean) and the car is handed to the
        // fire at burn-band health. Nothing of ours is left pointing at
        // either body. ONLY this body is forgotten - the force drives on.
        if (driver)
            driver->m_fHealth = 0.0f;
        if (v)
            v->m_fHealth = 90.0f;
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

} // namespace mc
