#pragma once
// Minecraft's chat: T opens it, / opens it with a slash, Enter sends - a message, or a command (Commands.h). The last
// lines stay at the bottom left for ten seconds; while the chat is open they all show, up to twenty. The keyboard
// writes into it (and into the creative search box) while it is open: the host keeps its keys to itself then.

#include <string>
#include <vector>

#include "Core.h"

namespace mc {

struct ChatLine {
    std::string text;
    uint32_t color = 0xFFFFFFFF;
    float age = 0.0f; // seconds on screen
};
extern std::vector<ChatLine> gChatLines; // the oldest first

struct ChatInput {
    std::string text;   // UTF-8
    size_t cursor = 0;  // byte offset into text
    float blink = 0.0f; // the cursor's clock
};
extern ChatInput gChatInput;

constexpr float kChatLineSeconds = 10.0f; // 200 ticks
constexpr int kChatWidth = 320;           // GUI pixels; longer lines wrap
constexpr uint32_t kChatError = 0xFFFF5555, kChatInfo = 0xFFAAAAAA;

void ChatPrint(const std::string& text, uint32_t color = 0xFFFFFFFF); // wraps it into lines
void ChatTick(float dt);
void ChatClear();
void OpenChat(const std::string& text = "", uint32_t skipChar = 0); // skipChar: the letter of the key that opened it

// what the keyboard typed, in order: characters (Unicode code points) and editing keys
enum TypeKey { TK_BACKSPACE = 1, TK_DELETE, TK_ENTER, TK_ESCAPE, TK_TAB, TK_LEFT, TK_RIGHT, TK_UP, TK_DOWN, TK_HOME, TK_END };
bool TextInputActive(); // the keyboard writes: the host keeps its keys
void TypeChar(uint32_t codepoint);
void TypeKey(int key);

// GUI pixels of a text in Minecraft's font (as the host draws it)
int GuiTextWidth(const std::string& text);
// the suggestions under what is typed (command names, item names...), shown above the chat line
const std::vector<std::string>& ChatSuggestions();

} // namespace mc
