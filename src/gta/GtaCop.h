#pragma once
// The GTA side of the Driver 2 cop (src/core/Cop.h has the brain itself):
// one real police car of the game's (model 596 for now - his own shape
// waits for the original disc, piece 3) and one contained driver of the
// game's own cop kind sitting in it, driven by pedal and steering fields
// the core writes every tick. Thin, like every bridge file: bodies and
// field writes live here, decisions never do.

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

int GtaSpawnCopBody(const Vec3& at, float headingDeg, const Vec3& face);
bool GtaCopBodyReport(int veh, Host::CopBody* out);
void GtaCopBodyDrive(int veh, float gas, float brake, float steer, bool siren);
void GtaCopBodySetHealth(int veh, float book01);
void GtaCopBodyRetire(int veh, bool burnOut);
float GtaPlayerVehicleImpact();

} // namespace mc
