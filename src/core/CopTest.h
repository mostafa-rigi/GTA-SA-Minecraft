#pragma once
// /coptest - the cop force's self-test (the complete update). The user's
// demand, after five fix rounds each discovered by one night of free
// play: ONE session must answer everything. The mod runs a scripted
// sequence around Grove against DECOYS - a plain civilian car driven by
// the game's own autopilot, a plain man standing in the road - while the
// user only watches, plus two short PLAYER phases whose instructions it
// prints in chat (shoot/ram the cop; then bail out of the chased car).
// Every scenario grades OUTCOMES off body reports (positions, speeds,
// health) and ends with a `coptest: PASS/FAIL <name> <numbers>` line in
// the log and the chat; the run ends with `coptest: DONE n/n PASS` and
// cleans up after itself (decoys retired, force stood down, felony page
// restored to what it was). The state machine and the grading live here,
// in the core, so the suite can run the whole machine against the fake
// host; the decoy BODIES are bridge work (GtaCop.cpp).

namespace mc {

// Start the sequence (also the /coptest command's body). False when a
// run is already going.
bool CopTestStart();
// Stop a running sequence now, with the same cleanup as a finished run.
void CopTestAbort();
// True while a run is going (the command toggles: typed again = abort).
bool CopTestRunning();
// Per-tick half, called at the end of CopTick - the test sets the prey
// override before the tick and grades after it, through this call.
void CopTestTick(float dt);
// A world reset (session teardown) forgets any run - without cleanup
// (the world the decoys lived in is already gone).
void CopTestReset();

} // namespace mc
