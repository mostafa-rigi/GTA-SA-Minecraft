#pragma once

#include "ModCommon.h"

#include "BlockMesh.h"

struct RwTexture;
struct RwRaster;

namespace mc {

struct Tex {
    RwTexture* tex = nullptr;
    int w = 0, h = 0;
    RwRaster* Raster() const;
};

extern Tex gAtlasTex; // blocks, items, cracks
extern Tex gGuiTex;   // hotbar, windows, icons
extern Tex gFontTex;  // pixel font
extern Tex gEntityTex; // steve skin, elytra, arrow, shadow
extern Tex gMenuTex;   // title screen: panorama, logo, buttons (optional)
extern Tex gSoldierTex[3]; // the CS soldier's own textures: uniform, backpack, M4A1 (optional -
                           // without them there is no soldier, and the spawn says so)
constexpr int kCopTexMax = 32; // slots for the Driver 2 cop's own textures
extern Tex gCopTex[kCopTexMax]; // his (page, CLUT) renders from the disc, cop_tex<i>.png
// his body is only worn when the whole set is here: the core's model AND
// every texture the bin names. Anything missing - the host car stays
// visible and the log has already said what (the named stand-in of
// pieces 1+2, never a silent wrong body).
bool CopBodyFilesReady();

// alpha channel of atlas.png kept on the CPU (item sprites get their extruded edges from it)
extern std::vector<uint8_t> gAtlasAlpha;
bool AtlasPixelOpaque(int tile, int px, int py);

bool LoadTextures();
void UnloadTextures();

// atlas UVs for a 16px tile (small inset avoids bleeding with point sampling)

} // namespace mc
