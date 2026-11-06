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

// How long a change of pose takes to blend over, in seconds. The GoldSrc engine
// cross-fades its players' sequences; a hard snap reads as stiffness in ours.
inline constexpr float kSoldierFadeSeconds = 0.16f;

// Everything the draw needs to know about what the brain is showing, including the
// pose he is fading OUT of (prev*): the blend is evaluated here, from both poses'
// bone locals, so the figure never snaps between two sequences.
struct SoldierDrawState {
    int category = 0;       // the SoldierPose he is in
    int variant = 0;        // which sequence of the category (his death, chosen per soldier)
    float time = 0;         // seconds into the pose (for the gait poses: unused, see gait)
    float gait = 0;         // gait cycles travelled (SP_WALK / SP_RUN are phase-locked to it)
    int prevCategory = -1;  // the pose he is fading out of, < 0: no fade running
    int prevVariant = 0;
    float prevTime = 0;
    float prevGait = 0;
    float fade = 1.0f;      // 0..1, how far the blend out of the previous pose has run
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

// The gait's own stride length in metres, measured off the model by the converter
// (SP_WALK / SP_RUN; 0 for the other categories). A gait cycle played while the man
// travels exactly this far leaves his feet neither skating nor moon-walking.
float SoldierStride(int category);

// The rifle's two ends in the world for one drawn pose: where the muzzle sits and
// where the butt sits, through the very transform the draw uses. False when the
// model file carries no gun anchors (a version 1 file). For the shot effects and
// for the tests that prove his aim agrees with his facing.
bool SoldierGunPose(const Pose& base, int category, int variant, float time, Vec3* muzzle, Vec3* butt);

// Draws the soldier at `base` (the EntityPose stand DrawMob built him) in the pose
// state the brain chose. His body parts switch textures through MT_SOLDIER + part
// index, like the entity atlas but his own files.
void DrawSoldierModel(const Pose& base, const SoldierDrawState& st, float light, float r, float g, float b);

} // namespace mc
