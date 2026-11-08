// The atlas implementation: thin queries over the generated static tables
// (AtlasData.inc, built by atlas-assets/build_atlas_data.py from the game's
// own data files - enex/grge sections of the IPLs, the inst placements of
// the text and binary IPLs, info.zon/map.zon; a handful of documented
// landmarks are flagged DOC in poi_data.json and cross-checked against
// the zone they land in before they are admitted).

#include "Atlas.h"

#include <cmath>
#include <cstring>

namespace mc {

#include "AtlasData.inc"

int AtlasPoiCount() { return (int)(sizeof(kAtlasPois) / sizeof(kAtlasPois[0])); }

bool AtlasPoiGet(int i, AtlasPoiInfo& out) {
    if (i < 0 || i >= AtlasPoiCount())
        return false;
    const AtlasPoiRow& r = kAtlasPois[i];
    out.name = r.name;
    out.kind = r.kind;
    out.refuge = r.refuge != 0;
    out.at = r.at;
    out.door = r.door;
    return true;
}

int AtlasNearestPoi(const Vec3& at, int kind, bool refugeOnly) {
    int best = -1;
    float bd = 1e18f;
    for (int i = 0; i < AtlasPoiCount(); ++i) {
        const AtlasPoiRow& r = kAtlasPois[i];
        if (kind >= 0 && r.kind != kind)
            continue;
        if (refugeOnly && !r.refuge)
            continue;
        const float d = (r.at - at).LengthSq();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

int AtlasRefugeAhead(const Vec3& preyPos, const Vec3& preyVel, const Vec3& bossPos) {
    const float sp = std::sqrt(preyVel.x * preyVel.x + preyVel.y * preyVel.y);
    if (sp < 2.0f)
        return -1; // not running anywhere: no destination to read
    const float vx = preyVel.x / sp, vy = preyVel.y / sp;
    int best = -1;
    float bestScore = 1e18f;
    for (int i = 0; i < AtlasPoiCount(); ++i) {
        const AtlasPoiRow& r = kAtlasPois[i];
        if (!r.refuge)
            continue;
        const float dx = r.door.x - preyPos.x, dy = r.door.y - preyPos.y;
        const float dPrey = std::sqrt(dx * dx + dy * dy);
        if (dPrey < 4.0f || dPrey > 150.0f)
            continue; // already there, or too far to be the plan
        const float dot = (dx * vx + dy * vy) / dPrey;
        if (dot < 0.35f)
            continue; // not in front of his run
        const float bx = r.door.x - bossPos.x, by = r.door.y - bossPos.y;
        const float dBoss = std::sqrt(bx * bx + by * by);
        if (dPrey > dBoss * 0.9f)
            continue; // the boss is closer to it than the prey is: no race to win
        const float score = dPrey - 40.0f * dot;
        if (score < bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

static const AtlasZoneRow* ZoneRowAt(const Vec3& pos) {
    const AtlasZoneRow* best = nullptr;
    float bestArea = 1e18f;
    for (size_t i = 0; i < sizeof(kAtlasZones) / sizeof(kAtlasZones[0]); ++i) {
        const AtlasZoneRow& z = kAtlasZones[i];
        if (pos.x < z.mn.x || pos.x > z.mx.x || pos.y < z.mn.y || pos.y > z.mx.y)
            continue;
        const float area = (z.mx.x - z.mn.x) * (z.mx.y - z.mn.y);
        if (area < bestArea) {
            bestArea = area;
            best = &z;
        }
    }
    return best;
}

const char* AtlasZoneNameAt(const Vec3& pos) {
    const AtlasZoneRow* z = ZoneRowAt(pos);
    return z ? z->name : "";
}

int AtlasZoneGangAt(const Vec3& pos) {
    const AtlasZoneRow* z = ZoneRowAt(pos);
    return z ? (int)z->gang : 0;
}

bool AtlasZoneBoxAt(const Vec3& pos, Vec3& mn, Vec3& mx, int& gang) {
    const AtlasZoneRow* z = ZoneRowAt(pos);
    if (!z)
        return false;
    mn = z->mn;
    mx = z->mx;
    gang = (int)z->gang;
    return true;
}

int AtlasDoorCount() { return (int)(sizeof(kAtlasDoors) / sizeof(kAtlasDoors[0])); }

bool AtlasDoorGet(int i, const char*& fam, Vec3& entry, Vec3& exit) {
    if (i < 0 || i >= AtlasDoorCount())
        return false;
    fam = kAtlasDoors[i].fam;
    entry = kAtlasDoors[i].entry;
    exit = kAtlasDoors[i].exit;
    return true;
}

int AtlasDoorNear(const Vec3& pos, float r) {
    int best = -1;
    float bd = r * r;
    for (int i = 0; i < AtlasDoorCount(); ++i) {
        const float d = (kAtlasDoors[i].entry - pos).LengthSq();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

bool AtlasDoorFamRange(const char* fam, int& first, int& count) {
    first = -1;
    count = 0;
    for (int i = 0; i < AtlasDoorCount(); ++i) {
        if (std::strcmp(kAtlasDoors[i].fam, fam) != 0)
            continue;
        if (first < 0)
            first = i;
        ++count;
    }
    return first >= 0;
}

int AtlasHeavenCount() { return (int)(sizeof(kAtlasHeavens) / sizeof(kAtlasHeavens[0])); }

bool AtlasHeavenGet(int i, const char*& fam, Vec3& entry) {
    if (i < 0 || i >= AtlasHeavenCount())
        return false;
    fam = kAtlasHeavens[i].fam;
    entry = kAtlasHeavens[i].entry;
    return true;
}

bool AtlasHeavenFamAt(const Vec3& pos, float r, const char*& famOut) {
    int best = -1;
    float bd = r * r;
    const int n = (int)(sizeof(kAtlasHeavens) / sizeof(kAtlasHeavens[0]));
    for (int i = 0; i < n; ++i) {
        const float d = (kAtlasHeavens[i].entry - pos).LengthSq();
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (best < 0)
        return false;
    famOut = kAtlasHeavens[best].fam;
    return true;
}

} // namespace mc
