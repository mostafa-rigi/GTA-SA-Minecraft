#pragma once
// Wooden doors: the one block that stands two cells tall and swings on a hinge.
//
// A door follows the bed's precedent (Beds.h): two block ids, the lower half - which is the item
// the player holds - and the upper half, which is only ever placed with it. The halves stand in
// cells one above the other and both carry the same meta, so the whole state of a door lives in
// the cells themselves and a save file needs nothing extra (World::Write stores the raw cell).
//
// The meta (four bits, all of them used):
//   bits 0-1  the way the door's front faces (a Face: FACE_EAST, ...), as a bed's meta does
//   bit  2    open: the panel stands swung against the side wall instead of across the doorway
//   bit  3    the hinge is on the right, seen from the front (clear: on the left)
//
// There is no bit left for crack stages, so a zombie breaking a door shows no cracks - it hits
// the wood (sound, chips) and after about five seconds the door breaks whole (Mobs.cpp).
#include "Core.h"
#include "GameTables.h"
#include "Shapes.h"

namespace mc {

constexpr uint8_t DOOR_OPEN = 0x4;  // meta bit: the door stands open
constexpr uint8_t DOOR_HINGE = 0x8; // meta bit: hinge on the right, seen from the front

bool IsDoorBlock(int block);       // either half of a wooden door
bool IsDoorUpper(int block);       // the upper half only
int DoorOtherBlock(int block);     // the other half's block, 0 for anything else
inline int DoorFacing(uint8_t meta) { return meta & 3; }
inline bool DoorIsOpen(uint8_t meta) { return (meta & DOOR_OPEN) != 0; }
inline bool DoorHingeRight(uint8_t meta) { return (meta & DOOR_HINGE) != 0; }

// the other half's cell: straight up from the lower half, straight down from the upper
Int3 DoorOtherCell(const Int3& p, int block);

// the one panel a door half fills in its own cell (three pixels thick, as in Minecraft):
// shut, it lies across the cell against the edge away from the front; open, it lies along
// the side wall on the hinge's side. Used for the mesh, for bumping into, and for the icon.
ShapeBox DoorPanelBox(uint8_t meta);

// open a shut door or shut an open one: both halves change together and the door's own
// sound plays. Takes either half's cell. False when there is no whole door there.
bool ToggleDoor(const Int3& cell);

} // namespace mc
