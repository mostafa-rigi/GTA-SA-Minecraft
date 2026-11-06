#include "Doors.h"

#include "Audio.h"
#include "World.h"

namespace mc {

bool IsDoorBlock(int block) {
    if (block <= ID_AIR || block >= NUM_BLOCKS)
        return false;
    return Block(block).shape == SHAPE_DOOR || Block(block).shape == SHAPE_DOOR_UPPER;
}

bool IsDoorUpper(int block) {
    return block > ID_AIR && block < NUM_BLOCKS && Block(block).shape == SHAPE_DOOR_UPPER;
}

int DoorOtherBlock(int block) {
    // by shape and neighbourhood in the table, not by key strings: the two halves are neighbours
    if (IsDoorUpper(block))
        return block - 1;
    if (block > ID_AIR && block + 1 < NUM_BLOCKS && Block(block).shape == SHAPE_DOOR && Block(block + 1).shape == SHAPE_DOOR_UPPER)
        return block + 1;
    return 0;
}

Int3 DoorOtherCell(const Int3& p, int block) {
    return IsDoorUpper(block) ? Int3{ p.x, p.y, p.z - 1 } : Int3{ p.x, p.y, p.z + 1 };
}

ShapeBox DoorPanelBox(uint8_t meta) {
    constexpr float T = 3.0f / 16.0f; // a door's thickness in Minecraft
    const int f = DoorFacing(meta);
    ShapeBox b;
    b.x0 = 0.0f;
    b.y0 = 0.0f;
    b.z0 = 0.0f;
    b.x1 = 1.0f;
    b.y1 = 1.0f;
    b.z1 = 1.0f;
    if (!DoorIsOpen(meta)) {
        // shut: across the cell, flush with the edge away from the side the front faces
        // (Minecraft's own models do the same: a door facing east has its panel at x 0..3)
        switch (f) {
        case FACE_EAST: b.x1 = T; break;
        case FACE_WEST: b.x0 = 1.0f - T; break;
        case FACE_NORTH: b.y1 = T; break;
        default: b.y0 = 1.0f - T; break; // FACE_SOUTH
        }
        return b;
    }
    // open: swung a quarter turn around the hinge corner, flat along the side wall on the
    // hinge's side. The hinge side is read from the front: the viewer's left hand points
    // along (f.y, -f.x) when the front faces f.
    const int lx = FACE_DIR[f].y, ly = -FACE_DIR[f].x; // the front viewer's left, as a direction
    const int hx = DoorHingeRight(meta) ? -lx : lx;    // the hinge end's direction
    const int hy = DoorHingeRight(meta) ? -ly : ly;
    if (f == FACE_EAST || f == FACE_WEST) {
        // the panel ran north-south; open, it runs east-west in a strip at the hinge end
        if (hy < 0)
            b.y1 = T;
        else
            b.y0 = 1.0f - T;
    } else {
        if (hx < 0)
            b.x1 = T;
        else
            b.x0 = 1.0f - T;
    }
    return b;
}

bool ToggleDoor(const Int3& cell) {
    const int b = gWorld.GetBlock(cell.x, cell.y, cell.z);
    if (!IsDoorBlock(b))
        return false;
    const Int3 lower = IsDoorUpper(b) ? Int3{ cell.x, cell.y, cell.z - 1 } : cell;
    const Int3 upper = Int3{ lower.x, lower.y, lower.z + 1 };
    const int lo = gWorld.GetBlock(lower.x, lower.y, lower.z);
    if (!IsDoorBlock(lo) || IsDoorUpper(lo) || gWorld.GetBlock(upper.x, upper.y, upper.z) != DoorOtherBlock(lo))
        return false; // a half on its own is not a door
    const uint8_t meta = (uint8_t)(VoxMeta(gWorld.Get(lower.x, lower.y, lower.z)) ^ DOOR_OPEN);
    gWorld.Set(lower.x, lower.y, lower.z, MakeVox(lo, meta));
    gWorld.Set(upper.x, upper.y, upper.z, MakeVox(DoorOtherBlock(lo), meta));
    const Vec3 at = Vec3((float)lower.x + 0.5f, (float)lower.y + 0.5f, (float)lower.z + 1.0f);
    PlaySfx(DoorIsOpen(meta) ? SND_DOOR_OPEN : SND_DOOR_CLOSE, &at);
    return true;
}

} // namespace mc
