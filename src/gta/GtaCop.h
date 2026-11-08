#pragma once
// The GTA side of the Driver 2 cop (src/core/Cop.h has the brain itself):
// one real police car of the game's (model 596) and one contained driver
// of the game's own cop kind sitting in it, driven by the core's pedal
// and steering words. The channel those words travel has been convicted
// and rebuilt twice: fix round 4's autopilot mirror (retired by the root
// step - the traffic autopilot's arrive-and-stop was the last-metres
// disease), then the root step's plain field writes from the
// gameProcessEvent callback (convicted live on 2026-11-08: the game's
// ProcessAI recomputes the fields inside the frame's processing,
// before physics, so a callback write is a letter to a house already
// emptied). Since the channel repair, the bridge claims the class
// vtable's ProcessAI slot itself and stamps the core's words at that
// desk - the last writer before physics, inside the processing. The
// evidence chain is in DRIVER2-STEP3-fa.md. Since piece 3 the 596 is
// only the body UNDERNEATH: his own Chicago shape (cop_car.bin from
// his disc) is drawn over it through the ModelSink and the host car +
// driver are hidden while the whole body set is loaded - what
// collides, drives and burns is still the 596; what the eye sees is
// him. Thin, like every bridge file: bodies and field writes live
// here, decisions never do.

#include "Host.h"

namespace mc {

// a new session tore the world down: forget the body, hands off
void CopSessionReset();
// frame halves around the core's CopTick (called from MobsUpdate): Pre
// keeps the player's own car-damage ledger; Post re-asserts the stored
// pedal/steer writes after the game's own per-frame processing has had
// its turn - the dossier's warning is that processing may overwrite the
// fields, and the read-back in Report is where any overwrite shows
void CopBodyPre(float dt);
void CopBodyPost();
// draws his own body over every living unit (called from Render3D in
// the same pass as MobsRender, the one CleanSA keeps)
void CopRender(float light);

int GtaSpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face);
int GtaSpawnCopBikeBody(const Vec3& at, float headingDeg, const Vec3& face);
void GtaCopBodyBail(int veh, bool out);
void GtaNearbyVehicles(const Vec3& centre, float radius, std::vector<Host::VehicleInfo>& out);
bool GtaCopBodyReport(int veh, Host::CopBody* out);
void GtaCopBodyDrive(int veh, float gas, float brake, float steer, bool siren);
void GtaCopBodyDriveTarget(int veh, const Vec3& at, float cruiseMps);
void GtaCopBodySetHealth(int veh, float book01);
void GtaCopBodyRetire(int veh, bool burnOut);
float GtaPlayerVehicleImpact();
// /coptest staging and decoys (the complete update): move a cop body
// outright; stand up / drive / read / retire the plain decoy bodies the
// self-test grades the force against.
void GtaCopBodyPlace(int veh, const Vec3& at, float headingDeg);
int GtaSpawnDecoyCar(const Vec3& at, float headingDeg);
void GtaDecoyCarDrive(int veh, const Vec3& at, float cruiseMps);
bool GtaDecoyReport(int veh, Vec3* pos, Vec3* vel);
void GtaRetireDecoy(int veh);
int GtaSpawnDecoyPed(const Vec3& at);
bool GtaDecoyPedReport(int ped, Vec3* pos, float* health);
void GtaRetireDecoyPed(int ped);
// the momentum channel's hands (the 5x update): land the core's priced
// throw on a vehicle (dv m/s added, yawRate rad/s of spin) or on a man
// on foot (ped 0 = the player himself, otherwise a decoy ped)
void GtaKickVehicle(int veh, const Vec3& dv, float yawRate);
void GtaKickPed(int ped, const Vec3& dv);

} // namespace mc
