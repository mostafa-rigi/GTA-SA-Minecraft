#pragma once
// Minecraft's commands, typed into the chat after a slash: /gamemode, /time, /weather, /give, /clear, /tp, /kill,
// /effect, /xp, /summon, /setblock, /fill, /gamerule, /say, /help - and /wanted for the GTA side. Their answers go
// into the chat. Coordinates are the world's: x east, y north, z up; ~ means the player's own.

#include <string>
#include <vector>

namespace mc {

void RunCommand(const std::string& line); // without the slash
// what the last word of a command line can become (Tab completes it)
std::vector<std::string> CompleteCommand(const std::string& line);
// an item or block by its Minecraft name ("diamond", "minecraft:oak_log"); 0 if there is none
int ItemByName(const std::string& name);

} // namespace mc
