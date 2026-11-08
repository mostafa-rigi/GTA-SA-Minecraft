#include "CopDiff.h"

#include <cstring>

// The region table. Every range is [start, end) in byte offsets off
// the SDK's VALIDATE_OFFSETs (plugin-sdk game_sa headers, cited per
// range); a word not covered by any range and inside
// [kCopBodyRegionStart, kCopBodyExtent) is CD_DRIVE - scalar drive
// state, adoptable subject to the value fence. The table is the
// brief's exclusion list made mechanical:
//   pointers        - every member the SDK declares as a pointer
//                     inside the region, by its validated offset
//   autopilot       - CAutoPilot at 0x390, size 0x98 (CAutoPilot.h)
//   identity        - creation time, colours, extras/upgrades,
//                     vehicle class/subclass, remap
//   health          - m_fHealth 0x4C0 (the book's display)
//   structural      - embedded objects whose insides mix scalars
//                     with pointers we cannot vet word by word
//   contact caches  - the wheel collision records: per-frame caches
//                     holding entity references and hit positions
namespace mc {
namespace {

struct Range {
    unsigned lo, hi;
    int cls;
};
constexpr Range kRanges[] = {
    // CVehicle's embedded audio entity (CVehicle.h: m_vehicleAudio
    // 0x138 .. m_pHandlingData 0x384): an AE subsystem object - its
    // insides are sound state and pointers, not wheel force
    { 0x138, 0x384, CD_STRUCT },
    // m_pHandlingData 0x384, m_pFlyingHandlingData 0x388 (the
    // handling FLAGS word at 0x38C between them and the autopilot
    // stays drive state - it is a scalar and it is handling)
    { 0x384, 0x38C, CD_POINTER },
    // CAutoPilot m_autoPilot 0x390 + 0x98: the middleman's whole
    // state. The vehicle FLAG words right behind it (0x428.. -
    // bEngineOn, bIsHandbrakeOn, bVehicleColProcessed,
    // bDriverLastFrame and their kin) are NOT in this range on
    // purpose: they are the body's own state and prime drive
    // territory
    { 0x390, 0x428, CD_AUTOPILOT },
    // m_nCreationTime 0x430
    { 0x430, 0x434, CD_IDENTITY },
    // m_nPrimaryColor..m_nQuaternaryColor 0x434..0x437
    { 0x434, 0x438, CD_IDENTITY },
    // m_anExtras 0x438, m_anUpgrades 0x43A.. (customisation identity)
    { 0x438, 0x458, CD_IDENTITY },
    // m_pDriver 0x460, m_apPassengers 0x464..0x483
    { 0x460, 0x484, CD_POINTER },
    // m_pEntityWeAreOn 0x48C, m_pFire 0x490
    { 0x48C, 0x494, CD_POINTER },
    // m_fHealth 0x4C0: the core's book displayed - the bridge
    // re-asserts it from the book every frame; copying the twin's
    // would only be overwritten, and the book is not drive state
    { 0x4C0, 0x4C4, CD_HEALTH },
    // m_pTractor 0x4C4, m_pTrailer 0x4C8, m_pWhoInstalledBombOnMe 0x4CC
    { 0x4C4, 0x4D0, CD_POINTER },
    // m_pWhoDetonatedMe 0x4E0
    { 0x4E0, 0x4E4, CD_POINTER },
    // m_pLastDamageEntity 0x50C
    { 0x50C, 0x510, CD_POINTER },
    // m_FrontCollPoly 0x51C, m_RearCollPoly 0x548: the body's
    // collision-shape cache (geometry, from the model)
    { 0x51C, 0x574, CD_STRUCT },
    // m_pOverheatParticle 0x578, m_pFireParticle 0x57C,
    // m_pDustParticle 0x580
    { 0x578, 0x584, CD_POINTER },
    // m_pCustomCarPlate 0x588
    { 0x588, 0x58C, CD_POINTER },
    // m_nVehicleClass 0x590, m_nVehicleSubClass 0x594
    { 0x590, 0x598, CD_IDENTITY },
    // m_anPreviousRemapTxd/m_nRemapTxd 0x598/0x59A and
    // m_pRemapTexture 0x59C: the remap identity word pair
    { 0x598, 0x5A0, CD_IDENTITY },
    // CAutomobile: m_damageManager 0x5A0.. (CDamageManager: the
    // cosmetic damage record - panels, lights, doors as shown)
    { 0x5A0, 0x5B8, CD_STRUCT },
    // m_doors 0x5B8..m_aCarNodes: CDoor bodywork state
    { 0x5B8, 0x648, CD_STRUCT },
    // m_aCarNodes 0x648..: RwFrame pointers, one per body node
    { 0x648, 0x6AC, CD_POINTER },
    // m_panels 0x6AC..: CPanel bodywork state
    { 0x6AC, 0x70C, CD_STRUCT },
    // m_swingingChassis 0x70C..: an object that holds an entity
    // pointer among its scalars - unvettable word by word
    { 0x70C, 0x724, CD_STRUCT },
    // m_wheelColPoint 0x724..0x7D4: CColPoint x4 - the wheels'
    // contact cache (surface, lighting, hit entity territory)
    { 0x724, 0x7D4, CD_CONTACT },
    // m_aDoorRotation 0x8CC..: door angles (bodywork)
    { 0x8CC, 0x8E4, CD_STRUCT },
    // m_pWheelCollisionEntity 0x8E8 and m_vWheelCollisionPos
    // 0x8F8..0x928: the last wheel contact, entity + positions
    { 0x8E8, 0x928, CD_CONTACT },
    // m_pExplosionVictim 0x928
    { 0x928, 0x92C, CD_POINTER },
    // m_pNitroParticle 0x978
    { 0x978, 0x97C, CD_POINTER },
};

// the best-known offsets wear their SDK names in the print (the
// census set and the pedal/flag words the earlier rounds convicted
// or cleared - a reader of the diff should meet them by name)
struct Name {
    unsigned off;
    const char* name;
};
constexpr Name kNames[] = {
    { 0x38C, "handlingFlags" },
    { 0x428, "vehicleFlags0 (bEngineOn/bHandbrake byte)" },
    { 0x42C, "vehicleFlags1" },
    { 0x494, "steer" },
    { 0x498, "steer2" },
    { 0x49C, "gas" },
    { 0x4A0, "brake" },
    { 0x4B4, "gear" },
    { 0x4B8, "gearChange" },
    { 0x4C0, "health" },
    { 0x58C, "rawSteer" },
    { 0x7D4, "suspFL" },
    { 0x804, "brakeCount" },
    { 0x828, "wheelRotation" },
    { 0x848, "wheelSpeed0" },
    { 0x868, "automobileFlags" },
    { 0x8A0, "extraTractionMult" },
    { 0x8A4, "nitroValue" },
    { 0x8B4, "moveForce" },
    { 0x960, "wheelContactCounts" },
    { 0x964, "revs" },
    { 0x968, "wheelState" },
};

bool PointsInto(unsigned val, unsigned base, unsigned extent) {
    return val >= base && val - base < extent;
}

} // namespace

int CopBodyWordClass(unsigned offset) {
    if (offset < kCopBodyRegionStart)
        return CD_HEADER;
    for (const Range& r : kRanges)
        if (offset >= r.lo && offset < r.hi)
            return r.cls;
    return CD_DRIVE;
}

const char* CopBodyClassName(int cls) {
    switch (cls) {
    case CD_DRIVE: return "drive";
    case CD_HEADER: return "header/physical";
    case CD_STRUCT: return "structural";
    case CD_POINTER: return "pointer";
    case CD_AUTOPILOT: return "autopilot";
    case CD_IDENTITY: return "identity";
    case CD_HEALTH: return "health";
    case CD_CONTACT: return "contact-cache";
    default: return "?";
    }
}

const char* CopBodyWordName(unsigned offset) {
    for (const Name& n : kNames)
        if (n.off == offset)
            return n.name;
    return nullptr;
}

bool CopBodyAdoptable(unsigned offset, unsigned anchorVal, unsigned twinVal,
                      unsigned anchorBase, unsigned twinBase, unsigned extent) {
    if (offset < kCopBodyRegionStart || offset + 4 > extent)
        return false;
    if (CopBodyWordClass(offset) != CD_DRIVE)
        return false;
    // the self-reference fence: a value that points into either
    // body's own extent is a link or an internal pointer, whatever
    // the range table said - links stay with their own body
    if (PointsInto(anchorVal, anchorBase, extent) ||
        PointsInto(anchorVal, twinBase, extent) ||
        PointsInto(twinVal, anchorBase, extent) ||
        PointsInto(twinVal, twinBase, extent))
        return false;
    return true;
}

int CopBodyDiffWords(const unsigned char* anchor, const unsigned char* twin,
                     unsigned extent, unsigned anchorBase, unsigned twinBase,
                     CopDiffWord* out, int outCap,
                     int* totalDiffOut, int* adoptableOut) {
    if (totalDiffOut)
        *totalDiffOut = 0;
    if (adoptableOut)
        *adoptableOut = 0;
    if (!anchor || !twin || extent < 4)
        return 0;
    if (extent > kCopBodyExtent)
        extent = kCopBodyExtent;
    int total = 0, adoptable = 0, filled = 0;
    for (unsigned off = 0; off + 4 <= extent; off += 4) {
        unsigned a, t;
        std::memcpy(&a, anchor + off, 4);
        std::memcpy(&t, twin + off, 4);
        if (a == t)
            continue;
        ++total;
        const bool ad = CopBodyAdoptable(off, a, t, anchorBase, twinBase, extent);
        if (ad)
            ++adoptable;
        if (out && filled < outCap) {
            CopDiffWord& w = out[filled++];
            w.offset = off;
            w.anchor = a;
            w.twin = t;
            w.cls = CopBodyWordClass(off);
            w.adoptable = ad;
        }
    }
    if (totalDiffOut)
        *totalDiffOut = total;
    if (adoptableOut)
        *adoptableOut = adoptable;
    return filled;
}

int CopBodyAdoptWords(unsigned char* anchorLive, const unsigned char* twinSnap,
                      const CopDiffWord* words, int wordCount,
                      unsigned* adoptedOffsetsOut, int outCap) {
    if (!anchorLive || !twinSnap || !words)
        return 0;
    int adopted = 0;
    for (int i = 0; i < wordCount; ++i) {
        if (!words[i].adoptable)
            continue;
        std::memcpy(anchorLive + words[i].offset, twinSnap + words[i].offset, 4);
        if (adoptedOffsetsOut && adopted < outCap)
            adoptedOffsetsOut[adopted] = words[i].offset;
        ++adopted;
    }
    return adopted;
}

} // namespace mc
