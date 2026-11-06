#include "Textures.h"

#include <cstdio>

#include "Image.h"
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

static bool LoadPng(const char* file, Tex& out, std::vector<uint8_t>* alphaOut = nullptr) {
    std::string path = ModPath(file);
    plugin::Image* img = nullptr;
    if (!plugin::CreateImageFromFile(path, img) || !img) {
        Log("ERROR: could not load %s", path.c_str());
        return false;
    }
    RwRaster* raster = RwRasterCreate(img->width, img->height, 0, rwRASTERTYPETEXTURE | rwRASTERFORMAT8888);
    if (!raster) {
        Log("ERROR: RwRasterCreate failed for %s", file);
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
    Log("Loaded %s (%dx%d)", file, out.w, out.h);
    return out.tex != nullptr;
}

// The Counter-Strike soldier's model file, read whole and handed to the core (the
// core never touches the disk). A missing or broken file is not fatal to the mod -
// it only means there is no soldier, and the core's loader has already shouted why.
static void LoadSoldierFiles() {
    char name[32];
    for (int i = 0; i < 3; ++i) {
        std::snprintf(name, sizeof(name), "cs_soldier_tex%d.png", i);
        LoadPng(name, gSoldierTex[i]);
    }
    const std::string path = ModPath("cs_soldier.bin");
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
