#!/usr/bin/env python3
"""Cut the Counter-Strike SEAL's texture out of his own model file and seat it on our soldier.

The soldier of the CS project (step 1) is a real entity of this mod's own core, drawn as a box man
the way the zombie and the skeleton are. His skin is not invented: it is cut, face by face, out of
the texture sheet inside urban.mdl - the SEAL Team 6 model of the user's own Counter-Strike 1.6
(the sheet inside that file is literally named SEAL_Working1.BMP) - and rearranged into the classic
64x64 humanoid layout that McModel reads, the same face-by-face rearrangement the spider's real
texture needed in the polish round. Every pixel is the game's own; only the arrangement is ours.

The 64x64 is patched into a copy of the entity atlas at ENT_SOLDIER (448, 256), the rectangle the
asset generator reserves for it (see gen_assets.py). No other rectangle of the atlas is touched.

Usage:
    python3 tools/cs_soldier_texture.py <urban.mdl> <entity.png in> <entity.png out>
"""
import struct
import sys

from PIL import Image


def read_mdl_textures(data):
    """GoldSrc studio model: return [(name, PIL Image)] for the embedded textures.

    These ripak files carry a header variant with 8 extra bytes before the bone count, so the
    texture table fields sit at offset 180/184 instead of the textbook 172/176; both are tried
    and the one whose entries validate (sane sizes, data ending inside the file) wins.
    """
    assert data[:4] == b"IDST", "not a GoldSrc model"
    for off in (180, 172):
        numtex, texindex = struct.unpack_from("<2i", data, off)
        if not (0 < numtex <= 64 and 0 < texindex < len(data)):
            continue
        out = []
        ok = True
        for i in range(numtex):
            e = texindex + i * 80
            name = data[e:e + 64].split(b"\0")[0].decode("ascii", "replace")
            _flags, w, h, index = struct.unpack_from("<4i", data, e + 64)
            if not (0 < w <= 4096 and 0 < h <= 4096 and 0 <= index and index + w * h + 768 <= len(data)):
                ok = False
                break
            pal = data[index + w * h: index + w * h + 768]
            img = Image.frombytes("P", (w, h), data[index: index + w * h])
            img.putpalette(pal)
            out.append((name, img.convert("RGB")))
        if ok:
            return out
    raise ValueError("no valid texture table found")


# where each face of the box man is cut from, on the SEAL's 512x512 sheet: (x0, y0, x1, y1).
# Named by what the piece of the sheet shows: the balaclava head at the bottom left, the black
# tactical vest at the top right, the backpack panel above the head, the green trousers with
# their pouches at the bottom right and the top left, the dark shoulder yoke in the middle left.
CUTS = {
    # head, classic layout: front (8,8), back (24,8), sides (0,8)/(16,8), top (8,0), bottom (16,0)
    "head_front": (25, 340, 165, 420),    # the balaclava: the eye slit and the mask
    "head_back": (55, 452, 150, 508),     # the back of the hood
    "head_side_l": (8, 360, 34, 452),     # hood cloth, one side
    "head_side_r": (140, 360, 166, 452),  # hood cloth, the other side
    "head_top": (40, 326, 140, 356),      # the top of the hood
    "head_bottom": (70, 470, 120, 505),   # the neck cloth
    # body: front (20,20), back (32,20), sides (16,20)/(28,20), top (20,16), bottom (28,16)
    "body_front": (330, 95, 490, 232),    # the vest: pouches, radio, straps
    "body_back": (160, 96, 286, 228),     # the backpack panel
    "body_side_l": (292, 80, 318, 220),   # the vest's side straps
    "body_side_r": (478, 80, 504, 220),
    "body_top": (330, 56, 470, 78),       # the shoulders of the vest
    "body_bottom": (330, 214, 470, 232),  # the vest's hem
    # arm (one texture, the other arm mirrors it): front (44,20), back (52,20),
    # sides (40,20)/(48,20), top (44,16), bottom (48,16) - the dark sleeve of the shoulder yoke
    "arm": (56, 246, 160, 330),
    # leg (one texture, the other leg mirrors it): front (8,20), back (16,20),
    # sides (4,20)/(12,20), top (8,16), bottom (12,16)
    "leg_front": (272, 262, 378, 495),    # a trouser leg with its knee pouch
    "leg_back": (20, 10, 120, 200),       # the other trouser panel
    "leg_side_l": (262, 280, 300, 480),
    "leg_side_r": (484, 280, 508, 480),
    "leg_top": (300, 258, 390, 276),      # the waistband
    "leg_bottom": (180, 350, 240, 450),   # the boot
}

# where each cut lands in the 64x64 humanoid layout: cut name -> (x, y, w, h)
PLACES = {
    "head_front": (8, 8, 8, 8), "head_back": (24, 8, 8, 8),
    "head_side_l": (0, 8, 8, 8), "head_side_r": (16, 8, 8, 8),
    "head_top": (8, 0, 8, 8), "head_bottom": (16, 0, 8, 8),
    "body_front": (20, 20, 8, 12), "body_back": (32, 20, 8, 12),
    "body_side_l": (16, 20, 4, 12), "body_side_r": (28, 20, 4, 12),
    "body_top": (20, 16, 8, 4), "body_bottom": (28, 16, 8, 4),
    "leg_front": (8, 20, 4, 12), "leg_back": (16, 20, 4, 12),
    "leg_side_l": (4, 20, 4, 12), "leg_side_r": (12, 20, 4, 12),
    "leg_top": (8, 16, 4, 4), "leg_bottom": (12, 16, 4, 4),
}
# the arm's six faces all read as sleeve cloth
ARM_PLACES = [(40, 20, 4, 12), (44, 20, 4, 12), (48, 20, 4, 12), (52, 20, 4, 12),
              (44, 16, 4, 4), (48, 16, 4, 4)]


def compose(sheet):
    tex = Image.new("RGBA", (64, 64), (0, 0, 0, 0))

    def put(name, x, y, w, h):
        x0, y0, x1, y1 = CUTS[name]
        piece = sheet.crop((x0, y0, x1, y1)).resize((w, h), Image.NEAREST)
        tex.paste(piece, (x, y))

    for name, (x, y, w, h) in PLACES.items():
        put(name, x, y, w, h)
    for (x, y, w, h) in ARM_PLACES:
        put("arm", x, y, w, h)
    return tex


def main():
    mdl_path, atlas_in, atlas_out = sys.argv[1], sys.argv[2], sys.argv[3]
    textures = read_mdl_textures(open(mdl_path, "rb").read())
    sheet = None
    for name, img in textures:
        if img.size == (512, 512):
            sheet = img
            print("sheet:", name, img.size)
    assert sheet is not None, "no 512x512 sheet in the model"
    soldier = compose(sheet)
    soldier.save(atlas_out + ".soldier64.png")
    atlas = Image.open(atlas_in).convert("RGBA")
    assert atlas.size == (512, 512), atlas.size
    before = atlas.copy()
    atlas.paste(soldier, (448, 256))
    # prove nothing outside the soldier's rectangle moved
    import numpy as np
    a, b = np.asarray(before).astype(int), np.asarray(atlas).astype(int)
    diff = np.abs(a - b).sum(axis=2)
    ys, xs = np.nonzero(diff)
    assert len(xs) == 0 or (xs.min() >= 448 and xs.max() < 512 and ys.min() >= 256 and ys.max() < 320), \
        "pixels changed outside ENT_SOLDIER"
    atlas.save(atlas_out)
    print("patched ENT_SOLDIER (448,256) into", atlas_out, "- changed pixels:", len(xs))


if __name__ == "__main__":
    main()
