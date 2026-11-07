#pragma once
// The Driver 2 cop's OWN body: the Chicago police car from the user's
// disc (CHICAGO.LEV, LUMP_CAR_MODELS, external model 0), converted
// offline by tools/convert_driver2_cop.py into cop_car.bin (format
// "D2C1" v1) and drawn here through the ModelSink, the same door the
// Counter-Strike soldier's body came through. His textures ride as
// cop_tex<i>.png beside it (one per (page, CLUT) pair his polys name,
// plus one solid white card for his flat-colour polys, whose records
// carry no texture - only RGB).
//
// The bin holds four meshes in a fixed order:
//   0  the clean body (128 verts, 252 tris)
//   1  the DENT TARGET: the same 128 vertices at their fully-dented
//      positions, no polys of its own. In the original this blob is
//      not a second body - DENTING.C blends every vertex from clean
//      toward it by the car's per-zone damage, and DrawCopCar does the
//      same blend with ONE uniform factor from his damage book (the
//      book prices the whole car, not zones - the simplification is
//      named here and in the piece-3 report).
//   2  the clean wheel (CLEANWHEEL), 3  the fast wheel (FASTWHEEL):
//      DrawCarWheels swaps them by spin speed in the original; the
//      caller here picks by the same idea. Wheel verts 8..23 carry the
//      spin flag: they are the group CARS.C rewrites every frame from
//      the spin angle, so they (and only they) rotate at draw time.
//
// There is deliberately no fallback body in the core: when this file
// has not loaded, DrawCopCar draws nothing and the bridge keeps the
// host car visible and says so in the log (the force already drives on
// the host body; a silent wrong body would be worse than the named
// stand-in - the deviation from the soldier's refuse-law is the
// bridge's, documented in GtaCop).

#include "Pose.h"

namespace mc {

// Loads the runtime model out of raw cop_car.bin bytes (the bridge
// reads the file - the core never touches the disk). Only format
// version 1 loads; a broken or foreign file leaves the model unloaded.
// Returns whether the model is ready afterwards.
bool LoadCopCarModel(const unsigned char* data, size_t size);
void UnloadCopCarModel();
bool CopCarModelReady();
int CopCarTextureCount(); // textures the loaded bin declares
int CopCarBodyTris();     // the clean body's triangle count (tests)

// The clean body's bounding box in core metres (x right, y forward,
// z up, origin at the model's own origin - the floor pan's underside
// sits at the low z, the light bar at the high z).
bool CopCarBodyExtents(Vec3* lo, Vec3* hi);

// His wheel centres in car space, from the LCF profile (CopProfile):
// x and the axle positions are his wheelDisp read through the draw law
// (DrawCarWheels negates vz; FixCarCos's -13 is already in the
// profile), the height is the converter's arch measurement
// (kCopWheelCenterZ - see CopCarModel.cpp for its provenance).
void CopWheelCenters(Vec3 out[4]);

// Draws his body at `pose` (car frame: pose.X = right, pose.Y =
// forward, pose.Z = up, pose.o = the car's origin at ground level in
// the middle of the car) with the four wheels at `wheelCenters`
// (car space, metres).
//   dent01: 0..1, his book's spent fraction - the vertex blend toward
//     the dent target runs at dent01 * 0.5, the original's own /2 for
//     a fully damaged zone (DENTING.C).
//   steer: the front wheels' yaw in radians (positive = toward his
//     left, -x); wheels 0 and 2 are the front axle (DrawCarWheels
//     steers the even wheelnums).
//   spin: the wheels' RotX angle in radians; forward travel DECREASES
//     it (the top of the wheel moves forward). Only flag verts rotate.
//   fastWheels: draw FASTWHEEL instead of CLEANWHEEL (his spin swap).
void DrawCopCar(const Pose& pose, float dent01, const Vec3 wheelCenters[4],
                float steer, float spin, bool fastWheels,
                float light, float r, float g, float b);

} // namespace mc
