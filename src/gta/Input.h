#pragma once
// How GTA SA's keyboard and mouse become Minecraft's actions. What the actions are and which key each one has
// by default is the core's business (src/core/Controls.h); this side reads the real keys and fills gControls.

#include "Controls.h"

namespace mc {

// Reads the keyboard (only while the GTA window has focus) and the mouse, and fills the core's gControls.
// Once per frame, before the game logic.
void PollKeys();
// raw keys, for what belongs to the mod in GTA and not to Minecraft (F6 mod on / off, F8 Steve / CJ, F9 radar, menus)
bool KeyPressed(int vk);
// GTA SA loses its DirectInput mouse after the menu or after the game was in the background (the camera stops
// turning until GTA's display settings make the device anew). Call every game frame: it makes the device anew
// whenever the game comes back.
void KeepMouseAlive();
// what the keyboard typed goes to the core (TypeChar / TypeKey); while the core takes the keyboard (the chat, the
// creative search) GTA does not see the keys. Call every game frame.
void FeedTypedText();

} // namespace mc
