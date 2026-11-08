#pragma once

#include "Core.h" // Vec3

namespace mc {

// The atlas: the whole of San Andreas as data in the soldier's head -
// extracted from the game's own files (see GTA-SA-ATLAS-fa.md and
// atlas-assets/). Places with exact positions and doors, the zone boxes
// (with the documented starting gang map laid over them), and the enex
// door families of every enterable interior: street doors on one side,
// the heaven record they pair with on the other.
//
// POI kinds (the numbers are baked into AtlasData.inc):
//   0 generic   1 hospital  2 police    3 ammu       4 pay'n'spray
//   5 mod shop  6 safehouse 7 food      8 gym        9 clothes
//  10 bar      11 strip    12 motel    13 24/7 shop 14 casino
//  15 airport  16 stadium  17 military 18 school
// Gang codes: 0 none, 1 Grove Street Families, 2 Ballas, 3 Los Santos
// Vagos, 4 Varrios Los Aztecas.

struct AtlasPoiInfo {
    const char* name = "";
    int kind = 0;
    bool refuge = false; // a place a fleeing man runs TO (heals, arms, hides, resprays)
    Vec3 at;             // the place itself (building anchor)
    Vec3 door;           // where you walk in (the enex entry when the data has one)
};

int AtlasPoiCount();
bool AtlasPoiGet(int i, AtlasPoiInfo& out);
// Nearest POI by its anchor; kind < 0 means any kind.
int AtlasNearestPoi(const Vec3& at, int kind, bool refugeOnly);
// Destination thinking: the prey is RUNNING (vel) - is a refuge in front of
// him, nearer to him than to the boss? Returns its POI index or -1. The
// caller intercepts at the place's DOOR, not at a bare map point.
int AtlasRefugeAhead(const Vec3& preyPos, const Vec3& preyVel, const Vec3& bossPos);

// Zones: the most specific (smallest) zone box containing the point wins.
const char* AtlasZoneNameAt(const Vec3& pos);
int AtlasZoneGangAt(const Vec3& pos); // gang code above; 0 = no gang / not in a gang zone
bool AtlasZoneBoxAt(const Vec3& pos, Vec3& mn, Vec3& mx, int& gang);

// Interior doors. kAtlasDoors holds every street-side enex entry, grouped
// contiguously by family name; kAtlasHeavens holds the interior-side
// records (entry = where the player materialises inside).
int AtlasDoorCount();
bool AtlasDoorGet(int i, const char*& fam, Vec3& entry, Vec3& exit);
int AtlasDoorNear(const Vec3& pos, float r); // door index whose ENTRY is within r (3D), -1 none
bool AtlasDoorFamRange(const char* fam, int& first, int& count); // contiguous range of a family
// Which interior family is the player inside? Matches his position against
// the heaven entries; on a hit, famOut names the family whose street doors
// he must come back out of.
bool AtlasHeavenFamAt(const Vec3& pos, float r, const char*& famOut);
// The heaven records themselves, enumerated (the exit-camping law's
// staging and its sims need to stand a man inside on purpose).
int AtlasHeavenCount();
bool AtlasHeavenGet(int i, const char*& fam, Vec3& entry);

} // namespace mc
