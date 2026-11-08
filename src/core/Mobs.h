#pragma once
// Minecraft's farm animals (cow, pig, sheep, chicken): how they wander, flee, follow food, breed, lay eggs, get
// sheared, milked, saddled, ridden and hurt; and the creeper, that comes out at night and blows up next to the player. The host draws them (it reads gMobs) and says where its ground lets
// a herd appear (Host::SpawnGround).

#include "Core.h"
#include "PoModel.h"     // PoDrawState rides in Po's Mob
#include "SoldierModel.h" // SoldierDrawState rides in the soldier's Mob

namespace mc {

struct BeingInfo; // Host.h: one of the host's own people near a point

enum MobKind { MOB_COW = 0, MOB_PIG, MOB_SHEEP, MOB_CHICKEN, MOB_CREEPER, MOB_WARDEN, MOB_ZOMBIE, MOB_SKELETON, MOB_SPIDER, MOB_SOLDIER, MOB_PO, MOB_KIND_COUNT };

// The two sides of the Counter-Strike war, kept apart from the first step on: the player and whoever
// fights at his side are TEAM_TERRORIST, the soldiers are TEAM_COUNTER. A soldier only ever goes for the
// other side - never for his own team, and never for the host's people in the street (they belong to no
// team at all). Every older kind stays TEAM_NONE and plays by the old rules.
enum MobTeam { TEAM_NONE = 0, TEAM_TERRORIST, TEAM_COUNTER };

struct Mob {
    int kind = 0;
    Vec3 pos, vel; // pos = feet
    float yaw = 0.0f;
    float health = 10.0f;
    float hurt = 0.0f;   // seconds of red flash left
    float death = -1.0f; // seconds since it died, < 0 alive
    float limbSwing = 0.0f, limbAmount = 0.0f;
    float headYaw = 0.0f, headPitch = 0.0f;
    float aiTimer = 0.0f;
    Vec3 goal{ 0, 1, 0 };
    bool moving = false;
    float panic = 0.0f;
    float sayTimer = 5.0f;
    float eggTimer = 120.0f;
    bool sheared = false;
    float woolTimer = 0.0f;
    float flap = 0.0f, flapSpeed = 0.0f, flapping = 1.0f;
    bool onGround = false;
    bool persistent = false;
    float groundZ = -1000.0f;
    int groundTick = 0;
    float fallFrom = -1000.0f;
    float love = 0.0f;  // seconds in love mode (was fed)
    float baby = 0.0f;  // seconds left as a baby
    float breedCooldown = 0.0f;
    float lavaTimer = 0.0f;
    float burn = 0.0f;      // seconds it keeps burning
    float hitByCar = 0.0f;  // seconds until a vehicle of the host's can hit it again
    uint32_t id = 0;
    bool saddled = false;
    Vec3 rideVel{ 0, 0, 0 };
    float swell = 0.0f; // creeper: 0..1 of its fuse (it blows up at 1)
    int swellDir = -1;  // creeper: +1 swelling, -1 calming down
    // warden
    float anger = 0.0f;        // 80 and up: it goes for the player
    bool roared = false;       // (it roars once when it gets angry)
    float roar = 0.0f;         // seconds of roaring left (it stands)
    float listen = 0.0f;       // seconds until it can hear the next vibration
    float attackCooldown = 0.0f, attackAnim = 0.0f; // its blows (attackAnim 1..0: the arms come down)
    float boom = -1.0f;        // seconds charging a sonic boom, < 0 none
    float boomCooldown = 0.0f;
    float calm = 0.0f;         // seconds without any anger (a minute: it digs back down)
    float heart = 1.0f, pulse = 0.0f; // until the next heartbeat; 1..0 after one
    float shoot = 0.0f;        // skeleton: seconds until it may loose the next arrow
    int huntBeing = -1;        // the host's number of the being it is after (-1: the player, or nobody)
    bool huntValid = false;    // it is after someone (huntBeing says whom; -1 = the player)
    float huntMemory = 0.0f;   // seconds a held prey is still remembered once it slips out of reach
    float doorHitTime = 0.0f;  // zombie: seconds of steady hitting on the door in doorCell
    Int3 doorCell{ 0, 0, -1 }; // zombie: the lower cell of the door it is hitting (z < 0: none)
    // the Counter-Strike soldier (MOB_SOLDIER); every other kind leaves these at their defaults
    int team = TEAM_NONE;      // whose side he fights for (MobTeam); the war kinds set it at spawn
    float armor = 0.0f;        // soldier: kevlar points, counted in half hearts like health
    Vec3 anchor{ 0, 0, 0 };    // soldier: the post he was summoned at; his patrol walks around it
    int patrolStep = 0;        // soldier: which patrol point he is walking to
    float patrolWait = 0.0f;   // soldier: seconds he still stands at a patrol point, checking the angles
    float patrolLeg = 0.0f;    // soldier: seconds this patrol leg has taken (a stuck leg is given up)
    float seenTime = 0.0f;     // soldier: seconds his enemy has been in his sight (his reaction runs on it)
    bool alerted = false;      // soldier: he knows the enemy is about (he saw him, or a shot told him)
    float alertFade = 0.0f;    // soldier: quiet seconds after which being alerted wears off
    Vec3 lastSeen{ 0, 0, 0 };  // soldier: where he last saw (or heard) his enemy
    float searchTime = 0.0f;   // soldier: seconds he still hunts around lastSeen before giving up
    float scanTime = 0.0f;     // soldier: seconds of looking around at lastSeen / at a patrol point
    float shotTimer = 0.0f;    // soldier: seconds until the next shot of this burst
    int burstShots = 0;        // soldier: shots left in this burst (0: between bursts)
    int burstSize = 0;         // soldier: how many shots this burst started with (the first is the tightest)
    float burstPause = 0.0f;   // soldier: seconds until the next burst
    int mag = 0;               // soldier: bullets left in the magazine
    float reloadT = 0.0f;      // soldier: seconds the reload still takes (> 0: he is reloading)
    int reloadStage = 0;       // soldier: which reload sound is still to come (clip out, clip in, bolt)
    float strafeTime = 0.0f;   // soldier: seconds until he changes his sideways drift in a fight
    float strafeDir = 1.0f;    // soldier: which way he drifts while he holds his angle
    float stepAcc = 0.0f;      // soldier: metres walked since his last footstep sound
    int soldierDeath = 0;      // soldier: which of the game's death sequences is his (set as he falls)
    float shootShow = 0.0f;    // soldier: seconds the shoot pose still holds after a shot (a burst must read as firing)
    // ---- the boss (the solo soldier, finished): his draw state is composed by
    // his brain every tick (SoldierModel's two layers), the rest is his craft
    SoldierDrawState draw;     // boss: the layered pose the renderer draws
    int bossState = 0;         // boss: 0 calm, 1 command, 2 combat, 3 hunt, 4 suspicious
    float detect = 0.0f;       // boss: the detection meter, 0..1 (fills by visible fraction)
    float primedT = 0.0f;      // boss: seconds his detection still runs primed (x2)
    bool sawPlayerAlive = true; // boss: the player was alive at his last tick (the death edge stands him down)
    bool negotiated = false;   // boss: this encounter already had its one talk-first chance
    bool fightStarted = false; // boss: a fight is (or was) in progress this encounter - the gate on talk-first (G): commands belong to the guard context only; summoned into a fight or already engaged, he fights at once and gives no orders
    float loudT = 99.0f;       // boss: seconds since a gunshot-class sound was last heard (the same gate)
    bool aggro = false;        // boss: the engagement law (the user's order, 2026-11-07): by default
                               // he has NOTHING to do with the player - no warnings, no commands.
                               // Only a provocation sets this: the player damages him, aims a
                               // weapon at him (sustained), or drives a vehicle at/into him.
    float aimAggroT = 0.0f;    // boss: seconds the player's gun has been pointed at him
    float staggerT = 0.0f;     // boss: a heavy hit's stagger - he cannot fire and barely
                               // moves while it runs (damage feedback, scaled by the hit)
    float heardT = 0.0f;       // boss: seconds a heard direction stays worth looking at
    Vec3 heardDir{ 0, 1, 0 };  // boss: where the last heard sound came from
    float coverT = 0.0f;       // boss: seconds before cover is re-judged
    float playerHostileT = 0.0f; // boss: seconds the player still counts as the one fighting him
    float fightWarmT = 0.0f;   // boss: seconds the last firefight with the player still counts as warm -
                               // while it runs (and the prey is inside the wider hunt area) no hunt may
                               // decay into patrol or an anchor walk; stamped by shots exchanged, by a
                               // live firing solution in combat, and by the player's own hits on him
    int attackerBeing = -1;    // boss: the street being who attacked him (K1), -1 none
    float attackerFresh = 0.0f; // boss: seconds that provocation stays fresh
    bool proxyHit = false;     // boss: the pending MobHurt came through his perception body (bridge sets it)
    int proxyAttacker = -1;    // boss: ...and this being is the one who dealt it
    int lastVoiceEv = -1;      // boss: the last voice line event he played (never twice in a row)
    float voiceCd = 0.0f;      // boss: seconds before he speaks again
    float hurtSayT = 0.0f;     // boss: hurt-voice throttle
    float flinchT = 0.0f;      // boss: seconds his damage flinch still shows
    bool hurtHigh = false;     // boss: the last hit came in high (head flinch, not gut)
    float aimYaw = 0.0f;       // boss: where his torso and rifle point
    float legYaw = 0.0f;       // boss: where his legs face (his mob yaw follows it)
    int pitchIdx = 0;          // boss: the aim grid's pitch row (0 level, 1 up, 2 down)
    bool crouch = false;       // boss: crouched to shoot at range
    float airT = 0.0f;         // boss: seconds airborne (the jump legs read it)
    float jumpCd = 0.0f;       // boss: seconds before he may jump again
    float dodgeT = 0.0f;       // boss: seconds his dodge still runs
    Vec3 dodgeDir{ 0, 1, 0 };  // boss: the dodge's direction
    float ramT = 0.0f;         // boss: seconds he is still getting up from a vehicle hit
    int pistolMag = 15;        // boss: USP rounds left
    bool pistolOut = false;    // boss: the USP is in his hand instead of the rifle
    int nades = 2;             // boss: HE grenades left this fight
    float nadeCd = 0.0f;       // boss: seconds before another grenade
    float coverHeldT = 0.0f;   // boss: seconds the target has sat unseen in cover
    float lostT = 0.0f;        // boss: seconds since the target was last visible
    float targetVisT = 0.0f;   // boss: seconds the target has been continuously visible
    Vec3 preyVel{ 0, 0, 0 };   // boss: the target's tracked velocity (interception)
    Vec3 exitDir{ 0, 1, 0 };   // boss: the way the prey was heading when he was last seen - the hunt's
                               // search sweeps along this line before it searches anywhere else
    float huntT = 0.0f;        // boss: seconds this hunt has run
    int huntPhase = 0;         // boss: 0 to the intercept point, 1 working the search points
    Vec3 huntDest{ 0, 0, 0 };  // boss: the intercept point
    std::vector<Vec3> searchPts; // boss: the hunt's search points, nearest first
    int searchIdx = 0;         // boss: which search point he is on
    float scanT = 0.0f;        // boss: seconds he has been slicing this corner
    std::vector<Vec3> route;   // boss: his current map route (waypoints)
    int routeIdx = 0;          // boss: the waypoint he is walking to
    float routeT = 0.0f;       // boss: seconds before the route is recomputed regardless
    Vec3 routeGoal{ 0, 0, 0 }; // boss: the destination the route was built for
    // ---- the atlas (the map as data; Atlas.h / GTA-SA-ATLAS-fa.md)
    int campFamFirst = -1;     // boss: first atlas door index of the interior family he is
                               // camping (the prey ducked inside), -1: no camp
    int campFamCount = 0;      // boss: how many street doors that family has
    const char* campFam = nullptr; // boss: the family's enex name (a static table string)
    Vec3 prevPlayerPos{ 0, 0, 0 }; // boss: the player's position at his last brain tick -
                               // interior jumps and teleports are read off the difference
    bool prevPlayerInit = false;   // boss: prevPlayerPos holds a real reading
    // ---- the smart pack: combat navigation on the graph, the settle, the lock
    float repathT = 0.0f;      // boss: seconds before combat navigation may repath again (the ~1 s throttle)
    Vec3 chaseDest{ 0, 0, 0 }; // boss: the point combat navigation is steering to (intercept or fallback)
    float navProgT = 0.0f;     // boss: seconds since his position last moved 1.2 m while a move was wanted
    Vec3 navProgPos{ 0, 0, 0 }; // boss: where that last progress was made
    int navStuckCount = 0;     // boss: consecutive stall breaks near navStuckPos (the second one bans the pocket's node)
    Vec3 navStuckPos{ 0, 0, 0 }; // boss: where the last stall broke
    float navFailT = 0.0f;     // boss: seconds before a FAILED combat route may be re-asked -
                               // the probe walks the no-route stretch meanwhile (the tracker
                               // law's cross-map legs made the retry cadence matter)
    float settleT = 0.0f;      // boss: seconds his speed has stayed under the settled line (Counter-Strike's stop-shoot recovery)
    float firePlantT = 0.0f;   // boss: seconds he stays planted to finish a burst at range (set by fire control)
    Vec3 reposAt{ 0, 0, 0 };   // boss: where the target last broke his line at a NEW hiding place
    float reposT = 0.0f;       // boss: seconds that repositioning still reads as fresh (the early grenade)
    int tgtLockBeing = -2;     // boss: the target his lock holds (-2 none yet, -1 the player, >= 0 a being)
    float tgtLockT = 0.0f;     // boss: seconds before the target lock is re-evaluated (and its minimum hold)
    std::vector<Vec3> patrolPts; // boss: his Grove Street posts (graph nodes when loaded)
    float idleVoiceT = 30.0f;  // boss: seconds to his next idle radio line
    float scanClock = 0.0f;    // boss: his head-scan clock on post
    Vec3 coverPoint{ 0, 0, 0 }; // boss: the cover he is using
    Vec3 fallbackPoint{ 0, 0, 0 }; // smart pack: the wounded retreat's chosen ground, kept until reached
    bool hasFallback = false;
    float fallbackBanT = 0.0f; // after a fallback proves unreachable, no new one for a while
    Vec3 peekPoint{ 0, 0, 0 }; // boss: the half-step out of it he fires from
    bool hasCover = false;     // boss: a cover point is currently valid
    bool peekPhase = false;    // boss: true while stepped out of cover to fire
    float peekT = 0.0f;        // boss: seconds left in this peek
    float hideT = 0.0f;        // boss: seconds left hidden in cover
    int cmdPhase = 0;          // boss: talk-first phase (0 window, 1 approach, 2 hold)
    float cmdT = 0.0f;         // boss: the command window left
    float holdT = 0.0f;        // boss: the stand-down hold left
    float aimViolT = 0.0f;     // boss: seconds the player has been aiming at him during the talk
    float shootClock = 0.0f;   // boss: seconds into the shoot upper (recoil cycles on it)
    float sprayIndex = 0.0f;   // boss: shots into the current spray (the recoil pattern reads it; it recovers when the trigger rests)
    float sprayIdle = 0.0f;    // boss: seconds since his last shot (the recovery clock)
    int spraySeed = 1;         // boss: this spray's sway seed (1.6's recoil is semi-random: a new spray sways anew)
    bool sprayPistol = false;  // boss: the spray index belongs to the pistol (switching weapons resets it)
    float moveSpeed = 0.0f;    // boss: the speed his movement chose this tick
    Vec3 aimGoal{ 0, 0, 0 };   // boss: the point his eyes go to (states leave their wish here)
    float aimRate = 4.0f;      // boss: how fast the aim turns toward aimGoal
    int groundBad = 0;         // soldier: consecutive ground samples his feet refused (see MoveMob)
    float poseLogT = 0.0f;     // boss diag: seconds since his last pose ring-log line
    int poseLogLeg = -1;       // boss diag: the leg category/variant the last ring-log line carried
    int poseLogLegVar = 0;
    int poseLogUp = -1;        // boss diag: the upper category/variant the last ring-log line carried
    int poseLogUpVar = 0;
    float nadeShowT = 0.0f;    // boss: seconds a thrown grenade still shows as his diag weapon
    int stateLogFrom = -1;     // boss diag: the state the transition logger last reported (-1: no baseline yet)
    const char* stateWhy = nullptr; // boss diag: the reason the current transition carries, set at the gate
    float steerTime = 0.0f;    // soldier: seconds his obstacle detour still holds
    Vec3 steerDir{ 0, 1, 0 };  // soldier: the detour heading his wall probe picked
    float steerSide = 0.0f;    // soldier: the side (+1/-1) his current obstacle is being rounded on; 0 = none
    // ---- the driver (the hybrid step, 2026-11-07): the game's own ped
    // locomotion walks him while this brain stays the commander. The bridge
    // reports his driver body's transform before the brain ticks
    // (MobDriverReport) and the core adopts it wholesale - position,
    // velocity, ground - and integrates NOTHING for him: one writer for his
    // feet. The brain's movement wishes leave as an objective POINT plus a
    // speed class (drvObj), which the bridge's seek task executes on the
    // street's own node flow. Legacy steering (probe/graph) runs whenever
    // drvOn is false - no body, the toggle off, or the mode not yet up.
    bool drvOn = false;         // a live driver body reported for this frame
    Vec3 drvPos{ 0, 0, 0 };     // his feet, as the body last reported them
    Vec3 drvPrevPos{ 0, 0, 0 }; // the report before it (velocity is their difference)
    bool drvPrevInit = false;
    Vec3 drvObj{ 0, 0, 0 };     // the objective point his feet are walking to
    float drvObjSpeed = 0.0f;   // the speed the objective carries (0: stand)
    bool drvObjValid = false;   // an objective has been issued at least once
    bool drvMoved = false;      // an objective was issued this tick (brain-local)
    int drvObjCount = 0;        // material objective issues so far (tests read it)
    float drvLogT = 0.0f;       // the objective log's throttle clock
    const char* drvWhy = nullptr; // the reason the last material issue carried
    // ---- the single-soldier door and the hop (2026-11-07)
    bool reanchor = false;      // a summon re-anchored him to a new point (the
                                // single-soldier invariant in SpawnMob): the
                                // bridge must place his body at his feet again -
                                // the driver's mode entry - and clears this word
                                // when it has. The legacy glue needs no word: it
                                // re-glues him the same frame by construction.
    bool drvHop = false;        // one hop ordered for his driver body: his feet
                                // stalled against a live objective with a
                                // jumpable obstacle ahead (the legacy probe's
                                // own reading). The bridge executes it on the
                                // body (the game's own jump task) and clears it.
    bool drvHopOut = false;     // the order stands with the bridge (set when
                                // ordered, cleared when the bridge spends the
                                // word): the seek after the landing owns a
                                // fresh fair start, watched through this flag.
    float drvStallT = 0.0f;     // seconds with no approach progress against a
                                // live move objective, AFTER the fair start
                                // (the hop's trigger clock; raw speed is never
                                // its witness - see the hop block in BossTick)
    float drvGraceT = 0.0f;     // the fair start the current seek still owns:
                                // a seek from standstill turns and accelerates
                                // before it flows, and none of that may be
                                // read as a stall while this runs down
    float drvProgDist = 1e9f;   // the closest distance to the objective this
                                // stall window has seen: progress is measured
                                // against it, never against last tick alone
    // ---- the long-haul law (2026-11-07, the tracker repair round): a real
    // ped body only lives while the game streams and processes the ground
    // around the PLAYER - the session of 2026-11-07 watched the driver
    // body fall out of the world at 510 m and be adopted, falling, for
    // thirteen minutes. Inside the user's own 300 m the native body
    // drives; beyond it the core walks him itself (streaming-independent)
    // and the body is parked as the glued sensor it used to be.
    bool drvCoreTravel = false; // the core owns his feet for this stretch:
                                // the bridge parks the body (no reports)
    bool drvDropBody = false;   // the word to the bridge: this body was
                                // declared gone (it fell) - retire and kill
                                // it; a fresh one is stood up on re-entry
    float drvReengageT = 0.0f;  // seconds before a body may be re-engaged
                                // after a fall declared the last one gone
    Vec3 drvGoodPos{ 0, 0, 0 }; // the last SANE driver report (grounded,
                                // not below the host's ground): the ground
                                // a fall hands his feet back to
    bool drvGoodInit = false;
    float drvFallT = 0.0f;      // seconds of fall evidence against the
                                // current report stream (MobDriverReport)
    // ---- core-mode travel (the same law's other half, and the off-path
    // freedom of the same round): when the core walks him, the movers
    // leave their destination here and the stall witness below reads it.
    Vec3 travelDest{ 0, 0, 0 }; // the destination the core movers ordered
                                // this stretch (chase or calm), flat-read
    bool travelOrder = false;   // a core mover ordered movement this tick
    float travelGraceT = 0.8f;  // the fair start a fresh core order owns
                                // (kCoreFairStart in Mobs.cpp; a new man
                                // starts with it, like a rebuilt body does)
    float travelStallT = 0.0f;  // seconds of no approach under a live core
                                // order, past the fair start (the hop's
                                // trigger clock - progress is its witness)
    float travelProgDist = 1e9f; // the closest approach this window has seen
    int coreHopCount = 0;       // stall-witnessed hops ordered (tests read it)
    float cutT = 0.0f;          // the off-path chooser's re-evaluation clock
    bool cutting = false;       // the chooser's verdict: the direct line is
                                // traversable, he is cutting across country
    // ---- Po (MOB_PO), step 1: a minimal presence brain - he follows the
    // player, plays his six own clips, and that is all (the fighting roster
    // is later steps). His pose state rides in poDraw; the rest is clocks.
    PoDrawState poDraw;         // Po: the whole-body pose the renderer draws
    float poHitT = 0.0f;        // Po: seconds his hit flinch still shows (set by MobHurt)
    float poAtkCd = 0.0f;       // Po: seconds before his next proximity attack
    float poAtkRest = 1.1f;     // Po: the CURRENT rest after a swing (re-rolled per
                                // swing, 1.1..2.6 s - a fixed rest read as a loop)
    float poLogT = 0.0f;        // Po: the per-second state-log clock
};
bool IsMonster(int kind);
extern std::vector<Mob> gMobs;

// The tracker law's switch (defined in Mobs.cpp, with the law's full text):
// true is the shipped law the user ordered on 2026-11-07 - the boss always
// knows where the player is and comes for him from the summon, no
// provocation needed. False restores the provocation-gated engagement law
// of the same morning whole. A variable, not a constant, so the test suite
// can pin BOTH laws; the game only ever ships it true.
extern bool gTrackerAlways;

// The off-path freedom's switch (defined in Mobs.cpp with the chooser):
// true is the shipped law the user ordered on 2026-11-07 - he is NOT bound
// to pedestrian paths; the graph is a tool for ground the direct line
// cannot honestly cross, not a cage. A variable for the same reason as
// the tracker switch: the suite pins both settings.
extern bool gOffPathCut;

// One visible shot of the soldier's rifle: the core decides it (SoldierShoot), the
// host draws it (a muzzle flash at `from`, a tracer line to `to`) and it is gone in
// a blink. The flash shows for kShotFlashSeconds, the tracer for kShotFxSeconds.
struct ShotFx {
    Vec3 from, to;   // muzzle -> where the bullet's line ended
    float age = 0.0f; // seconds since the shot
};
inline constexpr float kShotFlashSeconds = 0.05f;
inline constexpr float kShotFxSeconds = 0.09f;
extern std::vector<ShotFx> gShotFx;

float MobScale(const Mob& m);  // babies are half the size
float MobWidth(const Mob& m);
float MobHeight(const Mob& m);

struct MobHit {
    int index = -1;
    float dist = 0.0f;
    Vec3 point;
};

// now and then a small herd appears somewhere around the player (when the game rules and the host allow it)
void MobsSpawnTick(float dt, const Vec3& playerPos);
// One frame of every animal. playerHeld: what the player holds out (food they follow), 0 for nothing.
// beings: the host's own people near the player this frame (nullptr: none known); monsters may go for them too.
void MobsTick(float dt, const Vec3& playerPos, uint16_t playerHeld, bool playerOnFoot, const std::vector<BeingInfo>* beings = nullptr);
void MobsClear();
// kind: MOB_*. Returns the index or -1.
int SpawnMob(int kind, const Vec3& feet, bool persistent, bool baby = false);
MobHit MobsRaycast(const Vec3& origin, const Vec3& dir, float maxDist);
// halfHearts of damage coming from `from`; knock = knockback strength (1 = a normal hit)
void MobHurt(int index, float halfHearts, const Vec3& from, float knock);

// ---- the boss's doors in from the bridge (workstreams K and L)
// One of his rounds has landed on a vehicle at world `point`: which zone of
// the vehicle it is, priced in half hearts of vehicle damage - the wheel band
// (below the body's own centre line) also bursts the tyre nearest the point
// through Host::VehicleShotEffect, the engine bay (the front of the frame)
// takes double, the plain body takes the base. Pure geometry on the vehicle's
// own frame (centre + forward from Host::VehiclePose / the player's pose), so
// the numbers can be pinned in tests exactly as the game will pay them.
struct BossVehZone {
    float dmg = 0.0f;  // half hearts of vehicle damage for this hit
    bool tyre = false; // the hit is at wheel height: burst the nearest tyre too
};
BossVehZone BossVehicleZone(const Vec3& point, const Vec3& centre, const Vec3& fwd, float baseDmg);
// The street provoked him: this being attacked him, and he may fight back.
void MobProvoked(int index, int being, const Vec3& from);
// Hurt that landed on his perception body in the host (the invisible proxy the
// gangs shoot at): relayed into his real health, per-event capped, never a
// one-shot, and it names the attacker when the bridge knows him.
void MobProxyDamage(int index, float halfHearts, const Vec3& from, int attackerBeing);
// Po's own damage door (fix round): hurt that landed on HIS perception body
// in the host (an invisible sensor ped glued to him, the boss's pattern) is
// relayed into his real health book here - per-event capped like the
// soldier's door, so no single event is a one-shot. Before this door
// existed, player guns and melee could never reach Po at all: the game's
// fire lands on peds, Po is not one, and the only relay was soldier-gated.
void MobPoProxyDamage(int index, float halfHearts);
// A vehicle is charging him on a collision line: he dodges if there is room.
void MobVehicleThreat(int index, const Vec3& vehPos, const Vec3& vehVel, bool driverIsPlayer);
// A vehicle actually hit him: heavy, capped, never fatal by itself, and the
// driver is detected completely and at once.
void MobVehicleHit(int index, float speed, const Vec3& from, bool driverIsPlayer);
// ---- the driver body's word in from the bridge (the hybrid step)
// Once a frame, before MobsTick, while his driver body lives: the soldier
// adopts the body's transform (feet, ground state; his velocity is derived
// from consecutive reports) and the core stops integrating his position -
// the game's feet own it. MobDriverGone is the word that the body is gone
// (lost, retired, or the mode switched off): the core steers him itself
// again, from exactly where he stands - he never teleports either way.
void MobDriverReport(int index, const Vec3& feet, bool grounded, float dt);
void MobDriverGone(int index);

// ---- loud facts for his ears, reported by the parts of the core that make them
void NotePlayerShot(const Vec3& at, float radius); // a weapon loosed (Combat)
void NoteExplosion(const Vec3& at);                // a blast (Entities)

// The boss's shared world state (defined in Mobs.cpp at mc:: scope): the loud
// facts his hearing reads, his live thrown grenades, and where a hunted man
// broke his line of sight this session.
struct Stimulus {
    Vec3 at;
    float radius; // how far the sound honestly carries
    float ttl;
};
extern std::vector<Stimulus> gStimuli;
struct BossNade {
    Vec3 pos, vel;
    float fuse = 0.0f;
};
extern std::vector<BossNade> gBossNades;
extern std::vector<Vec3> gHidingSpots;
void NoteHidingSpot(const Vec3& at);
void MobPush(int index, const Vec3& velocity); // m/s added to the animal
// explosions and shock waves: damage and push fall off with the distance
void MobsRadial(const Vec3& at, float radius, float halfHearts, float push);
// use with the held item (shears, bucket, food, saddle) or getting on; true if the use was taken
bool MobInteract(int index, bool playerOnFoot);
Vec3 MobCentre(int index);
// stable ids (indices change when animals despawn)
uint32_t MobIdAt(int index);
int MobIndexById(uint32_t id);
// The player rides this animal: it walks with `velocity` (m/s). Returns false when it is gone.
bool MobRide(uint32_t id, const Vec3& velocity, Vec3* seat, float* yaw);

} // namespace mc
