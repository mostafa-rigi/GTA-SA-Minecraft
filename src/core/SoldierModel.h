#pragma once
// The Counter-Strike soldier's own 3D model: the SEAL player model from Counter-Strike
// 1.6's urban.mdl with his M4A1 from p_m4a1.mdl, converted offline by
// tools/convert_cs_soldier.py into cs_soldier.bin and drawn here through the ModelSink.
// The poses are the game's own animation sequences, composited the way the game does
// (a gait sequence below the pelvis, the weapon layer above it).
//
// There is deliberately NO box-model fallback: when this file has not loaded, MOB_SOLDIER
// does not spawn at all (Mobs.cpp) and nothing is drawn for him. A cube with a CS texture
// on it is not the soldier.

#include "Pose.h"

namespace mc {

// the pose categories the soldier's brain picks between; they index the baked sequences
enum SoldierPose {
    SP_IDLE = 0,  // idle1, standing at his post
    SP_WALK = 1,  // walk gait + the carbine aim layer
    SP_RUN = 2,   // run gait + the carbine aim layer
    SP_AIM = 3,   // standing in ref_aim_carbine
    SP_SHOOT = 4, // ref_shoot_carbine
    SP_RELOAD = 5, // ref_reload_carbine
    SP_DEATH = 6, // death1 / death2 / death3, chosen per soldier
};

// Loads the runtime model out of raw cs_soldier.bin bytes (the bridge reads the file -
// the core itself never touches the disk). A broken file leaves the model unloaded and
// says so in the log; returns whether the model is ready afterwards.
bool LoadSoldierModel(const unsigned char* data, size_t size);
void UnloadSoldierModel();
bool SoldierModelReady();

// The posed figure's lowest and highest point (metres over his feet) in one category at
// one moment; variant picks among the category's sequences. For the tests.
bool SoldierPoseSpan(int category, int variant, float time, float* loZ, float* hiZ);

// Draws the soldier at `base` (the EntityPose stand DrawMob built him) in the pose the
// brain chose, `time` seconds into it. His body parts switch textures through
// MT_SOLDIER + part index, like the entity atlas but his own files.
void DrawSoldierModel(const Pose& base, int category, int variant, float time, float light, float r, float g, float b);

} // namespace mc
