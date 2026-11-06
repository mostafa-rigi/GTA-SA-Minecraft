#pragma once
// The Counter-Strike soldier's own 3D model: the SEAL player model from Counter-Strike
// 1.6's urban.mdl with his M4A1 from p_m4a1.mdl, converted offline by
// tools/convert_cs_soldier.py into cs_soldier.bin (format version 3) and drawn here
// through the ModelSink.
//
// Version 3 stores the two animation layers SEPARATELY, the way the GoldSrc engine
// itself composes a player: a GAIT layer (idle/walk/run gaits in every direction the
// model owns - forward, strafe-left, strafe-right, back - plus crouch and jump legs)
// and an AIM layer (aim/shoot/reload/flinch uppers, baked at the aim grid's three
// pitch rows, standing and crouched). At draw time the lower-body bones come from
// the gait layer facing the MOVEMENT yaw and the spine-up bones from the aim layer,
// with the aim-vs-movement yaw difference distributed over the three spine bones -
// so a strafing soldier's legs step sideways while his rifle stays on the target,
// instead of the whole figure gliding like a statue (the user's verdict on v2).
// Deaths stay whole-body sequences: a falling man is one piece.
//
// There is deliberately NO box-model fallback: when this file has not loaded, MOB_SOLDIER
// does not spawn at all (Mobs.cpp) and nothing is drawn for him. A cube with a CS texture
// on it is not the soldier.

#include "Pose.h"

#include <array>

namespace mc {

// the pose categories the soldier's brain picks between; they index the baked sequences
enum SoldierPose {
    SP_IDLE = 0,   // idle1 (legs layer: standing; upper layer: the relaxed carry)
    SP_WALK = 1,   // walk gait (legs layer)
    SP_RUN = 2,    // run gaits (legs layer; variants: 0 fwd, 1 strafe-left, 2 strafe-right, 3 back)
    SP_AIM = 3,    // aim uppers (variants: 0..2 standing pitch level/up/down, 3..5 crouched)
    SP_SHOOT = 4,  // shoot uppers (variants like SP_AIM)
    SP_RELOAD = 5, // reload uppers (variant 0 standing, 1 crouched)
    SP_DEATH = 6,  // death1 / death2 / death3, whole-body, chosen per soldier
    SP_CROUCH = 7, // crouch_idle legs
    SP_JUMP = 8,   // the model's own jump legs, clamped over the arc
    SP_FLINCH = 9, // damage uppers (variant 0 gut, 1 head)
};

// which stored layer a sequence belongs to
enum SoldierLayer {
    SL_FULL = 0, // read whole (deaths)
    SL_LEG = 1,  // only its lower-body bones are read
    SL_UP = 2,   // only its spine-up bones are read
};

// How long a change of pose takes to blend over, in seconds. The GoldSrc engine
// cross-fades its players' sequences; a hard snap reads as stiffness in ours.
inline constexpr float kSoldierFadeSeconds = 0.16f;

// One bone's local transform, exposed so the brain can snapshot a composed pose
// for the cross-fade (the blend runs between composed poses, layers and all).
struct SoldierLocal {
    Vec3 pos;
    std::array<float, 4> q{ 0, 0, 0, 1 }; // x, y, z, w
};

// Everything the draw needs: the two layers' choices, their clocks, the two yaws
// (legs face the movement, torso aims), and the fade snapshot of the pose he is
// leaving. The mob's own yaw (the EntityPose the bridge builds) IS legYaw.
struct SoldierDrawState {
    int legCategory = SP_IDLE; // SP_IDLE / SP_WALK / SP_RUN / SP_CROUCH / SP_JUMP
    int legVariant = 0;        // run direction (SP_RUN only)
    float legTime = 0;         // seconds for the non-gait leg poses (idle, crouch, jump)
    float gait = 0;            // gait cycles travelled (SP_WALK / SP_RUN are phase-locked to it)
    int upCategory = SP_IDLE;  // SP_IDLE / SP_AIM / SP_SHOOT / SP_RELOAD / SP_FLINCH
    int upVariant = 0;         // pitch row (+3 crouched) or flinch kind
    float upTime = 0;          // seconds into the upper pose
    float aimYaw = 0;          // world yaw the torso and rifle hold
    float legYaw = 0;          // world yaw the legs hold (== the mob's yaw)
    int fullCategory = -1;     // >= 0: a whole-body pose (SP_DEATH) replaces both layers
    int fullVariant = 0;
    float fullTime = 0;
    // cross-fade out of the previous composed pose (pre-twist bone locals)
    bool hasPrev = false;
    std::array<SoldierLocal, 64> prevLocals{};
    int prevBones = 0;
    float fade = 1.0f; // 0..1, how far the blend out of the previous pose has run
};

// Loads the runtime model out of raw cs_soldier.bin bytes (the bridge reads the file -
// the core itself never touches the disk). Only format version 3 loads; an older file
// is refused loudly (rebake it with tools/convert_cs_soldier.py). A broken file leaves
// the model unloaded; returns whether the model is ready afterwards.
bool LoadSoldierModel(const unsigned char* data, size_t size);
void UnloadSoldierModel();
bool SoldierModelReady();
int SoldierBoneCount();
int SoldierSequenceCount(); // test support: how many layered sequences the loaded file carries

// Composes the draw state's bone locals (layers merged, no twist - the twist is a
// world-space step at composition time). False when a needed sequence is missing.
bool SoldierComposeLocals(const SoldierDrawState& st, SoldierLocal* out, int maxBones);

// The posed figure's lowest and highest point (metres over his feet) in one raw
// baked sequence at one moment. For the tests.
bool SoldierPoseSpan(int category, int layer, int variant, float time, float* loZ, float* hiZ);

// The gait's own stride length in metres, measured off the model by the converter.
// SoldierStride keeps the v2 meaning (SP_WALK / SP_RUN-forward); SoldierGaitStride
// answers per leg category + variant (strafe and back gaits have their own strides).
float SoldierStride(int category);
float SoldierGaitStride(int legCategory, int legVariant);

// The rifle's two ends in the world for one drawn pose: where the muzzle sits and
// where the butt sits, through the very transform the draw uses (layers, twist and
// all). For the shot effects and for the tests that prove his aim agrees with his
// facing - and his legs with his movement.
bool SoldierGunPose(const Pose& base, const SoldierDrawState& st, Vec3* muzzle, Vec3* butt);

// The world direction his legs are stepping in for one drawn pose (thigh-to-toe
// swing axis, ground-projected), through the draw's own transform. For the tests
// that prove the two layers face their own directions.
bool SoldierLegDir(const Pose& base, const SoldierDrawState& st, Vec3* dir);

// Draws the soldier at `base` (the EntityPose stand DrawMob built him, yaw = his
// leg yaw) in the pose state the brain chose. His body parts switch textures
// through MT_SOLDIER + part index, like the entity atlas but his own files.
void DrawSoldierModel(const Pose& base, const SoldierDrawState& st, float light, float r, float g, float b);

// The bone hierarchy's parents (test support: proving the waist twist never
// stretches a bone).
const int* SoldierBoneParents();
// The composed state's height span in model space (lowest and highest bone
// origin, metres) - the span of the whole figure the layers make together.
bool SoldierComposedSpan(const SoldierDrawState& st, float* lo, float* hi);

} // namespace mc
