#include "Chat.h"

#include "Commands.h"
#include "GameState.h"
#include "Host.h"
#include "Screens.h"

namespace mc {

std::vector<ChatLine> gChatLines;
ChatInput gChatInput;

namespace {
constexpr size_t kMaxLines = 100;
constexpr size_t kMaxInput = 256;
std::vector<std::string> gSent; // what was sent (the up / down keys bring it back)
size_t gSentPos = 0;
uint32_t gSkipChar = 0;   // the key that opened the chat also types its letter: not into the line
float gSkipTimer = 0.0f;  // (only just after it opened)
std::string gTabBase;     // the line before Tab started completing it
size_t gTabIndex = 0;

void AppendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += (char)cp;
    } else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

uint32_t NextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = (unsigned char)s[i++];
    if (c < 0x80)
        return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i < s.size(); ++k)
        cp = (cp << 6) | ((unsigned char)s[i++] & 0x3F);
    return cp;
}

size_t PrevChar(const std::string& s, size_t i) {
    while (i > 0) {
        --i;
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            break;
    }
    return i;
}

size_t NextChar(const std::string& s, size_t i) {
    if (i < s.size())
        ++i;
    while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80)
        ++i;
    return i;
}

void Edited() {
    gTabIndex = 0;
    gChatInput.blink = 0.0f;
}

void SetLine(const std::string& text) {
    gChatInput.text = text;
    gChatInput.cursor = text.size();
    Edited();
}

void Send() {
    std::string line = gChatInput.text;
    while (!line.empty() && line.back() == ' ')
        line.pop_back();
    CloseScreen();
    if (line.empty())
        return;
    if (gSent.empty() || gSent.back() != line)
        gSent.push_back(line);
    if (line[0] == '/')
        RunCommand(line.substr(1));
    else
        ChatPrint("<" + TheHost().PlayerName() + "> " + line);
}

// Tab: the last word becomes the next thing it can be
void Complete() {
    if (gTabIndex == 0)
        gTabBase = gChatInput.text;
    if (gTabBase.empty() || gTabBase[0] != '/')
        return;
    const std::vector<std::string> options = CompleteCommand(gTabBase.substr(1));
    if (options.empty())
        return;
    const size_t space = gTabBase.find_last_of(' ');
    const std::string head = space == std::string::npos ? "/" : gTabBase.substr(0, space + 1);
    const size_t keep = gTabIndex;
    SetLine(head + options[keep % options.size()]);
    gTabIndex = keep + 1;
}
} // namespace

int GuiTextWidth(const std::string& text) {
    int w = 0;
    for (size_t i = 0; i < text.size();)
        w += FONT_WIDTHS[FontSlotForCodepoint(NextCodepoint(text, i))] + 1;
    return w > 0 ? w - 1 : 0;
}

void ChatPrint(const std::string& text, uint32_t color) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        std::string rest = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        // wrap at the chat's width, between words where it can
        do {
            size_t cut = rest.size();
            if (GuiTextWidth(rest) > kChatWidth) {
                size_t i = 0, lastSpace = 0;
                std::string part;
                while (i < rest.size()) {
                    const size_t next = NextChar(rest, i);
                    if (GuiTextWidth(rest.substr(0, next)) > kChatWidth)
                        break;
                    if (rest[i] == ' ')
                        lastSpace = i;
                    i = next;
                }
                cut = lastSpace > 0 ? lastSpace : std::max<size_t>(i, 1);
            }
            gChatLines.push_back({ rest.substr(0, cut), color, 0.0f });
            rest = cut < rest.size() ? rest.substr(rest[cut] == ' ' ? cut + 1 : cut) : std::string();
        } while (!rest.empty());
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    while (gChatLines.size() > kMaxLines)
        gChatLines.erase(gChatLines.begin());
}

void ChatTick(float dt) {
    for (ChatLine& l : gChatLines)
        l.age += dt;
    gChatInput.blink += dt;
    gSkipTimer -= dt;
    if (gSkipTimer <= 0.0f)
        gSkipChar = 0;
}

void ChatClear() {
    gChatLines.clear();
    gSent.clear();
    gChatInput = ChatInput();
}

void OpenChat(const std::string& text, uint32_t skipChar) {
    OpenScreen(SCREEN_CHAT);
    SetLine(text);
    gSentPos = gSent.size();
    gSkipChar = skipChar;
    gSkipTimer = 0.3f;
}

bool TextInputActive() { return gGame.screen == SCREEN_CHAT || CreativeSearchActive(); }

void TypeChar(uint32_t cp) {
    switch (cp) {
    case 8: TypeKey(TK_BACKSPACE); return;
    case 9: TypeKey(TK_TAB); return;
    case 13: TypeKey(TK_ENTER); return;
    case 27: TypeKey(TK_ESCAPE); return;
    }
    if (cp < 32 || cp == 127)
        return;
    if (!TextInputActive()) {
        if (cp == '/' && gGame.screen == SCREEN_NONE && gGame.deathTime < 0.0f)
            OpenChat("/");
        return;
    }
    if (gSkipChar) {
        const bool same = cp == gSkipChar || cp == gSkipChar + 32 || cp + 32 == gSkipChar;
        gSkipChar = 0;
        if (same)
            return;
    }
    if (gGame.screen != SCREEN_CHAT) {
        CreativeSearchType(cp);
        return;
    }
    if (gChatInput.text.size() >= kMaxInput)
        return;
    std::string add;
    AppendUtf8(add, cp);
    gChatInput.text.insert(gChatInput.cursor, add);
    gChatInput.cursor += add.size();
    Edited();
}

void TypeKey(int key) {
    gSkipChar = 0;
    if (gGame.screen != SCREEN_CHAT) {
        if (CreativeSearchActive())
            CreativeSearchKey(key);
        return;
    }
    std::string& t = gChatInput.text;
    size_t& c = gChatInput.cursor;
    switch (key) {
    case TK_BACKSPACE:
        if (c > 0) {
            const size_t p = PrevChar(t, c);
            t.erase(p, c - p);
            c = p;
        }
        break;
    case TK_DELETE:
        if (c < t.size())
            t.erase(c, NextChar(t, c) - c);
        break;
    case TK_LEFT: c = PrevChar(t, c); break;
    case TK_RIGHT: c = NextChar(t, c); break;
    case TK_HOME: c = 0; break;
    case TK_END: c = t.size(); break;
    case TK_UP:
        if (gSentPos > 0)
            SetLine(gSent[--gSentPos]);
        return;
    case TK_DOWN:
        if (gSentPos < gSent.size()) {
            ++gSentPos;
            SetLine(gSentPos < gSent.size() ? gSent[gSentPos] : std::string());
        }
        return;
    case TK_TAB: Complete(); return;
    case TK_ENTER: Send(); return;
    case TK_ESCAPE: CloseScreen(); return;
    }
    Edited();
}

const std::vector<std::string>& ChatSuggestions() {
    static std::string forText = "\x01";
    static std::vector<std::string> list;
    const std::string& t = gChatInput.text;
    if (t != forText) {
        forText = t;
        list.clear();
        if (!t.empty() && t[0] == '/') {
            list = CompleteCommand(t.substr(1));
            if (list.size() == 1 && t.size() > 1 && t.substr(t.find_last_of(" /") + 1) == list[0])
                list.clear(); // (already typed out)
            if (list.size() > 10)
                list.resize(10);
        }
    }
    return list;
}

} // namespace mc
