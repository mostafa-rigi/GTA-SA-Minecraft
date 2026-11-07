#pragma once
// The GTA side of the Driver 2 cop (src/core/Cop.h has the brain itself):
// one real police car of the game's (model 596) and one contained driver
// of the game's own cop kind sitting in it, driven by pedal and steering
// fields the core writes every tick - and, since fix round 4, by the
// drive intent the bridge mirrors into the car's own autopilot, the
// channel this host's AI cars are actually driven through (the
// conviction and the evidence chain are in DRIVER2-STEP3-fa.md). Since piece 3 the 596 is only the
// body UNDERNEATH: his own Chicago shape (cop_car.bin from his disc) is
// drawn over it through the ModelSink and the host car + driver are
// hidden while the whole body set is loaded - what collides, drives
// and burns is still the 596; what the eye sees is him. Thin, like
// every bridge file: bodies and field writes live here, decisions
// never do.

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

} // namespace mc
