#pragma once
// Diagnostic support for the boss (the solo Counter-Strike soldier): five live
// bisect toggles the tester flips from the mod chat (/bosstest, Commands.cpp),
// and two phase timings the bridge reports back per frame for the core's
// long-frame logger (Mobs.cpp). Every flag defaults to the shipped behaviour
// and a toggle only ever changes the one thing it names - the diagnostic
// build is the boss build plus these switches and the logging around them.

namespace mc {

struct BossTestState {
    bool proxyOff = false;  // no perception body: the street has nothing of his to shoot at
    bool fxOff = false;     // no muzzle flash and no tracer is produced for his shots
    bool blendOff = false;  // pose changes cut hard: the cross-fade never blends
    bool layersOff = false; // one layer: the whole body composed from the gait sequence
    bool driverOff = false; // legacy feet: the core steers him itself (probe and graph) and
                            // his body is a glued sensor again, instead of the game's own
                            // ped locomotion walking him under the brain's objectives
};
extern BossTestState gBossTest;

// The bridge times its own two boss-adjacent phases (the proxy sync in the
// update, the mob render in the frame) and leaves the numbers here; the
// core's long-frame logger reads them back. They describe the frame that
// just rendered by the time the next tick reads them - the logger says so.
void BossTestNoteProxyMs(float ms);
void BossTestNoteRenderMs(float ms);
float BossTestProxyMs();
float BossTestRenderMs();

} // namespace mc
