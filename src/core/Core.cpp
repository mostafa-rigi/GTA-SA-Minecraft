#include "Core.h"

namespace mc {

// One generator for the whole core (see Core.h for why the CRT's rand() is
// not used). The state starts at 1, as an unseeded CRT rand() would.
static unsigned gRandState = 1;

void McsaSrand(unsigned seed) { gRandState = seed ? seed : 1u; }

int McsaRand() {
    gRandState = gRandState * 214013u + 2531011u;
    return (int)((gRandState >> 16) & 0x7FFFu);
}

float Rand01() { return (McsaRand() % 10000) / 10000.0f; }

} // namespace mc
