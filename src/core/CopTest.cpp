#include "CopTest.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "Chat.h"
#include "Cop.h"
#include "Host.h"
#include "NavGraph.h"

// The /coptest state machine. Design rules, all from the brief:
//  - every scenario grades OUTCOMES off body reports (positions,
//    speeds, health, the force's own books) - never intentions; a
//    scenario that cannot be staged honestly says SKIP and is not
//    counted, never dressed as a PASS;
//  - the decoy phases run with the prey override armed (the force
//    hunts the decoy); the two player phases run with it off and grade
//    the user's own hand;
//  - the run cleans up after itself: decoys retired, the force stood
//    down, the felony page restored to the value it had at the start.

namespace mc {
namespace {

enum Phase {
    PH_NONE,
    PH_SPIKE,
    PH_PURSUIT,
    PH_RUNOVER,
    PH_RECOVERY,
    PH_BLOCK,
    PH_PLAYER1,
    PH_PLAYER2_WAIT,
    PH_PLAYER2_BAIL,
    PH_FINISH
};

struct Result {
    char name[24];
    bool pass;
    bool counted; // SKIP lines are printed but never counted
    char numbers[192];
};

bool sRunning = false;
int sGuard = 0; // >0 while the test itself summons/dismisses: the
                // CopReset inside those calls must not abort the run
Phase sPhase = PH_NONE;
bool sEntered = false;
float sPhaseT = 0.0f;
float sTotalT = 0.0f;
Result sResults[12];
int sResultN = 0;
float sSavedFelony = 0.0f;

int sDecoyCar = -1;
int sDecoyPed = -1;
std::vector<Vec3> sRoute;
size_t sRouteIdx = 0;

// spike
Vec3 sSpikeStart;
float sSpikeSum = 0.0f, sSpikeWt = 0.0f;
// pursuit
float sStartDist = 0.0f;
bool sPursuitOpen = false;
float sMinDist = 1e9f, sPrevDist = 1e9f, sPrevDistT = 0.0f;
bool sArrived = false;
float sArriveT = 0.0f;
bool sRam = false;
float sRamClose = 0.0f;
int sSirenOn = 0, sSirenN = 0;
float sSampleT = 0.0f;
// run-over
Vec3 sPedStand;
float sPedHealth0 = 0.0f;
float sRoMin = 1e9f;
float sRoStillT = 0.0f;
// recovery
int sRecSlot = -1;
Vec3 sWedgePos;
int sRecCount0 = 0;
bool sRecFreed = false;
float sRecFreeT = 0.0f;
// block
float sBlockLatchT = 0.0f;
bool sBlockPlacedSeen = false;
float sBlockMinDist = 1e9f;
// player phases
float sP1FelonyPeak = 0.0f;
float sP2BailDist = 0.0f, sP2MinAfter = 1e9f, sP2DownT = 0.0f;
bool sP2BailSet = false;
// soak (the whole run): the worst continuous freeze of a pursuing unit
float sFreeze[kMaxCopUnits] = {};
float sWorstFreeze = 0.0f;
int sWorstSlot = -1;

void Say(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Log("%s", buf);
    ChatPrint(buf);
}

void Grade(const char* name, bool pass, bool counted, const char* fmt, ...) {
    char nums[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(nums, sizeof nums, fmt, ap);
    va_end(ap);
    if (sResultN < 12) {
        Result& r = sResults[sResultN++];
        std::snprintf(r.name, sizeof r.name, "%s", name);
        r.pass = pass;
        r.counted = counted;
        std::snprintf(r.numbers, sizeof r.numbers, "%s", nums);
    }
    if (!counted)
        Say("coptest: SKIP %s %s", name, nums);
    else
        Say("coptest: %s %s %s", pass ? "PASS" : "FAIL", name, nums);
}

void GoPhase(Phase p) {
    sPhase = p;
    sEntered = false;
    sPhaseT = 0.0f;
}

Vec3 PlayerNow() {
    Vec3 p(0, 0, 0);
    TheHost().PlayerPos(&p);
    return p;
}

bool GuardSummon(const Vec3& at, const Vec3& playerPos) {
    ++sGuard;
    const bool ok = CopSummon(at, playerPos);
    --sGuard;
    return ok;
}

void GuardDismiss() {
    ++sGuard;
    CopDismiss();
    --sGuard;
}

// the nearest living unit to a point (optionally pursuers only)
float NearestUnit(const Vec3& p, bool pursuersOnly, int* slotOut) {
    float best = 1e9f;
    int bs = -1;
    for (int i = 0; i < kMaxCopUnits; ++i) {
        const CopState& c = gCops[i];
        if (c.veh < 0 || c.mode == COP_NONE)
            continue;
        if (pursuersOnly && c.mode != COP_PURSUE)
            continue;
        const float d = (c.pos - p).Length();
        if (d < best) {
            best = d;
            bs = i;
        }
    }
    if (slotOut)
        *slotOut = bs;
    return best;
}

// a loop of Grove's own streets for the decoy: a square of road routes
// around its start; if the square will not lay, a there-and-back route
bool BuildLoop(const Vec3& start) {
    sRoute.clear();
    sRouteIdx = 0;
    static const float kOff[4][2] = { { 95, 0 }, { 95, 95 }, { 0, 95 }, { 0, 0 } };
    Vec3 cur = start;
    std::vector<Vec3> loop;
    bool ok = true;
    for (int i = 0; i < 4 && ok; ++i) {
        const Vec3 dst = start + Vec3(kOff[i][0], kOff[i][1], 0.0f);
        std::vector<Vec3> leg;
        if (!VehicleRoute(cur, dst, &leg, nullptr) || leg.size() < 2) {
            ok = false;
            break;
        }
        for (const Vec3& p : leg)
            loop.push_back(p);
        cur = dst;
    }
    if (ok && loop.size() >= 8) {
        sRoute = loop;
        return true;
    }
    std::vector<Vec3> leg;
    const Vec3 dst = start + Vec3(130, 50, 0);
    if (VehicleRoute(start, dst, &leg, nullptr) && leg.size() >= 2) {
        sRoute = leg;
        for (size_t i = leg.size() - 1; i-- > 0;)
            sRoute.push_back(leg[i]);
        return true;
    }
    return false;
}

// drive the decoy along its loop: the game's own autopilot, fed a
// look-ahead point down the route, the way the spike feeds its own
// A long straight there-and-back along one road: the roadblock
// phase's shape. A block belongs on a straight road ahead of a
// coming car, and a square loop defeats the phase twice over - the
// chase's sight line cuts through the block interiors at every
// corner, so the page loses the position (256 unseen ticks, the
// original's own law) before the block timer can ever fire.
bool BuildStraight(const Vec3& start, const Vec3& dir) {
    sRoute.clear();
    sRouteIdx = 0;
    std::vector<Vec3> leg;
    const Vec3 dst = start + dir * 230.0f;
    if (!VehicleRoute(start, dst, &leg, nullptr) || leg.size() < 2)
        return false;
    sRoute = leg;
    for (size_t i = leg.size() - 1; i-- > 0;)
        sRoute.push_back(leg[i]);
    return true;
}

void DriveDecoy(float cruise) {
    if (sDecoyCar < 0)
        return;
    Vec3 pos, vel;
    if (!TheHost().DecoyReport(sDecoyCar, &pos, &vel))
        return;
    if (sRoute.empty())
        return;
    for (int guard = 0; guard < 8; ++guard) {
        Vec3 d = sRoute[sRouteIdx] - pos;
        d.z = 0.0f;
        if (d.Length() < 9.0f)
            sRouteIdx = (sRouteIdx + 1) % sRoute.size();
        else
            break;
    }
    Vec3 feed = sRoute[sRouteIdx];
    Vec3 prev = pos;
    float covered = 0.0f;
    for (size_t k = 0; k < sRoute.size(); ++k) {
        const Vec3& p = sRoute[(sRouteIdx + k) % sRoute.size()];
        Vec3 seg = p - prev;
        seg.z = 0.0f;
        covered += seg.Length();
        feed = p;
        prev = p;
        if (covered >= 18.0f)
            break;
    }
    TheHost().DecoyCarDrive(sDecoyCar, feed, cruise);
}

Vec3 DecoyPos() {
    Vec3 p(0, 0, 0), v;
    if (sDecoyCar >= 0)
        TheHost().DecoyReport(sDecoyCar, &p, &v);
    return p;
}

bool SpawnDecoyCarNear(const Vec3& anchor, float dist, const Vec3& faceDir) {
    Vec3 want = anchor + faceDir * dist;
    Vec3 at = want;
    const int node = VehicleNearestNode(want, 40.0f);
    if (node >= 0 && VehicleNodePos(node, &at))
        want = at;
    const float heading = std::atan2(faceDir.x, faceDir.y) * 57.29578f;
    sDecoyCar = TheHost().SpawnDecoyCar(want, heading);
    return sDecoyCar >= 0;
}

void Cleanup() {
    if (sDecoyCar >= 0) {
        TheHost().RetireDecoy(sDecoyCar);
        sDecoyCar = -1;
    }
    if (sDecoyPed >= 0) {
        TheHost().RetireDecoyPed(sDecoyPed);
        sDecoyPed = -1;
    }
    CopPreyOverride(-1, -1);
    GuardDismiss();
    CopForceFelony(sSavedFelony);
    Say("coptest: cleanup - decoys retired, force stood down, felony page restored to %.0f",
        sSavedFelony);
}

// the last run's sheet, kept for the suite's hook after Finish
Result sSheet[12];
int sSheetN = 0;

void Finish() {
    int n = 0, passed = 0;
    for (int i = 0; i < sResultN; ++i) {
        if (!sResults[i].counted)
            continue;
        ++n;
        if (sResults[i].pass)
            ++passed;
    }
    sSheetN = sResultN;
    for (int i = 0; i < sResultN; ++i)
        sSheet[i] = sResults[i];
    Say("coptest: DONE %d/%d PASS (%.0f s)", passed, n, sTotalT);
    for (int i = 0; i < sResultN; ++i)
        if (sResults[i].counted && !sResults[i].pass)
            Say("coptest: FAILED: %s %s", sResults[i].name, sResults[i].numbers);
    Cleanup();
    sRunning = false;
    sPhase = PH_NONE;
}

// the no-freeze soak, sampled across every phase: a pursuing unit with
// a live feed sitting under 0.3 m/s - the frozen-unit signature of the
// 02:00 session - may never hold for more than 5 s
void SoakTick(float dt) {
    for (int i = 0; i < kMaxCopUnits; ++i) {
        const CopState& c = gCops[i];
        const bool frozen = c.veh >= 0 && c.mode == COP_PURSUE && c.feedLive &&
                            std::fabs(c.speed) < 0.3f;
        if (frozen) {
            sFreeze[i] += dt;
            if (sFreeze[i] > sWorstFreeze) {
                sWorstFreeze = sFreeze[i];
                sWorstSlot = i;
            }
        } else {
            sFreeze[i] = 0.0f;
        }
    }
}

void PhaseSpike(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: --- spike: the anchor drives his Grove route (watch his pace) ---");
        const Vec3 p = PlayerNow();
        GuardDismiss();
        if (!GuardSummon(p + Vec3(6, 0, 0), p)) {
            Grade("spike", false, true, "the anchor's body could not be made");
            GoPhase(PH_PURSUIT);
            return;
        }
        sSpikeStart = gCop.pos;
        sSpikeSum = 0.0f;
        sSpikeWt = 0.0f;
    }
    if (!gCop.steerCalDone)
        return; // the calibration second is not the route
    if (gCop.mode == COP_SPIKE) {
        sSpikeSum += std::fabs(gCop.speed) * dt;
        sSpikeWt += dt;
    }
    const float disp = (gCop.pos - sSpikeStart).Length();
    if (gCop.spikeDone || sPhaseT > 45.0f) {
        const float avg = sSpikeWt > 1.0f ? sSpikeSum / sSpikeWt : 0.0f;
        Grade("spike", avg >= 4.0f && disp >= 60.0f, true,
              "avg speed %.1f m/s over %.0f s, displacement %.0f m (bar: >=4 m/s, >=60 m)",
              avg, sSpikeWt, disp);
        GoPhase(PH_PURSUIT);
    } else if (sPhaseT > 90.0f) {
        Grade("spike", false, true, "the route never finished in 90 s (displacement %.0f m)", disp);
        GoPhase(PH_PURSUIT);
    }
}

void PhasePursuit(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: --- pursuit: the force hunts a decoy car at felony 4096 (arrival, ram, siren) ---");
        if (sDecoyCar >= 0) {
            TheHost().RetireDecoy(sDecoyCar);
            sDecoyCar = -1;
        }
        // the decoy starts ~58 m ahead of the anchor's nose, on the road
        Vec3 fwd = gCop.fwd;
        fwd.z = 0.0f;
        if (fwd.Length() < 0.1f)
            fwd = Vec3(0, 1, 0);
        fwd = fwd * (1.0f / fwd.Length());
        if (!SpawnDecoyCarNear(gCop.pos, 68.0f, fwd) || !BuildLoop(DecoyPos())) {
            Grade("arrival", false, true, "no decoy could be staged");
            Grade("ram", false, true, "no decoy could be staged");
            Grade("siren", false, true, "no decoy could be staged");
            GoPhase(PH_RUNOVER);
            return;
        }
        CopPreyOverride(sDecoyCar, -1);
        CopForceFelony(4096.0f);
        CopKnownSeed(); // the dispatcher's call: the car is reported
        sPursuitOpen = false;
        sMinDist = 1e9f;
        sPrevDist = 1e9f;
        sPrevDistT = 0.0f;
        sArrived = false;
        sRam = false;
        sSirenOn = 0;
        sSirenN = 0;
        sSampleT = 0.0f;
    }
    CopPreyOverride(sDecoyCar, -1); // re-asserted: a reset inside the
                                    // run (a wreck retirement) clears it
    DriveDecoy(11.0f);
    Vec3 dp, dv;
    if (!TheHost().DecoyReport(sDecoyCar, &dp, &dv)) {
        Grade("arrival", false, true, "the decoy was lost mid-scenario");
        Grade("ram", false, true, "the decoy was lost mid-scenario");
        Grade("siren", false, true, "the decoy was lost mid-scenario");
        GoPhase(PH_RUNOVER);
        return;
    }
    int nearSlot = -1;
    const float d = NearestUnit(dp, true, &nearSlot);
    if (!sPursuitOpen && CopPursuingCount() > 0) {
        sPursuitOpen = true;
        int anySlot = -1;
        sStartDist = NearestUnit(dp, false, &anySlot);
        Say("coptest: pursuit open - the force starts %.0f m out", sStartDist);
    }
    if (sPursuitOpen) {
        if (d < sMinDist)
            sMinDist = d;
        if (!sArrived && d < 4.0f) {
            sArrived = true;
            sArriveT = sPhaseT;
        }
        // the ram: contact read off the bodies - centres within 3.4 m
        // while the gap is closing at 5 m/s or better
        sPrevDistT += dt;
        if (sPrevDistT >= 0.25f) {
            if (sPrevDist < 1e8f) {
                const float closing = (sPrevDist - d) / sPrevDistT;
                if (d <= 3.4f && closing >= 5.0f && !sRam) {
                    sRam = true;
                    sRamClose = closing;
                }
            }
            sPrevDist = d;
            sPrevDistT = 0.0f;
        }
        sSampleT += dt;
        if (sSampleT >= 1.0f) {
            sSampleT = 0.0f;
            if (nearSlot >= 0) {
                ++sSirenN;
                if (gCops[nearSlot].repSiren)
                    ++sSirenOn;
            }
        }
    }
    const bool doneEarly = sArrived && sRam && sPhaseT > 12.0f;
    if (doneEarly || sPhaseT > 70.0f) {
        Grade("arrival", sArrived && sArriveT <= 45.0f && sStartDist >= 50.0f, true,
              "from %.0f m to %.1f m in %.0f s (bar: <4 m within 45 s from >=50 m)",
              sStartDist, sMinDist, sArrived ? sArriveT : sPhaseT);
        Grade("ram", sRam, true,
              "contact at closing %.1f m/s (bar: centres <=3.4 m at >=5 m/s closing)",
              sRamClose);
        const float ratio = sSirenN > 0 ? (float)sSirenOn / (float)sSirenN : 0.0f;
        Grade("siren", sSirenN >= 10 && ratio >= 0.95f, true,
              "siren on in %d/%d per-second samples (bar: >=95%%)", sSirenOn, sSirenN);
        GoPhase(PH_RUNOVER);
    }
}

void PhaseRunover(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: --- run-over stance: a man stands in the road; the force must reach HIM ---");
        if (sDecoyCar >= 0) {
            TheHost().RetireDecoy(sDecoyCar);
            sDecoyCar = -1;
        }
        int nearSlot = -1;
        NearestUnit(PlayerNow(), false, &nearSlot);
        const CopState& ref = nearSlot >= 0 ? gCops[nearSlot] : gCop;
        Vec3 fwd = ref.fwd;
        fwd.z = 0.0f;
        if (fwd.Length() < 0.1f)
            fwd = Vec3(0, 1, 0);
        fwd = fwd * (1.0f / fwd.Length());
        Vec3 want = ref.pos + fwd * 30.0f;
        const int node = VehicleNearestNode(want, 30.0f);
        if (node >= 0 && VehicleNodePos(node, &want))
            ;
        sDecoyPed = TheHost().SpawnDecoyPed(want);
        if (sDecoyPed < 0) {
            Grade("runover", false, true, "no decoy ped could be staged");
            GoPhase(PH_RECOVERY);
            return;
        }
        sPedStand = want;
        float hp = 0.0f;
        Vec3 pp;
        if (TheHost().DecoyPedReport(sDecoyPed, &pp, &hp)) {
            sPedStand = pp;
            sPedHealth0 = hp;
        }
        CopPreyOverride(-1, sDecoyPed);
        CopForceFelony(4096.0f);
        CopKnownSeed();
        sRoMin = 1e9f;
        sRoStillT = 0.0f;
    }
    CopPreyOverride(-1, sDecoyPed);
    Vec3 pp;
    float hp = 0.0f;
    if (!TheHost().DecoyPedReport(sDecoyPed, &pp, &hp)) {
        Grade("runover", false, true, "the decoy ped was lost mid-scenario");
        GoPhase(PH_RECOVERY);
        return;
    }
    const float d = NearestUnit(pp, true, nullptr);
    if (d < sRoMin)
        sRoMin = d;
    const float displaced = (pp - sPedStand).Length();
    const bool hit = hp < sPedHealth0 - 1.0f || displaced > 1.0f;
    if (sRoMin <= 3.5f || hit) {
        Grade("runover", true, true,
              "closest centre %.1f m, ped displaced %.1f m, health %.0f of %.0f (bar: <=3.5 m or the hit itself)",
              sRoMin, displaced, hp, sPedHealth0);
        GoPhase(PH_RECOVERY);
        return;
    }
    // the stop-short ring: a pursuer parked in the 3.5..9 m band
    int slot = -1;
    const float nd = NearestUnit(pp, true, &slot);
    if (slot >= 0 && nd > 3.5f && nd <= 9.0f && std::fabs(gCops[slot].speed) < 0.5f)
        sRoStillT += dt;
    else
        sRoStillT = 0.0f;
    if (sRoStillT > 6.0f) {
        Grade("runover", false, true,
              "a unit stopped short in the ring at %.1f m and held %.0f s", nd, sRoStillT);
        GoPhase(PH_RECOVERY);
        return;
    }
    if (sPhaseT > 35.0f) {
        Grade("runover", false, true, "never reached him in 35 s (closest %.1f m)", sRoMin);
        GoPhase(PH_RECOVERY);
    }
}

void PhaseRecovery(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: --- recovery: a unit is wedged nose-to-wall; he must free himself ---");
        // the wall the probes hunt stands where the decoy man stood
        // (his house front); capture his spot before he is retired,
        // and take the unit nearest HIM - the unit that just ran the
        // run-over act beside that wall. (The first cut probed from
        // the unit nearest the PLAYER: a ladder unit idling by the
        // player could be 50 m from the only wall on the ground, and
        // the scenario skipped itself.)
        Vec3 pedSpot = PlayerNow();
        if (sDecoyPed >= 0) {
            Vec3 dp;
            float dhp = 0.0f;
            if (TheHost().DecoyPedReport(sDecoyPed, &dp, &dhp))
                pedSpot = dp;
            TheHost().RetireDecoyPed(sDecoyPed);
            sDecoyPed = -1;
        }
        // stage: find a wall face within reach of a living unit,
        // then place that unit nose-to-it and put the target beyond
        // the far side
        int slot = -1;
        NearestUnit(pedSpot, false, &slot);
        sRecSlot = slot;
        bool staged = false;
        Vec3 wallDir, wallAt;
        if (slot >= 0) {
            const Vec3 base = gCops[slot].pos;
            for (int i = 0; i < 16 && !staged; ++i) {
                const float a = (float)i * (6.2831853f / 16.0f);
                const Vec3 dir(std::sin(a), std::cos(a), 0.0f);
                for (float probe = 3.0f; probe <= 14.0f; probe += 1.0f) {
                    const Vec3 a0 = base + dir * (probe - 1.0f);
                    const Vec3 a1 = base + dir * probe;
                    if (TheHost().LineBlocked(a0, a1)) {
                        wallDir = dir;
                        wallAt = a1;
                        staged = true;
                        break;
                    }
                }
            }
            if (staged) {
                CopState& c = gCops[slot];
                const Vec3 place = wallAt - wallDir * 2.85f;
                const float heading = std::atan2(wallDir.x, wallDir.y) * 57.29578f;
                TheHost().CopBodyPlace(c.veh, place, heading);
                sWedgePos = place;
                sRecCount0 = c.recoverCount;
                // the target: on the road 55 m past the wall's far
                // side - he must back out and route around the block
                Vec3 want = wallAt + wallDir * 55.0f;
                const int node = VehicleNearestNode(want, 45.0f);
                if (node >= 0 && VehicleNodePos(node, &want))
                    ;
                sDecoyPed = TheHost().SpawnDecoyPed(want);
                if (sDecoyPed >= 0) {
                    CopPreyOverride(-1, sDecoyPed);
                    CopForceFelony(4096.0f);
                    CopKnownSeed();
                } else {
                    staged = false;
                }
            }
        }
        if (!staged) {
            Grade("recovery", false, false,
                  "no wall within staging reach - the scenario could not be staged honestly here");
            GoPhase(PH_BLOCK);
            return;
        }
        sRecFreed = false;
        return; // the grading starts next tick: this tick's unit
                // positions are still the PRE-placement reports, and
                // judging the wedge against them would "free" him
                // before he was ever wedged
    }
    CopPreyOverride(-1, sDecoyPed);
    const CopState& c = gCops[sRecSlot];
    const float fromWedge = (c.pos - sWedgePos).Length();
    const int recs = c.recoverCount - sRecCount0;
    if (!sRecFreed && fromWedge >= 3.0f) {
        sRecFreed = true;
        sRecFreeT = sPhaseT;
        Say("coptest: the wedged unit is free (%.0f s, %d recoveries)", sPhaseT, recs);
    }
    if (sRecFreed && sPhaseT > sRecFreeT + 4.0f) {
        Vec3 pp;
        float hp = 0.0f;
        float toTarget = 1e9f;
        if (sDecoyPed >= 0 && TheHost().DecoyPedReport(sDecoyPed, &pp, &hp))
            toTarget = (c.pos - pp).Length();
        Grade("recovery", recs <= 4, true,
              "freed in %.0f s with %d recoveries (bar: <=4), then closed to %.0f m of the target",
              sRecFreeT, recs, toTarget);
        GoPhase(PH_BLOCK);
        return;
    }
    if (sPhaseT > 50.0f) {
        Grade("recovery", false, true,
              "still wedged after 50 s (moved %.1f m, %d recoveries)", fromWedge, recs);
        GoPhase(PH_BLOCK);
    }
}

void PhaseBlock(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: --- roadblock: the chain places a block on the decoy's road; it must wake ---");
        if (sDecoyPed >= 0) {
            TheHost().RetireDecoyPed(sDecoyPed);
            sDecoyPed = -1;
        }
        if (sDecoyCar >= 0) {
            TheHost().RetireDecoy(sDecoyCar);
            sDecoyCar = -1;
        }
        const Vec3 p = PlayerNow();
        GuardDismiss();
        if (!GuardSummon(p + Vec3(6, 0, 0), p)) {
            Grade("roadblock", false, false, "the force could not be restaged");
            GoPhase(PH_PLAYER1);
            return;
        }
        Vec3 fwd = gCop.fwd;
        fwd.z = 0.0f;
        if (fwd.Length() < 0.1f)
            fwd = Vec3(0, 1, 0);
        fwd = fwd * (1.0f / fwd.Length());
        if (!SpawnDecoyCarNear(gCop.pos, 68.0f, fwd) ||
            (!BuildStraight(DecoyPos(), fwd) && !BuildLoop(DecoyPos()))) {
            Grade("roadblock", false, false, "no decoy could be staged");
            GoPhase(PH_PLAYER1);
            return;
        }
        CopPreyOverride(sDecoyCar, -1);
        CopForceFelony(0.0f); // the arm latch needs the felony low first
        sBlockLatchT = 0.0f;
        sBlockPlacedSeen = false;
        sBlockMinDist = 1e9f;
    }
    CopPreyOverride(sDecoyCar, -1);
    sBlockLatchT += dt;
    if (sBlockLatchT > 0.6f && gThreat.felony < 2000.0f) {
        CopForceFelony(2400.0f);
        CopKnownSeed();
    }
    DriveDecoy(11.0f);
    Vec3 dp, dv;
    if (!TheHost().DecoyReport(sDecoyCar, &dp, &dv)) {
        Grade("roadblock", false, false, "the decoy was lost mid-scenario");
        GoPhase(PH_PLAYER1);
        return;
    }
    if (CopBlockCount() > 0)
        sBlockPlacedSeen = true;
    // wake: any block unit in pursuit, or the decoy already met one
    for (int i = kMaxCopCars; i < kMaxCopUnits; ++i) {
        const CopState& c = gCops[i];
        if (c.veh < 0 || !c.blockUnit)
            continue;
        const float d = (c.pos - dp).Length();
        if (d < sBlockMinDist)
            sBlockMinDist = d;
        if (c.mode == COP_PURSUE) {
            Grade("roadblock", true, true,
                  "%d cars placed, a block unit woke with the decoy %.0f m out",
                  CopBlockCount(), d);
            GoPhase(PH_PLAYER1);
            return;
        }
    }
    if (sPhaseT > 60.0f) {
        if (!sBlockPlacedSeen)
            Grade("roadblock", false, false,
                  "the chain never placed a block in 60 s - not graded");
        else
            Grade("roadblock", false, true,
                  "placed but never woke (closest decoy pass %.0f m)", sBlockMinDist);
        GoPhase(PH_PLAYER1);
    }
}

void PhasePlayer1(float dt) {
    if (!sEntered) {
        sEntered = true;
        if (sDecoyCar >= 0) {
            TheHost().RetireDecoy(sDecoyCar);
            sDecoyCar = -1;
        }
        CopPreyOverride(-1, -1);
        const Vec3 p = PlayerNow();
        GuardDismiss();
        GuardSummon(p + Vec3(6, 0, 0), p);
        CopForceFelony(0.0f);
        sP1FelonyPeak = 0.0f;
        Say("coptest: YOUR TURN - shoot the cop car or ram it, NOW (20 s)");
    }
    if (gThreat.felony > sP1FelonyPeak)
        sP1FelonyPeak = gThreat.felony;
    if (sP1FelonyPeak >= 659.0f && CopPursuingCount() > 0) {
        Grade("player-felony", true, true,
              "your hand took the felony to %.0f and the pursuit opened in %.0f s",
              sP1FelonyPeak, sPhaseT);
        GoPhase(PH_PLAYER2_WAIT);
        return;
    }
    if (sPhaseT > 25.0f) {
        Grade("player-felony", false, true,
              "no offence landed in 25 s (felony peak %.0f)", sP1FelonyPeak);
        GoPhase(PH_PLAYER2_WAIT);
    }
}

void PhasePlayer2Wait(float dt) {
    if (!sEntered) {
        sEntered = true;
        Say("coptest: YOUR TURN - get in a car and drive until the cop is chasing you (35 s)");
    }
    int slot = -1;
    const float d = NearestUnit(PlayerNow(), true, &slot);
    if (gThreat.playerInVehicle && CopPursuingCount() > 0 && d < 45.0f) {
        Say("coptest: NOW - get OUT of the car (or swap cars) and stay on foot (15 s)");
        sP2BailSet = false;
        GoPhase(PH_PLAYER2_BAIL);
        return;
    }
    if (sPhaseT > 35.0f) {
        Grade("bailout", false, true, "no chase formed to bail out of in 35 s");
        GoPhase(PH_FINISH);
    }
}

void PhasePlayer2Bail(float dt) {
    const Vec3 p = PlayerNow();
    if (!sP2BailSet && sPhaseT > 1.0f) {
        sP2BailSet = true;
        sP2BailDist = NearestUnit(p, true, nullptr);
        sP2MinAfter = sP2BailDist;
        sP2DownT = 0.0f;
    }
    if (sP2BailSet) {
        const float d = NearestUnit(p, true, nullptr);
        if (d < sP2MinAfter)
            sP2MinAfter = d;
        if (CopPursuingCount() == 0)
            sP2DownT += dt;
        else
            sP2DownT = 0.0f;
    }
    if (sPhaseT > 16.0f) {
        const bool held = sP2DownT <= 2.0f && CopPursuingCount() > 0;
        Grade("bailout", held && sP2MinAfter <= sP2BailDist + 2.0f, true,
              "pursuit %s for 15 s on foot; the closest unit came from %.0f m to %.0f m of YOU, not your car",
              held ? "held" : "broke", sP2BailDist, sP2MinAfter);
        GoPhase(PH_FINISH);
    }
}

} // namespace

bool CopTestStart() {
    if (sRunning)
        return false;
    sRunning = true;
    sResultN = 0;
    sSheetN = 0;
    sTotalT = 0.0f;
    sSavedFelony = gThreat.felony;
    sWorstFreeze = 0.0f;
    sWorstSlot = -1;
    for (int i = 0; i < kMaxCopUnits; ++i)
        sFreeze[i] = 0.0f;
    sDecoyCar = -1;
    sDecoyPed = -1;
    Say("coptest: starting - one session grades the whole force; just watch until YOUR TURN lines appear");
    GoPhase(PH_SPIKE);
    return true;
}

void CopTestAbort() {
    if (!sRunning)
        return;
    Say("coptest: aborted by hand - grading what stands and cleaning up");
    Grade("soak", sWorstFreeze <= 5.0f, true,
          "worst pursuing-unit freeze %.1f s (unit %d) (bar: <=5 s)", sWorstFreeze, sWorstSlot);
    Finish();
}

bool CopTestRunning() { return sRunning; }

void CopTestReset() {
    if (sGuard > 0)
        return; // our own summon/dismiss caused this reset
    sRunning = false;
    sPhase = PH_NONE;
    sDecoyCar = -1;
    sDecoyPed = -1;
}

CopTestSheet CopTestSheetForTest() {
    CopTestSheet s;
    for (int i = 0; i < sSheetN; ++i) {
        if (!sSheet[i].counted)
            continue;
        ++s.counted;
        if (sSheet[i].pass)
            ++s.passed;
    }
    return s;
}

bool CopTestVerdictForTest(const char* name, bool* passOut, bool* countedOut) {
    for (int i = 0; i < sSheetN; ++i) {
        if (std::strcmp(sSheet[i].name, name) == 0) {
            if (passOut)
                *passOut = sSheet[i].pass;
            if (countedOut)
                *countedOut = sSheet[i].counted;
            return true;
        }
    }
    return false;
}

void CopTestTick(float dt) {
    if (!sRunning)
        return;
    sTotalT += dt;
    sPhaseT += dt;
    SoakTick(dt);
    if (sTotalT > 600.0f) {
        Say("coptest: the run hit its 600 s cap - grading what stands");
        GoPhase(PH_FINISH);
    }
    switch (sPhase) {
    case PH_SPIKE: PhaseSpike(dt); break;
    case PH_PURSUIT: PhasePursuit(dt); break;
    case PH_RUNOVER: PhaseRunover(dt); break;
    case PH_RECOVERY: PhaseRecovery(dt); break;
    case PH_BLOCK: PhaseBlock(dt); break;
    case PH_PLAYER1: PhasePlayer1(dt); break;
    case PH_PLAYER2_WAIT: PhasePlayer2Wait(dt); break;
    case PH_PLAYER2_BAIL: PhasePlayer2Bail(dt); break;
    case PH_FINISH:
        Grade("soak", sWorstFreeze <= 5.0f, true,
              "worst pursuing-unit freeze %.1f s (unit %d) (bar: <=5 s)",
              sWorstFreeze, sWorstSlot);
        Finish();
        break;
    default: break;
    }
}

} // namespace mc
