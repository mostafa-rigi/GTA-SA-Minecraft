#pragma once
// The GTA side of the animals (src/core/Mobs.h has the animals themselves).

#include "ModCommon.h"

class CPlayerPed;

namespace mc {

void MobsUpdate(float dt, CPlayerPed* player); // herds appear, cars hit animals, the animals live a frame
void MobsRender(float light);
void BossProxySessionReset(); // a new session tore every ped down: forget the boss's perception body, hands off
float BridgePlayerShotAge(); // seconds since the player's gun last fired (the boss relay's own clock, shared with the cop body)
bool GtaAnimalSpawnGround(const CVector& from, CVector& out);

} // namespace mc
