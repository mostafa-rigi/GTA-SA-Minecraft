#include "Textures.h"

#include <cstdio>

#include "Image.h"
#include "NavGraph.h"
#include "Other.h"
#include "RenderWare.h"
#include "SoldierModel.h"

namespace mc {

Tex gAtlasTex, gGuiTex, gFontTex, gEntityTex, gMenuTex;
Tex gSoldierTex[3];

RwRaster* Tex::Raster() const { return tex ? RwTextureGetRaster(tex) : nullptr; }

std::vector<uint8_t> gAtlasAlpha;

bool AtlasPixelOpaque(int tile, int px, int py) {
    if (px < 0 || py < 0 || px > 15 || py > 15 || gAtlasAlpha.empty())
        return false;
    int x = (tile % ATLAS_TILES_PER_ROW) * 16 + px, y = (tile / ATLAS_TILES_PER_ROW) * 16 + py;
    if (y >= ATLAS_SIZE)
        return false;
    return gAtlasAlpha[(size_t)y * ATLAS_SIZE + x] > 100;
}

static bool LoadPngAt(const std::string& path, const char* label, Tex& out, std::vector<uint8_t>* alphaOut = nullptr) {
    plugin::Image* img = nullptr;
    if (!plugin::CreateImageFromFile(path, img) || !img) {
        Log("ERROR: could not load %s", path.c_str());
        return false;
    }
    RwRaster* raster = RwRasterCreate(img->width, img->height, 0, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    if (!raster) {
        Log("ERROR: RwRasterCreate failed for %s", label);
        img->Release();
        return false;
    }
    if (alphaOut) {
        alphaOut->resize((size_t)img->width * img->height);
        for (int i = 0; i < img->width * img->height; ++i)
            (*alphaOut)[i] = img->pixels[i * 4 + 3];
    }
    RwUInt8* dst = RwRasterLock(raster, 0, rwRASTERLOCKWRITE);
    if (dst) {
        int stride = raster->stride ? raster->stride : img->width * 4;
        for (int y = 0; y < img->height; ++y) {
            const uint8_t* src = img->pixels + y * img->width * 4;
            uint8_t* row = dst + y * stride;
            for (int x = 0; x < img->width; ++x) {
                row[x * 4 + 0] = src[x * 4 + 2]; // B
                row[x * 4 + 1] = src[x * 4 + 1]; // G
                row[x * 4 + 2] = src[x * 4 + 0]; // R
                row[x * 4 + 3] = src[x * 4 + 3]; // A
            }
        }
        RwRasterUnlock(raster);
    }
    out.tex = RwTextureCreate(raster);
    if (out.tex) {
        RwTextureSetFilterMode(out.tex, rwFILTERNEAREST);
        RwTextureSetAddressing(out.tex, rwTEXTUREADDRESSCLAMP);
    }
    out.w = img->width;
    out.h = img->height;
    img->Release();
    Log("Loaded %s (%dx%d)", label, out.w, out.h);
    return out.tex != nullptr;
}

static bool LoadPng(const char* file, Tex& out, std::vector<uint8_t>* alphaOut = nullptr) {
    return LoadPngAt(ModPath(file), file, out, alphaOut);
}

// Where one of the soldier's own files actually sits. The converter ships his
// files in their own cs-soldier folder and the boss install copied that folder
// into MinecraftSA as-is; the first real-model build kept the files loose in
// the MinecraftSA folder itself. Both layouts are honoured - the folder first,
// the loose file as the fallback - so neither install silently loses him.
// (This was a real refusal once: the bridge asked MinecraftSA\cs_soldier.bin,
// the shipped install had put the file in MinecraftSA\cs-soldier\, the open
// failed, and /summon soldier was refused with "cs_soldier.bin is not loaded".)
static std::string SoldierFilePath(const char* file) {
    const std::string inFolder = ModPath((std::string("cs-soldier\\") + file).c_str());
    FILE* probe = std::fopen(inFolder.c_str(), "rb");
    if (probe) {
        std::fclose(probe);
        return inFolder;
    }
    return ModPath(file);
}

// The Counter-Strike soldier's model file, read whole and handed to the core (the
// core never touches the disk). A missing or broken file is not fatal to the mod -
// it only means there is no soldier, and the core's loader has already shouted why.
static void LoadSoldierFiles() {
    char name[32];
    for (int i = 0; i < 3; ++i) {
        std::snprintf(name, sizeof(name), "cs_soldier_tex%d.png", i);
        LoadPngAt(SoldierFilePath(name), name, gSoldierTex[i]);
    }
    const std::string path = SoldierFilePath("cs_soldier.bin");
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        Log("ERROR: could not load %s - the Counter-Strike soldier cannot appear without his model file", path.c_str());
        return;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> bytes(size > 0 ? (size_t)size : 0);
    if (size <= 0 || std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        Log("ERROR: could not read %s - the Counter-Strike soldier cannot appear", path.c_str());
        std::fclose(f);
        return;
    }
    std::fclose(f);
    LoadSoldierModel(bytes.data(), bytes.size());
    // the road graph rides the same road in: vehicle_graph.bin, built from
    // the vehicle half of the game's own nodes*.dat (nav-assets/). In this
    // build it only has to load and route - the car step will drive on it.
    // A missing file is said out loud and costs nothing else.
    {
        const std::string vehPath = SoldierFilePath("vehicle_graph.bin");
        FILE* vf = std::fopen(vehPath.c_str(), "rb");
        if (!vf) {
            Log("vehicle_graph.bin not found in the MinecraftSA folder - the road graph stays dark until the car step");
        } else {
            std::fseek(vf, 0, SEEK_END);
            long vsize = std::ftell(vf);
            std::fseek(vf, 0, SEEK_SET);
            std::vector<unsigned char> vbytes(vsize > 0 ? (size_t)vsize : 0);
            if (vsize > 0 && std::fread(vbytes.data(), 1, vbytes.size(), vf) == vbytes.size())
                LoadVehicleGraph(vbytes.data(), vbytes.size());
            else
                Log("ERROR: could not read %s - the road graph stays dark", vehPath.c_str());
            std::fclose(vf);
        }
    }
    // his map of the city, the same road in: read whole, handed to the core. A
    // missing file only costs him the map (he falls back to probe steering and
    // the core's loader says so) - never the mod.
    const std::string navPath = SoldierFilePath("nav_graph.bin");
    FILE* nf = std::fopen(navPath.c_str(), "rb");
    if (!nf) {
        Log("nav_graph.bin not found in the MinecraftSA folder - the soldier will steer by probes only");
        return;
    }
    std::fseek(nf, 0, SEEK_END);
    long nsize = std::ftell(nf);
    std::fseek(nf, 0, SEEK_SET);
    std::vector<unsigned char> nbytes(nsize > 0 ? (size_t)nsize : 0);
    if (nsize <= 0 || std::fread(nbytes.data(), 1, nbytes.size(), nf) != nbytes.size()) {
        Log("ERROR: could not read %s - the soldier will steer by probes only", navPath.c_str());
        std::fclose(nf);
        return;
    }
    std::fclose(nf);
    LoadNavGraph(nbytes.data(), nbytes.size());
}

bool LoadTextures() {
    bool ok = LoadPng("atlas.png", gAtlasTex, &gAtlasAlpha);
    ok &= LoadPng("gui.png", gGuiTex);
    ok &= LoadPng("font.png", gFontTex);
    ok &= LoadPng("entity.png", gEntityTex);
    LoadPng("menu.png", gMenuTex); // without it GTA's own menu stays
    LoadSoldierFiles();           // the soldier is an extra, never a reason to fail the mod
    return ok;
}

static void Free(Tex& t) {
    if (t.tex) {
        RwTextureDestroy(t.tex);
        t.tex = nullptr;
    }
}

void UnloadTextures() {
    Free(gAtlasTex);
    Free(gGuiTex);
    Free(gFontTex);
    Free(gEntityTex);
    Free(gMenuTex);
    for (auto& t : gSoldierTex)
        Free(t);
    UnloadSoldierModel();
}

} // namespace mc
