#include "Cop.h"

#include <cmath>
#include <cstring>

#include "Atlas.h"
#include "Audio.h"
#include "CopTest.h"
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
// ships - the user's own choice, 2026-11-07, «آره سخت ترین بهترینه».
//
// THE 5x UPDATE (the user's new law, 2026-11-08, chosen in consultation
// AFTER the walls were named to him: San Andreas clips part of any
// paper number - streaming, physics stepping, ProcessAI's own steering
// - and he ordered the paper 5x anyway, «همون ۵ برابر رو بنویس», against
// an UNCHANGED world: his stats scale, the world's prices and speeds do
// not). This supersedes the old «خودش is the ceiling» law FOR THE COP
// (the boss and everything else keep it). The pace rows are five times
// the hardest row: speed1 217 -> 1085 units = 197 m/s on paper, speed2
// 294 -> 1470 = 266 m/s on paper. What the host's fields actually hold
// and what the car achieves are separate questions, answered honestly
// in DRIVER2-STEP3-fa.md's 5x table - the rows here are the paper law
// he ordered, nothing dressed down in advance. The thrust scales ride
// the same row ((difficulty+8) base, +(difficulty+4) on a straight).
// The MANOEUVRE speeds below the rows are not pace: the side-zone
// crawl, the overshoot reverse and the both-corners reverse are
// geometry speeds the original ties to the car's own frame (a reverse
// at 5x is not pressure, it is a different accident), and the recovery
// desire belongs to the recovery's own clock - they stay at his
// numbers, named here so the exception is a decision, not an omission.
constexpr int kDifficulty = 2;
constexpr float kSpeed1[3] = { 1015 * kV, 1050 * kV, 1085 * kV }; // 5x of {203, 210, 217}
constexpr float kSpeed2[3] = { 1190 * kV, 1330 * kV, 1470 * kV }; // 5x of {238, 266, 294}
constexpr float kSideSpeed = 110 * kV;    // 19.9 m/s, the side-zone crawl
constexpr float kBackSpeed = -70 * kV;    // -12.7 m/s, overshoot reverse
constexpr float kBlockedSpeed = -120 * kV; // -21.7 m/s, both corners shut
constexpr float kRecoverSpeed = 200 * kV; // 36.2 m/s desired in recovery
// The stuck witness's fair start (fix round 5): the original's 10-tick
// witness was written for its own physics, where a told car answers in
// the same tick. On this host the drive channel (the car's autopilot,
// fix round 4) has a start-up ramp of its own - the boss's legs taught
// the identical lesson and his fair start stands at 1.6 s - so a fresh
// drive order earns 1.5 s before stillness may accumulate as stuck.
constexpr float kCopFairStart = 1.5f;

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
constexpr float kWreckLine = 135000.0f;    // the 5x update: five times the
    // original's retirement line (COP_AI.C: totalDamage > 27000 &&
    // !immortal - the dying sequence). The book deepened; the prices
    // that fill it did not move (see the anchor below), so an RPG-class
    // blast that billed 8100 of 27000 now bills 8100 of 135000 -
    // roughly seventeen direct RPGs to retire him, not three-plus.
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
//   expensive - and since the 5x update deepened the book to 135000
//   with this anchor UNCHANGED, some seventeen direct hits from
//   retiring him - never one, and no longer three-plus.
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
// THE PLOUGH LAW's price split (the user's amendment, 2026-11-08
// 09:39 - "nothing ahead of them matters"): his book is for the PREY's
// fight. A collision away from the prey - traffic rammed aside,
// scenery struck at pace - bills a token share of the fight price, so
// ploughing a street of cars cannot burn the 135000 on the world. The
// split is read off the report itself: an impact is a FIGHT contact
// when the prey is at hand (the bridge's own ram filing, or the prey
// inside 6 m - the kick band's country); health the body lost in the
// same report as a scenery impact is collision-filed and rides the
// same token lane, while a smooth drain with no impact in it (fire)
// keeps the anchor rate of the standing law - fire is priced,
// collisions are not.
constexpr float kSceneryShare = 0.04f; // the token: 1/25 of the fight price
constexpr float kBlastPrice = 8100.0f; // one blast-class event: 300 host
                                       // points at the anchor - the most
                                       // one tick's health-delta channels
                                       // may bill, all of them together

// ---- THE 5x UPDATE: the momentum channel (impact x5), the damage
// book's sibling. The book prices what the world does TO the cop; this
// channel prices what the cop does to the PREY at contact, and pays it
// out as velocity through the host verbs (KickVehicle / KickPed) - the
// core owns the law, the host displays it, the same split as the book.
// The law, stated whole:
//  - contact is read geometrically, off the bodies' centres - the same
//    reading the /coptest ram grade uses: centres inside the band
//    (4.8 m car-to-car, about two body lengths of shell; 2.6 m to the
//    man himself) while the unit pursues and is itself moving;
//  - the throw's size is the TRANSFER FRACTION of the closing speed:
//    0.6 of it. The raw host outcome for a square hit between similar
//    cars keeps perhaps a tenth to a fifth of the closing in the prey;
//    0.6 is the felt ~5x of that outcome, and it is CAPPED at 30 m/s -
//    the physics-sanity cap, stated here and in the report, not hidden:
//    past it the host's own stepping tunnels bodies through each other
//    and a bigger number would buy cartoon, not force;
//  - the size ramps in between the tap threshold (3 m/s of closing:
//    under it, nothing - a parked car's tap stays a tap) and full
//    contact (8 m/s), so lesser contacts scale down smoothly instead
//    of switching on;
//  - a flank or off-centre hit adds a yaw kick proportional to the
//    prey centre's signed offset from the cop's line of travel:
//    0.045 rad/s per metre of offset per m/s of throw, capped at
//    3 rad/s. Square-on, the offset is zero and there is no spin;
//  - one throw per approach (CopState::kickArmed): the latch re-arms
//    only when the centres have separated past the band + 3 m;
//  - a fired throw is also the re-attack order (the user's pressure
//    law): the unit's stuck witness is wiped and any recovery the
//    impact provoked is cancelled on the spot - he re-engages at
//    batter pace through the ordinary feed, with no re-path pause.
constexpr float kKickBandCar = 4.8f;
constexpr float kKickBandFoot = 2.6f;
constexpr float kKickTap = 3.0f;
constexpr float kKickFull = 8.0f;
constexpr float kKickTransfer = 0.6f;
constexpr float kKickCap = 30.0f;
constexpr float kKickSpin = 0.045f;
constexpr float kKickSpinCap = 3.0f;

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
// THE 5x UPDATE divides the base by five (pressure x5 where the slots
// allow): 64 ticks at felony 0 = 1.28 s, 32 ticks at the cap = 0.64 s -
// replacements arrive inside the ~1.3 s -> ~0.6 s band.
constexpr float kRespawnTicks = 64.0f; // 320 / 5
constexpr float kRespawnScale = 2048.0f / 4096.0f;
// The cutoff (ControlNumberOfCops + its use in CopControl1): when more
// pursuers are live than the ladder wants, cutOffDistance is set to the
// wanted-th nearest pursuer's distance, and a pursuer beyond it reads
// his desired speed through cutOffPowerScale = 2048 of 4096 - one half.
// THE 5x UPDATE removes the discount at the felony cap (the manager's
// cutoff block reads the page): at max felony far units fight at full
// fed speed - with 5x pace rows the cutoff's purpose (keeping the far
// units from arriving hotter than the near ones can use) is gone, and
// the user's pressure order names the result. Below the cap the
// original's half scale stands untouched.
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

// a unit ordered off (COP_LEAVE) gives his post back first: a post is
// an order to stand somewhere, and a man driving away cannot hold
// one - a leaving unit that kept its claim froze the post bookkeeping
// around a ghost (the escape-ambush sim convicted it: the far unit
// was stood down by felony decay mid-ambush and carried the post
// into its drive-off). The post trackers all read the holding flag,
// so clearing it here releases every claim cleanly.
void GiveUpPost(CopState& c) {
    c.holding = false;
    c.holdArrived = false;
}

// The manager's books (ControlNumberOfCops / StepSim state).
bool sForce = false;        // a summon stood the force up this session
float sRespawnT = 0.0f;     // the ladder's spawn clock, seconds
int sWanted = 0;            // the ladder's wanted count this tick
float sCutoff = 1e9f;       // the cutoff distance in force (1e9: none)
bool sBlockWasLow = true;   // the arm latch: felony was at/under 1318
bool sBlockTimerOn = false;
float sBlockTimer = 0.0f;   // the armed roadblock count, seconds

// The /coptest prey override (the complete update): while one of these
// is armed, CopTick fills the threat record from the DECOY's reports
// instead of the player's, and the whole force hunts the decoy. -1/-1
// is the standing state: the player is the prey.
int sPreyVeh = -1;
int sPreyPed = -1;
bool PreyOverrideOn() { return sPreyVeh >= 0 || sPreyPed >= 0; }

// ---- the 39-package's force-level state. The tactics tick (1 s
// cadence, TacticsTick below) owns the assignments; the per-tick
// trackers in CopTick own the windows. All of it resets in CopReset.
float sTacticsT = 0.0f;   // the tactics cadence's clock
float sPursuitT = 0.0f;   // item 21: continuous seconds of pursuit at
                          // the felony cap - the heat clock
int sHeat = 0;            // item 21: escalation steps, 0..5
float sPreyTopSeen = 0.0f; // item 26: the prey's measured top speed
                          // this pursuit (m/s) - spawns price off it
bool sHandover = false;   // item 16: a pursuer fell - the ladder
                          // replaces him NOW, near where he fell
Vec3 sHandoverPos;
float sHandoverT = 0.0f;
float sCrashT = 0.0f;     // item 31: the prey just crashed - the
                          // strike window, seconds left
float sSpdOld = 0.0f;     // the crash witness's older speed sample
float sSpdAge = 1e9f;     // seconds since that sample was taken
float sSyncT = 0.0f;      // item 11: the synchronised-hit window
float sBikeT = 0.0f;      // item 36: the motorcycle's fielding clock
bool sAirborne = false;   // item 35: the prey's car is in the air
bool sAirFell = false;    // item 35: ...and has started falling
float sAirLaunchZ = 0.0f; // item 35: the height he left the ground at
Vec3 sLandPoint;          // item 35: the predicted touchdown point
bool sLandValid = false;  // item 35: a landing prediction stands
float sLandedT = 0.0f;    // item 35: seconds left of the landing
                          // window (a grip-less car, priced throws)
float sTurnRate = 0.0f;   // item 12: the prey's smoothed yaw rate
bool sTurnStrike = false; // |turn rate| past the strike line, with a
                          // pursuer close enough to spend it
float sHeadPrev = 0.0f;   // the yaw witness's last heading
bool sHeadOk = false;
// layer B's posts and block state
float sPostsT = 0.0f;     // the posts tick's cadence (items 9/27/34)
float sRearT = 0.0f;      // item 9: the rear-exit guard's watch left
int sRearSlot = -1;
float sRearCool = 0.0f;   // and the quiet after a spent watch
float sMouthT = 0.0f;     // item 34: the dead-end mouth watch left
int sMouthSlot = -1;
bool sIndoor = false;     // item 27: the prey is inside a family
char sIndoorFam[64] = ""; // the family's name, for the release
Vec3 sBlockPoint;         // item 2: the standing block's anchor point
Vec3 sBlockDir(0, 1, 0);  // and its road direction
float sCreepT = 0.0f;     // item 2: the living block's cadence
bool sBlockLive = false;  // a block stands (its point means something)
float sWallOpenSide = 0.0f; // item 13: -1/+1 = the prey's open side
                          // (the other has world geometry within 9 m),
                          // 0 = both open or both walled
// layer C's presence-and-shape state
struct EscapeRec {        // items 3+20: a written escape - the
    Vec3 junction, exit;  // junction he vanished at, the exit he was
    float coolT = 0.0f;    // making for, and its reuse cooldown
};
EscapeRec sEscape[6];
int sEscapeNext = 0, sEscapeCount = 0;
int sAmbushSlot = -1;     // the posted ambush's unit (-1 = none)
float sAmbushT = 0.0f;    // its remaining watch
Vec3 sLastSeenPos;        // the prey's place at last sight, and his
Vec3 sLastSeenVel;        // velocity then - the page itself keeps no
                          // such snapshot (its pos is the anchor's
                          // own body), so the escape book takes its
                          // own, refreshed every seen tick
bool sPreyOffRoad = false; // item 18: his nearest road node is 25+ m
                           // away - he has left the graph
Vec3 sWrecks[4];          // item 30: where units burned out - a
int sWreckNext = 0, sWreckCount = 0; // wreck in a block's line IS
                          // part of the line
bool sRingOn = false;     // item 7: the ring around a pinned man
int sRingSlot[kMaxCopUnits];
int sRingCount = 0;
Vec3 sRingCentre;
bool sShoreOn = false;    // item 29: the shoreline watch
int sShoreSlot[3];
// layer D+E's state: the priced disable and the sound law
int sTyreStage = 0;       // item 10: tyres the prey's car has lost
float sTyreT = 0.0f;      // the spacing between stages
int sTyreVeh = -1;        // the car the stages belong to
int sRadioLines = 0;      // item 39: radio lines fired this page
int sRadioMask = 0;       // the kinds fired, as a bitmask (1 open,
                          // 2 rung, 4 bail, 8 block, 16 death, 32 join)
bool sRadioOpen = false;  // the pursuit's open line has been called
int sRadioRung = 0;       // the ladder size last announced
bool sPrevInVeh = false;  // the prey's last-tick seat (the bail edge)
bool sForceHeard = false; // night witness: a pursuer has closed
                          // within 18 m this pursuit

// item 39: the force's one radio voice. Every line is LOGGED and
// COUNTED - the user's live complaint was that the force never
// talked, so the fix is measurable in the log and the sheet, not
// just audible - and fired at the raised gain his ear asked for.
// (The bridge's own two call sites are deleted in the bridge commit;
// the core is the only voice now.)
void RadioLine(int kind, const Vec3& at) {
    static bool flip = false;
    flip = !flip;
    SoundEvent ev = SND_COP_RADIO_07;
    const char* what = "pursuit open";
    switch (kind) {
    case 1:
        ev = flip ? SND_COP_RADIO_07 : SND_COP_RADIO_08;
        what = "pursuit open";
        break;
    case 2:
        ev = flip ? SND_COP_RADIO_05 : SND_COP_RADIO_06;
        what = "the felony rung climbs";
        break;
    case 4:
        ev = SND_COP_RADIO_09;
        what = "he bails out";
        break;
    case 8:
        ev = SND_COP_RADIO_12;
        what = "roadblock set";
        break;
    case 16:
        ev = SND_COP_RADIO_15;
        what = "a unit is down";
        break;
    case 32:
        ev = flip ? SND_COP_RADIO_10 : SND_COP_RADIO_11;
        what = "another unit joins";
        break;
    }
    PlaySfx(ev, &at, 1.5f);
    ++sRadioLines;
    sRadioMask |= kind;
    Log("cop radio: %s (line %d this page)", what, sRadioLines);
}

// item 39: the smart siren. A pursuer wails while he closes or
// batters; the siren sleeps while the force statically surrounds a
// pinned man (a posted unit that has arrived - the ring, a camp, a
// shore post), a driver out of his car wails nothing, and at night
// it stays asleep until a pursuer has actually closed within 18 m
// this pursuit - the force arrives dark and announces itself at
// contact (item 32's stealth, folded into the one siren law).
bool SirenFor(const CopState& c) {
    if (c.mode != COP_PURSUE)
        return false;
    if (c.bailState == 1)
        return false;
    if (c.holding && c.holdArrived)
        return false;
    if (TheHost().Dark() && !sForceHeard)
        return false;
    return true;
}

float WreckLineOf(const CopState& c) {
    return c.wreckLine > 0.0f ? c.wreckLine : kWreckLine;
}
// item 21's pace: each full minute of max-felony pursuit buys the
// force 8% more pace, five steps deep - time works for the force,
// never for the man (the user's escalation order).
float HeatPace() { return 1.0f + 0.08f * (float)sHeat; }
// The one multiplier every pace-row reading rides: the cutoff's
// scale (the original's), the counter-pick's (item 26) and the
// heat's (item 21) compose here so no call site can forget one.
float PaceMul(const CopState& c) {
    return c.powerScale * c.paceScale * HeatPace();
}

float BlockDelay() {
    // THE 5x UPDATE: the whole armed count is divided by five, the
    // random tail included (pressure x5) - the hardest row's
    // 800..1055 ticks (16..21 s) become 3.2..4.2 s. THE 39-PACKAGE:
    // item 21's heat divides it again - the longer a max-felony chase
    // runs, the faster the next block is laid over his road.
    return (kBlockDelayTicks[kDifficulty] + (float)(McsaRand() & 0xFF)) * kTick / 5.0f /
           (1.0f + (float)sHeat);
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
// blinds. Ours read through the host's own wall oracle instead of the
// original's cell PVS - the shape is his, the eyes are the host's, and
// that swap is named here rather than hidden.
// FIX ROUND 5 - whose wall the ray reads: the ray starts at the car's
// own centre, INSIDE his own body, and the host's oracle counts
// vehicles as walls (GtaHost's LineBlocked passes checkVehicles to
// ProcessLineOfSight) - so the plain ray was blocked by his own car on
// every reading, at every range, worst at point blank where it exits
// through the whole body. His sight ran on the struck channels alone.
// The ray now runs with his own vehicle ignored (the bridge's
// LineBlockedSkipVeh - the same verb the boss's round built when the
// target's own car blinded the soldier), and when the man is himself
// in a car a second ray ignores HIS shell the same way: sight is
// blocked only when both readings are blocked. Within the peripheral
// range there is no cone test at all (the original's own shape), so
// at 1-3 m - beside him, behind him - the geometry has no dead zone;
// the own-car ray was the whole of the near blindness.
// THE COMPLETE UPDATE - the two-shell flicker: round 5's pair of rays
// still left a car-to-car chase's sight flickering 0/1 at aim 8-15 m
// (the 02:00 session's unit lines), because EACH single-skip ray is
// blinded by the OTHER car's shell: the ray that skips the cop's car
// ends inside the PLAYER's car, and the ray that skips the player's
// car starts inside the COP's. A third reading runs between the cars'
// outer skins - from his nose to the prey's tail, plain oracle, both
// shells excluded geometrically - and when the cars stand closer than
// the two half-lengths (~5.2 m of centres) there is nothing between
// them but air and the reading is clear by construction. Walls
// between the cars still count: the segment only starts past his own
// nose and ends before the prey's tail.
bool CopSees(const CopState& c, bool hot) {
    const Vec3& copPos = c.pos;
    const Vec3& copFwd = c.fwd;
    const Vec3 to = gThreat.playerPos - copPos;
    const float dist = to.Length();
    if (std::fabs(to.z) > 1500 * kM)
        return false;
    const float front = (hot ? 16320 : 7820) * kM;
    const float periph = (hot ? 5440 : 2720) * kM;
    if (dist > front)
        return false;
    const auto clear = [&]() {
        if (!TheHost().LineBlockedSkipVeh(copPos, gThreat.playerPos, c.veh))
            return true;
        if (gThreat.playerInVehicle && gThreat.playerVehicle >= 0 &&
            !TheHost().LineBlockedSkipVeh(copPos, gThreat.playerPos,
                                          gThreat.playerVehicle))
            return true;
        if (gThreat.playerInVehicle) {
            Vec3 dir = to;
            dir.z = 0.0f;
            const float dl = dir.Length();
            if (dl < 5.2f)
                return true; // the shells touch: nothing between them
            dir = dir * (1.0f / dl);
            return !TheHost().LineBlocked(copPos + dir * 2.6f,
                                          gThreat.playerPos - dir * 2.6f);
        }
        return false;
    };
    if (dist <= periph)
        return clear();
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
    return clear();
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
    c.cmdSteer = steer; // the word as sent - the unit line's half of
                        // the commanded-vs-landed pair
    TheHost().CopBodyDrive(c.veh, gas, brake, steer, siren);
    // THE ROOT STEP (2026-11-08): nothing rides beside the pedals any
    // more. Every round's convicted defect - the crawl, the
    // BLOCKPLAYER missions, the negative wants, the orbiting, the
    // ring that never settled - was born in the translation between
    // this brain and the host's traffic autopilot, which took the
    // feed's destination+pace and drove it the way a traffic driver
    // drives: arrive, stop, loop, re-approach. The feed verb
    // (CopBodyDriveTarget) is retired for cops; the bridge writes
    // these pedal/steer words onto the car itself, with no autopilot
    // mission and no driver drive-task, after the game's own
    // processing (the frame's last word - see GtaCop.cpp). The brain
    // always computed real wheel/pedal words (CopControl's wheel and
    // thrust); until this step they were thrown away in favour of a
    // destination. Now they are the drive.
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

// The far pursuit's steering-aim walk length. Born as the 5x feed
// law (the distance the mirror was fed at); the root step keeps the
// numbers and changes their job: the far mode aims its WHEEL at the
// road field's descent point, and how far down the road that point
// stands is what this law still decides - 25 m of base plus 0.9 s of
// travel, capped at 180 m. The old floor's reason (a nearer point
// made the autopilot brake to arrive, and the crawl fed itself) died
// with the middleman; at the old 1x paces the law still lands within
// a few metres of the original's own |v|*0.216 + 2.66 m, extended to
// the 5x rows the same way it was then.
float FeedLookAhead(float speed) {
    return Clamp(25.0f + 0.9f * std::fabs(speed), 30.0f, 180.0f);
}

// Route capture, the complete update's route law: a waypoint counts
// as taken when the car comes within the capture radius of it OR has
// drawn level with it (the next waypoint is nearer than it is). The
// second clause is new and the mirror channel is why: a pedal-era
// body steered AT the current waypoint and passed through its circle;
// a body driven by the autopilot feed beelines at the look-ahead
// point tens of metres down the road, and on a bend its chord can
// pass a waypoint 8-10 m wide without ever entering the circle - the
// index then never advances, the look-ahead stays anchored behind
// the car, and the route stalls (the complete update's sims convicted
// exactly this in both route drivers: the spike never finished, and
// stood-down units orbited at ~47 m and never pinged out).
// THE 5x UPDATE: the capture radius itself is speed-shaped - 7 m at
// the crawl it was tuned at, +0.1 m per m/s of travel, capped at
// 25 m. A car covering 4 m a tick at the new rows crosses a 7 m
// circle in under two ticks and its chord on a bend stands wider of
// the waypoint than the whole circle; the drawn-level clause still
// does the precise work, the wider circle is the fast car's slack.
float RouteCapture(float speed) {
    return Clamp(7.0f + 0.1f * std::fabs(speed), 7.0f, 25.0f);
}
void RouteAdvance(const std::vector<Vec3>& route, size_t& idx, const Vec3& pos,
                  float capture) {
    // reseed first: if a point a little further along the route is
    // already within capture distance, the car has drawn level with
    // THAT stretch of road - a hairpin can put a later point at his
    // feet while the head sits uncaptured around the bend, and the
    // one-step drawn-level clause below cannot see past the local
    // minimum between them (the complete update's sims: a stood-down
    // unit parked forever 7.3 m wide of its first waypoint, its feed
    // satisfied by the stretch of route that passed over his spot)
    {
        size_t best = idx;
        float bestD = capture;
        for (size_t j = idx; j < route.size() && j < idx + 12; ++j) {
            Vec3 to = route[j] - pos;
            to.z = 0.0f;
            const float d = to.Length();
            if (d < bestD) {
                bestD = d;
                best = j;
            }
        }
        if (best > idx)
            idx = best;
    }
    while (idx < route.size()) {
        Vec3 to = route[idx] - pos;
        to.z = 0.0f;
        const float d = to.Length();
        if (d < capture) {
            ++idx;
            continue;
        }
        if (idx + 1 < route.size()) {
            Vec3 tn = route[idx + 1] - pos;
            tn.z = 0.0f;
            if (tn.Length() < d) {
                ++idx;
                continue;
            }
        }
        break;
    }
}

// (The autopilot feed's route look-ahead stood here until the root
// step: it existed only so the host's destination was always a point
// the car had NOT reached. With no middleman, the route drivers steer
// at their own waypoint - the pedal-era design the capture law above
// was written for - and the function has no consumer left.)

// The momentum channel's arithmetic (the constants and the whole law
// stand at kKickBandCar above): pure geometry on the two velocities
// and the centres' offset, no state - CopTick calls it at a contact,
// the sims pin it directly through CopKickCompute.
Vec3 KickCompute(const Vec3& copVel, const Vec3& preyVel, const Vec3& rel,
                 float* spinOut, float* closingOut) {
    *spinOut = 0.0f;
    *closingOut = 0.0f;
    Vec3 flat = rel;
    flat.z = 0.0f;
    const float dist = flat.Length();
    Vec3 cv = copVel;
    cv.z = 0.0f;
    const float copSpeed = cv.Length();
    if (dist < 0.01f || copSpeed < 1.0f)
        return Vec3(); // no line to price on, or no momentum to give
    const Vec3 dir = flat * (1.0f / dist);
    Vec3 rv = preyVel - copVel;
    rv.z = 0.0f;
    const float closing = -(rv.x * dir.x + rv.y * dir.y);
    *closingOut = closing;
    if (closing < kKickTap)
        return Vec3(); // a tap: the ramp below starts at zero here
    const float ramp = Clamp((closing - kKickTap) / (kKickFull - kKickTap), 0.0f, 1.0f);
    const float mag = std::fmin(kKickTransfer * closing * ramp, kKickCap);
    if (mag <= 0.01f)
        return Vec3();
    const Vec3 travel = cv * (1.0f / copSpeed);
    // the prey centre's signed offset from the cop's line of travel
    // (the z of travel x rel): zero square-on, and its sign is the
    // side the spin answers to
    const float offset = travel.x * flat.y - travel.y * flat.x;
    *spinOut = Clamp(kKickSpin * offset * mag, -kKickSpinCap, kKickSpinCap);
    return travel * mag;
}

} // namespace

Vec3 CopKickCompute(const Vec3& copVel, const Vec3& preyVel, const Vec3& rel,
                    float* spinOut, float* closingOut) {
    return KickCompute(copVel, preyVel, rel, spinOut, closingOut);
}

namespace {

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
        // THE ROOT STEP: the calibration finally measures what it was
        // always staged to measure - the pedal channel itself. Under
        // the mirror it drove a feed and concluded NOT MEASURABLY
        // about its own sign; now the commanded +0.6 turns the BODY or
        // it does not, and the cross product below is the channel's
        // first live receipt (graded again in every session's log).
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
    RouteAdvance(c.spikeRoute, c.spikeIdx, c.pos, RouteCapture(c.speed));
    if (c.spikeIdx >= c.spikeRoute.size())
        return; // the finish branch at the top takes the next tick
    const Vec3 wp = c.spikeRoute[c.spikeIdx];
    Vec3 to = wp - c.pos;
    to.z = 0.0f;
    const float dist = to.Length();
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
    // capture circle, and an orbiting spike proves nothing. THE 5x
    // UPDATE scales this pace with the rest of him (55 / 30 - these
    // two numbers were always OUR stand-in for a city pace, not one
    // of his rows, and the order was 5x in all respects); the capture
    // radius grows with the speed by the same shape, which is what
    // keeps the faster spike from orbiting its points. THE ROOT STEP:
    // the spike steers at its own waypoint now, exactly the pedal-era
    // design - the round-5 look-ahead feed existed only because the
    // middleman braked to arrive at a near destination, and with no
    // middleman the look-ahead has no reader left.
    float want = 55.0f;
    if (dist < 25.0f || std::fabs(err) > 0.7f)
        want = 30.0f;
    float gas = 0.0f, brake = 0.0f;
    if (c.speed < want)
        gas = Clamp((want - c.speed) * 0.2f, 0.25f, 1.0f);
    else
        brake = Clamp((c.speed - want) * 0.2f, 0.0f, 0.6f);
    c.target = wp;
    c.desiredSpeed = want;
    Drive(c, gas, brake, steer, false);
    c.spikeLogT -= dt;
    if (c.spikeLogT <= 0.0f) {
        c.spikeLogT = 1.0f;
        Host::CopBody rep;
        if (TheHost().CopBodyReport(c.veh, &rep) && rep.ok) {
            // the census rides the spike line too (the missing-link
            // step): the spike is the first grader, and its per-second
            // line is where the anchor's drive state is read while
            // the route runs - same segment shape as the unit line
            char cen[384];
            FormatCensus(rep.census, cen, sizeof cen);
            Log("cop spike: pos (%.1f, %.1f, %.1f) speed %.1f m/s | read-back gas %.2f brake %.2f steer %.2f"
                " | hook gas %.2f brake %.2f steer %.2f n %d"
                " | engine %d handbrake %d broken %d status %d mission %d cruise %d/%.0f driver-in %d | %s",
                rep.pos.x, rep.pos.y, rep.pos.z,
                rep.vel.x * rep.fwd.x + rep.vel.y * rep.fwd.y + rep.vel.z * rep.fwd.z,
                rep.gas, rep.brake, rep.steer,
                rep.hookGas, rep.hookBrake, rep.hookSteer, rep.hookStamps,
                rep.engineOn ? 1 : 0, rep.handbrake ? 1 : 0, rep.engineBroken ? 1 : 0,
                rep.status, rep.mission, rep.cruise, rep.cruiseF, rep.driverInVehicle ? 1 : 0, cen);
        }
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
    RouteAdvance(c.leaveRoute, c.leaveIdx, c.pos, RouteCapture(c.speed));
    if (c.leaveIdx >= c.leaveRoute.size())
        return; // the re-lay branch at the top takes the next tick
    const Vec3 wp = c.leaveRoute[c.leaveIdx];
    Vec3 to = wp - c.pos;
    to.z = 0.0f;
    const float dist = to.Length();
    to = to * (1.0f / dist);
    const float cross = c.fwd.x * to.y - c.fwd.y * to.x;
    const float dot = c.fwd.x * to.x + c.fwd.y * to.y;
    const float err = std::atan2(cross, dot);
    const float steer = dot < 0.0f ? (cross >= 0.0f ? -1.0f : 1.0f)
                                   : Clamp(-err * 1.4f, -1.0f, 1.0f);
    float gas = 0.0f, brake = 0.0f;
    // the leave pace rides the same 5x as the spike's (a stood-down
    // unit clears the field five times as fast - the ping-out edge
    // has not moved, only his speed toward it)
    const float want = dist < 25.0f || std::fabs(err) > 0.7f ? 30.0f : 55.0f;
    if (c.speed < want)
        gas = Clamp((want - c.speed) * 0.2f, 0.25f, 1.0f);
    else
        brake = Clamp((c.speed - want) * 0.2f, 0.0f, 0.6f);
    // THE ROOT STEP: the leave steers at its own waypoint by pedals,
    // like the spike - the round-5 look-ahead feed has no reader left
    c.target = wp;
    c.desiredSpeed = want;
    Drive(c, gas, brake, steer, false);
}

// ---------------------------------------------------------------------------
// THE COMPLETE UPDATE's feed law stood here (PursuitFeed) until the
// root step deleted it. Its two intents survive in direct form, and
// the report names both translations:
//  - the through-drive (a fed point projected through the prey so the
//    middleman would not brake on him) is NATIVE under direct drive:
//    the front zone's desire never brakes at the target - its thrust
//    word brakes only on speed error - and after a pass the back
//    zone's negative desire and the brake field do the original's own
//    loop-back. There is no arrival to cancel, so there is nothing to
//    project.
//  - its batter/crash pace floor for the SIDE zones (a manoeuvre
//    desire must not crawl while the felony stands at the ladder's
//    top) now floors the DESIRE itself, in the side branch of
//    CopControl below - the front zone never needed it (its desire
//    is already the row) and the negative desires were always served
//    by the brake field directly, never by a point behind the car.

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
// ---------------------------------------------------------------------------
// The hold controller (the 39-package's post law): a unit with a post
// drives to it and stands there when it arrives. Items 2 (the block
// creep), 9 (the rear-exit guard), 27 (the door camps), 34 (the
// dead-end mouth) - and layer C's ring, ambush and shore posts - all
// spend this one mechanism; a post is a place, not a new brain.
// THE ROOT STEP rewrites its drive as an explicit BRAKE-TO-POST law:
// under the feed, the post was an autopilot arrival (the one arrival
// the middleman was ever FOR - and the ring still never settled,
// because the units around the man were pursuit units fed THROUGH
// him, not post units). Now the core owns the whole curve: approach
// at the distance-shaped pace, brake on the same curve as the post
// nears, and hold with the standstill law inside the arrival band -
// reach and STAY is the post's whole definition, and the /coptest
// machine grades exactly that (the ring's settle witness and the
// direct act's settle manoeuvre both read this law's outcome).
void HoldControl(CopState& c, float dt) {
    Vec3 to = c.holdPoint - c.pos;
    to.z = 0.0f;
    const float dist = to.Length();
    c.target = c.holdPoint;
    if (dist < 2.2f) {
        c.holdArrived = true;
        c.desiredSpeed = 0.0f;
        if (c.mode == COP_BLOCK)
            c.holding = false; // a crept block car sleeps at its new
                               // slot - the post is spent on arrival
        StandDrive(c);
        return;
    }
    c.holdArrived = false;
    // a post is an order, not a life sentence: a unit wedged short of
    // its post - ordered to move, speed ~0, still metres out - for
    // over two seconds gives the post back and returns to the
    // pursuit, whose own recovery law exists for exactly this. A held
    // post must never be the reason a wedged unit never recovers (the
    // /coptest recovery act convicted it: its wedged unit was
    // re-claimed by a re-forming ring and froze, post-bound, for the
    // whole act). The still-clock is the post's own: it starts when
    // the post is taken (a pursuit stuck-clock already hot from the
    // chase must not eat a fresh post in the tick it lands - the
    // ambush sim convicted exactly that inheritance).
    if (c.holdPoint.x != c.postPoint.x || c.holdPoint.y != c.postPoint.y ||
        c.holdPoint.z != c.postPoint.z) {
        c.postPoint = c.holdPoint;
        c.postStillT = 0.0f;
    }
    if (dist > 4.0f && c.speed < 0.5f)
        c.postStillT += dt;
    else
        c.postStillT = 0.0f;
    if (c.postStillT > 2.0f) {
        c.holding = false;
        c.postStillT = 0.0f;
        Log("cop: a unit cannot reach his post - the post is given back and the pursuit has him again");
        return;
    }
    // the brake-to-post curve: the approach pace falls off with the
    // distance (20 m/s open road, walking pace inside the last few
    // metres), and the pedal word is proportional on the error
    // against that pace - gas while under it, brake while over it,
    // with the brake growing as the band nears so the arrival is a
    // stop, not a pass. Inside 2.2 m the standstill law above holds
    // the post.
    const float cruise = Clamp(dist * 0.8f, 3.0f, 20.0f);
    c.desiredSpeed = cruise;
    // the steer is the leave law's, not a bare cross product: a cross
    // is ZERO dead astern as well as dead ahead, so a proportional-
    // cross steer has a dead spot exactly where an overshot post
    // sits - the first direct-drive run's settle drove straight to
    // 600 m with the post behind it and the steer reading 0.00. The
    // angle-error form (atan2) with a full-lock answer in the rear
    // hemisphere is the law the spike and the leave already steer by.
    float steer = 0.0f;
    if (dist > 0.01f) {
        Vec3 dirv = to * (1.0f / dist);
        const float cross = c.fwd.x * dirv.y - c.fwd.y * dirv.x;
        const float dot = c.fwd.x * dirv.x + c.fwd.y * dirv.y;
        const float err = std::atan2(cross, dot);
        steer = dot < 0.0f ? (cross >= 0.0f ? -1.0f : 1.0f)
                           : Clamp(-err * 1.4f, -1.0f, 1.0f);
    }
    float gas = 0.0f, brake = 0.0f;
    if (c.speed < cruise)
        gas = Clamp((cruise - c.speed) * 0.25f, 0.0f, 0.85f);
    else
        brake = Clamp((c.speed - cruise) * 0.3f, 0.0f, 0.85f);
    c.sirenCmd = SirenFor(c); // item 39: the siren is a law now, not a literal
    Drive(c, gas, brake, steer, c.sirenCmd);
}

void CopControl(CopState& c, float dt) {
    const bool playerInCar = gThreat.playerInVehicle;
    const Vec3 pPos = gThreat.playerPos;
    const Vec3 pVel = gThreat.playerVel;

    // the prey's frame, hoisted (the 39-package): his nose follows
    // his velocity on the road; standing still, the host's own pose
    // answers (the tail only needs a direction to stand behind - at
    // a standstill any error is a metre, inside the pacing blend's
    // own slack). The intercept below aims in this frame, and the
    // package's aim laws - roles, the PIT, the head-on, the front
    // unit's brake-check - read it after, so it is computed once.
    Vec3 pFwd(0, 1, 0);
    {
        Vec3 flat = pVel;
        flat.z = 0.0f;
        if (flat.Length() > 2.0f) {
            pFwd = flat * (1.0f / flat.Length());
        } else if (playerInCar && sPreyVeh < 0 && sPreyPed < 0) {
            // (the pose verb answers for the PLAYER's own car; under
            // a /coptest prey override the prey is a decoy, whose
            // standing nose the velocity fallback above already
            // covers as well as a standstill allows)
            Vec3 cc, f, chest, v;
            if (TheHost().PlayerVehiclePose(&cc, &f, &chest, &v)) {
                f.z = 0.0f;
                if (f.Length() > 0.01f)
                    pFwd = f * (1.0f / f.Length());
            }
        }
    }
    const Vec3 pRight(pFwd.y, -pFwd.x, 0.0f);
    bool headOn = false; // item 19, set in the aim laws below

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
        // THE COMPLETE UPDATE's first conviction: these two probes were
        // the ONE place round 5's own-shell lesson was never applied.
        // The probe segments start at the car's own centre and run
        // 1.6 m - INSIDE his own body - and the plain oracle counts his
        // own car as a wall, so both corners read blocked on every
        // reading, at every range, forever. The zone law then read the
        // blocked branch from everywhere: in FRONT the desire still
        // came from the speed rows, but the moment the geometry leaned
        // to a side zone the desire collapsed to kBlockedSpeed (-21.7),
        // and that signed value is what the unit line reported as
        // `want` at EVERY range, and what the feed law turned into the
        // crawl the bridge printed. The probes now skip his own
        // vehicle, exactly like the sight rays round 5 already fixed.
        // THE PLOUGH LAW (the user's amendment, 2026-11-08 09:39 -
        // "nothing ahead of them matters"): the probes ask the
        // WORLD-only reading now. A vehicle standing in the probe line
        // is not geometry: a street of traffic ahead must read CLEAR
        // corners, so the zone law holds the FRONT desire at the pace
        // rows and the unit drives through at speed - no braking, no
        // swerve, no detour for anything movable. Only solid world
        // (buildings, pillars - the geometry the host itself cannot
        // pass) may still read blocked, and that case belongs to the
        // recovery law below, as it always did.
        c.frontLClear = !TheHost().LineBlockedWorld(c.pos, base - right * (150 * kM));
        c.frontRClear = !TheHost().LineBlockedWorld(c.pos, base + right * (150 * kM));
    }

    // the stuck witness's fair start runs down first (fix round 5):
    // a fresh drive order - the spawn, the pursuit entry, a recovery
    // just spent - earns the drive channel its start-up window before
    // stillness may be read as stuck
    if (c.fairStartT > 0.0f)
        c.fairStartT -= dt;

    // the recovery runs its own clock first (the original checks it before
    // anything else steers): reverse, on the rotating strategy's wheel.
    if (c.recoveryT > 0.0f) {
        c.recoveryT -= dt;
        if (c.recoveryT <= 0.0f) {
            c.fairStartT = kCopFairStart; // a spent recovery is a fresh
                // drive order: the swing out of it is not stuck evidence
            // THE COMPLETE UPDATE: a recovery must be seen to COMPLETE,
            // not just to expire - judged against where it began. (The
            // 02:00 session's log tail was a wall of bare "stuck"
            // lines at speed 0 with no completion anywhere; this line
            // is the receipt the next wedge is graded against.)
            const float moved = (c.pos - c.recoveryFrom).Length();
            if (moved >= 1.5f)
                Log("cop: recovery completed - he backed %.1f m clear and is moving again (strategy %d)",
                    moved, c.recoverStrategy);
            else
                Log("cop: recovery spent but he is still on the spot (moved %.1f m) - the next witness will judge again",
                    moved);
        }
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
        // THE ROOT STEP retires the complete update's feed-served
        // recovery, and its history is kept straight: the complete
        // update was right ABOUT THE OLD WORLD - under autopilot
        // ownership the pedals were ProcessAI's outputs and a
        // pedal-only recovery had no channel. That world is gone.
        // With no mission and no drive task on the body, the words
        // above - a reversed desire riding the brake field, the
        // strategy's wheel swing on the steer field - ARE the channel,
        // the same words the recovery computed in every era; the
        // 11 m rear feed point and its leaned geometry have no reader
        // left and are gone. The manoeuvre is unchanged: direction
        // chosen once at the start, wheel swung by the strategy's
        // pattern, 22 ticks of it.
        c.sirenCmd = SirenFor(c); // item 39: the siren is a law now, not a literal
        Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), c.sirenCmd);
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
        // the fair start gates the accumulation (fix round 5): inside
        // the window after a fresh drive order the clock simply does
        // not run - the eye session of 2026-11-08 had fresh pursuit
        // units cycling strategies 1..7 within three seconds of
        // joining, convicting cars that had barely been told to move
        // (and whose mirror-driven crawl the earlier rounds explain).
        // Past the window the original's witness stands unchanged: a
        // truly wedged car still reverses out on the same clock.
        if (c.fairStartT <= 0.0f &&
            std::fabs(c.speed) < 6 * kV && std::fabs(c.desiredSpeed) > 6 * kV &&
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
        c.recoveryFrom = c.pos; // the completion is judged from here
        ++c.recoverCount;
        if (c.recoverCount >= 6)
            Log("cop: stuck - reverse recovery, strategy %d (recovery #%d for this unit - a count /coptest grades)",
                c.recoverStrategy, c.recoverCount);
        else
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
        if (!playerInCar)
            target = intercept; // SA adaptation, the user's order
                // (2026-11-07): the hunt binds to the MAN, never to his
                // car - bailed out on foot, there is no tail to stand
                // behind, so he drives at the man himself and blocks his
                // path with the car. In Driver 2 the man is never out of
                // a car and FELONY.C's gates all read the car; the
                // player-binding is ours, named as such in the step report.
                // THE ZERO-MERCY AMENDMENT (2026-11-08) moves this branch
                // ABOVE the batter flank: at max felony the flank offset
                // used to win even over a man on foot, which put the aim
                // 2.5 m BESIDE him - a pass-by offset on the one prey the
                // order names exactly («کامل خودم رو دقیقا هدف بگیرن چه
                // پیاده چه با ماشین»). On foot the target is the man
                // himself at EVERY felony, batter included: no flank, no
                // tail, no offset. The flank below is a car geometry and
                // only ever applies to a man in a vehicle.
        else if (c.batter)
            target = intercept + pRight * (5.0f * 0.5f); // his flank: 5 units,
                // the matrix arithmetic of the original lands at ~2.5 m
        else
            target = intercept - pFwd * 2.5f; // the CarTail: behind his rear
        // ---- the 39-package's aim laws, laid over the original's
        // target (car prey only - the man on foot is aimed at himself
        // at every felony, the zero-mercy amendment above, and no
        // role or manoeuvre in this package moves that point)
        if (playerInCar) {
            Vec3 relU = c.pos - pPos;
            relU.z = 0.0f;
            const float distU = relU.Length();
            const float latU = relU.Dot(pRight);
            const float fwdU = relU.Dot(pFwd);
            // item 19, the head-on: he is driving AT this unit (the
            // unit stands in his forward cone, closing) - the unit
            // does not take the tail or a flank; it takes his centre
            // (the desire press rides in the front zone below).
            // Chicken is a two-player game only if both play. THE
            // GATE'S SECOND HALF is the suite's own conviction: the
            // unit must not already be OUTRUNNING him - a unit that
            // overshoots ahead of the prey in the ordinary dance sits
            // in his forward cone too, and the first staging of this
            // law read that as a head-on forever: floored at the
            // straight row it fled the chase at full pace and lost
            // him. A head-on is a MEETING - it ends the moment the
            // unit is the faster one running away.
            // (and the unit must not be the tactics tick's posted
            // FRONT unit: that unit stands in his forward cone BY
            // ASSIGNMENT, brake-check post in hand - the head-on
            // pattern is for a unit he drives at by circumstance,
            // and letting it override the post collapses the front
            // unit's aim onto his centre, which the suite's roles
            // staging convicted on sight)
            headOn = pVel.Length() > 8.0f && distU > 2.0f && distU < 70.0f &&
                     fwdU > 0.6f * distU && c.vel.Length() < pVel.Length() + 6.0f &&
                     c.role != 3;
            if (headOn || sSyncT > 0.0f || c.lastRamT > 0.0f) {
                // the head-on, the synchronised hit (item 11: the
                // flank aim collapses to the centre for the window)
                // and the dying unit's last ram (item 8) all aim at
                // the man himself, dead centre
                target = intercept;
            } else if (c.batter && distU < 15.0f) {
                // item 6, the PIT: at batter, close, the aim is his
                // rear quarter on THIS unit's own side - the hit that
                // turns him, priced again in the kick's spin
                const float side = latU >= 0.0f ? 1.0f : -1.0f;
                target = intercept - pFwd * 1.9f + pRight * (side * 0.95f);
            } else if (c.role == 1) {
                target = intercept + pRight * 3.4f; // item 1: the
                    // right flank rides at his door, not his centre
            } else if (c.role == 2) {
                target = intercept - pRight * 3.4f; // the left flank
            } else if (c.role == 3) {
                target = intercept + pFwd * 9.0f; // item 1: the front
                    // unit runs ahead of him, to brake-check him
            }
            // item 24/25's aim override (a traffic car in the
            // corridor to be shoved into him, a wounded friend to be
            // pushed into the fight) outranks every pattern above:
            // the tactics tick only sets it for one tick at a time
            if (c.aimOverrideOn)
                target = c.aimOverride;
            // item 35 outranks even those: while he is in the air
            // there is no car to shove or flank - there is only the
            // landing he is making for
            if (sLandValid)
                target = sLandPoint;
        }
    } else {
        // FAR: WibbleDownTheRoad - walk the field's descent from where he
        // stands by his speed and 680 units of road, and aim at the point
        // it reaches; a straight descent earns the faster table.
        // FIX ROUND 5 - the floor under the walk: in the original the
        // descent point is a STEERING AIM and his speed comes from his
        // tables, so a point 2.7 m ahead at a standstill is harmless.
        // On this host the same point becomes the autopilot's
        // DESTINATION, and ProcessAI brakes to arrive at a destination:
        // at crawl speed the walk is |v|*0.216 + 2.66 m, the car brakes
        // to arrive, its speed stays at the crawl, the walk stays 2.7 m
        // - the crawl feeds itself. The walk therefore never goes
        // under 30 m of road (the descent stops at the field's goal on
        // its own, so the floor can never overshoot the man).
        // THE 5x UPDATE - the walk's length is FeedLookAhead's law
        // (its header): the original's own term was |v|*0.216 + 2.66 m,
        // which at the new paper rows still names a point ~60 m out
        // for a car covering that in a quarter second. The law's 0.9 s
        // of travel, capped at 180 m, is the walk: under the middleman
        // it was the feed's reach (an SA-channel adaptation, named);
        // THE ROOT STEP keeps it as the steering aim's reach - the
        // descent point the wheel aims at stands that far down the
        // road, which is also what keeps a fast approach's steering
        // calm instead of hunting waypoint to waypoint.
        const float ahead = FeedLookAhead(c.speed);
        Vec3 far, farther;
        bool straight = false;
        // item 28, the stacked floors: when the prey stands more
        // than a storey above or below this unit, every second unit
        // starts its descent from a point at the PREY's height - the
        // nearest-node snap then lands on his floor's road instead
        // of crawling the ramps up from its own. (Most stacked nodes
        // in the graph are crossings and underpasses, where the same
        // law simply reads the crossing's upper road - named, not
        // hidden: a multi-storey car park is the case it serves,
        // the crossings are the cost it pays.)
        Vec3 from = c.pos;
        if (std::fabs(pPos.z - c.pos.z) > 4.0f && ((&c - gCops) % 2 == 1))
            from.z = pPos.z;
        if (c.bikeUnit && (pPos - c.pos).Length() < 90.0f) {
            // item 36: at working range a bike does not ride the
            // road field's descent - it cuts at the man directly,
            // the way a bike actually hunts (and where a car would
            // be reading the road ahead, the bike is already coming)
            target = pPos;
        } else if (sPreyOffRoad) {
            // item 18: he has left the road - grass, dirt, a car
            // park, the beach. The descent would keep the unit on
            // the graph while he cuts the field; instead the unit
            // leaves the road too and the feed drives cross-country
            // at the man himself, the same fallback the no-field
            // case already uses, now chosen on purpose
            target = pPos;
        } else if (Page().fieldOk && VehicleFieldDescent(from, ahead, &far)) {
            target = far;
            if (VehicleFieldDescent(from, ahead * 2.2f, &farther)) {
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
        c.desiredSpeed = (straight ? kSpeed2[kDifficulty] : kSpeed1[kDifficulty]) * PaceMul(c);
        // the 39-package's far floors: a prey mid-turn (item 12) or a
        // unit spending its last ram (item 8) never reads the far
        // desire under the strike rows
        if (sTurnStrike)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed1[kDifficulty] * PaceMul(c));
        if (c.lastRamT > 0.0f)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed2[kDifficulty] * PaceMul(c));
        // (pathStraight is consumed by the speed choice and the thrust
        // bonus below; it is recomputed with the target every far tick)
        if (sLandValid)
            target = sLandPoint; // item 35: the far road walk is
                                  // pointless under an airborne prey -
                                  // the landing is the destination
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
        // THE ROOT STEP: the far branch's descent point is a STEERING
        // AIM now, nothing more - the wheel above was always computed
        // toward it; the round-5 sentence "the feed is the descent
        // point itself" is retired with the feed.
        c.sirenCmd = SirenFor(c); // item 39: the siren is a law now, not a literal
        Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), c.sirenCmd);
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
        c.desiredSpeed = kSpeed1[kDifficulty] * PaceMul(c);
        // the anti-rear-end pacing: close and not battering, blend the
        // desire toward just under the man's own speed. Car-to-car only:
        // on foot the man gets the full pressure speed (the same user
        // order - a swap or a bail-out cleanses nothing).
        // THE 5x UPDATE makes the batter gate below the whole law, in
        // words: at felony >= 2457 (c.batter) the blend is DEAD - his
        // speed is never tied to the prey's again, and the side zones
        // floor their desire at the batter pace to match (the root
        // step moved that floor from the retired feed onto the desire
        // itself). Below
        // batter the original's blend stands as it was. THE 39-PACKAGE
        // adds its own two deaths to the blend: a prey mid-turn (item
        // 12) and a prey in his crash window (item 31) are struck at
        // full pace, never paced - those are the two moments he
        // cannot answer, and tying our speed to his then is mercy.
        if (cost < kPaceCost && !c.batter && playerInCar && sCrashT <= 0.0f && !sTurnStrike) {
            const float pSpeed = pVel.Length();
            const float w = cost / kPaceCost;
            const float paced = std::fmax(pSpeed - 20 * kV, 20 * kV);
            c.desiredSpeed = w * c.desiredSpeed + (1.0f - w) * paced;
            if (c.desiredSpeed < 0.0f)
                c.desiredSpeed = 0.0f;
        }
        // the 39-package's front-zone rulings, after the blend:
        // item 1's brake-check - the FRONT unit, standing ahead of
        // him on his own line and close, caps his desire just under
        // the prey's own speed: the prey may brake, or he may hit
        // the unit; running through the gap is the one answer he is
        // not offered
        if (c.role == 3 && playerInCar) {
            Vec3 relU = c.pos - pPos;
            relU.z = 0.0f;
            if (relU.Dot(pFwd) > 2.0f && relU.Length() < 16.0f)
                c.desiredSpeed = std::fmin(c.desiredSpeed,
                                           std::fmax(pVel.Length() - 2.0f, 8.0f));
        }
        // item 19's head-on desire: his own pace, pressed a fifth
        // harder - the unit meets him coming, it does not flee him
        // at the straight row (the same conviction as the gate: a
        // full-row floor here swung the unit's turn so wide at the
        // overshoot that it left the road and the chase)
        if (headOn)
            c.desiredSpeed = std::fmax(c.desiredSpeed,
                                       pVel.Length() * 1.2f / (25.0f / 138.0f) * PaceMul(c));
        // items 12/31's strike floors and item 8's last ram, again:
        // whatever the blend or the brake-check just said, a turning
        // or crashed prey is struck at the rows, and a dying unit
        // spends its window at the straight row
        if (sTurnStrike || sCrashT > 0.0f)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed1[kDifficulty] * PaceMul(c));
        if (c.lastRamT > 0.0f)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed2[kDifficulty] * PaceMul(c));
        wantWheel = t > 0.5f ? Clamp(n * 512.0f / t, -512.0f, 512.0f)
                             : (n >= 0.0f ? 512.0f : -512.0f);
    } else if (zone == ZONE_BACK) {
        c.desiredSpeed = kBackSpeed;
        wantWheel = ((n < 0.0f) != (c.speed < 0.0f)) ? -284.0f : 284.0f;
    } else {
        c.desiredSpeed = kSideSpeed;
        // the strike floors ride the side zones too (items 12/31/8):
        // a unit swinging around the prey's side when the window
        // opens does not get to keep manoeuvre pace - the strike is
        // struck at the rows in every forward zone. The BACK zone
        // alone is exempt: its reverse is a mechanical extraction,
        // not a pace decision, and flooring it would floor the
        // turnaround itself
        if (sTurnStrike || sCrashT > 0.0f)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed1[kDifficulty] * PaceMul(c));
        if (c.lastRamT > 0.0f)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed2[kDifficulty] * PaceMul(c));
        // THE ROOT STEP's one moved floor: while the felony stands at
        // batter, a side zone's manoeuvre desire does not crawl - the
        // complete update enforced this on the FEED's cruise (a side
        // desire fed a strike pace); with the feed gone the DESIRE
        // itself carries it, ahead of the blocked override below (a
        // cornered unit's reverse stays the extraction it always was)
        if (c.batter)
            c.desiredSpeed = std::fmax(c.desiredSpeed, kSpeed1[kDifficulty] * PaceMul(c));
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
    // THE ROOT STEP: no feed follows this Drive - the words computed
    // above (the zone law's wheel, the thrust law's pedals, the signed
    // desire honouring itself through the brake field in the back and
    // blocked zones) ARE the drive. What PursuitFeed used to add on
    // top of them is accounted for in the stub comment where it stood:
    // the through-drive is native (nothing here brakes AT the target),
    // and the side zones' batter floor rides the desire itself (see
    // the side branch).
    c.sirenCmd = SirenFor(c); // item 39: the siren is a law now, not a literal
    Drive(c, g, b, Clamp(c.wheel / 512.0f, -1.0f, 1.0f), c.sirenCmd);
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

bool SpawnUnit(int slot, const Vec3& at, const Vec3& face, bool block, bool bike = false) {
    PageSnapshot snap;
    const bool keepPage = slot == 0;
    if (keepPage)
        SnapshotPage(&snap);
    Vec3 d = face - at;
    const float heading = std::atan2(d.x, d.y) * 57.29578f;
    // item 36: a bike order asks the host for a bike body first; a
    // host with no bike in it answers -1 and the order honestly
    // falls back to a car (logged - the unit is then a car in every
    // way that matters, and bikeUnit stays false)
    int veh = -1;
    bool gotBike = false;
    if (bike) {
        veh = TheHost().SpawnCopBikeBody(at, heading, face);
        if (veh >= 0)
            gotBike = true;
        else
            Log("cop: this host has no bike body - the motorcycle order falls back to a car");
    }
    if (veh < 0)
        veh = TheHost().SpawnCopBody(at, heading, face);
    if (veh < 0)
        return false;
    gCops[slot] = CopState();
    CopState& c = gCops[slot];
    c.veh = veh;
    c.pos = at;
    c.wreckLine = kWreckLine;
    // item 26's counter-pick: the replacement is measured against
    // the prey this pursuit has actually seen, not the standard
    // unit. The scale is his DEMONSTRATED ceiling against his
    // present pace (both m/s, read at the spawn): a man who has
    // shown 45 m/s and now cruises at 20 gets a unit scaled to the
    // fastest version of him (45 x 1.12 / 20), capped at half again.
    // The first spawn of a pursuit, before any measurement, rides
    // 1.0. Under the 5x rows the absolute desire is already far past
    // any prey - this scale's real work is keeping the unit's
    // prey-relative pacing honest against the fastest him on record
    {
        const float nowMps = gThreat.playerVel.Length();
        c.paceScale = Clamp(sPreyTopSeen * 1.12f / std::fmax(nowMps, 12.0f), 1.0f, 1.5f);
    }
    c.blockUnit = block;
    c.bikeUnit = gotBike;
    c.mode = block ? COP_BLOCK : COP_IDLE;
    c.fairStartT = kCopFairStart; // a fresh body is a fresh drive
        // order: the stuck witness waits out its start-up (fix round 5)
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
bool sSpawnFront = false; // item 5: the batter alternation's side
bool sSpawnFlip = false;  // and its toggle, reset with the page so
                          // every pursuit's first batter spawn is
                          // the head-on one
bool LadderSpawnPoint(const Vec3& playerPos, Vec3* out, const Vec3* prefer = nullptr) {
    // item 5: at the batter rung, alternate spawns prefer his front;
    // the flip happens once per query (this function runs exactly
    // once per spawn), and only at batter - at ordinary felonies the
    // behind law stands untouched
    sSpawnFront = false;
    if (Page().felony >= kFelonyCap - 1.0f) {
        sSpawnFlip = !sSpawnFlip;
        sSpawnFront = sSpawnFlip;
    }
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
        // item 5: at the batter rung the ladder stops arriving only
        // from behind - alternate spawns score his FRONT hemisphere
        // instead, so the next car greens up ahead of him, head-on
        // (sSpawnFront flips once per point query, above the loop)
        float score = behind * 100.0f - std::fabs(d - kSpawnRing);
        if (sSpawnFront)
            score = -behind * 100.0f - std::fabs(d - kSpawnRing);
        // item 16's handover preference: among the in-band candidates
        // the one nearest the fallen unit's spot scores up - the band
        // itself (50..95 m from the man, on the road) is NOT widened:
        // a replacement still never lands in his face, it just takes
        // the side of the ring the pressure was lost on. While a
        // handover is active the preference REPLACES the generic
        // behind score rather than bidding against it: the behind
        // rule is a cold spawn's guess at where pressure belongs,
        // and at a standstill its default facing (south) is no
        // information at all - the suite's first staging of this
        // law watched a south default outvote an east fall by
        // construction, every time
        if (prefer) {
            Vec3 dp = np - *prefer;
            dp.z = 0.0f;
            score = -std::fabs(d - kSpawnRing) - dp.Length() * 1.5f;
        }
        // THE PRESENCE TUNE (the user's ابهت order, 2026-11-08): a
        // point a living unit already stands on is worth less - each
        // unit within 30 m of the candidate costs it 60 of score, so
        // successive ladder spawns walk AROUND the ring instead of
        // stacking on the one best node and convoying in from a
        // single street. The force reads as a fan closing from
        // several directions; the discount is strong but not absolute
        // (a sparse corner of the map still takes its only node).
        for (int s = 0; s < kMaxCopUnits; ++s) {
            if (!UnitLiving(gCops[s]))
                continue;
            Vec3 du = gCops[s].pos - np;
            du.z = 0.0f;
            if (du.Length() < 30.0f)
                score -= 60.0f;
        }
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
// block: TWO ROWS of two cars across it, asleep - the front row at the
// block point (lateral ±2.3, noses across the lanes, piece 2's law) and
// a second row one car-length of road BEHIND it toward the approaching
// player, staggered to the opposite laterals (±1.4) with its noses
// tipped ~25 degrees back toward the traffic, the original's angled
// second row (piece 2 shipped one row and named this as its follow-up;
// the complete update carries it in).
// item 15: the block stands at his BRAKING distance, not a fixed
// 48 m: v^2/(2a) at a = 6.5 m/s^2, a hard stop on asphalt - a fast
// man meets the line where he can no longer stop short of it, a
// slow man's block stays a meeting. Floored at 35 (the old band's
// near end) and capped at 110, where the graph runs out
float BlockAheadFor(float preySpeedMps) {
    return Clamp(preySpeedMps * preySpeedMps / (2.0f * 6.5f), 35.0f, 110.0f);
}

// one block car's slot around a block point - PlaceRoadblock's row
// geometry, extracted whole: item 2's creep re-reads the same slots
// around a moved point, and item 22's airport row stands by it
void BlockSlotPos(const Vec3& at, const Vec3& segDir, int idx, Vec3* carAt, Vec3* face) {
    const Vec3 lateral(segDir.y, -segDir.x, 0.0f); // across the lanes
    const int row = idx / 2, col = idx % 2;
    if (row == 0) {
        const float side = col == 0 ? 2.3f : -2.3f;
        *carAt = at + lateral * side;
        // the nose turns across the road, toward the far side: the
        // two cars meet nose-outward across the lanes between them
        *face = *carAt - lateral * (side > 0.0f ? 5.0f : -5.0f);
    } else {
        // the second row: 6 m of road back toward the player (segDir
        // runs with his travel), staggered to the opposite laterals,
        // noses across the road AND tipped back toward the traffic -
        // the angled row: a driver who threads the first gap meets a
        // nose, not a lane
        const float side = col == 0 ? -1.4f : 1.4f;
        *carAt = at - segDir * 6.0f + lateral * side;
        *face = *carAt - lateral * (side > 0.0f ? 5.0f : -5.0f) - segDir * 2.2f;
    }
}

// stand a block row at a point: the spawn half of every placement
// (the plain roadblock, item 22's airport approach, and - at one
// fewer car - item 30's wreck-discounted row in layer C)
int PlaceBlockAt(const Vec3& at, const Vec3& segDir, int maxCars) {
    // item 30: a wreck already standing in the line IS part of the
    // line - when a burned-out unit lies within 45 m of the point,
    // the first berth is left to the wreck and one car fewer is
    // placed (never below a two-car line)
    int startIdx = 0;
    if (maxCars > 2) {
        for (int i = 0; i < sWreckCount; ++i) {
            Vec3 dw = sWrecks[i] - at;
            dw.z = 0.0f;
            if (dw.Length() < 45.0f) {
                startIdx = 1;
                Log("cop: a wreck already stands in this line - one berth is his, and one car fewer is placed");
                break;
            }
        }
    }
    int placed = 0;
    for (int slot = kMaxCopCars; slot < kMaxCopCars + kMaxBlockCars && placed + startIdx < maxCars; ++slot) {
        if (UnitLiving(gCops[slot]))
            continue;
        const int idx = placed + startIdx;
        Vec3 carAt, face;
        BlockSlotPos(at, segDir, idx, &carAt, &face);
        if (SpawnUnit(slot, carAt, face, true)) {
            ++placed;
            Log("cop: roadblock car asleep across the road at (%.0f, %.0f) (row %d)",
                carAt.x, carAt.y, idx / 2 + 1);
        }
    }
    if (placed > 0) {
        sBlockPoint = at;
        sBlockDir = segDir;
        sBlockLive = true;
        RadioLine(8, at); // item 39: a set block is announced
    }
    return placed;
}

// the block point on the prey's road, `ahead` metres along the found
// route (PlaceRoadblock's fan and walk, extracted for items 2/15/22).
// With preferBridge (item 9) the fan keeps looking past the first
// workable route for a point standing over water: a bridge is a wall
// on both sides - the one crossing he cannot leave, and no verge to
// run. The first workable point is kept as the fallback
bool BlockRoutePoint(const Vec3& playerPos, float ahead, Vec3* outAt, Vec3* outSegDir,
                     bool preferBridge) {
    Vec3 pDir(0, 1, 0);
    {
        Vec3 v = gThreat.playerVel;
        v.z = 0.0f;
        if (v.Length() > 2.0f) {
            pDir = v * (1.0f / v.Length());
        } else {
            Vec3 cc, f, chest, vv;
            if (gThreat.playerInVehicle && !PreyOverrideOn() && TheHost().PlayerVehiclePose(&cc, &f, &chest, &vv)) {
                f.z = 0.0f;
                if (f.Length() > 0.01f)
                    pDir = f * (1.0f / f.Length());
            }
        }
    }
    static const float kFanDeg[9] = { 0, 10, -10, 20, -20, 35, -35, 55, -55 };
    bool have = false;
    Vec3 fallbackAt, fallbackDir;
    for (int fi = 0; fi < 9; ++fi) {
        const float a = kFanDeg[fi] * 0.0174533f;
        const float ca = std::cos(a), sa = std::sin(a);
        const Vec3 dir(pDir.x * ca - pDir.y * sa, pDir.x * sa + pDir.y * ca, 0.0f);
        // the fan's reach must cover the point it hunts, with the
        // road's own winding on top: at item 15's braking distances
        // (up to 110 m of ROAD) a 64 m fan crowned the block at the
        // route's end instead, and even a 140 m fan read 85 where
        // 110 was the law - the reach is the distance, half again,
        // and a margin
        const Vec3 dest = playerPos + dir * (ahead * 1.6f + 30.0f);
        std::vector<Vec3> route;
        float roadLen = 0.0f;
        if (!VehicleRoute(playerPos, dest, &route, &roadLen) || route.size() < 2 || roadLen < 35.0f)
            continue;
        // the point: `ahead` metres of road along the route
        float covered = 0.0f;
        Vec3 at = route.back();
        Vec3 segDir = pDir;
        for (size_t i = 1; i < route.size(); ++i) {
            Vec3 seg = route[i] - route[i - 1];
            seg.z = 0.0f;
            const float sl = seg.Length();
            if (covered + sl >= ahead) {
                const float t = sl > 0.001f ? (ahead - covered) / sl : 0.0f;
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
        if (!preferBridge) {
            *outAt = at;
            *outSegDir = segDir;
            return true;
        }
        if (!have) {
            have = true;
            fallbackAt = at;
            fallbackDir = segDir;
        }
        float wl = 0.0f;
        if (TheHost().WaterLevel(at, &wl) && wl < at.z - 1.0f && wl > at.z - 30.0f) {
            *outAt = at; // over water, not in it: a bridge deck
            *outSegDir = segDir;
            Log("cop: the block takes the bridge - water under the road at the crossing");
            return true;
        }
    }
    if (have) {
        *outAt = fallbackAt;
        *outSegDir = fallbackDir;
        return true;
    }
    return false;
}

bool PlaceRoadblock(const Vec3& playerPos) {
    const float ahead = BlockAheadFor(gThreat.playerVel.Length()); // item 15
    Vec3 at, segDir;
    if (!BlockRoutePoint(playerPos, ahead, &at, &segDir,
                         Page().felony >= 1638.0f)) { // item 9's bridge taste
        Log("cop: roadblock wanted but no road ahead of the player took it");
        return false;
    }
    const int placed = PlaceBlockAt(at, segDir, kMaxBlockCars);
    if (placed > 0) {
        Log("cop: ROADBLOCK placed %.0f m ahead of the player (%d car%s)",
            ahead, placed, placed > 1 ? "s" : "");
        return true;
    }
    return false; // the road was found but no block slot is free
}

// item 22: the airport run, read in advance. At the ladder's top,
// when he is driving AT an airport (his heading carries him to it
// and it is near enough to be the run's end), the block does not
// stand on his road at braking distance - it stands on the approach
// itself, 40 m of road before the door, where the airport's own
// roads give him no second way in. (Stations: the atlas carries no
// station kind - named in the report, not invented here.)
bool TryAirportBlock(const Vec3& playerPos) {
    if (Page().felony < 2457.0f)
        return false;
    Vec3 v = gThreat.playerVel;
    v.z = 0.0f;
    if (v.Length() < 4.0f)
        return false;
    const Vec3 vd = v * (1.0f / v.Length());
    int best = -1;
    float bestD = 550.0f;
    AtlasPoiInfo poi;
    for (int i = 0; i < AtlasPoiCount(); ++i) {
        if (!AtlasPoiGet(i, poi) || poi.kind != 15)
            continue;
        Vec3 to = poi.at - playerPos;
        to.z = 0.0f;
        const float d = to.Length();
        if (d < 60.0f || d > 550.0f)
            continue;
        // a loose gate on purpose: the LS approach road bends some
        // 64 degrees off the anchor mid-run (the suite's staging
        // read dot 0.44 at the fire moment and a 0.5 gate silently
        // declined) - the ROUTE to the door's road access, checked
        // below, is the commitment proof; the snapshot heading only
        // excludes a man driving away
        if (to.Dot(vd) < 0.25f * d)
            continue;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    if (best < 0 || !AtlasPoiGet(best, poi))
        return false;
    // the run's end is the door's ROAD ACCESS: an airport's anchor
    // stands on its runways, where no road route exists (the suite's
    // first staging routed at the anchor and silently declined) -
    // the approach a car actually drives ends at the nearest road
    // point to the door
    Vec3 doorRoad = poi.door;
    {
        const int dn = VehicleNearestNode(poi.door, 120.0f);
        Vec3 np;
        if (dn >= 0 && VehicleNodePos(dn, &np))
            doorRoad = np;
    }
    std::vector<Vec3> route;
    float roadLen = 0.0f;
    if (!VehicleRoute(playerPos, doorRoad, &route, &roadLen) || route.size() < 2 ||
        roadLen < 80.0f)
        return false;
    const float want = roadLen - 40.0f;
    float covered = 0.0f;
    Vec3 at = route.back();
    Vec3 segDir = vd;
    for (size_t i = 1; i < route.size(); ++i) {
        Vec3 seg = route[i] - route[i - 1];
        seg.z = 0.0f;
        const float sl = seg.Length();
        if (covered + sl >= want) {
            const float t = sl > 0.001f ? (want - covered) / sl : 0.0f;
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
    const int placed = PlaceBlockAt(at, segDir, kMaxBlockCars);
    if (placed > 0) {
        Log("cop: the airport run is read - the block stands on his approach to %s, 40 m of road before the door",
            poi.name);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The tactics tick (the 39-package): once a second, the force is given
// its shape - who leads, who flanks, who brake-checks from the front
// (item 1); which side of the prey is walled, so the batter comes from
// the open side and throws him at the wall (item 13); and whether two
// units stand on opposite sides of him in range of the synchronised
// hit (item 11). CopControl reads the assignments; nothing here drives.
void TacticsTick(const Vec3& preyPos) {
    // items 24/25: the aim overrides are THIS tick's property - the
    // tactics tick owns them, clears every one, and re-earns the ones
    // whose geometry still holds; CopControl only reads them
    for (int slot = 0; slot < kMaxCopUnits; ++slot)
        gCops[slot].aimOverrideOn = false;
    int ps[kMaxCopUnits];
    int n = 0;
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (!UnitLiving(c))
            continue;
        c.role = 0;
        if (c.mode == COP_PURSUE && c.bailState == 0 && !c.holding)
            ps[n++] = slot;
    }
    if (n == 0) {
        sWallOpenSide = 0.0f;
        return;
    }
    // the prey's frame: his nose follows his velocity on the road
    Vec3 pFwd(0, 1, 0);
    {
        Vec3 flat = gThreat.playerVel;
        flat.z = 0.0f;
        if (flat.Length() > 2.0f)
            pFwd = flat * (1.0f / flat.Length());
    }
    const Vec3 pRight(pFwd.y, -pFwd.x, 0.0f);
    auto latOf = [&](int slot) {
        Vec3 rel = gCops[slot].pos - preyPos;
        rel.z = 0.0f;
        return rel.Dot(pRight);
    };
    auto fwdOf = [&](int slot) {
        Vec3 rel = gCops[slot].pos - preyPos;
        rel.z = 0.0f;
        return rel.Dot(pFwd);
    };
    auto distOf = [&](int slot) {
        Vec3 rel = gCops[slot].pos - preyPos;
        rel.z = 0.0f;
        return rel.Length();
    };
    // the lead: the nearest pursuer, always
    int lead = ps[0];
    for (int i = 1; i < n; ++i)
        if (distOf(ps[i]) < distOf(lead))
            lead = ps[i];
    // item 13: the wall reading - solid world within 9 m on either
    // side of the man. The open side is where the batter wants to be:
    // a hit struck FROM the open side throws him AT the wall.
    sWallOpenSide = 0.0f;
    if (gThreat.playerInVehicle) {
        const bool wR = TheHost().LineBlockedWorld(preyPos + pRight * 9.0f, preyPos);
        const bool wL = TheHost().LineBlockedWorld(preyPos - pRight * 9.0f, preyPos);
        if (wR && !wL)
            sWallOpenSide = -1.0f;
        else if (wL && !wR)
            sWallOpenSide = 1.0f;
    }
    // item 11: the synchronised hit - two pursuers inside 20 m on
    // OPPOSITE sides of him, both genuinely beside him (|lat| > 2):
    // for the window's length their flank aim collapses to his centre
    // and they arrive together
    for (int i = 0; i < n && sSyncT <= 0.0f; ++i) {
        if (distOf(ps[i]) > 20.0f || std::fabs(latOf(ps[i])) < 2.0f)
            continue;
        for (int j = i + 1; j < n; ++j) {
            if (distOf(ps[j]) > 20.0f || std::fabs(latOf(ps[j])) < 2.0f)
                continue;
            if ((latOf(ps[i]) > 0.0f) != (latOf(ps[j]) > 0.0f)) {
                sSyncT = 1.5f;
                Log("cop: two units stand on opposite sides of him - the synchronised hit is armed");
                break;
            }
        }
    }
    if (n < 3)
        return; // lead alone (or lead + one): no flanks to assign
    // the front (item 1): with three or more, the unit standing
    // furthest AHEAD of him on his own road - if he truly is ahead -
    // takes the brake-check role
    int front = -1;
    float frontBest = 4.0f;
    for (int i = 0; i < n; ++i) {
        if (ps[i] == lead)
            continue;
        const float f = fwdOf(ps[i]);
        if (f > frontBest) {
            frontBest = f;
            front = ps[i];
        }
    }
    if (front >= 0)
        gCops[front].role = 3;
    // the flanks: the strongest lateral stands on each side, among
    // those with no other role. A flank on the prey's WALLED side is
    // stood down to a second lead - the batter belongs on the open
    // side (item 13), and a man hugging a wall is not flanked into it.
    int flankR = -1, flankL = -1;
    float latR = 0.5f, latL = -0.5f;
    for (int i = 0; i < n; ++i) {
        const int s = ps[i];
        if (s == lead || s == front)
            continue;
        const float l = latOf(s);
        if (l > latR) {
            latR = l;
            flankR = s;
        }
        if (l < latL) {
            latL = l;
            flankL = s;
        }
    }
    if (flankR >= 0 && sWallOpenSide >= 0.0f)
        gCops[flankR].role = 1;
    if (flankL >= 0 && sWallOpenSide <= 0.0f)
        gCops[flankL].role = 2;

    // ---- item 24: traffic as a weapon. A civilian car standing in
    // a pursuer's corridor, with the prey just past it, is not an
    // obstacle - it is a free ram: the unit aims at the car itself
    // for this second, and the host's own physics delivers the shove
    // into him. The kick stays prey-only; what the traffic car does
    // to him afterwards is the world's arithmetic, not a priced hit.
    for (int i = 0; i < n; ++i) {
        CopState& c = gCops[ps[i]];
        std::vector<Host::VehicleInfo> near;
        TheHost().NearbyVehicles(c.pos, 18.0f, near);
        for (const Host::VehicleInfo& v : near) {
            Vec3 to = v.pos - c.pos;
            to.z = 0.0f;
            const float d = to.Length();
            if (d < 3.0f || d > 18.0f)
                continue;
            const Vec3 dirv = to * (1.0f / d);
            if (dirv.Dot(c.fwd) < 0.7f)
                continue;
            const Vec3 right(c.fwd.y, -c.fwd.x, 0.0f);
            if (std::fabs(to.Dot(right)) > 2.6f)
                continue;
            Vec3 past = preyPos - v.pos;
            past.z = 0.0f;
            if (past.Length() > 35.0f || past.Dot(dirv) < 0.0f)
                continue;
            c.aimOverride = v.pos;
            c.aimOverrideOn = true;
            break;
        }
    }

    // ---- item 25: the wounded friend is spent as the last ram. A
    // pursuer whose book is past 60% and who can barely move, with
    // the prey within 25 m, is worth more as momentum than as a
    // hunter: the nearest sound pursuer is aimed at his rear quarter
    // for this second, to shove him into the hit. Read fresh each
    // tick like item 24 - a geometry that stops holding stops being
    // aimed at.
    {
        int wounded = -1;
        for (int i = 0; i < n; ++i) {
            const CopState& w = gCops[ps[i]];
            if (w.totalDamage > 0.6f * WreckLineOf(w) && w.vel.Length() < 2.0f &&
                (w.pos - preyPos).Length() < 25.0f) {
                wounded = ps[i];
                break;
            }
        }
        if (wounded >= 0) {
            int helper = -1;
            float bestD = 1e18f;
            for (int i = 0; i < n; ++i) {
                if (ps[i] == wounded)
                    continue;
                const float d = (gCops[ps[i]].pos - gCops[wounded].pos).Length();
                if (d < bestD) {
                    bestD = d;
                    helper = ps[i];
                }
            }
            if (helper >= 0) {
                const CopState& w = gCops[wounded];
                gCops[helper].aimOverride = w.pos - w.fwd * 3.2f;
                gCops[helper].aimOverrideOn = true;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The posts tick (the 39-package, layer B): once a second, the force
// is given its PLACES - the doors of the building he is hiding in
// (item 27), the rear exit he just came through (item 9), the mouth
// of the dead end he is driving into (item 34). Posts are held
// through the one hold mechanism; nothing here drives either.
void PostsTick(const Vec3& preyPos) {
    CopState& page = Page();
    // ---- item 27: the man is indoors. He is SOMEWHERE KNOWN - the
    // building - so his position is never "lost" while he is inside,
    // and the force camps the family's street doors: whichever door
    // he comes out of, a car is standing at it
    const char* fam = nullptr;
    if (AtlasHeavenFamAt(preyPos, 4.0f, fam)) {
        page.known = true;
        page.unseenT = 0.0f;
        if (!sIndoor || std::strcmp(sIndoorFam, fam) != 0 != 0) {
            std::snprintf(sIndoorFam, sizeof(sIndoorFam), "%s", fam);
            Log("cop: the prey went inside (%s) - the force camps its doors", fam);
        }
        sIndoor = true;
        int first = 0, count = 0;
        if (AtlasDoorFamRange(fam, first, count) && count > 0) {
            // the family's street doors, nearest first (door counts
            // per family are small; a plain insertion pass serves)
            int order[16];
            int nd = count < 16 ? count : 16;
            for (int i = 0; i < nd; ++i)
                order[i] = first + i;
            for (int i = 1; i < nd; ++i) {
                const int o = order[i];
                const char* f2;
                Vec3 eO, xO, eJ, xJ;
                AtlasDoorGet(o, f2, eO, xO);
                int j = i - 1;
                while (j >= 0) {
                    AtlasDoorGet(order[j], f2, eJ, xJ);
                    if ((eJ - preyPos).Length() <= (eO - preyPos).Length())
                        break;
                    order[j + 1] = order[j];
                    --j;
                }
                order[j + 1] = o;
            }
            int assigned = 0;
            for (int slot = 0; slot < kMaxCopUnits && assigned < nd && assigned < 4; ++slot) {
                CopState& c = gCops[slot];
                if (!UnitLiving(c) || (c.mode != COP_PURSUE && c.mode != COP_IDLE))
                    continue;
                const char* f3;
                Vec3 entry, ex;
                if (!AtlasDoorGet(order[assigned], f3, entry, ex))
                    break;
                c.holdPoint = entry;
                c.holding = true;
                ++assigned;
            }
        }
        return; // while he is inside, the doors are the only posts
    }
    if (sIndoor) {
        sIndoor = false;
        int first = 0, count = 0;
        if (AtlasDoorFamRange(sIndoorFam, first, count)) {
            for (int slot = 0; slot < kMaxCopUnits; ++slot) {
                CopState& c = gCops[slot];
                if (!c.holding)
                    continue;
                for (int i = first; i < first + count; ++i) {
                    const char* f4;
                    Vec3 entry, ex;
                    if (AtlasDoorGet(i, f4, entry, ex) &&
                        (c.holdPoint - entry).Length() < 3.0f) {
                        c.holding = false;
                        break;
                    }
                }
            }
        }
        Log("cop: the prey is outside again - the door camps stand down");
    }

    Vec3 pFwd(0, 1, 0);
    {
        Vec3 flat = gThreat.playerVel;
        flat.z = 0.0f;
        if (flat.Length() > 2.0f)
            pFwd = flat * (1.0f / flat.Length());
    }
    // ---- item 9, the rear-exit guard: at the felony cap, while he
    // runs, one pursuer is re-tasked for ten seconds to hold the road
    // point 30 m BEHIND him - the exit he just came through is a door
    // too, and it stays watched while the pack presses from the front
    if (sRearSlot >= 0) {
        sRearT -= 1.0f;
        if (sRearT <= 0.0f) {
            if (sRearSlot < kMaxCopUnits && gCops[sRearSlot].holding)
                gCops[sRearSlot].holding = false;
            sRearSlot = -1;
            sRearCool = 5.0f; // a spent watch is spent: five seconds
                // before another is posted, or the release is a
                // fiction the same tick re-picks over (the suite's
                // stand-down read convicted exactly that)
        }
    }
    if (sRearCool > 0.0f)
        sRearCool -= 1.0f;
    if (sRearSlot < 0 && sRearCool <= 0.0f && page.felony >= kFelonyCap - 1.0f &&
        gThreat.playerInVehicle) {
        int pick = -1;
        float farD = -1.0f;
        for (int slot = 0; slot < kMaxCopUnits; ++slot) {
            const CopState& c = gCops[slot];
            if (!UnitLiving(c) || c.mode != COP_PURSUE || c.holding)
                continue;
            const float d = (c.pos - preyPos).Length();
            if (d > farD) {
                farD = d;
                pick = slot;
            }
        }
        if (pick >= 0) {
            const Vec3 backPt = preyPos - pFwd * 30.0f;
            const int node = VehicleNearestNode(backPt, 45.0f);
            Vec3 np;
            if (node >= 0 && VehicleNodePos(node, &np)) {
                gCops[pick].holdPoint = np;
                gCops[pick].holding = true;
                gCops[pick].holdArrived = false;
                sRearSlot = pick;
                sRearT = 10.0f;
                Log("cop: one unit holds the rear exit - the road behind him stays watched");
            }
        }
    }
    // ---- item 34, the dead-end trap: walk his road forward the way
    // he is driving it; if it dies (a degree-1 tip) with a real
    // pocket behind it, the second unit holds the last junction -
    // the mouth - and the pocket has one way out, past a waiting car
    if (sMouthSlot >= 0) {
        sMouthT -= 1.0f;
        if (sMouthT <= 0.0f) {
            if (sMouthSlot < kMaxCopUnits && gCops[sMouthSlot].holding)
                gCops[sMouthSlot].holding = false;
            sMouthSlot = -1;
        }
    }
    if (sMouthSlot < 0 && page.felony >= 1638.0f && gThreat.playerInVehicle &&
        gThreat.playerVel.Length() > 3.0f) {
        const int start = VehicleNearestNode(preyPos, 30.0f);
        if (start >= 0) {
            Vec3 vd = gThreat.playerVel;
            vd.z = 0.0f;
            vd = vd * (1.0f / vd.Length());
            int cur = start, prev = -1, mouth = start, tip = -1;
            float walked = 0.0f;
            Vec3 curPos;
            VehicleNodePos(cur, &curPos);
            for (int step = 0; step < 40; ++step) {
                const int deg = VehicleNodeDegree(cur);
                if (deg >= 3)
                    mouth = cur;
                if (deg <= 1 && cur != start) {
                    tip = cur;
                    break;
                }
                int next = -1;
                float bestAlign = -2.0f;
                Vec3 nextPos;
                for (int i = 0; i < deg; ++i) {
                    const int to = VehicleNodeEdgeTo(cur, i);
                    if (to < 0 || to == prev)
                        continue;
                    Vec3 tp;
                    if (!VehicleNodePos(to, &tp))
                        continue;
                    Vec3 dirv = tp - curPos;
                    dirv.z = 0.0f;
                    const float l = dirv.Length();
                    if (l < 0.01f)
                        continue;
                    const float al = dirv.Dot(vd) / l;
                    if (al > bestAlign) {
                        bestAlign = al;
                        next = to;
                        nextPos = tp;
                    }
                }
                if (next < 0)
                    break; // the road loops on - a junction, no pocket
                walked += (nextPos - curPos).Length();
                prev = cur;
                cur = next;
                curPos = nextPos;
                if (walked > 160.0f)
                    break;
            }
            if (tip >= 0 && walked >= 25.0f) {
                // the second-nearest pursuer takes the mouth (the
                // lead stays on him - a trap with nobody inside it
                // pressing is just a parked car)
                int near1 = -1, near2 = -1;
                float d1 = 1e18f, d2 = 1e18f;
                for (int slot = 0; slot < kMaxCopUnits; ++slot) {
                    const CopState& c = gCops[slot];
                    if (!UnitLiving(c) || c.mode != COP_PURSUE || c.holding)
                        continue;
                    const float d = (c.pos - preyPos).Length();
                    if (d < d1) {
                        d2 = d1;
                        near2 = near1;
                        d1 = d;
                        near1 = slot;
                    } else if (d < d2) {
                        d2 = d;
                        near2 = slot;
                    }
                }
                Vec3 mp;
                if (near2 >= 0 && VehicleNodePos(mouth, &mp)) {
                    gCops[near2].holdPoint = mp;
                    gCops[near2].holding = true;
                    gCops[near2].holdArrived = false;
                    sMouthSlot = near2;
                    sMouthT = 8.0f;
                    Log("cop: his road dies %.0f m ahead - a unit holds the mouth of the dead end",
                        walked);
                }
            }
        }
    }

    // ---- item 3, the memory used: a written junction he passes
    // again earns its ambush in a later pursuit - one far unit
    // pre-holds the recorded exit, once per record per 20 s, and
    // never while another ambush stands
    if (page.known && page.felony > kFelonyTrigger && sAmbushSlot < 0) {
        for (int i = 0; i < sEscapeCount; ++i) {
            EscapeRec& r = sEscape[i];
            if (r.coolT > 0.0f)
                continue;
            Vec3 dj = preyPos - r.junction;
            dj.z = 0.0f;
            if (dj.Length() > 35.0f)
                continue;
            for (int s = 0; s < kMaxCopUnits; ++s) {
                CopState& u = gCops[s];
                if (!UnitLiving(u) || u.mode != COP_PURSUE || u.holding)
                    continue;
                const float dpick = (u.pos - preyPos).Length();
                if (dpick < 50.0f || dpick > 150.0f)
                    continue; // the same reach ceiling as the fresh
                              // ambush: inside the post's lifetime
                u.holding = true;
                u.holdArrived = false;
                u.holdPoint = r.exit;
                sAmbushSlot = s;
                sAmbushT = 8.0f;
                r.coolT = 20.0f;
                Log("cop: the written escape is remembered - a far unit pre-holds its exit before he reaches it");
                break;
            }
            break; // one ambush per posts tick
        }
    }

    // ---- item 7: the ring. A man on foot, standing or nearly so,
    // at felony 1319+, is surrounded where he stands: the whole
    // pursuing force holds a 7.5 m circle around him, engines on
    // (they stay pursuers on the hold mechanism - the drive-through
    // law governs a MOVING man; a pinned one is held, not run
    // through). The ring breaks the moment he moves off its centre,
    // mounts, or the felony falls under the line.
    {
        Vec3 vel = gThreat.playerVel;
        vel.z = 0.0f;
        const bool want = !gThreat.playerInVehicle && page.known &&
                          page.felony >= 1319.0f && vel.Length() < 2.0f;
        if (sRingOn) {
            Vec3 dc = preyPos - sRingCentre;
            dc.z = 0.0f;
            if (!want || dc.Length() > 4.0f) {
                for (int i = 0; i < sRingCount; ++i) {
                    CopState& u = gCops[sRingSlot[i]];
                    u.holding = false;
                    u.holdArrived = false;
                }
                sRingCount = 0;
                sRingOn = false;
                Log("cop: the ring opens - he moves again");
            }
        }
        if (want && !sRingOn) {
            // the ring is the WHOLE force, not the ladder alone: a
            // woken block car left hunting drives through the man
            // while the ring tries to settle around him (the
            // /coptest ring act convicted exactly that - three
            // woken block units dragged its man for 30 s and the
            // ring never settled). Every free pursuer takes a post;
            // the posts are as many as the force, spread evenly.
            // And each unit takes the post nearest ITS OWN side,
            // claimed greedily by distance - a unit ordered to a
            // far-side post would drive through him to reach it,
            // and a surrounding that runs him over on its way in is
            // no surrounding at all.
            int cand[kMaxCopUnits];
            int m = 0;
            for (int s = 0; s < kMaxCopUnits; ++s) {
                CopState& u = gCops[s];
                if (UnitLiving(u) && u.mode == COP_PURSUE && !u.holding)
                    cand[m++] = s;
            }
            if (m >= 2) {
                Vec3 posts[kMaxCopUnits];
                bool claimed[kMaxCopUnits] = {};
                for (int i = 0; i < m; ++i) {
                    const float a = 0.7853982f + (float)i * (6.2831853f / (float)m);
                    posts[i] = preyPos + Vec3(std::sin(a), std::cos(a), 0.0f) * 7.5f;
                }
                int n = 0;
                for (int ci = 0; ci < m; ++ci) {
                    CopState& u = gCops[cand[ci]];
                    int bestPost = -1;
                    float bestD = 1e18f;
                    for (int i = 0; i < m; ++i) {
                        if (claimed[i])
                            continue;
                        const float d = (posts[i] - u.pos).Length();
                        if (d < bestD) {
                            bestD = d;
                            bestPost = i;
                        }
                    }
                    if (bestPost < 0)
                        break;
                    claimed[bestPost] = true;
                    sRingSlot[n] = cand[ci];
                    u.holding = true;
                    u.holdArrived = false;
                    u.holdPoint = posts[bestPost];
                    ++n;
                }
                if (n >= 2) {
                    sRingOn = true;
                    sRingCount = n;
                    sRingCentre = preyPos;
                    Log("cop: the ring closes - %d units hold a circle around him, engines on", n);
                } else {
                    for (int i = 0; i < n; ++i) {
                        CopState& u = gCops[sRingSlot[i]];
                        u.holding = false;
                    }
                }
            }
        }
    }

    // ---- item 29: the shoreline. While he swims the force cannot
    // follow him into the water, so it holds the shore instead: up
    // to three units post at the road points nearest him, spread
    // around, and wait for the bank he must climb. Released the
    // moment he is out of the water.
    {
        float wl = 0.0f;
        const bool swimming = !gThreat.playerInVehicle &&
                              TheHost().WaterLevel(preyPos, &wl) &&
                              wl >= preyPos.z - 0.5f;
        if (sShoreOn && !swimming) {
            for (int i = 0; i < 3; ++i) {
                if (sShoreSlot[i] < 0)
                    continue;
                CopState& u = gCops[sShoreSlot[i]];
                u.holding = false;
                u.holdArrived = false;
                sShoreSlot[i] = -1;
            }
            sShoreOn = false;
            Log("cop: he is out of the water - the shoreline watch stands down");
        }
        if (swimming && !sShoreOn) {
            int n = 0;
            for (int s = 0; s < kMaxCopUnits && n < 3; ++s) {
                CopState& u = gCops[s];
                if (!UnitLiving(u) || u.mode != COP_PURSUE || u.holding)
                    continue;
                const float a = 0.5f + (float)n * 2.0943951f;
                const Vec3 wantPt = preyPos + Vec3(std::sin(a), std::cos(a), 0.0f) * 30.0f;
                const int node = VehicleNearestNode(wantPt, 60.0f);
                Vec3 np;
                if (node < 0 || !VehicleNodePos(node, &np))
                    continue;
                sShoreSlot[n] = s;
                u.holding = true;
                u.holdArrived = false;
                u.holdPoint = np;
                ++n;
            }
            if (n >= 1) {
                sShoreOn = true;
                Log("cop: he swims - %d units hold the shoreline around him", n);
            }
        }
    }
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
    sPreyVeh = -1;
    sPreyPed = -1;
    sTacticsT = 0.0f;
    sPursuitT = 0.0f;
    sHeat = 0;
    sPreyTopSeen = 0.0f;
    sHandover = false;
    sHandoverT = 0.0f;
    sCrashT = 0.0f;
    sSpdOld = 0.0f;
    sSpdAge = 1e9f;
    sSyncT = 0.0f;
    sBikeT = 0.0f;
    sAirborne = false;
    sAirFell = false;
    sLandValid = false;
    sLandedT = 0.0f;
    sTurnRate = 0.0f;
    sTurnStrike = false;
    sHeadOk = false;
    sWallOpenSide = 0.0f;
    sPostsT = 0.0f;
    sRearT = 0.0f;
    sRearSlot = -1;
    sRearCool = 0.0f;
    sMouthT = 0.0f;
    sMouthSlot = -1;
    sIndoor = false;
    sIndoorFam[0] = '\0';
    sCreepT = 0.0f;
    sBlockLive = false;
    sEscapeNext = 0;
    sEscapeCount = 0;
    sAmbushSlot = -1;
    sAmbushT = 0.0f;
    sPreyOffRoad = false;
    sWreckNext = 0;
    sWreckCount = 0;
    sRingOn = false;
    sRingCount = 0;
    sShoreOn = false;
    sSpawnFlip = false;
    sTyreStage = 0;
    sTyreT = 0.0f;
    sTyreVeh = -1;
    sRadioLines = 0;
    sRadioMask = 0;
    sRadioOpen = false;
    sRadioRung = 0;
    sPrevInVeh = false;
    sForceHeard = false;
    sBlockLive = false;
    gThreat.copAlive = false;
    gThreat.copPursuing = false;
    gThreat.felony = 0.0f;
    gThreat.ourUnitsLost = 0;
    CopTestReset(); // a world reset forgets a running test too (its own
                    // guard keeps a test that summoned us alive)
}

void CopDismiss() {
    for (int i = 0; i < kMaxCopUnits; ++i)
        if (gCops[i].veh >= 0)
            TheHost().CopBodyRetire(gCops[i].veh, false);
    CopReset();
}

void CopPreyOverride(int decoyVeh, int decoyPed) {
    sPreyVeh = decoyVeh;
    sPreyPed = decoyPed;
}

void CopForceFelony(float felony) {
    Page().felony = Clamp(felony, 0.0f, kFelonyCap);
    gThreat.felony = Page().felony;
}

void CopKnownSeed() {
    Page().known = true;
    Page().unseenT = 0.0f;
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
    c.wreckLine = kWreckLine;
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
    // feed, unchanged: both brains read the same man from the same record).
    // THE COMPLETE UPDATE: while a /coptest prey override is armed, the
    // record is filled from the DECOY instead - a car decoy reads as a
    // man in a car, a ped decoy as the man on foot - and every use of
    // the tick's `playerPos` below follows `preyPos`. The override is
    // the only door through which the force ever hunts anything but
    // the player, and only the test arms it.
    if (sPreyVeh >= 0) {
        Vec3 dp, dv;
        if (TheHost().DecoyReport(sPreyVeh, &dp, &dv)) {
            gThreat.playerPos = dp;
            gThreat.playerVel = dv;
            gThreat.playerVehicle = sPreyVeh;
            gThreat.playerInVehicle = true;
        } else {
            gThreat.playerPos = playerPos;
            gThreat.playerVel = TheHost().PlayerVelocity();
            gThreat.playerVehicle = TheHost().PlayerVehicle();
            gThreat.playerInVehicle = gThreat.playerVehicle >= 0;
        }
    } else if (sPreyPed >= 0) {
        Vec3 dp;
        float hp = 0.0f;
        if (TheHost().DecoyPedReport(sPreyPed, &dp, &hp)) {
            gThreat.playerPos = dp;
            gThreat.playerVel = Vec3();
            gThreat.playerVehicle = -1;
            gThreat.playerInVehicle = false;
        } else {
            gThreat.playerPos = playerPos;
            gThreat.playerVel = TheHost().PlayerVelocity();
            gThreat.playerVehicle = TheHost().PlayerVehicle();
            gThreat.playerInVehicle = gThreat.playerVehicle >= 0;
        }
    } else {
        gThreat.playerPos = playerPos;
        gThreat.playerVel = TheHost().PlayerVelocity();
        gThreat.playerVehicle = TheHost().PlayerVehicle();
        gThreat.playerInVehicle = gThreat.playerVehicle >= 0;
    }
    const Vec3 preyPos = gThreat.playerPos;

    if (!sForce) {
        gThreat.copAlive = false;
        gThreat.copPursuing = false;
        gThreat.felony = page.felony;
        CopTestTick(dt); // the test grades even a failed stand-up
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
    float struckDriverU[kMaxCopUnits] = {};
    bool anySeen = false;
    bool blastNoted = false; // item 33: one blast, one felony jump
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
        //
        // THE PLOUGH LAW's split (kSceneryShare): is this report's
        // impact a FIGHT contact - the prey at hand, by the bridge's
        // own ram filing or by measure (inside 6 m, the kick band's
        // country)? A fight impact pays the full BCOLLIDE price and
        // attributes to the player. Any other impact - traffic rammed
        // aside, scenery struck at pace - pays the token share and
        // attributes nothing, and health the body lost in the same
        // report is collision-filed and rides the token lane with it.
        // A smooth drain with no impact in it (fire) is untouched by
        // the split and keeps the anchor rate, as the standing law
        // prices it.
        Vec3 preyGap = preyPos - c.pos;
        preyGap.z = 0.0f;
        const bool fightContact = rep.struckRam > 0.5f || preyGap.Length() <= 6.0f;
        const bool impactSpike = rep.impactDv > kImpactFree;
        float impactPrice = 0.0f;
        if (impactSpike) {
            impactPrice = std::fmin((rep.impactDv - kImpactFree) * kImpactRate, kImpactCap) *
                          kPursuerToughness;
            if (!fightContact)
                impactPrice *= kSceneryShare;
            c.totalDamage += impactPrice;
        }
        // the health-delta channels, blast-capped as one sum (fix round
        // 3, see kBlastPrice): a frame that strips the whole body bills
        // one blast, never the whole book. The player's share of the
        // delta (shot + driver channels) scales by the same fraction
        // the cap leaves standing, so the ourUnitsLost attribution at
        // retirement reads the billed book, not the raw host numbers.
        const float worldPts = rep.struckWorld *
                               ((impactSpike && !fightContact) ? kSceneryShare : 1.0f);
        const float rawDeltaPts =
            (rep.struckShot + rep.struckDriver) * kSaBookRate + worldPts * kSaBookRate;
        const float deltaBill = std::fmin(rawDeltaPts, kBlastPrice);
        c.totalDamage += deltaBill;
        float playerDeltaPts = (rep.struckShot + rep.struckDriver) * kSaBookRate;
        if (rawDeltaPts > deltaBill && rawDeltaPts > 0.0f)
            playerDeltaPts *= deltaBill / rawDeltaPts;
        c.playerBookDamage += playerDeltaPts +
                              (fightContact ? impactPrice : 0.0f);
        c.health = Clamp(1.0f - c.totalDamage / WreckLineOf(c), 0.0f, 1.0f);
        // item 33: a blast-class billing (the tick's health-delta
        // channels hit the one-blast cap whole) is the heavy-weapon
        // signature - and the answer is FELONY.C's own type 11,
        // whose placid value IS the cap (4096): the felony jumps
        // straight to the ceiling the moment a blast lands on a
        // unit, it does not climb the ladder. Once per tick, however
        // many units the same blast caught.
        if (deltaBill >= kBlastPrice * 0.99f && !blastNoted) {
            blastNoted = true;
            if (page.felony < kFelonyCap) {
                page.felony = kFelonyCap;
                Log("cop: a blast-class hit on a unit - the felony jumps straight to the cap (his own type 11)");
            }
        }
        // item 8: the dying unit's last ram - past 80% of his book
        // he stops preserving himself: one window of full-pace,
        // centre-aimed attack, his throws priced half again, and
        // then he is spent either way
        if (c.mode == COP_PURSUE && !c.lastRamDone && c.lastRamT <= 0.0f &&
            c.totalDamage > 0.8f * WreckLineOf(c)) {
            c.lastRamT = 4.0f;
            Log("cop: unit %d is dying (book %.0f%% spent) - one last ram at full pace",
                slot, 100.0f * c.totalDamage / WreckLineOf(c));
        }
        if (c.lastRamT > 0.0f)
            c.lastRamT -= dt;
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
        if (c.totalDamage > WreckLineOf(c) && c.mode != COP_WRECKED) {
            const bool wasPursuing = c.mode == COP_PURSUE;
            c.mode = COP_WRECKED;
            c.holding = false; // a spent unit holds no post
            // item 30: the wreck's place is written down - a block
            // placed over it counts it as part of the line
            sWrecks[sWreckNext] = c.pos;
            sWreckNext = (sWreckNext + 1) % 4;
            if (sWreckCount < 4)
                ++sWreckCount;
            Log("cop: a unit's book is spent (%.0f of his points) - he retires and burns as one unit, car and driver together",
                c.totalDamage);
            if (wasPursuing) {
                // item 16: the handover is armed - the ladder's next
                // spawn ignores its timer and enters near the fall,
                // so the pressure never actually lets go of him
                sHandover = true;
                sHandoverPos = c.pos;
                sHandoverT = 3.0f;
            }
            if (c.playerBookDamage * 2.0f >= c.totalDamage) {
                ThreatNoteOurUnitLost();
                Log("cop: that retirement was the player's work - ourUnitsLost = %d (the replacement step's counter)",
                    gThreat.ourUnitsLost);
            }
            RadioLine(16, c.pos); // item 39: a unit down is announced
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
        struckDriverU[slot] = rep.struckDriver;
        // the body's own drive-state words, stashed for the per-second
        // unit line: THE ROOT STEP makes the pedal read-backs the
        // line's centre - repSteer/repGas/repBrake are the fields as
        // the BODY answered them, printed beside the commanded words,
        // so a session convicts the channel itself from numbers. The
        // mission/cruise pair stays beside them as the middleman's
        // death certificate: under direct drive a moving unit must
        // read mission NONE and cruise 0 - the moment a live session's
        // line shows a mission on a driving cop, something else has
        // claimed the body and the step's premise is broken on screen
        c.repSteer = rep.steer;
        c.repGas = rep.gas;
        c.repBrake = rep.brake;
        // the channel repair's third reading rides the same stash:
        // what the ProcessAI-door last stamped on this body and how
        // many frames it has stamped - the direct act grades its
        // advance, the unit line prints it beside cmd and body
        c.repHookSteer = rep.hookSteer;
        c.repHookGas = rep.hookGas;
        c.repHookBrake = rep.hookBrake;
        c.repHookStamps = rep.hookStamps;
        // the census mirror (the missing-link step): the whole block,
        // stashed with the rest of the read-backs - the unit line
        // prints it beside cmd/body/hook, so the session's diff
        // against a host-driven decoy is read off matching segments
        c.repCensus = rep.census;
        c.repMission = rep.mission;
        c.repCruise = rep.cruise;
        c.repCruiseF = rep.cruiseF;
        c.repSiren = rep.siren;
        c.repDriverOut = rep.driverOut; // item 38: the bail book's
        c.repDriverPos = rep.driverPos; // read-back, beside the rest
        // the victim of a hit knows - the original's felony note for
        // being struck never asks for his eyes; the unit's own sight
        // feeds the page's seen flag, whoever of them saw the man.
        // FIX ROUND 5: the DRIVER channel joins the ram and shot
        // channels here. A hit on the seated driver is the loudest
        // "the man is hurting us" the body reports, and it is the
        // player's work by the containment's own construction (the
        // driver is invisible, blast- and fire-proofed; the only
        // unproofed inbound damage he can take is the player's own
        // fire and melee) - yet before this round it priced the book
        // in silence: a man could shoot the driver out of the car at
        // point blank and the page read "position lost" over his
        // corpse-to-be. Position-lost is now impossible while he is
        // actively damaging the unit through ANY of its channels.
        // THE 5x UPDATE: the momentum channel pays out. Contact is the
        // geometric reading (the centres' distance inside the band -
        // KickCompute prices it off the closing speed); the throw goes
        // to whatever the man is in - his vehicle, ANY vehicle, a
        // motorcycle included: the channel has no per-class softening
        // anywhere, a bike takes the full priced launch (the user's
        // zero-mercy amendment) - or to the man himself on foot. Only
        // a pursuing unit throws: a parked unit the man crashes into
        // has no momentum order to give, and its book already prices
        // his arrival.
        bool struckHim = false;
        {
            const float band = gThreat.playerInVehicle ? kKickBandCar : kKickBandFoot;
            Vec3 relKick = gThreat.playerPos - c.pos;
            relKick.z = 0.0f;
            const float kickDist = relKick.Length();
            if (kickDist > band + 3.0f)
                c.kickArmed = true; // the centres separated: a fresh
                                    // approach may throw again
            if (c.mode == COP_PURSUE && c.kickArmed && kickDist <= band) {
                float spin = 0.0f, closing = 0.0f;
                Vec3 dv = KickCompute(c.vel, gThreat.playerVel, relKick, &spin, &closing);
                if (dv.Length() > 0.01f) {
                    // the 39-package's throw multipliers, all priced
                    // on the same contact the formula just read:
                    // item 6 (the PIT): a contact in the prey's rear
                    // half - this unit behind his centre plane - is a
                    // hit meant to turn him, so its spin is priced
                    // half again (the linear throw is untouched: the
                    // square-on pin in the suite grades exactly it);
                    // item 4: the heavy unit throws a quarter again;
                    // item 8: a dying unit's last ram throws half
                    // again, and spending it closes his window.
                    if (gThreat.playerInVehicle) {
                        Vec3 pf = gThreat.playerVel;
                        pf.z = 0.0f;
                        if (pf.Length() > 2.0f) {
                            pf = pf * (1.0f / pf.Length());
                            Vec3 behind = c.pos - gThreat.playerPos;
                            behind.z = 0.0f;
                            if (behind.Dot(pf) < -0.5f)
                                spin *= 1.6f;
                        }
                    }
                    float tScale = 1.0f;
                    if (c.heavyUnit)
                        tScale *= 1.25f;
                    if (sLandedT > 0.0f) {
                        tScale *= 1.5f; // item 35: the landing hit -
                            // inside the touchdown window he has no
                            // grip to answer a throw with
                        Log("cop: the landing hit - the throw is priced half again while he has no grip");
                    }
                    if (c.lastRamT > 0.0f) {
                        tScale *= 1.5f;
                        c.lastRamT = 0.0f;
                        c.lastRamDone = true;
                        Log("cop: unit %d spent his last ram on the contact", slot);
                    }
                    if (tScale != 1.0f) {
                        dv = dv * tScale;
                        if (dv.Length() > 36.0f)
                            dv = dv * (36.0f / dv.Length()); // the
                                // sanity cap, raised a fifth for the
                                // multipliers and no further
                        spin = Clamp(spin * tScale, -kKickSpinCap * 1.5f, kKickSpinCap * 1.5f);
                    }
                    c.kickArmed = false;
                    ++c.kickCount;
                    struckHim = true;
                    if (gThreat.playerInVehicle && gThreat.playerVehicle >= 0)
                        TheHost().KickVehicle(gThreat.playerVehicle, dv, spin);
                    else if (!gThreat.playerInVehicle)
                        TheHost().KickPed(PreyOverrideOn() && sPreyPed >= 0 ? sPreyPed : 0, dv);
                    // item 10 (the honest half): his car is taken
                    // apart in priced stages - every full-blooded
                    // kick (closing 10+) buys the next TYRE, one per
                    // six seconds, four in all, burst through the
                    // host's own shot-effect verb at the corner the
                    // stage names (front pair first, then the rear).
                    // A different car starts a fresh count. Steering
                    // and power sabotage have NO verb on this host
                    // and are not faked - the report names that wall
                    // inside the item; and the player's car is never
                    // HurtVehicle'd, which would bill his own felony
                    // ledger for our work.
                    if (gThreat.playerInVehicle && gThreat.playerVehicle >= 0) {
                        if (gThreat.playerVehicle != sTyreVeh) {
                            sTyreVeh = gThreat.playerVehicle;
                            sTyreStage = 0;
                            sTyreT = 0.0f;
                        }
                        if (closing >= 10.0f && sTyreStage < 4 && sTyreT <= 0.0f) {
                            Vec3 pf = gThreat.playerVel;
                            pf.z = 0.0f;
                            pf = pf.Length() > 2.0f ? pf * (1.0f / pf.Length()) : Vec3(0, 1, 0);
                            const Vec3 rt(pf.y, -pf.x, 0.0f);
                            const Vec3 corner = gThreat.playerPos +
                                                pf * (sTyreStage < 2 ? 1.6f : -1.6f) +
                                                rt * ((sTyreStage % 2 == 0) ? 0.8f : -0.8f);
                            TheHost().VehicleShotEffect(gThreat.playerVehicle, corner, 1);
                            ++sTyreStage;
                            sTyreT = 6.0f;
                            Log("cop: the hit costs him a tyre (%d of 4) - the disable is priced off the kicks themselves",
                                sTyreStage);
                        }
                    }
                    // immediate re-attack (the pressure law): the
                    // throw IS the contact - the post-impact stillness
                    // is not stuck evidence, a recovery the impact
                    // provoked does not stand, and the next tick's
                    // ordinary feed re-engages him at batter pace with
                    // no re-path pause. The loop after a pass or a hit
                    // is this plus the zone law, for as long as the
                    // felony holds and he is in range.
                    c.stuckT = 0.0f;
                    c.fairStartT = kCopFairStart;
                    c.recoveryT = 0.0f;
                    Log("cop: unit %d threw the %s (kick #%d) - dv %.1f m/s, spin %.2f rad/s, closing %.1f m/s",
                        slot, gThreat.playerInVehicle ? "vehicle he is in" : "man himself",
                        c.kickCount, dv.Length(), spin, closing);
                }
            }
        }
        // the sight word for this unit, struckHim included: the
        // victim of a hit knows (round 5, the struck channels below),
        // and the STRIKER knows by the same epistemics - a unit whose
        // body just threw the man has physically touched him, and at
        // the 5x pace rows the overshoot that follows a contact runs
        // outside the original's sight gates for seconds at a time.
        // Without the striker's word the page read "position lost"
        // over a force that had rammed him four times in the act (the
        // first 5x sim run's log convicted it in blocks (b) and (l)).
        // Contact is knowledge, in both directions.
        bool sees = CopSees(c, page.felony > kFelonyTrigger) ||
                    rep.struckRam > 0.5f || rep.struckShot > 0.5f ||
                    rep.struckDriver > 0.5f || struckHim;
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
        sLastSeenPos = preyPos; // items 3+20's snapshot, taken while
        sLastSeenVel = gThreat.playerVel; // sight is a fact, not at
                                // the loss, when it is already gone
    } else if (page.known) {
        page.unseenT += dt;
        if (page.unseenT > kLostAfter) {
            page.known = false;
            Log("cop: position lost (256 unseen ticks)");
            // items 3+20: a FLIGHT is written down - felony over the
            // trigger and his speed at last sight over 8 m/s. The
            // junction is his last known place, the exit the point
            // his velocity was making for; the farthest pursuer (the
            // near ones keep searching the spot) ambushes the exit
            // for six seconds. A man who merely steps out of sight
            // teaches the force nothing.
            Vec3 v = sLastSeenVel;
            v.z = 0.0f;
            if (page.felony > kFelonyTrigger && v.Length() > 8.0f) {
                EscapeRec& r = sEscape[sEscapeNext];
                r.junction = sLastSeenPos;
                r.exit = sLastSeenPos + v * (45.0f / v.Length());
                r.coolT = 0.0f;
                sEscapeNext = (sEscapeNext + 1) % 6;
                if (sEscapeCount < 6)
                    ++sEscapeCount;
                if (sAmbushSlot >= 0 && gCops[sAmbushSlot].holding) {
                    gCops[sAmbushSlot].holding = false;
                    gCops[sAmbushSlot].holdArrived = false;
                }
                sAmbushSlot = -1;
                int best = -1;
                float bestD = 50.0f;
                for (int s = 0; s < kMaxCopUnits; ++s) {
                    CopState& u = gCops[s];
                    if (!UnitLiving(u) || u.mode != COP_PURSUE || u.holding)
                        continue;
                    const float d = (u.pos - sLastSeenPos).Length();
                    // farthest WITHIN the post's own lifetime: the
                    // post stands 6 s, so a unit past ~150 m is not
                    // ambushing an exit, he is commuting to it - and
                    // the unbound farthest once picked a woken block
                    // car half a map away, which the ladder then stood
                    // down in the same tick and the ambush died unborn
                    if (d > bestD && d <= 150.0f) {
                        bestD = d;
                        best = s;
                    }
                }
                if (best >= 0) {
                    CopState& u = gCops[best];
                    u.holding = true;
                    u.holdArrived = false;
                    u.holdPoint = r.exit;
                    sAmbushSlot = best;
                    sAmbushT = 6.0f;
                    Log("cop: the escape is written down (junction %.0f,%.0f) - a far unit ambushes the exit he was making for",
                        r.junction.x, r.junction.y);
                }
            }
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
    // (under a prey override this block stays silent: the impact ledger
    // it reads is the real player's own car, and the speeding note would
    // price the decoy's cruise - /coptest sets the felony directly and
    // grades its own scenarios; the struck notes below stay live, since
    // a unit struck BY THE PREY is the prey's act whoever the prey is)
    if (!PreyOverrideOn() && gThreat.playerInVehicle && anySeen) {
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
        if (struckDriverU[slot] > 0.5f) {
            // hitting the DRIVER is hitting the cop (fix round 5): the
            // same offence as the shot at his car, at the same named
            // price - before this round the channel billed the book
            // and told the felony nothing, so the on-foot man's most
            // direct offence was the one the system could not hear
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

    // ---- the 39-package's per-tick trackers (the tactics tick below
    // spends what these measure)
    {
        const float preySpeed = gThreat.playerVel.Length();
        bool anyPursuer = false;
        float nearPursuer = 1e9f;
        for (int slot = 0; slot < kMaxCopUnits; ++slot) {
            const CopState& u = gCops[slot];
            if (UnitLiving(u) && u.mode == COP_PURSUE) {
                anyPursuer = true;
                const float d = (u.pos - preyPos).Length();
                if (d < nearPursuer)
                    nearPursuer = d;
            }
        }
        // item 12: the prey's yaw rate, off his own velocity - a man
        // swinging his car around is a man whose car is unbalanced,
        // and that is when the strike lands
        if (preySpeed > 5.0f && dt > 0.0f) {
            const float head = std::atan2(gThreat.playerVel.x, gThreat.playerVel.y);
            if (sHeadOk) {
                float dh = head - sHeadPrev;
                while (dh > 3.14159265f)
                    dh -= 6.2831853f;
                while (dh < -3.14159265f)
                    dh += 6.2831853f;
                sTurnRate = sTurnRate * 0.7f + (dh / dt) * 0.3f;
            }
            sHeadPrev = head;
            sHeadOk = true;
        } else {
            sTurnRate *= 0.9f;
        }
        // the strike line is a SWING - a hard correction, a spin
        // recovery, the start of a U-turn: at 1.0 rad/s the car is
        // genuinely unbalanced. Ordinary road curvature at chase
        // speed reads 0.2-0.4 rad/s and must NOT trip this: the first
        // staging of this law at 0.45 killed the pacing blend on
        // every bend of the suite's road and the anchor rammed its
        // own book dry mid-chase - the line is the conviction that
        // came back from that run
        sTurnStrike = std::fabs(sTurnRate) > 1.0f && anyPursuer && nearPursuer < 30.0f;
        // item 31: the crash witness - his speed against the sample
        // from half a second ago. A drop past 7 m/s is a crash (a
        // wall, a spin, a wreck he is still gathering up), and the
        // window that opens is when the force strikes hardest
        sSpdAge += dt;
        if (sSpdAge >= 0.5f) {
            sSpdOld = preySpeed;
            sSpdAge = 0.0f;
        }
        if (sCrashT > 0.0f)
            sCrashT -= dt;
        // the window opens on a drop AND a crawl: a crash leaves the
        // man stopped or gathering himself up at walking pace, while
        // plain braking to a cruise is not a crash and buys him no
        // such moment (the suite's own chase steps 35 -> 20 m/s at
        // its start; the first staging of this witness, drop alone,
        // read that step as a crash and spent the anchor's book on
        // full-pace rams - the crawl condition is that run's lesson)
        if (anyPursuer && page.felony > kFelonyTrigger && sSpdOld - preySpeed > 7.0f &&
            preySpeed < 8.0f && sCrashT <= 0.0f) {
            sCrashT = 3.0f;
            for (int slot = 0; slot < kMaxCopUnits; ++slot) {
                CopState& u = gCops[slot];
                if (UnitLiving(u) && (u.pos - preyPos).Length() < 45.0f) {
                    u.stuckT = 0.0f; // his gathering-up is not their
                    u.fairStartT = 0.0f; // standing still - no witness
                }                        // may call this moment stuck
            }
            Log("cop: the prey crashed (%.0f -> %.0f m/s) - the strike window opens",
                sSpdOld, preySpeed);
        }
        // item 21's heat clock and item 26's measurement share the
        // pursuit's life: both run only while a pursuit is actually
        // on, the heat only at the felony cap, and both die with it
        if (anyPursuer) {
            if (preySpeed > sPreyTopSeen)
                sPreyTopSeen = preySpeed;
            if (page.felony >= kFelonyCap - 1.0f) {
                sPursuitT += dt;
                sHeat = (int)(sPursuitT / 60.0f);
                if (sHeat > 5)
                    sHeat = 5;
            }
        } else {
            sPursuitT = 0.0f;
            sHeat = 0;
            sPreyTopSeen = 0.0f;
        }
        if (sSyncT > 0.0f)
            sSyncT -= dt;
        if (sHandoverT > 0.0f) {
            sHandoverT -= dt;
            if (sHandoverT <= 0.0f)
                sHandover = false;
        }
        // layer C: the ambush's watch runs out the same way, and the
        // written escapes' reuse cooldowns tick down with it
        if (sAmbushT > 0.0f) {
            sAmbushT -= dt;
            if (sAmbushT <= 0.0f && sAmbushSlot >= 0) {
                CopState& u = gCops[sAmbushSlot];
                if (u.holding) {
                    u.holding = false;
                    u.holdArrived = false;
                    Log("cop: the ambush watch is over - its unit hunts again");
                }
                sAmbushSlot = -1;
            }
        }
        for (int i = 0; i < sEscapeCount; ++i)
            if (sEscape[i].coolT > 0.0f)
                sEscape[i].coolT -= dt;
        if (sTyreT > 0.0f)
            sTyreT -= dt; // item 10: the disable's stage spacing
    }

    // item 35: the ramp jump. The prey's own velocity feed carries
    // the whole story - a launch (z-velocity up past 4), the fall
    // (z-velocity under -1), and the touchdown (the fall's snap back
    // to ~0). While he is in the air the force does not chase the car
    // it sees; it drives at the LANDING the arc is making for,
    // recomputed every tick against the launch height, and a kick
    // that lands inside the touchdown window is priced half again:
    // a car in its first grip-less instants cannot answer a throw.
    // (Whether a pursuer's own body catches the same ramp is the
    // host's physics; the brain's part - be at the landing, priced -
    // is this tracker and the two wirings that read it.)
    if (gThreat.playerInVehicle) {
        const Vec3& pv = gThreat.playerVel;
        if (!sAirborne && pv.z > 4.0f) {
            sAirborne = true;
            sAirFell = false;
            sAirLaunchZ = preyPos.z;
            Log("cop: he is airborne - the force drives at his landing, not at the car in the air");
        }
        if (sAirborne) {
            if (pv.z < -1.0f)
                sAirFell = true;
            if (sAirFell && pv.z > -1.0f) {
                sAirborne = false;
                sAirFell = false;
                sLandValid = false;
                sLandedT = 0.6f;
                Log("cop: touchdown - the landing window is open");
            } else {
                const float dz = preyPos.z - sAirLaunchZ;
                const float disc = pv.z * pv.z + 19.6f * dz;
                const float tf = (pv.z + std::sqrt(disc > 0.0f ? disc : 0.0f)) / 9.8f;
                if (tf > 0.05f) {
                    sLandPoint = preyPos + Vec3(pv.x, pv.y, 0.0f) * tf;
                    sLandPoint.z = preyPos.z;
                    sLandValid = true;
                } else {
                    sLandValid = false;
                }
            }
        }
    } else {
        sAirborne = false;
        sAirFell = false;
        sLandValid = false;
    }
    if (sLandedT > 0.0f)
        sLandedT -= dt;

    // ---- the manager: pursuit transitions, the ladder, the cutoff
    const bool wantPursue = page.felony > kFelonyTrigger && page.known;
    int livingLadder = 0;
    for (int slot = 0; slot < kMaxCopCars; ++slot)
        if (UnitLiving(gCops[slot]))
            ++livingLadder;
    sWanted = LadderWanted(page.felony);

    // item 39's announced moments, read off the manager's own state:
    // the pursuit's opening call, a rung climbing while it runs, the
    // prey bailing out of his car mid-pursuit, and the night witness
    // (a pursuer inside 18 m means the force has been heard)
    if (wantPursue && !sRadioOpen) {
        sRadioOpen = true;
        sRadioRung = sWanted;
        RadioLine(1, preyPos);
    } else if (!wantPursue) {
        sRadioOpen = false;
        sRadioRung = 0;
        sForceHeard = false;
    }
    if (sRadioOpen && sWanted > sRadioRung) {
        sRadioRung = sWanted;
        RadioLine(2, preyPos);
    }
    if (sPrevInVeh && !gThreat.playerInVehicle && page.felony > kFelonyTrigger)
        RadioLine(4, preyPos);
    sPrevInVeh = gThreat.playerInVehicle;
    if (wantPursue && !sForceHeard) {
        for (int slot = 0; slot < kMaxCopCars; ++slot) {
            const CopState& u = gCops[slot];
            if (UnitLiving(u) && u.mode == COP_PURSUE &&
                (u.pos - preyPos).Length() < 18.0f) {
                sForceHeard = true;
                break;
            }
        }
    }

    // a summon's spike runs while the case is cold (piece 0's proof is
    // not the ladder's to cancel) - but piece 1's design always said the
    // SAME unit leaves the spike and pursues the moment the felony
    // crosses with the position known. THE COMPLETE UPDATE's fourth
    // conviction: the loop below used to skip COP_SPIKE unconditionally,
    // so the spike NEVER ended early - in the 02:00 session the anchor
    // sat in mode 1 (COP_SPIKE) at felony 4096 for the whole sampled
    // window while the ladder hunted without its lead car. Restored:
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (!UnitLiving(c))
            continue;
        c.batter = page.felony >= kBatterFelony;
        if (c.mode == COP_SPIKE && wantPursue && page.steerCalDone) {
            // (the calibration second always runs first: it is the
            // session's only source for the steering sign every unit
            // reads, and an informed summon into the boss's fight does
            // not get to skip it - he rolls out facing the man, the
            // sign settles, and THEN he pursues)
            c.spikeDone = true; // the spike's proof is superseded by the
                                // real thing: the anchor is a unit now
            c.mode = COP_PURSUE;
            c.fairStartT = kCopFairStart;
            Log("cop: the spike is over - the anchor joins the pursuit himself (felony %.0f)", page.felony);
            continue;
        }
        if (c.mode == COP_SPIKE || c.mode == COP_LEAVE)
            continue;
        if (c.mode == COP_BLOCK) {
            // PassiveCopTasks' conversion, on our wake terms: felony hot,
            // position known, and the player closing on the block - or
            // the block car itself struck, which wakes it on the spot
            const float d = (c.pos - preyPos).Length();
            if ((wantPursue && d < kBlockWakeDist) || struckRamU[slot] > 0.5f ||
                struckShotU[slot] > 0.5f) {
                c.mode = COP_PURSUE;
                c.holding = false; // a woken block is a pursuit, not a post
                c.holdArrived = false;
                c.fairStartT = kCopFairStart; // a waking unit is a fresh
                    // drive order too (fix round 5)
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
            if (c.mode != COP_PURSUE) {
                Log("cop: %s - felony %.0f over the trigger, position known", slot == 0 ? "the pursuit opens" : "a unit joins the pursuit", page.felony);
                c.fairStartT = kCopFairStart; // the pursuit entry is a
                    // fresh drive order: its start-up is not stuck
                    // evidence (fix round 5)
            }
            c.mode = COP_PURSUE;
        } else if (!wantPursue && c.mode == COP_PURSUE) {
            // a block-woken unit with the case gone leaves like the rest;
            // a plain unit stands down where it stands
            if (c.blockUnit) {
                c.mode = COP_LEAVE;
                GiveUpPost(c);
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
            GiveUpPost(c);
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
                    const float d = (c.pos - preyPos).Length();
                    if (d > farDist) {
                        farDist = d;
                        farSlot = slot;
                    }
                }
                if (farSlot < 0)
                    break;
                CopState& c = gCops[farSlot];
                c.mode = COP_LEAVE;
                GiveUpPost(c);
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
    // THE 5x UPDATE removes the discount at the felony cap: at max
    // felony there is no line at all - far units fight at full fed
    // speed, so the pressure closes on him from several streets at
    // once instead of queueing politely behind the near units (the
    // user's presence order: he faces a force arriving, never one
    // wandering car at a time).
    {
        float pDist[kMaxCopUnits];
        int nP = 0;
        for (int slot = 0; slot < kMaxCopUnits; ++slot)
            if (UnitLiving(gCops[slot]) && gCops[slot].mode == COP_PURSUE)
                pDist[nP++] = (gCops[slot].pos - preyPos).Length();
        for (int i = 1; i < nP; ++i) {
            const float d = pDist[i];
            int j = i - 1;
            while (j >= 0 && pDist[j] > d) {
                pDist[j + 1] = pDist[j];
                --j;
            }
            pDist[j + 1] = d;
        }
        if (nP > sWanted && sWanted >= 1 && page.felony < kFelonyCap)
            sCutoff = pDist[sWanted - 1];
        else
            sCutoff = 1e9f;
        for (int slot = 0; slot < kMaxCopUnits; ++slot) {
            CopState& c = gCops[slot];
            if (UnitLiving(c) && c.mode == COP_PURSUE)
                c.powerScale = (c.pos - preyPos).Length() > sCutoff ? kCutoffScale : 1.0f;
            else
                c.powerScale = 1.0f;
        }
    }

    // the tactics tick (the 39-package): roles, the wall reading and
    // the synchronised-hit window, refreshed on a 1 s cadence - the
    // assignments are strategy, not steering, and the cadence keeps
    // their probes off the per-tick bill
    sTacticsT -= dt;
    if (sTacticsT <= 0.0f) {
        sTacticsT = 1.0f;
        // item 18's witness, on the same cadence: he is off-road
        // when no road node stands within 25 m of him
        sPreyOffRoad = VehicleNearestNode(preyPos, 25.0f) < 0;
        TacticsTick(preyPos);
    }
    // the posts tick (layer B): the force's places - door camps, the
    // rear-exit guard, the dead-end mouth - on the same 1 s cadence
    sPostsT -= dt;
    if (sPostsT <= 0.0f) {
        sPostsT = 1.0f;
        PostsTick(preyPos);
    }

    // the spawn clock (ControlNumberOfCops): while the ladder is short,
    // the delay shrinks with the felony - 64 ticks at felony 0, half
    // that at the cap, through the named stand-in scale (the 5x
    // update's divided base: the whole ladder fills inside ~2-3 s of
    // a felony jump, which is the arrival the presence order wants).
    // When the clock runs out and a ladder slot is free, the unit
    // enters at range. THE 39-PACKAGE: item 21's heat divides the
    // delay again (a long max-felony chase refills faster and
    // faster), and item 16's handover skips the clock entirely - a
    // fallen pursuer is replaced on the next tick, entering near
    // where he fell, so the pressure never actually lets go.
    if (livingLadder < sWanted) {
        sRespawnT += dt;
        const float delayTicks = kRespawnTicks * (1.0f - page.felony * kRespawnScale / kFelonyCap) /
                                 (1.0f + (float)sHeat);
        if (sHandover || sRespawnT >= delayTicks * kTick) {
            sRespawnT = 0.0f;
            for (int slot = 0; slot < kMaxCopCars; ++slot) {
                if (UnitLiving(gCops[slot]))
                    continue;
                Vec3 at;
                if (LadderSpawnPoint(preyPos, &at, sHandover ? &sHandoverPos : nullptr)) {
                    if (SpawnUnit(slot, at, preyPos, false)) {
                        // item 4 (the built half): the ladder's LAST
                        // car, joining at the top rungs, is the heavy
                        // one - a book half again as deep (202500)
                        // and kicks carrying layer A's heavy
                        // multiplier. The wall half is a visibly
                        // bigger body: the Chicago mesh is baked
                        // once, at its own size, and the draw has no
                        // scale path - named in the report, not faked
                        if (livingLadder == sWanted - 1 && page.felony >= 2457.0f) {
                            gCops[slot].heavyUnit = true;
                            gCops[slot].wreckLine = 202500.0f;
                            Log("cop: the ladder's last car is the heavy one - his book runs half again as deep");
                        }
                        if (sRadioOpen && livingLadder >= 1)
                            RadioLine(32, at); // item 39: a join into
                                // a live pursuit is announced
                        Log("cop: the ladder sends another unit - he enters at %.0f m on the road (felony %.0f, wanted %d)",
                            (at - preyPos).Length(), page.felony, sWanted);
                        ++livingLadder;
                        if (sHandover) {
                            sHandover = false;
                            Log("cop: the handover is spent - the replacement entered near the fall");
                        }
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

    // item 36: the motorcycle cop - the surrounding force, not the
    // ladder (the contract files it there, and the ladder's last
    // slot already belongs to item 4's heavy): at the top rungs,
    // with the ladder full, one bike is fielded in a free
    // surrounding slot and hunts as a unit in every law but its
    // far-mode feed. When the case closes it rides off like any
    // surplus unit. A 1 s cadence fields and retires it - nothing
    // here runs per-frame that the ladder does not already run.
    {
        bool bikeLives = false;
        for (int slot = 0; slot < kMaxCopUnits; ++slot)
            if (gCops[slot].bikeUnit && UnitLiving(gCops[slot]))
                bikeLives = true;
        if (bikeLives && page.felony <= kFelonyTrigger) {
            for (int slot = 0; slot < kMaxCopUnits; ++slot) {
                CopState& c = gCops[slot];
                if (c.bikeUnit && UnitLiving(c) && c.mode != COP_LEAVE) {
                    c.mode = COP_LEAVE;
                    c.leaveRoute.clear();
                    c.leaveIdx = 0;
                    GiveUpPost(c);
                    Log("cop: the case is over - the motorcycle cop rides off");
                }
            }
        } else if (!bikeLives && wantPursue && page.felony >= 2457.0f &&
                   livingLadder >= sWanted && sWanted >= 2) {
            sBikeT += dt;
            if (sBikeT >= 1.0f) {
                sBikeT = 0.0f;
                const int slot = kMaxCopUnits - 1; // the bike's own
                    // berth, behind the roadblock's four - nothing
                    // else in the force ever takes it
                if (!UnitLiving(gCops[slot])) {
                    Vec3 at;
                    if (LadderSpawnPoint(preyPos, &at) &&
                        SpawnUnit(slot, at, preyPos, false, true)) {
                        gCops[slot].mode = COP_PURSUE;
                        Log("cop: a motorcycle cop joins the hunt");
                    }
                }
            }
        } else {
            sBikeT = 0.0f;
        }
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
            if (pursuing && page.felony >= kBlockFireFelony) {
                if (!TryAirportBlock(preyPos)) // item 22 reads the
                    PlaceRoadblock(preyPos);   // airport run first
            }
            sBlockTimer = BlockDelay();
        }
    }

    // item 2, the living block: while a block stands asleep and the
    // man is still far from it, his crossing is recomputed on a 2 s
    // cadence - if his braking point has moved on by a real margin,
    // the cars creep to their slots around the new point at a street
    // pace. A woken block is a pursuit, not a post, and the creep
    // never runs under his nose (55 m) where the wake owns them
    if (sBlockLive && CopBlockCount() > 0) {
        sCreepT -= dt;
        if (sCreepT <= 0.0f) {
            sCreepT = 2.0f;
            bool allAsleep = true;
            for (int slot = kMaxCopCars; slot < kMaxCopCars + kMaxBlockCars; ++slot)
                if (UnitLiving(gCops[slot]) && gCops[slot].mode != COP_BLOCK)
                    allAsleep = false;
            if (allAsleep && (preyPos - sBlockPoint).Length() > 55.0f) {
                Vec3 at, segDir;
                if (BlockRoutePoint(preyPos, BlockAheadFor(gThreat.playerVel.Length()),
                                    &at, &segDir, false) &&
                    (at - sBlockPoint).Length() > 12.0f) {
                    for (int slot = kMaxCopCars; slot < kMaxCopCars + kMaxBlockCars; ++slot) {
                        CopState& c = gCops[slot];
                        if (!UnitLiving(c) || c.mode != COP_BLOCK)
                            continue;
                        Vec3 carAt, face;
                        BlockSlotPos(at, segDir, slot - kMaxCopCars, &carAt, &face);
                        c.holdPoint = carAt;
                        c.holding = true;
                        c.holdArrived = false;
                    }
                    sBlockPoint = at;
                    sBlockDir = segDir;
                    Log("cop: the block creeps - his crossing moved on, the cars roll to the new line");
                }
            }
        }
    } else {
        sCreepT = 0.0f;
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
        if (page.fieldT <= 0.0f || (page.fieldGoal - preyPos).Length() > 25.0f) {
            page.fieldT = 0.75f;
            page.fieldGoal = preyPos;
            page.fieldOk = VehicleFieldTo(preyPos);
        }
    }

    // item 38: at the top rungs the drivers get out and fight. The
    // order goes to a pursuing car whose prey is on foot, pinned at
    // arm's length and nearly stopped; while the driver is out his
    // car stands where it stopped, and the unit's book keeps billing
    // through the driver channel wherever he stands (the bridge
    // measures him there - the one-unit law does not care where the
    // driver is). The return is the prey mounting anything, the
    // range opening past 60 m, the felony falling under the
    // trigger, or the unit being stood down. The fight itself is
    // the bridge's ped work; the core owns the order, the book and
    // the return.
    for (int slot = 0; slot < kMaxCopUnits; ++slot) {
        CopState& c = gCops[slot];
        if (!UnitLiving(c) || c.blockUnit || c.bikeUnit)
            continue;
        if (c.bailState == 0) {
            if (c.mode == COP_PURSUE && page.felony >= 2457.0f &&
                !gThreat.playerInVehicle &&
                (c.pos - preyPos).Length() < 14.0f && c.speed < 2.0f) {
                TheHost().CopBodyBail(c.veh, true);
                c.bailState = 1;
                c.bailT = 0.0f;
                Log("cop: unit %d's driver is out - he fights on foot now", slot);
            }
        } else {
            c.bailT += dt;
            if (c.mode != COP_PURSUE || gThreat.playerInVehicle ||
                (c.pos - preyPos).Length() > 60.0f ||
                page.felony <= kFelonyTrigger) {
                TheHost().CopBodyBail(c.veh, false);
                c.bailState = 0;
                Log("cop: unit %d's driver is back in his car", slot);
            }
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
            if (c.bailState == 1)
                StandDrive(c); // item 38: the driver is out - the
                               // car stands where it stopped, a
                               // blocker with its door open
            else if (c.holding)
                HoldControl(c, dt);
            else
                CopControl(c, dt);
            break;
        case COP_LEAVE:
            LeaveDrive(c, dt, preyPos);
            break;
        case COP_IDLE:
            if (c.holding)
                HoldControl(c, dt);
            else
                StandDrive(c);
            break;
        case COP_BLOCK:
            if (c.holding)
                HoldControl(c, dt); // item 2: the living block creeps
            else
                StandDrive(c);
            break;
        default:
            break;
        }
        // the per-second unit line: EVERY living unit, the brain's
        // intent beside the body's own read-backs, so a session
        // separates decision from delivery by numbers alone - and the
        // felony story (the page's level, the position known, the
        // force seeing) rides the same line, with the siren flag read
        // off the body itself. THE ROOT STEP's pair: `cmd` is the word
        // the core SENT (steer/gas/brake as Drive() last wrote them),
        // `body` is the word the car ANSWERED with on its last report
        // - commanded vs landed, every second, beside the mission the
        // body reports (which must read 0/NONE under direct drive).
        // THE CHANNEL REPAIR adds the third reading between them:
        // `hook` is what the bridge's ProcessAI replacement stamped
        // at the desk physics reads (and n, its stamp count) - the
        // 14:44 session proved cmd and body alone can tell opposite
        // stories about the same fields at different frame points;
        // the hook segment is the reading at the point that decides
        // motion, and a session convicts the channel by comparing
        // all three.
        // The aim distance is the brain's own target again (the feed
        // point it replaced in round 5 no longer exists).
        c.unitLogT -= dt;
        if (c.unitLogT <= 0.0f) {
            c.unitLogT = 1.0f;
            // THE MISSING-LINK STEP adds the fourth reading: the
            // state census, appended whole - the fields this line's
            // three pedal stories cannot show (gear, wheel contact,
            // the autopilot's internals), printed for our body in
            // exactly the shape the /coptest pursuit act prints for
            // a host-driven decoy, so the diff is mechanical
            char cen[384];
            FormatCensus(c.repCensus, cen, sizeof cen);
            Log("cop unit %d: mode %d speed %.1f want %.1f | cmd steer %.2f gas %.2f brake %.2f"
                " | body steer %.2f gas %.2f brake %.2f | hook steer %.2f gas %.2f brake %.2f n %d"
                " | mission %d cruise %d/%.0f siren %d"
                " | aim %.0f m | felony %.0f known %d seen %d kicks %d | %s",
                slot, (int)c.mode, c.speed, c.desiredSpeed,
                c.cmdSteer, c.gas, c.brake,
                c.repSteer, c.repGas, c.repBrake,
                c.repHookSteer, c.repHookGas, c.repHookBrake, c.repHookStamps,
                c.repMission, c.repCruise, c.repCruiseF, c.repSiren ? 1 : 0,
                (c.target - c.pos).Length(),
                page.felony, page.known ? 1 : 0, page.seen ? 1 : 0, c.kickCount, cen);
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
        const float d = (c.pos - preyPos).Length();
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

    // the self-test grades the tick just driven (it set the prey and
    // the felony before it, and reads the outcome after it)
    CopTestTick(dt);
}

// ---- the state census formatter (the missing-link step): one
// segment shape for every census printing (unit line, spike line,
// the /coptest pursuit act's anchor/decoy pair), so two lines diff
// field by field without re-parsing. The field order is the Host.h
// census block's order: gear state, the raw/second steer words, the
// fake-physics and autopilot internals, the rev counter; then the
// wheel-contact triple, the per-wheel states, suspension, wheel
// speeds, the brake integrator and the burnout/brakes word.
void FormatCensus(const Host::CopBody::Census& c, char* out, size_t outSize) {
    std::snprintf(out, outSize,
                  "cen gear %d gc %.2f raw %.2f st2 %.2f fake %d tmp %d flg %d sw %u rev %.2f"
                  " | gnd %d/%d/%d ws %d/%d/%d/%d susp %.2f/%.2f/%.2f/%.2f"
                  " wsp %.1f/%.1f/%.1f/%.1f bc %.2f burn %d",
                  c.gear, c.gearChange, c.rawSteer, c.steer2,
                  c.fakePhysics, c.tempAction, c.ctrlFlags, c.switched, c.revs,
                  c.contact, c.onGround, c.driveGround,
                  c.wheelState[0], c.wheelState[1], c.wheelState[2], c.wheelState[3],
                  c.susp[0], c.susp[1], c.susp[2], c.susp[3],
                  c.wheelSpeed[0], c.wheelSpeed[1], c.wheelSpeed[2], c.wheelSpeed[3],
                  c.brakeCount, c.burnout);
}

// ---- piece 3: the decoded profile's accessors -------------------------
int CopRadioLinesFired() { return sRadioLines; }
int CopRadioEventMask() { return sRadioMask; }

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
