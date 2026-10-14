#include "Input.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include "CPad.h"
#include "RenderWare.h"

#include "Chat.h"
#include "Config.h"
#include "ModCommon.h"

namespace mc {

namespace {
bool gPrev[256] = {};
bool gNow[256] = {};

bool WindowFocused() {
    HWND w = RsGlobal.ps ? RsGlobal.ps->window : nullptr;
    return w && GetForegroundWindow() == w;
}

// ---- Minecraft's actions on this keyboard
int gBinding[ACT_COUNT] = {}; // Windows virtual key of every action
bool gBound = false;

// what a key of the core's list is called on Windows
int VkOf(Key k) {
    if (k >= KEY_0 && k <= KEY_9)
        return '0' + (k - KEY_0);
    if (k >= KEY_A && k <= KEY_Z)
        return 'A' + (k - KEY_A);
    if (k >= KEY_F1 && k <= KEY_F12)
        return VK_F1 + (k - KEY_F1);
    switch (k) {
    case KEY_MOUSE_LEFT: return VK_LBUTTON;
    case KEY_MOUSE_RIGHT: return VK_RBUTTON;
    case KEY_MOUSE_MIDDLE: return VK_MBUTTON;
    case KEY_SPACE: return VK_SPACE;
    case KEY_ESCAPE: return VK_ESCAPE;
    case KEY_ENTER: return VK_RETURN;
    case KEY_SHIFT: return VK_SHIFT;
    case KEY_LSHIFT: return VK_LSHIFT;
    case KEY_CTRL: return VK_CONTROL;
    case KEY_LCTRL: return VK_LCONTROL;
    default: return 0;
    }
}

// Minecraft's default keys, except where MinecraftSA.ini names another one
void BindActions() {
    for (int a = 0; a < ACT_COUNT; ++a)
        gBinding[a] = VkOf(kActions[a].key);
    const struct { Action action; int vk; } fromIni[] = {
        { ACT_INVENTORY, gConfig.keyInventory }, { ACT_DROP, gConfig.keyDrop },     { ACT_GAME_MODE, gConfig.keyGameMode },
        { ACT_PERSPECTIVE, gConfig.keyCamera },  { ACT_SWAP_HANDS, gConfig.keySwap },
    };
    for (const auto& b : fromIni)
        if (b.vk > 0 && b.vk < 256)
            gBinding[b.action] = b.vk;
}

void FillControls() {
    if (!gBound) {
        BindActions();
        gBound = true;
    }
    // the mouse is GTA's own reading of it
    const auto& mouse = CPad::NewMouseControllerState;
    const auto& before = CPad::OldMouseControllerState;
    for (int a = 0; a < ACT_COUNT; ++a) {
        const int vk = gBinding[a];
        bool down, pressed;
        switch (vk) {
        case VK_LBUTTON:
            down = mouse.lmb != 0;
            pressed = mouse.lmb && !before.lmb;
            break;
        case VK_RBUTTON:
            down = mouse.rmb != 0;
            pressed = mouse.rmb && !before.rmb;
            break;
        case VK_MBUTTON:
            down = mouse.mmb != 0;
            pressed = mouse.mmb && !before.mmb;
            break;
        default:
            down = vk > 0 && vk < 256 && gNow[vk];
            pressed = down && !gPrev[vk];
            break;
        }
        gControls.Set((Action)a, down, pressed);
    }
    gControls.scrollUp = mouse.wheelUp != 0;
    gControls.scrollDown = mouse.wheelDown != 0;
}
} // namespace

void PollKeys() {
    bool focus = WindowFocused();
    for (int k = 1; k < 256; ++k) {
        gPrev[k] = gNow[k];
        gNow[k] = focus && (GetAsyncKeyState(k) & 0x8000) != 0;
    }
    FillControls();
}

namespace {
WNDPROC gGameWndProc = nullptr;
std::vector<std::pair<int, uint32_t>> gTyped; // (an editing key, 0) or (0, a character)

LRESULT CALLBACK TextWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CHAR) {
        uint32_t cp = (uint32_t)w;
        if (!IsWindowUnicode(h) && w >= 0x80) {
            const char c = (char)w;
            wchar_t wc = 0;
            if (MultiByteToWideChar(CP_ACP, 0, &c, 1, &wc, 1) == 1)
                cp = wc;
        }
        gTyped.push_back({ 0, cp });
        if (TextInputActive())
            return 0;
    } else if (TextInputActive() && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN)) {
        int k = 0;
        switch (w) {
        case VK_LEFT: k = TK_LEFT; break;
        case VK_RIGHT: k = TK_RIGHT; break;
        case VK_UP: k = TK_UP; break;
        case VK_DOWN: k = TK_DOWN; break;
        case VK_HOME: k = TK_HOME; break;
        case VK_END: k = TK_END; break;
        case VK_DELETE: k = TK_DELETE; break;
        }
        if (k)
            gTyped.push_back({ k, 0 });
        if (w != VK_SNAPSHOT && w != VK_F12)
            return 0; // the game does not see the keys while one types
    }
    return CallWindowProcA(gGameWndProc, h, m, w, l);
}
} // namespace

void FeedTypedText() {
    if (!gGameWndProc && RsGlobal.ps && RsGlobal.ps->window)
        gGameWndProc = (WNDPROC)SetWindowLongPtrA(RsGlobal.ps->window, GWLP_WNDPROC, (LONG_PTR)&TextWndProc);
    std::vector<std::pair<int, uint32_t>> typed;
    typed.swap(gTyped);
    for (const auto& t : typed) {
        if (t.first)
            TypeKey(t.first);
        else
            TypeChar(t.second);
    }
}

void KeepMouseAlive() {
    static DWORD lastFrame = 0;
    static bool wasFocused = true;
    const DWORD now = GetTickCount();
    const bool focused = WindowFocused();
    const bool resumed = (lastFrame != 0 && now - lastFrame > 400) || (focused && !wasFocused);
    lastFrame = now;
    wasFocused = focused;
    psGlobalType* ps = RsGlobal.ps;
    if (!resumed || !ps || !ps->diMouse)
        return;
    auto* dev = static_cast<IDirectInputDevice8A*>(ps->diMouse);
    dev->Unacquire();
    dev->Release();
    ps->diMouse = nullptr; // CPad::UpdateMouse makes it again, exclusive outside the menu
}

bool KeyPressed(int vk) { return vk > 0 && vk < 256 && gNow[vk] && !gPrev[vk]; }
} // namespace mc
