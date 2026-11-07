#pragma once
// How the Minecraft sounds get out of the speakers in GTA SA: through BASS (bass.dll ships with CLEO in the game
// folder). Which sound plays when is the core's business (src/core/Audio.h declares PlaySfx and SetLoopSfx; they
// are defined here).

#include <cstddef>

#include "Audio.h"
#include "ModCommon.h"

namespace mc {

// the same with a position in GTA's own vector type
inline void PlaySfx(SoundEvent ev, const CVector* pos, float volume = 1.0f, float pitch = 1.0f) {
    if (!pos) {
        PlaySfx(ev, (const Vec3*)nullptr, volume, pitch);
        return;
    }
    const Vec3 at = *pos;
    PlaySfx(ev, &at, volume, pitch);
}
inline void PlaySfx(SoundEvent ev, std::nullptr_t, float volume = 1.0f, float pitch = 1.0f) {
    PlaySfx(ev, (const Vec3*)nullptr, volume, pitch);
}

void PauseAllSfx(bool pause);
void ShutdownSfx();

// A positional loop with its own channel per id, for the cop force
// (piece 3): SetLoopSfx has exactly one channel and refuses to start
// under CleanSA, but the force lives in CleanSA and every unit needs
// its own siren and engine. Call every frame while it should sound -
// the channel follows `pos` with PlaySfx's own distance/pan law over
// `range` metres - and call with on=false to silence it. Switching
// `ev` restarts that id's loop on the new sample (the engine's two
// layers ride this). Ids are the caller's: GtaCop uses slot*2 for the
// siren and slot*2+1 for the engine.
void SetPosLoop(int id, SoundEvent ev, bool on, const Vec3& pos, float volume, float pitch, float range);
void StopPosLoops();

} // namespace mc
