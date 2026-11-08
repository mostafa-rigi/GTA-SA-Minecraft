#pragma once
// THE FULL DIFF (2026-11-08): the diff/adoption engine of the /coptest
// diff act - pure arithmetic over two raw vehicle-body snapshots, so
// the suite can grade the shipping filter against synthetic buffers
// and the bridge can run it over two real CAutomobile extents.
//
// Why this exists, in one paragraph (the long form is in
// DRIVER2-STEP3-fa.md): four live builds proved the pedal channel
// byte-for-byte - cmd == body == hook at the claimed ProcessAI desk,
// the park latch (m_fBrakeCount) cleared under drive orders - and the
// anchor STILL never moved: gear 1, revs answering the gas, wheel
// speeds dead at zero. The 19-field census was a hand-picked
// instrument and it is exhausted. The diff is not hand-picked: it
// compares the WHOLE body of our stamped anchor against the whole
// body of a twin the game itself drives, word by word, and the
// adoption copies the twin's DRIVE-STATE words onto the anchor - if
// the car then drives, the copied set is the convicted set, named by
// offset, and the next build writes exactly those words on purpose.
//
// The region law (the brief's, applied to the SDK's own layout):
// adoption may touch only scalar state inside CVehicle/CAutomobile
// (offsets [0x138, 0x988) - CPhysical's size to CAutomobile's
// VALIDATE_SIZE). Excluded by range, each with its reason at the
// table in CopDiff.cpp: the header/physical block below 0x138
// (vtable, placement, position, velocity - outside the region by
// definition), declared pointer members, the autopilot block (the
// middleman's state: the root step's law keeps OUR autopilot at
// NONE, and a copied destination+cruise+mission would hand the car
// back to the middleman), identity (creation time, colours,
// extras/upgrades, class/remap), the health word (the book's
// display, re-asserted by the bridge's Post every frame), embedded
// subsystem objects whose insides cannot be vetted word by word
// (audio entity, damage manager, doors, panels, swinging chassis,
// collision polygons), and the contact caches (wheel collision
// points/entity/positions - per-frame caches that hold entity
// references). One value fence rides on top of the ranges: a word
// whose value on EITHER side points into either body's own extent
// is self-referential (list links, internal pointers) and is never
// adopted, whatever its class. Every declared pointer member inside
// the region is range-excluded by name; the residual risk - an
// undeclared pointer hiding in an SDK field_XXXX gap that is not
// self-referential - is named here rather than hidden: those gaps
// are single bytes/shorts in the layout, and the anchor is our own
// body in a test session.

#include <cstddef>

namespace mc {

// the whole CAutomobile extent the SDK validates
// (CAutomobile.h: VALIDATE_SIZE(CAutomobile, 0x988)); the bridge
// static_asserts its sizeof against this, the fake uses it as its
// synthetic extent size, so both run the same numbers
constexpr unsigned kCopBodyExtent = 0x988;
constexpr unsigned kCopBodyWords = kCopBodyExtent / 4;
// where CVehicle's own members begin (CPhysical.h:
// VALIDATE_SIZE(CPhysical, 0x138); CVehicle.h: m_vehicleAudio at
// 0x138 is the first member past it)
constexpr unsigned kCopBodyRegionStart = 0x138;

// the classes a differing word can fall into; the adoption takes
// CD_DRIVE only
enum CopDiffClass {
    CD_DRIVE = 0,    // scalar drive state inside the region: adoptable
    CD_HEADER,       // below the region: vtable/placement/entity/physical
    CD_STRUCT,       // embedded subsystem / bodywork state
    CD_POINTER,      // a declared pointer member
    CD_AUTOPILOT,    // the autopilot block (the middleman's state)
    CD_IDENTITY,     // creation time, colours, extras, class, remap
    CD_HEALTH,       // m_fHealth: the book's display word
    CD_CONTACT,      // contact caches (wheel collision points/entity)
    CD_COUNT
};

// one differing 4-byte word of the two snapshots
struct CopDiffWord {
    unsigned offset = 0; // byte offset into the body
    unsigned anchor = 0; // the anchor's word
    unsigned twin = 0;   // the twin's word
    int cls = CD_DRIVE;  // CopDiffClass
    bool adoptable = false; // class + region + value fences all pass
};

// the class of any byte offset in the body (CD_HEADER below the
// region start, CD_DRIVE for unlisted scalars inside it)
int CopBodyWordClass(unsigned offset);
// the class's short name, for the diff print ("drive", "autopilot", ...)
const char* CopBodyClassName(int cls);
// the SDK member name for the best-known offsets (the census set,
// the pedals, the flags), or nullptr - the diff print labels what
// it can and leaves the rest as bare offsets
const char* CopBodyWordName(unsigned offset);
// the adoption fence for one differing word: inside the region,
// CD_DRIVE class, and neither side's value pointing into either
// body's own extent (the self-reference fence; bases are the two
// bodies' addresses as integers - the bridge passes the real
// pointers, the fake passes its synthetic bases)
bool CopBodyAdoptable(unsigned offset, unsigned anchorVal, unsigned twinVal,
                      unsigned anchorBase, unsigned twinBase, unsigned extent);
// compare two snapshots word by word over `extent` bytes (clamped
// to kCopBodyExtent): fills up to outCap differing words into out,
// returns how many were filled, and reports the true differing
// total and the adoptable count through the out-params (either may
// be null). A null buffer or a sub-word extent fills nothing.
int CopBodyDiffWords(const unsigned char* anchor, const unsigned char* twin,
                     unsigned extent, unsigned anchorBase, unsigned twinBase,
                     CopDiffWord* out, int outCap,
                     int* totalDiffOut, int* adoptableOut);
// land the adoption: write the twin snapshot's word onto the live
// anchor bytes at every adoptable word in the diff list; records
// the adopted offsets (up to outCap) and returns the adopted count.
// Only the listed words are written - the caller owns the pointers
// and the moment (the bridge calls this inside one frame callback,
// snapshot and write in the same call, so nothing runs between).
int CopBodyAdoptWords(unsigned char* anchorLive, const unsigned char* twinSnap,
                      const CopDiffWord* words, int wordCount,
                      unsigned* adoptedOffsetsOut, int outCap);

} // namespace mc
