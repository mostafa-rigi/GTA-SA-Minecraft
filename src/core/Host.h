#pragma once

#include <string>
// The game Minecraft runs in ("the host"): its own map, its water and weather, and what only it can do. The core
// asks through this interface; the host registers its answers once with SetHost. Every question has a "there is
// no such thing here" answer by default, so a host without a map of its own (the offline tests) says nothing.

#include "Core.h"

namespace mc {

// kinds of explosions, for the host's own fire ball and damage
enum BlastKind { BLAST_TNT = 0, BLAST_SMALL, BLAST_FIRE };

struct Target;   // Interact.h
struct PickRay;  // Interact.h
struct VoxelHit; // World.h

// a cell of the host's map that an explosion opened
struct BlastedCell {
    Int3 cell;
    int block;    // what it was made of
    bool surface; // it could be seen
};

// What a struck surface is made of, in the coarse classes bullet penetration
// prices by (CsRules). The bridge reads it off the collision at the trace
// point; a host that cannot say leaves HS_NONE, and nothing is penetrated on
// faith - an unknown surface stops a bullet like a wall.
enum HitSurface {
    HS_NONE = 0,
    HS_GLASS,
    HS_WOOD_THIN,
    HS_WOOD,
    HS_FENCE,   // chain-link and grates
    HS_FOLIAGE, // hedges and bushes
    HS_METAL,
    HS_STONE,   // concrete, brick, rock: the world's own walls
    HS_OBJECT   // a loose object (door, crate, prop) of no clearer material
};

// What a blow or a shot met among the host's own things.
struct HostHit {
    bool hit = false;       // a wall, a vehicle, somebody
    float dist = 1e9f;      // how far along the line
    Vec3 point;
    int vehicle = -1;       // what was hit is a vehicle (the host's number for it)
    int object = -1;        // ... or a loose object (its number for it)
    Vec3 local;             // where on that vehicle / object, in the host's own terms
    int being = -1;         // somebody was hit (the host's number for him); he may sit behind what was hit, in a vehicle
    float beingDist = 1e9f;
    Vec3 beingPoint;
    int surface = HS_NONE;  // what the first struck surface is made of (HitSurface)
    float depth = 0.0f;     // how deep the trace point sits inside that surface's
                            // shape, in metres - a thickness hint for penetration;
                            // 0 when the host cannot say
};

// One of the host's own people near a point: his number, and where he is.
struct BeingInfo {
    int being = -1; // the host's number for him
    Vec3 pos;       // where he is (his middle, like the player's position)
};

// Whose shot it is, in the host's own numbers.
struct ShotOwner {
    bool hostile = false; // not the player's: it can hit him
    int shooter = -1;     // who shot it when it is not the player's (-1: nobody in particular)
    int vehicle = -1;     // the vehicle it left from and passes through (-1: none)
};

enum HurtKind { HURT_FIST, HURT_SWORD, HURT_SHOT };
enum HostVehicle { HOST_BOAT, HOST_MINECART };

struct Host {
    virtual ~Host() = default;
    // ---- its map
    virtual bool SolidCell(const Int3& c) { return false; }                  // its ground fills the cell
    virtual bool WallBetween(const Int3& a, const Int3& b) { return false; } // one of its walls between two neighbours
    virtual bool Supports(const Int3& c) { return false; }                   // it carries a block standing in the cell
    virtual bool CapAt(const Int3& c, float* top) { return false; }          // a partly filled cell, solid up to *top
    virtual bool GroundBelow(const Vec3& from, float maxDrop, float* z) { return false; } // its own ground under a point
    virtual bool LineBlocked(const Vec3& a, const Vec3& b) { return false; } // a wall, a car, a prop between two points
    // the same line, but one vehicle does not count as standing in it: a man
    // asking "can I see the driver of THAT car" must not be answered by the
    // car's own bodywork, the way the shot trace already ignores a vehicle
    virtual bool LineBlockedSkipVeh(const Vec3& a, const Vec3& b, int vehRef) { return LineBlocked(a, b); }
    // the world-only reading: SOLID geometry alone between two points -
    // buildings and props; vehicles and peds never count as standing in
    // it. The cop's corner probes ask this (the plough law): a traffic
    // car ahead is something to drive through, not a wall to read
    virtual bool LineBlockedWorld(const Vec3& a, const Vec3& b) { return LineBlocked(a, b); }
    virtual bool WaterLevel(const Vec3& at, float* level) { return false; }  // its own water (sea, pools) at that spot
    virtual bool FlammableUnder(const Int3& c) { return false; }             // bushes, dry grass under the cell
    // earth of its ground under a point (sand too when asked); *z: how high it is
    virtual bool SoilBelow(const Vec3& from, bool sandToo, float* z) { return false; }
    virtual bool CellBlocked(const Int3& c) { return false; }                // someone or something of its own is in the cell
    virtual bool Raining() { return false; }                                 // rain puts fires out
    virtual bool Outdoors() { return true; }                                 // not inside one of its buildings
    virtual bool Dark() { return false; }                                    // night on its map: monsters come out
    virtual bool SpawnGround(const Vec3& from, Vec3* ground) { return false; } // a spot below `from` where animals may appear
    // the cell is its own ground that was dug out (drawn only through its holes); *depthShade: darker deeper down
    virtual bool OwnGround(const Int3& c, float* depthShade) { return false; }
    // ---- what the player looks at
    // Something of the host's along the ray (`blocks`: what the ray hits among our blocks). True when the host
    // decided: `out` is its thing, or stays invalid because nothing is within reach. False: the block it is, if any.
    virtual bool Pick(const PickRay& ray, const VoxelHit& blocks, Target& out) { return false; }
    virtual void BreakTarget() {}                                            // gTarget, a thing of the host's, was mined
    // ---- the player, as long as the host keeps him (feet + 1 m; false: there is no player)
    virtual bool PlayerPos(Vec3* pos) { return false; }
    virtual std::string PlayerName() { return "Steve"; }
    virtual void KillPlayer() {}                                             // /kill
    virtual void SetWeather(int kind) {}                                     // /weather: 0 clear, 1 rain, 2 thunder
    virtual void SetWanted(int level) {}                                     // /wanted: its police, 0..6
    virtual Vec3 PlayerVelocity() { return Vec3(); }                         // m/s (his vehicle's when he sits in one)
    virtual bool PlayerOnGround() { return true; }
    virtual bool PlayerFalling() { return false; }                           // coming down through the air: his blows are critical
    virtual int PlayerVehicle() { return -1; }                               // its number for the vehicle he sits in, -1: on foot
    virtual void MovePlayer(const Vec3& to) {}                               // an ender pearl took him there
    virtual void HurtPlayer(float halfHearts) {}
    // the player's own body armour, on the host's 0..100 scale, and its wear:
    // the core prices kevlar law itself (its damage channel bypasses the
    // game's own armour), so it reads the vest here and wears it down there
    virtual float PlayerArmor() { return 0.0f; }
    virtual void WearPlayerArmor(float halfHearts) {}
    // ---- its own people and vehicles in a fight
    // how far the first of them is along the ray; `nothing` when there is none within reach
    virtual float AimDistance(const Vec3& origin, const Vec3& dir, float reach, float nothing) { return nothing; }
    virtual HostHit BlowTrace(const Vec3& origin, const Vec3& dir, float reach) { return HostHit(); } // the player's blow
    virtual HostHit ShotTrace(const Vec3& from, const Vec3& dir, float len, const ShotOwner& by) { return HostHit(); }
    virtual void HurtBeing(int being, float halfHearts, int how, const ShotOwner* by) {} // HurtKind; by nullptr: the player
    virtual void PushBeing(int being, const Vec3& velocity) {}               // m/s added to somebody on foot
    virtual HostHit BeingTrace(const Vec3& origin, const Vec3& dir, float reach) { return HostHit(); } // people only
    virtual bool UseOnBeing(int being) { return false; }                     // the use button on him (a villager trades)
    virtual void NearbyBeings(const Vec3& centre, float radius, std::vector<BeingInfo>& out) {} // its people near a point
    virtual void HurtVehicle(int vehicle, float halfHearts) {}
    // ---- the boss's vehicle senses (workstream L): the player's car as a target
    // The player's vehicle described for aimed fire: centre, forward, the driver's
    // chest position, velocity. False when he is on foot. The seat position is the
    // bridge's best knowledge of where the driver actually sits.
    virtual bool PlayerVehiclePose(Vec3* centre, Vec3* forward, Vec3* driverChest, Vec3* velocity) { return false; }
    // Any vehicle described for aimed fire: centre and forward, by its host
    // number (the same numbers ShotTrace and HurtVehicle trade in). The boss
    // reads a hit's zone (wheel band, engine bay, body) off this frame when one
    // of his rounds lands on a vehicle that is not the player's own - a parked
    // car in his line, a bike shielding the man he is shooting at. False when
    // the number names nothing the host still has.
    virtual bool VehiclePose(int vehicle, Vec3* centre, Vec3* forward) { return false; }
    // One aimed shot's effect on a vehicle at a world hit point:
    // effect 1 = burst the tyre nearest that point; effect 0 = nothing extra (the
    // body damage itself goes through HurtVehicle). Native thresholds (smoke,
    // fire, immobilisation) are the game's own business.
    virtual void VehicleShotEffect(int vehicle, const Vec3& hitPoint, int effect) {}
    // Is the player aiming a weapon at this world point (within radius)? The
    // game's own peds react to being aimed at; the boss polls the same fact.
    virtual bool PlayerAimingAt(const Vec3& point, float radius) { return false; }
    // ---- the Driver 2 cop's body (step 1): one real car of the host's, with
    // a contained driver of the host's own kind sitting in it, driven by the
    // core writing pedal and steering fields on it every tick. The report is
    // the spike's evidence: the pedal/steer values are read BACK from the
    // vehicle, never echoed from what the core last wrote - if the host's
    // own processing overwrites them, the read-back is where it shows.
    struct CopBody {
        bool ok = false;          // the body still resolves and lives
        Vec3 pos, fwd, vel;       // centre, forward (unit), velocity (m/s)
        float health = 1.0f;      // 0..1 of the host's own vehicle health
        float steer = 0.0f;       // read back: -1..1 of full lock
        float gas = 0.0f;         // read back: 0..1
        float brake = 0.0f;       // read back: 0..1
        bool siren = false;       // read back: the vehicle's own siren flag
        // damage since the last report that the host attributes to the
        // player, split by how it arrived (the boss relay's discipline:
        // a blow is priced once, by the channel that saw it). These price
        // the FELONY. His survival book is priced separately, below.
        float struckRam = 0.0f;   // host health points lost to his vehicle
        float struckShot = 0.0f;  // host health points lost to his gunfire
        float struckWorld = 0.0f; // host health points lost to everything
                                  // else (walls, fire, the world's own hits)
        // health points the seated DRIVER lost since the last report. He
        // is not a separate life: the cop is one unit (car + driver, as in
        // Driver 2, where no separate driver death exists) - the bridge
        // holds his health up and bills every hit on him to the cop's book
        float struckDriver = 0.0f;
        // the hardest single impact since the last report, as the car's
        // own velocity change (m/s): the collision signature the core
        // prices his damage book by, on the original's own curve
        float impactDv = 0.0f;
        // the drive state, read back like the pedals (fix round 4): the
        // switches the host's own automobile processing reads on its way
        // to the wheels - engine, handbrake, broken-engine, the entity
        // status, the autopilot's mission and cruise as the bridge last
        // found them, and whether the seated driver carries the host's
        // own in-vehicle flag. A body that answers the pedals but never
        // moves names its blocker among these, in the log, in plain
        // words - the spike line prints them every second.
        bool engineOn = false;
        bool handbrake = false;
        bool engineBroken = false;
        int status = -1;          // the host's eEntityStatus, raw
        int mission = -1;         // the autopilot's eCarMission, raw
        int cruise = 0;           // the autopilot's cruise setting, raw
        // the autopilot's traffic-speed cap read back beside it (the
        // 5x update): the cruise char above is a narrow field and
        // saturates at its width; this float is the field that held
        // the true ask. THE ROOT STEP retires the feed, but both
        // fields stay in the report as the middleman's witnesses: a
        // direct-driven body must answer mission NONE and cruise 0
        // while it drives, and the unit line prints the pair so a
        // session can see the claim stay empty
        float cruiseF = 0.0f;
        bool driverInVehicle = false;
        // THE CHANNEL REPAIR's witness (2026-11-08): the words the
        // direct-drive channel last stamped AT THE DESK PHYSICS READS
        // (the bridge's ProcessAI replacement, stamping the core's
        // stored command inside the vehicle's own control pass), and
        // how many frames it has stamped on this body so far. The
        // 14:44 live session convicted the old witnesses' timing: the
        // spike's fresh read-back showed gas 1.00 because it read the
        // fields microseconds after our own write inside the same
        // callback, while the unit line's tick-start report read gas
        // 0.00 / brake 0.50-1.00 because the game's ProcessAI had
        // already recomputed the fields inside that frame's
        // processing - same fields, two frame points, opposite
        // stories. These four are the third reading, taken at the
        // only point that decides motion: hook* must track the
        // commanded words while the body is ordered to drive, and
        // hookStamps must keep advancing. A host with no door to
        // stamp at (the fakes) answers the command itself, which IS
        // what their physics consumes, and counts its drive calls.
        float hookSteer = 0.0f;
        float hookGas = 0.0f;
        float hookBrake = 0.0f;
        int hookStamps = 0;
        // THE STATE CENSUS (the missing-link step, 2026-11-08): every
        // drive-state field on the body that plausibly gates or
        // shapes drive force, read back beside the pedals. The live
        // session on the channel build proved the desk claim works
        // (stamps advancing ~65/s, cmd == body == hook at the desk)
        // and the car STILL never moved - so something the original
        // ProcessAI maintained, beyond the three pedal words, is a
        // link between the desk and wheel force. The census is the
        // instrument that names it: the bridge fills these for a cop
        // body (here) and for a host-driven decoy (DecoyCensus below)
        // in the same shape, and the log prints both per second, so
        // one session diffs a moving car against our stamped car
        // field by field. The offsets are the plugin-sdk's
        // VALIDATE_OFFSETs, cited at the bridge's FillCensus. A host
        // with no such state (the fakes) answers trivially - gear 1,
        // wheels on the ground - which is stated wherever they do:
        // the census is host-domain state, and the fakes grade the
        // law, never the door.
        struct Census {
            int gear = -1;          // CVehicle::m_nCurrentGear
            float gearChange = 0.0f; // CVehicle::m_fGearChangeCount
            float rawSteer = 0.0f;  // CVehicle::m_fRawSteerAngle
            float steer2 = 0.0f;    // CVehicle::m_f2ndSteerAngle
            int fakePhysics = -1;   // CPhysical::m_nFakePhysics
            int tempAction = -1;    // CAutoPilot::m_nTempAction
            int ctrlFlags = -1;     // CAutoPilot::m_nCarCtrlFlags
            unsigned switched = 0;  // CAutoPilot::m_nTimeSwitchedToRealPhysics
            float revs = 0.0f;      // m_fGasPedalAudioRevs (car/bike)
            int contact = -1;       // wheels touching (car/bike layouts)
            int onGround = -1;      // wheels on the ground
            int driveGround = -1;   // drive wheels on ground last frame
            int wheelState[4] = {-1, -1, -1, -1}; // eWheelState per wheel
            float susp[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // car: suspension
                            // compression; bike: suspension heights
            float wheelSpeed[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // car: wheel
                            // speed; bike: wheel angular velocity
            float brakeCount = 0.0f; // the brake integrator (car/bike)
            int burnout = -1;       // car: m_bDoingBurnout; bike: brakes-on
        };
        Census census;
        // the 39-package's bail-out (item 38): once the core orders the
        // driver out (CopBodyBail), the bridge fights him on foot and
        // reports him here - out of the car, and where he stands. His
        // damage keeps arriving through struckDriver above: the measure
        // in the bridge's Pre runs on the driver wherever he is, which
        // is what keeps the one-unit book whole across the bail.
        bool driverOut = false;
        Vec3 driverPos;
    };
    // Stand the body up at `at` facing `headingDeg`, its nose turned toward
    // `face` (the original's justPinged: a fresh cop starts facing his man).
    // Returns the host's number for the car, or -1 when no body could be made.
    virtual int SpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face) { return -1; }
    // The 39-package's motorcycle unit (item 36): the same body
    // contract on the host's own police bike - reports, drives, health
    // display and retirement all ride the car verbs above by the
    // returned number; only the standing-up differs. A host with no
    // bike body answers -1 and the force falls back to a car, logged.
    virtual int SpawnCopBikeBody(const Vec3& at, float headingDeg, const Vec3& face) { return -1; }
    virtual bool CopBodyReport(int veh, CopBody* out) { return false; }
    // The bail order (item 38): out=true takes the seated driver out of
    // the named body and sets him fighting the player on foot (the
    // bridge's own ped machinery, the soldier's pattern); out=false
    // warps him back into his seat by the same warp that seated him.
    // The core owns WHEN; the bridge owns HOW, and reports the result
    // through CopBody::driverOut/driverPos.
    virtual void CopBodyBail(int veh, bool out) {}
    // The traffic around a point (item 24): the host's own vehicles
    // near `centre`, by the same numbers the kick and pose verbs trade
    // in. Cop bodies, decoys and the player's own vehicle are NOT
    // traffic and never appear here. A host that cannot enumerate
    // answers nothing, and the law that reads this stays silent.
    struct VehicleInfo {
        int veh = -1;
        Vec3 pos;
        Vec3 vel;
    };
    virtual void NearbyVehicles(const Vec3& centre, float radius, std::vector<VehicleInfo>& out) {}
    // gas/brake 0..1, steer -1..1 of full lock. Siren: the host's own siren
    // flag follows `siren` while the body is driven.
    virtual void CopBodyDrive(int veh, float gas, float brake, float steer, bool siren) {}
    // The drive's INTENT verb - RETIRED by the root step (2026-11-08)
    // and kept in the interface only so existing hosts still link.
    // Its history, kept straight: under the middleman design the core
    // named a destination+pace beside the pedals and the bridge
    // mirrored it into the car's traffic autopilot, because under
    // autopilot ownership the pedal fields were that channel's
    // outputs. Every convicted defect of every round was born in that
    // translation (the autopilot's native arrive-and-stop against a
    // through-point is the orbiting the user's eye finally convicted).
    // No core caller remains: Drive() sends the pedal words alone, and
    // hosts answer this verb with silence (the bridge's is a no-op).
    // A future host that re-implements it changes nothing unless the
    // core calls it again - it does not.
    virtual void CopBodyDriveTarget(int veh, const Vec3& at, float cruiseMps) {}
    // His survival law is HIS, not the host's (the user's order): the core
    // keeps his damage book on the original's scale and commands the
    // body's DISPLAYED health from it - book01 in (0,1] maps into the
    // host's safe band (damage stages show, the burn band is never
    // entered); book01 == 0 hands the body to the fire. The bridge holds
    // the displayed value against the host's own damage every frame.
    virtual void CopBodySetHealth(int veh, float book01) {}
    // burnOut: his book is spent - the driver is taken out and the body is
    // left to the host's own fire, not deleted. Otherwise both bodies are
    // removed outright (a re-summon, a session's end).
    virtual void CopBodyRetire(int veh, bool burnOut) {}
    // The strongest impact the player's own vehicle took since the last call
    // (host health points it lost in one step), consumed by the call - the
    // felony system's ears for "he hit something", read off his own car's
    // damage the way the game's books would show it.
    virtual float PlayerVehicleImpact() { return 0.0f; }
    // Move a cop body outright (test staging only: /coptest wedges a unit
    // nose-to-wall to grade its recovery). The bridge writes the matrix
    // the way the spawn does. Nothing in play ever calls this.
    virtual void CopBodyPlace(int veh, const Vec3& at, float headingDeg) {}
    // ---- the momentum channel (the 5x update): the core owns the law
    // (Cop.cpp prices the throw off the contact's closing speed, caps
    // it, and aims it); the host only displays it. KickVehicle adds dv
    // (m/s) to the named vehicle's velocity and yawRate (rad/s) to its
    // spin - veh is the host's own number for the vehicle, the same
    // numbers PlayerVehicle and the decoy spawns hand out; a number
    // that names nothing is ignored. KickPed does the same for a man
    // on foot: ped 0 is the player himself, any other number is a
    // decoy ped's. What the host's physics then does with a thrown
    // car or man (slide, roll, ragdoll) is the host's own business,
    // exactly like a collision it resolved itself.
    virtual void KickVehicle(int veh, const Vec3& dv, float yawRate) {}
    virtual void KickPed(int ped, const Vec3& dv) {}
    // ---- the /coptest decoys: plain host bodies the force is graded
    // against, so one session can answer what a night of free play used
    // to answer one defect at a time. A decoy car is an ordinary civilian
    // car with an ordinary contained driver, driven by the game's own
    // autopilot through DecoyCarDrive; a decoy ped is an ordinary man
    // standing where he was put. The core grades OUTCOMES off their
    // reports (positions, speeds, health) - never off intentions.
    virtual int SpawnDecoyCar(const Vec3& at, float headingDeg) { return -1; }
    virtual void DecoyCarDrive(int veh, const Vec3& at, float cruiseMps) {}
    virtual bool DecoyReport(int veh, Vec3* pos, Vec3* vel) { return false; }
    // The state census of a decoy body (the missing-link step): the
    // same CopBody shape the cop report fills - pedals, mission and
    // the whole census block - read off a HOST-DRIVEN body, so the
    // /coptest pursuit act can print a moving car's drive state
    // beside our stamped anchor's, per second, and the session's diff
    // is mechanical. False when the host cannot read the body.
    virtual bool DecoyCensus(int veh, CopBody* out) { return false; }
    virtual void RetireDecoy(int veh) {}
    virtual int SpawnDecoyPed(const Vec3& at) { return -1; }
    // health: the man's current host health (a run-over shows here and in
    // his displacement from where he was put).
    virtual bool DecoyPedReport(int ped, Vec3* pos, float* health) { return false; }
    virtual void RetireDecoyPed(int ped) {}
    // ---- the fishing hook
    virtual HostHit HookTrace(const Vec3& from, const Vec3& dir, float len) { return HostHit(); } // in the way of the flying hook
    virtual bool HookPoint(const HostHit& hooked, Vec3* at) { return false; } // where the hook on its thing is now (false: gone)
    virtual bool Fling(const HostHit& what, const Vec3& velocity) { return false; } // throws it through the air (false: gone)
    // a burst of wind throws its people, vehicles and loose things away from `centre` (the player too when asked)
    virtual void Gust(const Vec3& centre, float radius, float side, float up, bool playerToo) {}
    // ---- what only the host can do
    virtual void FluidPlaced(const Int3& c) {}                               // a bucket was emptied here
    virtual void Douse(const Vec3& at, float radius) {}                      // water was poured: its fires there go out
    virtual void Ignite(const Vec3& at, float seconds, int spread) {}        // a fire of its own (spread: how often it may jump)
    virtual float ClockHours() { return 12.0f; }                             // its clock, 0..24
    virtual void SetClock(float hours, bool nextDay) {}                      // a night slept through
    virtual void ClearWeather() {}                                           // (the storm is over too)
    virtual bool Thunderstorm() { return false; }                            // a trident calls down lightning
    virtual bool PlaceVehicle(int kind, const Vec3& at, float headingDeg) { return false; } // HostVehicle
    virtual void BoostVehicle(float seconds) {}                              // a firework strapped to the player's vehicle
    virtual bool ItemAttack(int special) { return false; }                   // attack / use with an item that is the host's
    virtual bool ItemUse(int special) { return false; }                      // own business; true: the click was used
    virtual void Explosion(const Vec3& at, int kind) {}                      // its own fire ball and damage (BlastKind)
    virtual void BlastMap(const Vec3& at, float radius, std::vector<BlastedCell>& opened) {} // craters in its map
};
void SetHost(Host* host); // nullptr: no host
Host& TheHost();

// highest solid surface (the host's ground, its caps, blocks) below `from`; false if there is none within `maxDrop`
bool GroundBelow(const Vec3& from, float maxDrop, float* zOut);

} // namespace mc
