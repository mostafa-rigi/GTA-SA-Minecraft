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

#include "Core.h"
#include "GtaMobs.h"   // BridgePlayerShotAge
#include "PedSkins.h"  // PedSkinsExcludePed

using namespace plugin;

namespace mc {
namespace {

constexpr unsigned char kCreatedByMission = 2;
constexpr int kCopCarModel = 596;   // SA's LSPD cruiser: the stand-in body
constexpr int kCopDriverModel = 280; // SA's LSPD officer: the stand-in driver

struct CopBodyState {
    CVehicle* veh = nullptr;
    int vehRef = -1;
    CPed* driver = nullptr;
    int driverRef = -1;
    bool seated = false;
    float seatT = 0.0f;      // seconds since spawn; the enter task's budget
    bool seatRetried = false;
    float lastHealth = 1000.0f;
    float struckRam = 0.0f;  // priced since the last report, consumed by it
    float struckShot = 0.0f;
    // the core's last command, re-asserted by Post every frame
    float gas = 0.0f, brake = 1.0f, steer = 0.0f;
    bool siren = false;
    bool haveCmd = false;
    // the player's own vehicle damage ledger (Pre keeps it)
    CVehicle* playerVeh = nullptr;
    float playerVehHealth = 0.0f;
    float playerImpactPeak = 0.0f;
};
CopBodyState s;

// Pool-ref discipline, the boss body's own law: a stored pointer is only
// ever touched after the pool itself confirms the slot still holds it.
CVehicle* LiveVeh(int ref) {
    if (ref < 0 || !CPools::ms_pVehiclePool)
        return nullptr;
    CVehicle* v = CPools::ms_pVehiclePool->GetAtRef(ref);
    return v == s.veh ? v : nullptr;
}
CPed* LiveDriver() {
    if (s.driverRef < 0 || !CPools::ms_pPedPool)
        return nullptr;
    CPed* p = CPools::ms_pPedPool->GetAtRef(s.driverRef);
    return p == s.driver ? p : nullptr;
}

void ApplyCommand(CVehicle* veh) {
    // The control surface, verified against the SDK headers in-tree:
    // m_fGasPedal 0x49C, m_fBreakPedal right behind it, m_fSteerAngle
    // 0x494 (CVehicle.h VALIDATE_OFFSETs), the siren as the vehicle's own
    // flag (bSirenOrAlarm) that the game's own ProcessSirenAndHorn reads.
    veh->m_fGasPedal = s.gas;
    veh->m_fBreakPedal = s.brake;
    veh->m_fSteerAngle = s.steer;
    veh->bSirenOrAlarm = s.siren;
    veh->bIsLawEnforcer = s.siren; // he is, while he pursues
    if (s.seated)
        veh->bEngineOn = true;
}

} // namespace

void CopSessionReset() {
    // the world is gone (or going): forget hands and pointers alike
    s = CopBodyState();
}

void CopBodyPre(float dt) {
    (void)dt;
    // the player's own car, priced per frame: the felony system's ears
    // for "he hit something" are his own car's health steps, peaked so a
    // hard hit is still on the books when the core asks once per tick
    CVehicle* pv = FindPlayerVehicle();
    if (pv != s.playerVeh) {
        s.playerVeh = pv;
        s.playerVehHealth = pv ? pv->m_fHealth : 0.0f;
    } else if (pv) {
        const float lost = s.playerVehHealth - pv->m_fHealth;
        if (lost > s.playerImpactPeak)
            s.playerImpactPeak = lost;
        if (pv->m_fHealth > s.playerVehHealth || lost > 0.0f)
            s.playerVehHealth = pv->m_fHealth; // repairs re-baseline; a
                // slow grind keeps stepping down from the last reading
    }
    // seating: the enter task is the game's own; watch it land
    CVehicle* veh = LiveVeh(s.vehRef);
    CPed* driver = LiveDriver();
    if (veh && driver && !s.seated) {
        s.seatT += dt;
        if (driver->m_pVehicle == veh && veh->m_pDriver == driver) {
            s.seated = true;
            Log("cop body: the driver is seated - the pedals are the core's from here");
        } else if (s.seatT > 8.0f && !s.seatRetried) {
            s.seatRetried = true;
            driver->m_pIntelligence->m_TaskMgr.SetTask(new CTaskComplexEnterCarAsDriver(veh),
                                                       TASK_PRIMARY_PRIMARY, false);
            Log("cop body: the driver had not sat in 8 s - the enter task was issued once more");
        } else if (s.seatT > 20.0f) {
            // said out loud, in the log the spike verdict is read from:
            // if this line ever prints, piece 0 is NO-GO in the game
            Log("cop body: THE DRIVER NEVER SAT (20 s) - field control has no driver to work through");
            s.seatT = -1e9f; // say it once
        }
    }
}

void CopBodyPost() {
    CVehicle* veh = LiveVeh(s.vehRef);
    if (veh && s.haveCmd)
        ApplyCommand(veh);
}

int GtaSpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face) {
    (void)headingDeg; // the face point decides the nose, like the original's
                      // justPinged: a fresh cop starts facing his man
    CopSessionReset();
    Command<Commands::REQUEST_MODEL>(kCopCarModel);
    Command<Commands::REQUEST_MODEL>(kCopDriverModel);
    Command<Commands::LOAD_ALL_MODELS_NOW>();
    if (!Command<Commands::HAS_MODEL_LOADED>(kCopCarModel) ||
        !Command<Commands::HAS_MODEL_LOADED>(kCopDriverModel))
        return -1;
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
        Log("cop body: CREATE_CAR made no vehicle - summon refused");
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
    s.lastHealth = veh->m_fHealth;

    // the driver: the game's own cop kind, pool-built and contained from
    // birth - the boss body's discipline, word for word (PED_EMPTY, senses
    // zeroed, mission-created so population never takes him)
    auto* ppool = CPools::ms_pPedPool;
    if (!ppool)
        return -1;
    CCopPed* cop = new CCopPed(COP_TYPE_CITYCOP);
    CPed* driver = cop;
    if (!driver)
        return -1;
    driver->SetModelIndex(kCopDriverModel);
    Command<Commands::MARK_MODEL_AS_NO_LONGER_NEEDED>(kCopDriverModel);
    driver->SetCharCreatedBy(kCreatedByMission);
    driver->ClearWeapons();
    CWorld::Add(driver);
    const int idx = ppool->GetIndex(driver);
    if (idx < 0 || idx >= ppool->m_nSize || ppool->GetAt(idx) != driver || ppool->IsFreeSlotAtIndex(idx)) {
        CWorld::Remove(driver);
        delete driver;
        Log("cop body: the driver did not land in the ped pool (index %d) - summon refused", idx);
        return -1;
    }
    if (driver->m_pIntelligence) {
        driver->m_pIntelligence->SetPedDecisionMakerType(eDecisionMakerType::PED_EMPTY);
        driver->m_pIntelligence->SetSeeingRange(0.0f);
        driver->m_pIntelligence->SetHearingRange(0.0f);
    }
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
    CVehicle* v = LiveVeh(veh);
    if (!v || !out)
        return false;
    CPed* driver = LiveDriver();
    if (driver && driver->m_fHealth <= 0.0f)
        return false; // a dead driver drives nothing: the body is done
    out->ok = true;
    out->pos = Vec3(v->m_matrix->pos.x, v->m_matrix->pos.y, v->m_matrix->pos.z);
    out->fwd = Vec3(v->m_matrix->up.x, v->m_matrix->up.y, v->m_matrix->up.z);
    out->vel = Vec3(v->m_vecMoveSpeed.x, v->m_vecMoveSpeed.y, v->m_vecMoveSpeed.z) * 50.0f;
    out->health = v->m_fHealth / 1000.0f;
    // READ BACK from the vehicle itself - never the stored command. If the
    // game's own processing overwrote a pedal between our two asserts,
    // this is the number that says so.
    out->gas = v->m_fGasPedal;
    out->brake = v->m_fBreakPedal;
    out->steer = v->m_fSteerAngle;
    out->siren = v->bSirenOrAlarm;
    // the damage since the last report, split by how it arrived: his car
    // close (his own vehicle within 6 m) prices a ram; the player's gun
    // fresh (< 1 s, the boss relay's own clock) prices a shot; a loss far
    // from both is the world's own crash and bills nobody
    const float lost = s.lastHealth - v->m_fHealth;
    if (lost > 0.0f) {
        s.lastHealth = v->m_fHealth;
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
    } else if (v->m_fHealth > s.lastHealth) {
        s.lastHealth = v->m_fHealth; // a repair re-baselines the ledger
    }
    out->struckRam = s.struckRam;
    out->struckShot = s.struckShot;
    s.struckRam = 0.0f;
    s.struckShot = 0.0f;
    return true;
}

void GtaCopBodyDrive(int veh, float gas, float brake, float steer, bool siren) {
    if (veh != s.vehRef)
        return;
    s.gas = gas;
    s.brake = brake;
    s.steer = steer;
    s.siren = siren;
    s.haveCmd = true;
    CVehicle* v = LiveVeh(veh);
    if (v)
        ApplyCommand(v); // now; Post asserts it again after the game
}

void GtaCopBodyRetire(int veh) {
    if (veh != s.vehRef)
        return;
    CVehicle* v = LiveVeh(veh);
    CPed* driver = LiveDriver();
    if (driver) {
        CWorld::Remove(driver);
        delete driver;
    }
    if (v) {
        CWorld::Remove(v);
        delete v;
    }
    CopSessionReset();
}

float GtaPlayerVehicleImpact() {
    const float peak = s.playerImpactPeak;
    s.playerImpactPeak = 0.0f;
    return peak;
}

} // namespace mc
