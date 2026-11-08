#pragma once
// Po, the Kung Fu Panda: his own real model, ripped live from his own game
// (the verified Ninja Ripper rip with full skin weights), his own 108-bone
// skeleton from the Hong Kong level file, and six of his own decoded
// animations, baked offline by tools/convert_po.py into po.bin (format
// POM1 v1) and drawn here through the ModelSink, exactly the soldier's road.
//
// Unlike the soldier he is NOT one-bone-per-vertex GoldSrc skinning: his
// vertices carry up to four bone influences with weights (the rip's own
// blend data), so the skinning here is true linear-blend skinning against
// per-bone inverse bind matrices. Unlike the soldier he has ONE animation
// layer - whole-body sequences only - because his decoded clips are
// rotation-only keys with no translation channel: every clip plays IN
// PLACE and the core entity's own movement carries him across the ground,
// his gait phase locked to the distance covered at each gait's measured
// stride, the way the soldier's gaits already work.
//
// There is deliberately NO box-model fallback: when po.bin has not loaded,
// MOB_PO does not spawn at all (Mobs.cpp) and nothing is drawn for him.

#include "Pose.h"

#include <array>

namespace mc {

// the pose categories Po's brain picks between; they index the baked sequences
enum PoPose {
    PP_IDLE = 0,   // clip 52: standing, feet planted, his calm loop
    PP_WALK = 1,   // clip 14: stepping (provisional identification, see PO-STEP1-fa.md)
    PP_RUN = 2,    // clip 17: the forward-lean charge (provisional)
    PP_ATTACK = 3, // clip 61: standing overhead chops
    PP_HIT = 4,    // clip 29: the sharp standing flinch
    PP_DIE = 5,    // clip 18: launched back, ends crumpled (provisional)
};

// How long a change of pose takes to blend over, in seconds (the soldier's value).
inline constexpr float kPoFadeSeconds = 0.16f;

// One bone's local transform, exposed so the brain can snapshot a composed
// pose for the cross-fade.
struct PoLocal {
    Vec3 pos;
    std::array<float, 4> q{ 0, 0, 0, 1 }; // x, y, z, w
};

// Everything the draw needs: the current whole-body category, its clock (or,
// for the gaits, the phase in cycles travelled), and the fade snapshot of
// the pose he is leaving.
struct PoDrawState {
    int category = PP_IDLE;
    float time = 0; // seconds into the pose (idle / attack / hit / die)
    float gait = 0; // gait cycles travelled (PP_WALK / PP_RUN are phase-locked to it)
    // cross-fade out of the previous composed pose
    bool hasPrev = false;
    std::array<PoLocal, 128> prevLocals{};
    int prevBones = 0;
    float fade = 1.0f; // 0..1, how far the blend out of the previous pose has run
};

// Loads the runtime model out of raw po.bin bytes (the bridge reads the file -
// the core itself never touches the disk). Only format version 1 loads. A
// broken file leaves the model unloaded; returns whether the model is ready.
bool LoadPoModel(const unsigned char* data, size_t size);
void UnloadPoModel();
bool PoModelReady();
int PoBoneCount();
int PoSequenceCount(); // test support
int PoVertexCount();   // test support / the creation log line

// Composes the draw state's bone locals (cross-fade NOT included - the
// renderer blends through PoRenderLocals' twin inside PoModel.cpp; this is
// the raw category pose the brain snapshots and the tests measure).
bool PoComposeLocals(const PoDrawState& st, PoLocal* out, int maxBones);

// The baked duration of one category's sequence, seconds (0 when missing).
float PoPoseDuration(int category);
// A gait's own stride length in metres, measured off the model by the
// converter (PP_WALK / PP_RUN; 0 for the rest).
float PoStride(int category);

// Draws Po at `base` (the EntityPose stand DrawMob built him) in the pose
// state the brain chose. His single texture goes through MT_PO.
void DrawPoModel(const Pose& base, const PoDrawState& st, float light, float r, float g, float b);

// The bone hierarchy's parents (test support).
const int* PoBoneParents();
// Test support: the exact matrices and skinned positions the renderer
// composes for a draw state - per bone, the world matrix and the skin
// matrix (world x inverse bind), each as 12 floats (3x3 rows, then the
// translation), plus every skinned vertex in model space, in file order.
// This is DrawPoModel's own composition (cross-fade included), exposed so
// the offline harness can diff the engine path against the reference
// skinning of the source data, vertex for vertex.
bool PoDebugSkin(const PoDrawState& st, std::vector<float>& world12, std::vector<float>& skin12,
                 std::vector<Vec3>& pos);
// Test support: the skinned figure's true extents for a draw state, composed
// exactly as the renderer composes it (cross-fade included).
bool PoRenderExtents(const PoDrawState& st, float* maxRadial, float* loZ, float* hiZ);

// ---- the render watchdog (the soldier's diagnostic, same law)
// A standing panda is ~1.5 m of figure; no honest pose of his puts a vertex
// outside these bounds. DrawPoModel measures the exact vertex buffer it is
// about to emit against them every frame: past them, the frame is NOT drawn
// and a PO POSE EXPLOSION line with the full pose state goes to the log.
inline constexpr float kPoWatchdogRadius = 4.0f;
inline constexpr float kPoWatchdogLoZ = -1.5f;
inline constexpr float kPoWatchdogHiZ = 3.5f;
// The watchdog's verdict for one draw state.
bool PoPoseExplodes(const PoDrawState& st);

} // namespace mc
