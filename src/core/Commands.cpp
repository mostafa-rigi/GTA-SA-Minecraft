#include "Commands.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#include "Chat.h"
#include "GameState.h"
#include "Host.h"
#include "Inventory.h"
#include "Items.h"
#include "Mobs.h"
#include "Survival.h"
#include "World.h"

namespace mc {

namespace {
using Args = std::vector<std::string>;

// (the Turkish texts put the words in another order: %1$s positional arguments)
std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
#ifdef _MSC_VER
    _vsprintf_p(buf, sizeof(buf), fmt, ap);
#else
    vsnprintf(buf, sizeof(buf), fmt, ap);
#endif
    va_end(ap);
    return buf;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    return s;
}

std::string Player() { return TheHost().PlayerName(); }

void Ok(const std::string& s) { ChatPrint(s); }
void Fail(const std::string& s) { ChatPrint(s, kChatError); }

bool ParseInt(const std::string& s, int* v) {
    if (s.empty())
        return false;
    char* end = nullptr;
    const long n = std::strtol(s.c_str(), &end, 10);
    if (*end)
        return false;
    *v = (int)n;
    return true;
}

// a coordinate: a number, or ~ / ~n from the player's own
bool ParseCoord(const std::string& s, float own, float* v) {
    if (s.empty())
        return false;
    float base = 0.0f;
    std::string n = s;
    if (s[0] == '~') {
        base = own;
        n = s.substr(1);
        if (n.empty()) {
            *v = base;
            return true;
        }
    }
    char* end = nullptr;
    const float f = std::strtof(n.c_str(), &end);
    if (*end)
        return false;
    *v = base + f;
    return true;
}

bool PlayerFeet(Vec3* feet) {
    Vec3 p;
    if (!TheHost().PlayerPos(&p))
        return false;
    *feet = p - Vec3(0, 0, 1.0f); // (the host's player position is his middle)
    return true;
}

bool ParsePos(const Args& a, size_t i, Vec3* out) {
    Vec3 me;
    PlayerFeet(&me);
    return a.size() >= i + 3 && ParseCoord(a[i], me.x, &out->x) && ParseCoord(a[i + 1], me.y, &out->y) &&
           ParseCoord(a[i + 2], me.z, &out->z);
}

// "@s", "@p" or the player's name in front of what a command acts on
size_t SkipTarget(const Args& a, size_t i) {
    if (i < a.size() && (a[i][0] == '@' || Lower(a[i]) == Lower(Player())))
        return i + 1;
    return i;
}

const char* const kMobNames[MOB_KIND_COUNT] = { "cow", "pig", "sheep", "chicken", "creeper", "warden" };
const char* const kMobNamesTr[MOB_KIND_COUNT] = { "İnek", "Domuz", "Koyun", "Tavuk", "Creeper", "Warden" };
const char* const kMobNamesEn[MOB_KIND_COUNT] = { "Cow", "Pig", "Sheep", "Chicken", "Creeper", "Warden" };
const char* const kEffectKeys[EFFECT_COUNT] = { "absorption", "regeneration", "fire_resistance", "resistance", "hunger", "poison" };
const char* const kGameRules[] = { "keepInventory", "doMobSpawning", "mobGriefing" };

std::string StripNs(const std::string& s) { return s.rfind("minecraft:", 0) == 0 ? s.substr(10) : s; }

int EffectByName(const std::string& s) {
    for (int e = 0; e < EFFECT_COUNT; ++e)
        if (StripNs(Lower(s)) == kEffectKeys[e])
            return e;
    return -1;
}

int MobByName(const std::string& s) {
    for (int m = 0; m < MOB_KIND_COUNT; ++m)
        if (StripNs(Lower(s)) == kMobNames[m])
            return m;
    return -1;
}

bool* GameRule(const std::string& name) {
    if (name == "keepInventory")
        return &gRules.keepInventory;
    if (name == "doMobSpawning")
        return &gRules.animals;
    if (name == "mobGriefing")
        return &gRules.explosionsBreakBlocks;
    return nullptr;
}

// ---------------------------------------------------------------- the commands
void CmdGamemode(const Args& a) {
    if (a.size() < 2)
        return Fail("/gamemode <survival|creative>");
    const std::string m = Lower(a[1]);
    int mode = -1;
    if (m == "survival" || m == "s" || m == "0")
        mode = MODE_SURVIVAL;
    else if (m == "creative" || m == "c" || m == "1")
        mode = MODE_CREATIVE;
    if (mode < 0)
        return Fail(Fmt(Tr("Unknown game mode: %s", "Bilinmeyen oyun modu: %s"), a[1].c_str()));
    gGame.gameMode = mode;
    gWorld.dirty = true;
    std::string msg = LangStr(STR_GAMEMODE_SET);
    const size_t at = msg.find("%s");
    if (at != std::string::npos)
        msg.replace(at, 2, LangStr(mode == MODE_CREATIVE ? STR_CREATIVE : STR_SURVIVAL));
    Ok(msg);
}

// Minecraft's day: tick 0 is 06:00, 1000 ticks an hour
float TicksToHours(int ticks) {
    const float h = std::fmod(6.0f + ticks / 1000.0f, 24.0f);
    return h < 0.0f ? h + 24.0f : h;
}
int HoursToTicks(float hours) { return (int)(std::fmod(hours - 6.0f + 24.0f, 24.0f) * 1000.0f + 0.5f); }

void CmdTime(const Args& a) {
    Host& h = TheHost();
    if (a.size() >= 2 && Lower(a[1]) == "query") {
        return Ok(Fmt(Tr("The time is %d", "Saat: %d"), HoursToTicks(h.ClockHours())));
    }
    if (a.size() < 3 || (Lower(a[1]) != "set" && Lower(a[1]) != "add"))
        return Fail("/time <set|add|query> <day|noon|night|midnight|ticks>");
    int ticks = 0;
    const std::string v = Lower(a[2]);
    if (v == "day")
        ticks = 1000;
    else if (v == "noon")
        ticks = 6000;
    else if (v == "night")
        ticks = 13000;
    else if (v == "midnight")
        ticks = 18000;
    else if (!ParseInt(v, &ticks))
        return Fail(Fmt(Tr("Invalid time: %s", "Geçersiz zaman: %s"), a[2].c_str()));
    const float now = h.ClockHours();
    if (Lower(a[1]) == "add")
        ticks += HoursToTicks(now);
    const float hours = TicksToHours(ticks);
    h.SetClock(hours, hours < now || ticks >= 24000);
    Ok(Fmt(Tr("Set the time to %d", "Saat %d olarak ayarlandı"), ((ticks % 24000) + 24000) % 24000));
}

void CmdWeather(const Args& a) {
    const std::string w = a.size() >= 2 ? Lower(a[1]) : "";
    const int kind = w == "clear" ? 0 : w == "rain" ? 1 : w == "thunder" ? 2 : -1;
    if (kind < 0)
        return Fail("/weather <clear|rain|thunder>");
    TheHost().SetWeather(kind);
    static const char* const en[3] = { "Set the weather to clear", "Set the weather to rain", "Set the weather to rain & thunder" };
    static const char* const tr[3] = { "Hava açık yapıldı", "Hava yağmurlu yapıldı", "Hava fırtınalı yapıldı" };
    Ok(Tr(en[kind], tr[kind]));
}

void CmdGive(const Args& a) {
    const size_t i = SkipTarget(a, 1);
    if (a.size() <= i)
        return Fail("/give <item> [count]");
    const int id = ItemByName(a[i]);
    if (!id)
        return Fail(Fmt(Tr("Unknown item: %s", "Bilinmeyen eşya: %s"), a[i].c_str()));
    int count = 1;
    if (a.size() > i + 1 && (!ParseInt(a[i + 1], &count) || count < 1 || count > 6400))
        return Fail(Fmt(Tr("Invalid count: %s", "Geçersiz sayı: %s"), a[i + 1].c_str()));
    const int maxStack = std::max<int>(1, Item(id).maxStack);
    for (int left = count; left > 0;) {
        ItemStack s;
        s.id = (uint16_t)id;
        s.count = (uint8_t)std::min(left, maxStack);
        left -= s.count;
        const int notFit = gInv.Add(s);
        if (notFit > 0) {
            s.count = (uint8_t)notFit;
            DropStackAtPlayer(s, false);
        }
    }
    gWorld.dirty = true;
    Ok(Fmt(Tr("Gave %d [%s] to %s", "%3$s oyuncusuna %1$d [%2$s] verildi"), count, ItemName(id), Player().c_str()));
}

void CmdClear(const Args& a) {
    const size_t i = SkipTarget(a, 1);
    const int only = a.size() > i ? ItemByName(a[i]) : 0;
    if (a.size() > i && !only)
        return Fail(Fmt(Tr("Unknown item: %s", "Bilinmeyen eşya: %s"), a[i].c_str()));
    int removed = 0;
    auto wipe = [&](ItemStack& s) {
        if (!s.Empty() && (!only || s.id == only)) {
            removed += s.count;
            s.Clear();
        }
    };
    for (auto& s : gInv.slots)
        wipe(s);
    for (auto& s : gInv.armor)
        wipe(s);
    wipe(gInv.offhand);
    gWorld.dirty = true;
    if (!removed)
        return Fail(Fmt(Tr("No items were found on player %s", "%s oyuncusunda eşya bulunamadı"), Player().c_str()));
    Ok(Fmt(Tr("Removed %d item(s) from player %s", "%2$s oyuncusundan %1$d eşya silindi"), removed, Player().c_str()));
}

void CmdKill(const Args&) {
    TheHost().KillPlayer();
    Ok(Fmt(Tr("Killed %s", "%s öldürüldü"), Player().c_str()));
}

void CmdTp(const Args& a) {
    Vec3 to;
    if (!ParsePos(a, SkipTarget(a, 1), &to))
        return Fail("/tp <x> <y> <z>   (z = " + std::string(Tr("height", "yükseklik")) + ")");
    TheHost().MovePlayer(to + Vec3(0, 0, 1.0f));
    Ok(Fmt(Tr("Teleported %s to %.1f, %.1f, %.1f", "%s, %.1f, %.1f, %.1f konumuna ışınlandı"), Player().c_str(), to.x, to.y, to.z));
}

void CmdEffect(const Args& a) {
    const std::string sub = a.size() >= 2 ? Lower(a[1]) : "";
    if (sub == "clear") {
        ClearEffects();
        return Ok(Fmt(Tr("Removed every effect from %s", "%s oyuncusunun tüm etkileri kaldırıldı"), Player().c_str()));
    }
    const size_t i = SkipTarget(a, 2);
    if (sub != "give" || a.size() <= i)
        return Fail("/effect <give|clear> <effect> [seconds] [amplifier]");
    const int e = EffectByName(a[i]);
    if (e < 0)
        return Fail(Fmt(Tr("Unknown effect: %s", "Bilinmeyen etki: %s"), a[i].c_str()));
    int seconds = 30, amp = 0;
    if (a.size() > i + 1 && (!ParseInt(a[i + 1], &seconds) || seconds < 1))
        return Fail(Fmt(Tr("Invalid number: %s", "Geçersiz sayı: %s"), a[i + 1].c_str()));
    if (a.size() > i + 2 && (!ParseInt(a[i + 2], &amp) || amp < 0 || amp > 4))
        return Fail(Fmt(Tr("Invalid number: %s", "Geçersiz sayı: %s"), a[i + 2].c_str()));
    AddEffect(e, (float)seconds, amp);
    Ok(Fmt(Tr("Applied effect %s to %s", "%s etkisi %s oyuncusuna verildi"), EffectName(e), Player().c_str()));
}

void CmdXp(const Args& a) {
    const std::string sub = a.size() >= 2 ? Lower(a[1]) : "";
    const size_t i = SkipTarget(a, 2);
    int n = 0;
    if ((sub != "add" && sub != "give") || a.size() <= i || !ParseInt(a[i], &n) || n < 0)
        return Fail("/xp add <amount> [points|levels]");
    if (a.size() > i + 1 && Lower(a[i + 1]) == "levels") {
        gSurvival.xpLevel += n;
        return Ok(Fmt(Tr("Gave %d experience levels to %s", "%2$s oyuncusuna %1$d tecrübe seviyesi verildi"), n, Player().c_str()));
    }
    AddXp(n);
    Ok(Fmt(Tr("Gave %d experience points to %s", "%2$s oyuncusuna %1$d tecrübe puanı verildi"), n, Player().c_str()));
}

void CmdSummon(const Args& a) {
    if (a.size() < 2)
        return Fail("/summon <cow|pig|sheep|chicken|creeper|warden> [x y z]");
    const int kind = MobByName(a[1]);
    if (kind < 0)
        return Fail(Fmt(Tr("Unknown entity: %s", "Bilinmeyen varlık: %s"), a[1].c_str()));
    Vec3 at;
    if (a.size() >= 5) {
        if (!ParsePos(a, 2, &at))
            return Fail("/summon <entity> [x y z]");
    } else {
        Vec3 feet;
        PlayerFeet(&feet);
        Vec3 d(gGame.lookDir.x, gGame.lookDir.y, 0.0f);
        const float l = d.Length();
        at = feet + (l > 0.01f ? d * (2.5f / l) : Vec3(0, 2.5f, 0));
    }
    if (SpawnMob(kind, at, true) < 0)
        return Fail(Tr("Unable to summon entity", "Varlık çağrılamadı"));
    Ok(Fmt(Tr("Summoned new %s", "Yeni %s çağrıldı"), Tr(kMobNamesEn[kind], kMobNamesTr[kind])));
}

bool BlockArg(const std::string& s, int* id) {
    *id = StripNs(Lower(s)) == "air" ? ID_AIR : ItemByName(s);
    return *id == ID_AIR || (IsBlockItem(*id) && Block(*id).shape != SHAPE_BED_HEAD);
}

void CmdSetblock(const Args& a) {
    Vec3 p;
    int id = 0;
    if (a.size() < 5 || !ParsePos(a, 1, &p))
        return Fail("/setblock <x> <y> <z> <block>");
    if (!BlockArg(a[4], &id))
        return Fail(Fmt(Tr("Unknown block: %s", "Bilinmeyen blok: %s"), a[4].c_str()));
    const int x = FloorI(p.x), y = FloorI(p.y), z = FloorI(p.z);
    gWorld.Set(x, y, z, MakeVox(id));
    Ok(Fmt(Tr("Changed the block at %d, %d, %d", "%d, %d, %d konumundaki blok değiştirildi"), x, y, z));
}

void CmdFill(const Args& a) {
    Vec3 p0, p1;
    int id = 0;
    if (a.size() < 8 || !ParsePos(a, 1, &p0) || !ParsePos(a, 4, &p1))
        return Fail("/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>");
    if (!BlockArg(a[7], &id))
        return Fail(Fmt(Tr("Unknown block: %s", "Bilinmeyen blok: %s"), a[7].c_str()));
    const int x0 = std::min(FloorI(p0.x), FloorI(p1.x)), x1 = std::max(FloorI(p0.x), FloorI(p1.x));
    const int y0 = std::min(FloorI(p0.y), FloorI(p1.y)), y1 = std::max(FloorI(p0.y), FloorI(p1.y));
    const int z0 = std::min(FloorI(p0.z), FloorI(p1.z)), z1 = std::max(FloorI(p0.z), FloorI(p1.z));
    const long long volume = (long long)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
    if (volume > 32768)
        return Fail(Fmt(Tr("Too many blocks in the specified area (maximum 32768, specified %lld)",
                           "Belirtilen alanda çok fazla blok var (en fazla 32768, belirtilen %lld)"), volume));
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                gWorld.Set(x, y, z, MakeVox(id));
    Ok(Fmt(Tr("Successfully filled %lld block(s)", "%lld blok dolduruldu"), volume));
}

void CmdGamerule(const Args& a) {
    if (a.size() < 2)
        return Fail("/gamerule <keepInventory|doMobSpawning|mobGriefing> [true|false]");
    bool* rule = GameRule(a[1]);
    if (!rule)
        return Fail(Fmt(Tr("Unknown game rule: %s", "Bilinmeyen oyun kuralı: %s"), a[1].c_str()));
    if (a.size() < 3)
        return Ok(Fmt(Tr("Gamerule %s is currently set to: %s", "%s oyun kuralı şu an: %s"), a[1].c_str(), *rule ? "true" : "false"));
    const std::string v = Lower(a[2]);
    if (v != "true" && v != "false")
        return Fail(Fmt(Tr("Invalid boolean: %s", "Geçersiz değer: %s"), a[2].c_str()));
    *rule = v == "true";
    if (rule == &gRules.animals)
        gRules.monsters = *rule;
    Ok(Fmt(Tr("Gamerule %s is now set to: %s", "%s oyun kuralı artık: %s"), a[1].c_str(), v.c_str()));
}

void CmdSay(const Args& a) {
    std::string msg;
    for (size_t i = 1; i < a.size(); ++i)
        msg += (i > 1 ? " " : "") + a[i];
    ChatPrint("[" + Player() + "] " + msg);
}

void CmdWanted(const Args& a) {
    int level = 0;
    if (a.size() < 2 || !ParseInt(a[1], &level) || level < 0 || level > 6)
        return Fail("/wanted <0-6>");
    TheHost().SetWanted(level);
    Ok(Fmt(Tr("Wanted level set to %d", "Aranma seviyesi %d yapıldı"), level));
}

void CmdHelp(const Args& a);

struct Command {
    const char* name;
    const char* usage;
    void (*run)(const Args&);
};
const Command kCommands[] = {
    { "clear", "/clear [item]", CmdClear },
    { "effect", "/effect <give|clear> <effect> [seconds] [amplifier]", CmdEffect },
    { "fill", "/fill <x1> <y1> <z1> <x2> <y2> <z2> <block>", CmdFill },
    { "gamemode", "/gamemode <survival|creative>", CmdGamemode },
    { "gamerule", "/gamerule <rule> [true|false]", CmdGamerule },
    { "give", "/give <item> [count]", CmdGive },
    { "help", "/help", CmdHelp },
    { "kill", "/kill", CmdKill },
    { "say", "/say <message>", CmdSay },
    { "setblock", "/setblock <x> <y> <z> <block>", CmdSetblock },
    { "summon", "/summon <entity> [x y z]", CmdSummon },
    { "time", "/time <set|add|query> <value>", CmdTime },
    { "tp", "/tp <x> <y> <z>", CmdTp },
    { "wanted", "/wanted <0-6>", CmdWanted },
    { "weather", "/weather <clear|rain|thunder>", CmdWeather },
    { "xp", "/xp add <amount> [points|levels]", CmdXp },
};

void CmdHelp(const Args&) {
    ChatPrint(Tr("--- Commands (x east, y north, z up; ~ = here) ---", "--- Komutlar (x doğu, y kuzey, z yukarı; ~ = burası) ---"), kChatInfo);
    for (const Command& c : kCommands)
        ChatPrint(c.usage, kChatInfo);
}

Args Split(const std::string& line) {
    Args a;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ')
            ++i;
        size_t j = i;
        while (j < line.size() && line[j] != ' ')
            ++j;
        if (j > i)
            a.push_back(line.substr(i, j - i));
        i = j;
    }
    return a;
}

void AddMatches(std::vector<std::string>& out, const std::string& prefix, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (std::string(list[i]).rfind(prefix, 0) == 0)
            out.push_back(list[i]);
}
} // namespace

int ItemByName(const std::string& name) {
    const std::string key = StripNs(Lower(name));
    if (key.empty())
        return 0;
    for (int id = 1; id < NUM_BLOCKS; ++id)
        if (key == Block(id).key && Block(id).shape != SHAPE_BED_HEAD)
            return id;
    for (int id = FIRST_ITEM; id < ITEM_END; ++id)
        if (key == Item(id).key)
            return id;
    return 0;
}

void RunCommand(const std::string& line) {
    const Args a = Split(line);
    if (a.empty())
        return;
    const std::string name = Lower(a[0]);
    for (const Command& c : kCommands)
        if (name == c.name || (name == "teleport" && std::string(c.name) == "tp") ||
            (name == "experience" && std::string(c.name) == "xp")) {
            c.run(a);
            return;
        }
    Fail(Fmt(Tr("Unknown command: %s (see /help)", "Bilinmeyen komut: %s (/help yaz)"), a[0].c_str()));
}

std::vector<std::string> CompleteCommand(const std::string& line) {
    std::vector<std::string> out;
    Args a = Split(line);
    const bool newWord = line.empty() || line.back() == ' ';
    if (newWord)
        a.push_back("");
    if (a.size() == 1) {
        for (const Command& c : kCommands)
            if (std::string(c.name).rfind(Lower(a[0]), 0) == 0)
                out.push_back(c.name);
        return out;
    }
    const std::string cmd = Lower(a[0]), last = Lower(a.back());
    const size_t argIndex = a.size() - 1;
    static const char* const kModes[] = { "survival", "creative" };
    static const char* const kWeathers[] = { "clear", "rain", "thunder" };
    static const char* const kTimeSub[] = { "set", "add", "query" };
    static const char* const kTimes[] = { "day", "noon", "night", "midnight" };
    static const char* const kEffectSub[] = { "give", "clear" };
    static const char* const kXpSub[] = { "add" };
    static const char* const kXpKind[] = { "points", "levels" };
    static const char* const kBools[] = { "true", "false" };
    if (cmd == "gamemode" && argIndex == 1)
        AddMatches(out, last, kModes, 2);
    else if (cmd == "weather" && argIndex == 1)
        AddMatches(out, last, kWeathers, 3);
    else if (cmd == "time" && argIndex == 1)
        AddMatches(out, last, kTimeSub, 3);
    else if (cmd == "time" && argIndex == 2 && Lower(a[1]) == "set")
        AddMatches(out, last, kTimes, 4);
    else if (cmd == "effect" && argIndex == 1)
        AddMatches(out, last, kEffectSub, 2);
    else if (cmd == "effect" && argIndex == 2)
        AddMatches(out, last, kEffectKeys, EFFECT_COUNT);
    else if ((cmd == "xp" || cmd == "experience") && argIndex == 1)
        AddMatches(out, last, kXpSub, 1);
    else if ((cmd == "xp" || cmd == "experience") && argIndex == 3)
        AddMatches(out, last, kXpKind, 2);
    else if (cmd == "summon" && argIndex == 1)
        AddMatches(out, last, kMobNames, MOB_KIND_COUNT);
    else if (cmd == "gamerule" && argIndex == 1) {
        for (const char* r : kGameRules)
            if (Lower(r).rfind(last, 0) == 0)
                out.push_back(r);
    } else if (cmd == "gamerule" && argIndex == 2)
        AddMatches(out, last, kBools, 2);
    else if (((cmd == "give" || cmd == "clear") && argIndex == 1) || (cmd == "setblock" && argIndex == 4) ||
             (cmd == "fill" && argIndex == 7)) {
        const std::string key = StripNs(last);
        const bool blocksOnly = cmd == "setblock" || cmd == "fill";
        for (int id = 1; id < ITEM_END && out.size() < 50; id = id + 1 == NUM_BLOCKS ? FIRST_ITEM : id + 1)
            if (std::string(Item(id).key).rfind(key, 0) == 0 && (!blocksOnly || IsBlockItem(id)) &&
                !(IsBlockItem(id) && Block(id).shape == SHAPE_BED_HEAD))
                out.push_back(Item(id).key);
    }
    return out;
}

} // namespace mc
