#include "Sound.h"

#include <map>

#include "Config.h"

#include "CCamera.h"
#include "CMenuManager.h"
#include "RenderWare.h"

namespace mc {

namespace {
typedef BOOL(WINAPI* BASS_Init_t)(int, DWORD, DWORD, HWND, const void*);
typedef DWORD(WINAPI* BASS_ErrorGetCode_t)();
typedef DWORD(WINAPI* BASS_SampleLoad_t)(BOOL, const void*, unsigned __int64, DWORD, DWORD, DWORD);
typedef DWORD(WINAPI* BASS_SampleGetChannel_t)(DWORD, DWORD);
typedef BOOL(WINAPI* BASS_ChannelSetAttribute_t)(DWORD, DWORD, float);
typedef BOOL(WINAPI* BASS_ChannelGetAttribute_t)(DWORD, DWORD, float*);
typedef BOOL(WINAPI* BASS_ChannelPlay_t)(DWORD, BOOL);
typedef BOOL(WINAPI* BASS_ChannelStop_t)(DWORD);
typedef BOOL(WINAPI* BASS_ChannelPause_t)(DWORD);

constexpr DWORD BASS_SAMPLE_LOOP = 4;
constexpr DWORD BASS_SAMPLE_OVER_POS = 0x30000;
constexpr DWORD BASS_ATTRIB_FREQ = 1, BASS_ATTRIB_VOL = 2, BASS_ATTRIB_PAN = 3;
constexpr DWORD BASS_ERROR_ALREADY = 14;

struct Bass {
    BASS_Init_t Init;
    BASS_ErrorGetCode_t ErrorGetCode;
    BASS_SampleLoad_t SampleLoad;
    BASS_SampleGetChannel_t SampleGetChannel;
    BASS_ChannelSetAttribute_t SetAttr;
    BASS_ChannelGetAttribute_t GetAttr;
    BASS_ChannelPlay_t Play;
    BASS_ChannelStop_t Stop;
    BASS_ChannelPause_t Pause;
} B;

int gState = 0; // 0 untried, 1 ok, -1 failed
std::unordered_map<std::string, DWORD> gSamples;
std::vector<DWORD> gActive;
DWORD gLoopChan = 0;
int gLoopEvent = -1;

bool Ready() {
    if (gState != 0)
        return gState > 0;
    gState = -1;
    HMODULE h = LoadLibraryA(ModPath("bass.dll").c_str());
    if (!h)
        h = LoadLibraryA((GameDir() + "\\bass.dll").c_str());
    if (!h) {
        Log("Sound: bass.dll not found (MinecraftSA folder or game folder), no sounds");
        return false;
    }
#define LOAD(field, name) B.field = (decltype(B.field))GetProcAddress(h, name); if (!B.field) { Log("Sound: missing " name); return false; }
    LOAD(Init, "BASS_Init");
    LOAD(ErrorGetCode, "BASS_ErrorGetCode");
    LOAD(SampleLoad, "BASS_SampleLoad");
    LOAD(SampleGetChannel, "BASS_SampleGetChannel");
    LOAD(SetAttr, "BASS_ChannelSetAttribute");
    LOAD(GetAttr, "BASS_ChannelGetAttribute");
    LOAD(Play, "BASS_ChannelPlay");
    LOAD(Stop, "BASS_ChannelStop");
    LOAD(Pause, "BASS_ChannelPause");
#undef LOAD
    HWND wnd = RsGlobal.ps ? RsGlobal.ps->window : nullptr;
    if (!B.Init(-1, 44100, 0, wnd, nullptr) && B.ErrorGetCode() != BASS_ERROR_ALREADY) {
        Log("Sound: BASS_Init failed (%u)", B.ErrorGetCode());
        return false;
    }
    gState = 1;
    Log("Sound: BASS ready");
    return true;
}

DWORD Sample(const char* file, bool loop) {
    std::string key = std::string(file) + (loop ? "#loop" : "");
    auto it = gSamples.find(key);
    if (it != gSamples.end())
        return it->second;
    std::string path = ModPath("sounds\\") + file;
    // loop samples allow 8 channels, not 1: the cop force runs one siren
    // and one engine loop per unit off the same two sample files
    DWORD s = B.SampleLoad(FALSE, path.c_str(), 0, 0, loop ? 8 : 4, (loop ? BASS_SAMPLE_LOOP : 0) | BASS_SAMPLE_OVER_POS);
    if (!s)
        Log("Sound: cannot load %s (%u)", file, B.ErrorGetCode());
    gSamples[key] = s;
    return s;
}

float MasterVolume() {
    return Clamp((unsigned char)FrontEndMenuManager.m_nPrefsSfxVolume / 64.0f, 0.0f, 1.0f) * gConfig.soundVolume;
}

// PlaySfx's positional law, shared with the loops: how loud a sound at
// `pos` is here, and where it sits in the stereo field.
void PosVolPan(const Vec3& pos, float range, float* vol, float* pan) {
    CVector cam = TheCamera.GetPosition();
    CVector d = ToGta(pos) - cam;
    float dist = d.Magnitude();
    *vol *= Clamp(1.0f - dist / range, 0.0f, 1.0f);
    *pan = 0.0f;
    if (dist > 0.01f) {
        // forward x up: the real right-hand direction (the matrix itself stores the left vector)
        CVector right = CVector::Cross(TheCamera.m_mCameraMatrix.up, TheCamera.m_mCameraMatrix.at);
        *pan = Clamp((d.x * right.x + d.y * right.y + d.z * right.z) / dist, -1.0f, 1.0f) * 0.7f;
    }
}

// The cop force's positional loops (see Sound.h): one BASS channel per
// caller id, its native frequency remembered so pitch never compounds.
struct PosLoop {
    DWORD chan = 0;
    int ev = -1;
    float baseFreq = 44100.0f;
};
std::map<int, PosLoop> gPosLoops;
} // namespace

void PlaySfx(SoundEvent ev, const Vec3* pos, float volume, float pitch) {
    if (ev >= SND_COUNT || !Ready())
        return;
    // CleanSA: the Minecraft world's sounds stay silent. Only the appended
    // visitors' blocks at the end of the event table play - the soldier's
    // Counter-Strike block (his rifle and pistol, steps, hurt, voice) and,
    // after it, the Driver 2 cop's block (his siren, engine, radio) - and an
    // explosion, which in this mode can only be his grenade's: every
    // Minecraft source of one (creepers, TNT) is gated off. The blocks are
    // append-only by the table's own law, so "from SND_M4A1_SHOOT on" stays
    // exactly the visitors' sets.
    if (gConfig.cleanSA && ev < SND_M4A1_SHOOT && ev != SND_EXPLODE)
        return;
    const SoundEventDef& e = kSoundEvents[ev];
    if (e.count == 0)
        return;
    const SoundFile& f = kSoundFiles[e.first + rand() % e.count];
    float vol = volume * f.volume * MasterVolume();
    float pan = 0.0f;
    if (pos) {
        float range = 16.0f * std::max(1.0f, volume);
        PosVolPan(*pos, range, &vol, &pan);
    }
    if (vol <= 0.01f)
        return;
    DWORD s = Sample(f.file, false);
    if (!s)
        return;
    DWORD ch = B.SampleGetChannel(s, 0);
    if (!ch)
        return;
    float freq = 44100.0f;
    B.GetAttr(ch, BASS_ATTRIB_FREQ, &freq);
    B.SetAttr(ch, BASS_ATTRIB_FREQ, freq * pitch * f.pitch);
    B.SetAttr(ch, BASS_ATTRIB_VOL, Clamp(vol, 0.0f, 1.0f));
    B.SetAttr(ch, BASS_ATTRIB_PAN, pan);
    B.Play(ch, FALSE);
}

void SetLoopSfx(SoundEvent ev, bool on, float volume) {
    if (!Ready())
        return;
    if (on && gConfig.cleanSA)
        return; // the only loop is the elytra wind - a CleanSA sky has none (stopping still works)
    if (!on) {
        if (gLoopChan)
            B.Stop(gLoopChan);
        gLoopChan = 0;
        gLoopEvent = -1;
        return;
    }
    if (gLoopEvent != ev) {
        if (gLoopChan)
            B.Stop(gLoopChan);
        gLoopChan = 0;
        const SoundEventDef& e = kSoundEvents[ev];
        if (e.count == 0)
            return;
        DWORD s = Sample(kSoundFiles[e.first].file, true);
        if (!s)
            return;
        gLoopChan = B.SampleGetChannel(s, 0);
        gLoopEvent = ev;
        if (gLoopChan)
            B.Play(gLoopChan, TRUE);
    }
    if (gLoopChan)
        B.SetAttr(gLoopChan, BASS_ATTRIB_VOL, Clamp(volume * MasterVolume(), 0.0f, 1.0f));
}

void SetPosLoop(int id, SoundEvent ev, bool on, const Vec3& pos, float volume, float pitch, float range) {
    if (!Ready())
        return;
    auto it = gPosLoops.find(id);
    if (!on) {
        if (it != gPosLoops.end()) {
            if (it->second.chan)
                B.Stop(it->second.chan);
            gPosLoops.erase(it);
        }
        return;
    }
    if (ev < 0 || ev >= SND_COUNT)
        return;
    const SoundEventDef& e = kSoundEvents[ev];
    if (e.count == 0)
        return;
    PosLoop& lp = gPosLoops[id];
    if (lp.ev != (int)ev || !lp.chan) {
        if (lp.chan)
            B.Stop(lp.chan);
        lp.chan = 0;
        DWORD s = Sample(kSoundFiles[e.first].file, true);
        if (!s)
            return;
        lp.chan = B.SampleGetChannel(s, 0);
        lp.ev = ev;
        if (!lp.chan)
            return;
        lp.baseFreq = 44100.0f;
        B.GetAttr(lp.chan, BASS_ATTRIB_FREQ, &lp.baseFreq);
        B.Play(lp.chan, TRUE);
    }
    float vol = volume * MasterVolume(), pan = 0.0f;
    PosVolPan(pos, range, &vol, &pan);
    B.SetAttr(lp.chan, BASS_ATTRIB_FREQ, lp.baseFreq * pitch);
    B.SetAttr(lp.chan, BASS_ATTRIB_VOL, Clamp(vol, 0.0f, 1.0f));
    B.SetAttr(lp.chan, BASS_ATTRIB_PAN, pan);
}

void StopPosLoops() {
    if (gState > 0)
        for (auto& kv : gPosLoops)
            if (kv.second.chan)
                B.Stop(kv.second.chan);
    gPosLoops.clear();
}

void PauseAllSfx(bool pause) {
    if (gState <= 0)
        return;
    if (gLoopChan) {
        if (pause)
            B.Pause(gLoopChan);
        else
            B.Play(gLoopChan, FALSE);
    }
    for (auto& kv : gPosLoops) {
        if (!kv.second.chan)
            continue;
        if (pause)
            B.Pause(kv.second.chan);
        else
            B.Play(kv.second.chan, FALSE);
    }
}

void ShutdownSfx() {
    if (gState > 0 && gLoopChan)
        B.Stop(gLoopChan);
    gLoopChan = 0;
    StopPosLoops();
}

} // namespace mc
