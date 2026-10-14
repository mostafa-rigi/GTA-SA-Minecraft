#include "Screens.h"

#include "Chat.h"

#include <algorithm>
#include <utility>

#include "Audio.h"
#include "GameState.h"
#include "Inventory.h"
#include "Items.h"

namespace mc {

std::vector<UiSlot> gSlots;
int gWinW = 176, gWinH = 166;

namespace {
const int kTabTitles[TAB_COUNT] = { STR_TAB_BUILDING, STR_TAB_COLORED, STR_TAB_NATURAL, STR_TAB_FUNCTIONAL, STR_TAB_TOOLS,
                                    STR_TAB_COMBAT, STR_TAB_FOOD, STR_TAB_INGREDIENTS, STR_TAB_REDSTONE,
                                    STR_TAB_SPAWN_EGGS, STR_TAB_INVENTORY, STR_TAB_SEARCH };
std::string gSearch; // the search tab's text
} // namespace
const char* TabName(int tab) { return LangStr(kTabTitles[std::clamp(tab, 0, TAB_COUNT - 1)]); }
const uint16_t kTabIcons[TAB_COUNT] = { ID_BRICKS, ID_CYAN_WOOL, ID_GRASS_BLOCK, ID_CRAFTING_TABLE, ID_DIAMOND_PICKAXE,
                                        ID_DIAMOND_SWORD, ID_GOLDEN_APPLE, ID_IRON_INGOT, ID_REDSTONE, ID_PIG_SPAWN_EGG,
                                        ID_CHEST, ID_SPYGLASS };
// (Minecraft: building, coloured, natural, functional, redstone on top; tools, combat, food, ingredients, spawn eggs
// below; the search at the top right, the survival inventory at the bottom right)
const TabPos kTabPos[TAB_COUNT] = { { true, 0 },  { true, 1 },  { true, 2 },  { true, 3 },  { false, 0 }, { false, 1 },
                                    { false, 2 }, { false, 3 }, { true, 4 },  { false, 4 }, { false, 6 }, { true, 6 } };

namespace {
ItemStack gResult2, gResult3, gTrash;
ItemStack gPalette[45];

ChestState* OpenChest() { return &gWorld.chests[gGame.openPos]; }

void AddPlayerSlots(int x0, int mainY, int hotbarY) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 9; ++c)
            gSlots.push_back({ &gInv.slots[9 + r * 9 + c], x0 + c * 18, mainY + r * 18, SK_NORMAL, G_MAIN, 9 + r * 9 + c });
    for (int c = 0; c < 9; ++c)
        gSlots.push_back({ &gInv.slots[c], x0 + c * 18, hotbarY, SK_NORMAL, G_HOTBAR, c });
}

void MoveInto(ItemStack& src, const std::vector<ItemStack*>& dests) {
    if (src.Empty())
        return;
    int maxStack = MaxStack(src.id);
    for (ItemStack* d : dests) {
        if (src.count == 0)
            break;
        if (!d->Empty() && d->SameItem(src) && d->count < maxStack) {
            int mv = std::min<int>(src.count, maxStack - d->count);
            d->count += (uint8_t)mv;
            src.count -= (uint8_t)mv;
        }
    }
    for (ItemStack* d : dests) {
        if (src.count == 0)
            break;
        if (d->Empty()) {
            int mv = std::min<int>(src.count, maxStack);
            *d = src;
            d->count = (uint8_t)mv;
            src.count -= (uint8_t)mv;
        }
    }
    if (src.count == 0)
        src.Clear();
}

std::vector<ItemStack*> PlayerDests(bool hotbarFirst, bool includeHotbar = true, bool includeMain = true) {
    std::vector<ItemStack*> v;
    if (hotbarFirst && includeHotbar)
        for (int i = 0; i < 9; ++i)
            v.push_back(&gInv.slots[i]);
    if (includeMain)
        for (int i = 9; i < INV_SIZE; ++i)
            v.push_back(&gInv.slots[i]);
    if (!hotbarFirst && includeHotbar)
        for (int i = 8; i >= 0; --i)
            v.push_back(&gInv.slots[i]);
    return v;
}

// Minecraft's moveItemStackTo over the player's slots: out of a chest or a result slot it goes backwards (the hotbar
// from its right end, then the inventory from the bottom), out of anything else forwards (the inventory from the top,
// then the hotbar from the left)
std::vector<ItemStack*> PlayerDestsFrom(bool backwards) {
    std::vector<ItemStack*> v;
    if (backwards) {
        for (int i = 8; i >= 0; --i)
            v.push_back(&gInv.slots[i]);
        for (int i = INV_SIZE - 1; i >= 9; --i)
            v.push_back(&gInv.slots[i]);
    } else {
        for (int i = 9; i < INV_SIZE; ++i)
            v.push_back(&gInv.slots[i]);
        for (int i = 0; i < 9; ++i)
            v.push_back(&gInv.slots[i]);
    }
    return v;
}

bool ArmorFits(int armorIndex, uint16_t id) { return Item(id).armorSlot == armorIndex + 1; }

void QuickMove(UiSlot& sl) {
    ItemStack& st = *sl.st;
    if (st.Empty())
        return;
    if (sl.group == G_HOTBAR || sl.group == G_MAIN) {
        // armour goes on first
        int as = Item(st.id).armorSlot;
        if (as && gInv.armor[as - 1].Empty() && gGame.screen != SCREEN_CHEST) {
            bool elytra = st.id == ID_ELYTRA;
            gInv.armor[as - 1] = st;
            st.Clear();
            PlaySfx(elytra ? SND_EQUIP_ELYTRA : SND_EQUIP_GENERIC);
            return;
        }
        if (gGame.screen == SCREEN_CHEST) {
            std::vector<ItemStack*> d;
            for (auto& c : OpenChest()->slots)
                d.push_back(&c);
            MoveInto(st, d);
            return;
        }
        if (gGame.screen == SCREEN_FURNACE) {
            FurnaceState* f = OpenFurnace();
            int kind = CookKindForBlock(gWorld.GetBlock(gGame.openPos.x, gGame.openPos.y, gGame.openPos.z));
            if (CookResult(kind, st.id)) {
                MoveInto(st, { &f->input });
                return;
            }
            if (FuelTicks(st.id) > 0) {
                MoveInto(st, { &f->fuel });
                return;
            }
        }
        if (sl.group == G_HOTBAR)
            MoveInto(st, PlayerDests(false, false, true));
        else
            MoveInto(st, PlayerDests(true, true, false));
        return;
    }
    MoveInto(st, PlayerDestsFrom(sl.group == G_CONTAINER));
}

bool CanTakeResult(const ItemStack& result) {
    ItemStack& c = gInv.cursor;
    if (c.Empty())
        return true;
    return c.SameItem(result) && c.count + result.count <= MaxStack(c.id);
}

void TakeCraftResult(UiSlot& sl, bool shift) {
    ItemStack* grid = sl.kind == SK_RESULT2 ? gInv.craft : gInv.craft3;
    int w = sl.kind == SK_RESULT2 ? 2 : 3;
    for (int guard = 0; guard < 64; ++guard) {
        ItemStack res = MatchRecipe(grid, w, w);
        if (res.Empty())
            return;
        if (shift) {
            int free = 0;
            for (auto& s : gInv.slots) {
                if (s.Empty())
                    free += MaxStack(res.id);
                else if (s.SameItem(res))
                    free += MaxStack(res.id) - s.count;
            }
            if (free < res.count)
                return;
            ItemStack made = res;
            MoveInto(made, PlayerDestsFrom(true));
            ConsumeIngredients(grid, w * w);
            continue;
        }
        if (!CanTakeResult(res))
            return;
        if (gInv.cursor.Empty())
            gInv.cursor = res;
        else
            gInv.cursor.count += res.count;
        ConsumeIngredients(grid, w * w);
        return;
    }
}

void ClickSlot(UiSlot& sl, int button, bool shift) {
    ItemStack& a = *sl.st;
    ItemStack& c = gInv.cursor;

    if (sl.kind == SK_RESULT2 || sl.kind == SK_RESULT3) {
        TakeCraftResult(sl, shift);
        return;
    }
    if (sl.kind == SK_TRASH) {
        if (shift) {
            for (auto& s : gInv.slots)
                s.Clear();
        }
        c.Clear();
        return;
    }
    if (sl.kind == SK_PALETTE) {
        if (!c.Empty()) {
            c.Clear();
            return;
        }
        if (a.Empty())
            return;
        ItemStack give = a;
        give.count = (uint8_t)(button == 0 ? MaxStack(a.id) : 1);
        if (shift)
            gInv.Add(give);
        else
            c = give;
        return;
    }
    if (shift) {
        QuickMove(sl);
        return;
    }
    if (sl.kind == SK_OUTPUT) {
        if (a.Empty())
            return;
        if (c.Empty()) {
            c = a;
            a.Clear();
        } else if (c.SameItem(a)) {
            int mv = std::min<int>(a.count, MaxStack(c.id) - c.count);
            c.count += (uint8_t)mv;
            a.count -= (uint8_t)mv;
            if (!a.count)
                a.Clear();
        }
        return;
    }
    if (sl.kind == SK_FUEL && !c.Empty() && FuelTicks(c.id) == 0)
        return;
    if (sl.kind == SK_ARMOR) {
        if (!c.Empty() && !ArmorFits(sl.index, c.id))
            return;
        if (!c.Empty() && c.count > 1)
            return;
        std::swap(a, c);
        if (!a.Empty())
            PlaySfx(a.id == ID_ELYTRA ? SND_EQUIP_ELYTRA : SND_EQUIP_GENERIC);
        return;
    }

    if (button == 0) {
        if (c.Empty()) {
            c = a;
            a.Clear();
        } else if (a.Empty()) {
            a = c;
            c.Clear();
        } else if (a.SameItem(c)) {
            int mv = std::min<int>(c.count, MaxStack(a.id) - a.count);
            a.count += (uint8_t)mv;
            c.count -= (uint8_t)mv;
            if (!c.count)
                c.Clear();
        } else {
            std::swap(a, c);
        }
    } else {
        if (c.Empty()) {
            if (!a.Empty()) {
                int take = (a.count + 1) / 2;
                c = a;
                c.count = (uint8_t)take;
                a.count -= (uint8_t)take;
                if (!a.count)
                    a.Clear();
            }
        } else if (a.Empty()) {
            a = c;
            a.count = 1;
            if (--c.count == 0)
                c.Clear();
        } else if (a.SameItem(c)) {
            if (a.count < MaxStack(a.id)) {
                a.count++;
                if (--c.count == 0)
                    c.Clear();
            }
        } else {
            std::swap(a, c);
        }
    }
}
} // namespace

FurnaceState* OpenFurnace() { return &gWorld.furnaces[gGame.openPos]; }

bool CreativeInventoryTab() { return gGame.screen == SCREEN_CREATIVE && gGame.creativeTab == CAT_COUNT; }

namespace {
// for searching: lower case, Turkish letters too (I, İ and ı all as i)
std::string Fold(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        const unsigned char n = i + 1 < s.size() ? (unsigned char)s[i + 1] : 0;
        if (c >= 'A' && c <= 'Z') {
            out += (char)(c == 'I' ? 'i' : c - 'A' + 'a');
        } else if (c == 0xC4 && (n == 0xB0 || n == 0xB1)) {
            out += 'i';
            ++i;
        } else if ((c == 0xC3 && (n == 0x87 || n == 0x96 || n == 0x9C)) || (c == 0xC4 && n == 0x9E) || (c == 0xC5 && n == 0x9E)) {
            out += (char)c;
            out += (char)(c == 0xC3 ? n + 0x20 : n + 1);
            ++i;
        } else {
            out += (char)c;
        }
    }
    return out;
}
} // namespace

bool CreativeSearchActive() { return gGame.screen == SCREEN_CREATIVE && gGame.creativeTab == TAB_SEARCH; }
const std::string& CreativeSearchText() { return gSearch; }

void CreativeSearchType(uint32_t cp) {
    if (gSearch.size() >= 50)
        return;
    if (cp < 0x80) {
        gSearch += (char)cp;
    } else if (cp < 0x800) {
        gSearch += (char)(0xC0 | (cp >> 6));
        gSearch += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        gSearch += (char)(0xE0 | (cp >> 12));
        gSearch += (char)(0x80 | ((cp >> 6) & 0x3F));
        gSearch += (char)(0x80 | (cp & 0x3F));
    }
    gGame.creativeScroll = 0;
}

void CreativeSearchKey(int key) {
    if (key == TK_BACKSPACE && !gSearch.empty()) {
        size_t i = gSearch.size() - 1;
        while (i > 0 && ((unsigned char)gSearch[i] & 0xC0) == 0x80)
            --i;
        gSearch.erase(i);
        gGame.creativeScroll = 0;
    } else if (key == TK_DELETE) {
        gSearch.clear(); // (Minecraft: a right click on the box)
        gGame.creativeScroll = 0;
    } else if (key == TK_ESCAPE) {
        CloseScreen();
    }
}

const std::vector<uint16_t>& PaletteItems() {
    if (gGame.creativeTab != TAB_SEARCH)
        return CreativeItems(std::min(gGame.creativeTab, (int)CAT_COUNT - 1));
    static std::string forText = "\x01";
    static int forLanguage = -1;
    static std::vector<uint16_t> list;
    if (forText != gSearch || forLanguage != gLanguage) {
        forText = gSearch;
        forLanguage = gLanguage;
        list.clear();
        const std::string q = Fold(gSearch);
        for (int c = 0; c < CAT_COUNT; ++c)
            for (uint16_t id : CreativeItems(c)) {
                std::string key = Item(id).key;
                for (char& ch : key)
                    if (ch == '_')
                        ch = ' ';
                if (q.empty() || Fold(ItemName(id)).find(q) != std::string::npos || key.find(q) != std::string::npos)
                    list.push_back(id);
            }
    }
    return list;
}

int PaletteRows() { return ((int)PaletteItems().size() + 8) / 9; }

void BuildSlots() {
    gSlots.clear();
    switch (gGame.screen) {
    case SCREEN_INVENTORY:
        gWinW = 176; gWinH = 166;
        AddPlayerSlots(8, 84, 142);
        for (int i = 0; i < 4; ++i)
            gSlots.push_back({ &gInv.armor[i], 8, 8 + i * 18, SK_ARMOR, G_ARMOR, i });
        gSlots.push_back({ &gInv.offhand, 77, 62, SK_OFFHAND, G_ARMOR, 0 });
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 2; ++c)
                gSlots.push_back({ &gInv.craft[r * 2 + c], 98 + c * 18, 18 + r * 18, SK_NORMAL, G_GRID, r * 2 + c });
        gResult2 = MatchRecipe(gInv.craft, 2, 2);
        gSlots.push_back({ &gResult2, 154, 28, SK_RESULT2, G_CONTAINER, 0 });
        break;
    case SCREEN_CRAFTING:
        gWinW = 176; gWinH = 166;
        AddPlayerSlots(8, 84, 142);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                gSlots.push_back({ &gInv.craft3[r * 3 + c], 30 + c * 18, 17 + r * 18, SK_NORMAL, G_GRID, r * 3 + c });
        gResult3 = MatchRecipe(gInv.craft3, 3, 3);
        gSlots.push_back({ &gResult3, 124, 35, SK_RESULT3, G_CONTAINER, 0 });
        break;
    case SCREEN_FURNACE: {
        gWinW = 176; gWinH = 166;
        AddPlayerSlots(8, 84, 142);
        FurnaceState* f = OpenFurnace();
        gSlots.push_back({ &f->input, 56, 17, SK_NORMAL, G_FURNACE_IN, 0 });
        gSlots.push_back({ &f->fuel, 56, 53, SK_FUEL, G_FURNACE_FUEL, 0 });
        gSlots.push_back({ &f->output, 116, 35, SK_OUTPUT, G_CONTAINER, 0 });
        break;
    }
    case SCREEN_CHEST: {
        gWinW = 176; gWinH = 167;
        AddPlayerSlots(8, 85, 143);
        ChestState* c = OpenChest();
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 9; ++col)
                gSlots.push_back({ &c->slots[r * 9 + col], 8 + col * 18, 18 + r * 18, SK_NORMAL, G_CONTAINER, r * 9 + col });
        break;
    }
    case SCREEN_CREATIVE: {
        gWinW = 195; gWinH = 136;
        if (CreativeInventoryTab()) {
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 9; ++c)
                    gSlots.push_back({ &gInv.slots[9 + r * 9 + c], 9 + c * 18, 54 + r * 18, SK_NORMAL, G_MAIN, 9 + r * 9 + c });
            const int ax[4] = { 54, 54, 108, 108 }, ay[4] = { 6, 33, 6, 33 };
            for (int i = 0; i < 4; ++i)
                gSlots.push_back({ &gInv.armor[i], ax[i], ay[i], SK_ARMOR, G_ARMOR, i });
            gSlots.push_back({ &gInv.offhand, 35, 20, SK_OFFHAND, G_ARMOR, 0 });
            gTrash.Clear();
            gSlots.push_back({ &gTrash, 173, 112, SK_TRASH, G_CONTAINER, 0 });
        } else {
            const auto& items = PaletteItems();
            int maxScroll = std::max(0, PaletteRows() - 5);
            gGame.creativeScroll = std::clamp(gGame.creativeScroll, 0, maxScroll);
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 9; ++c) {
                    int i = (gGame.creativeScroll + r) * 9 + c;
                    ItemStack& p = gPalette[r * 9 + c];
                    p.Clear();
                    if (i < (int)items.size()) {
                        p.id = items[i];
                        p.count = 1;
                    }
                    gSlots.push_back({ &p, 9 + c * 18, 18 + r * 18, SK_PALETTE, G_CONTAINER, r * 9 + c });
                }
        }
        for (int c = 0; c < 9; ++c)
            gSlots.push_back({ &gInv.slots[c], 9 + c * 18, 112, SK_NORMAL, G_HOTBAR, c });
        break;
    }
    default:
        break;
    }
}

UiSlot* SlotAt(float gx, float gy) {
    for (auto& sl : gSlots)
        if (gx >= sl.gx - 1 && gx < sl.gx + 17 && gy >= sl.gy - 1 && gy < sl.gy + 17)
            return &sl;
    return nullptr;
}

void TabBox(int tab, float& x, float& y, float& w, float& h) {
    const TabPos& p = kTabPos[tab];
    x = (float)(p.col == 6 ? gWinW - 26 : p.col * 27); // the last column sits at the right edge
    y = (float)(p.top ? -28 : gWinH - 4);
    w = 26.0f;
    h = 32.0f;
}

int TabAt(float gx, float gy) {
    if (gGame.screen != SCREEN_CREATIVE)
        return -1;
    for (int t = 0; t < TAB_COUNT; ++t) {
        float x, y, w, h;
        TabBox(t, x, y, w, h);
        if (gx >= x && gx < x + w && gy >= y && gy < y + h)
            return t;
    }
    return -1;
}

bool OverWindow(float gx, float gy) { return gx >= 0.0f && gy >= 0.0f && gx < gWinW && gy < gWinH; }


const char* ScreenTitle() {
    switch (gGame.screen) {
    case SCREEN_INVENTORY: return LangStr(STR_CRAFTING);
    case SCREEN_CRAFTING: return BlockName(ID_CRAFTING_TABLE);
    case SCREEN_FURNACE: return BlockName(gWorld.GetBlock(gGame.openPos.x, gGame.openPos.y, gGame.openPos.z));
    case SCREEN_CHEST: return BlockName(ID_CHEST);
    case SCREEN_CREATIVE: return TabName(gGame.creativeTab);
    }
    return "";
}

void ScreenClick(float gx, float gy, bool left, bool right, bool shift) {
    if (!left && !right)
        return;
    int tab = TabAt(gx, gy);
    if (tab >= 0 && left) {
        gGame.creativeTab = tab;
        gGame.creativeScroll = 0;
        PlaySfx(SND_CLICK);
    } else if (UiSlot* sl = SlotAt(gx, gy)) {
        ClickSlot(*sl, left ? 0 : 1, shift);
        gWorld.dirty = true;
    } else if (!OverWindow(gx, gy) && !gInv.cursor.Empty()) {
        ItemStack drop = gInv.cursor;
        if (right) {
            drop.count = 1;
            if (--gInv.cursor.count == 0)
                gInv.cursor.Clear();
        } else {
            gInv.cursor.Clear();
        }
        DropStackAtPlayer(drop, true);
    }
    BuildSlots();
}

void ScreenScroll(int rows) {
    if (gGame.screen == SCREEN_CREATIVE && !CreativeInventoryTab())
        gGame.creativeScroll += rows;
}

} // namespace mc
