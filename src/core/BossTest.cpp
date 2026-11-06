#include "BossTest.h"

namespace mc {

BossTestState gBossTest;

namespace {
float gProxyMs = 0.0f;
float gRenderMs = 0.0f;
} // namespace

void BossTestNoteProxyMs(float ms) { gProxyMs = ms; }
void BossTestNoteRenderMs(float ms) { gRenderMs = ms; }
float BossTestProxyMs() { return gProxyMs; }
float BossTestRenderMs() { return gRenderMs; }

} // namespace mc
