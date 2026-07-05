#!/usr/bin/env python3
"""gen_tiles.py — generates art/tiles.png, the environment/prop tile sheet.

Layout: 3 cols x 1 row of 32x32 cells.
  cell 0 — floor: two-tone checker of 8px blocks (opaque)
  cell 1 — wall:  brick courses with mortar lines (opaque)
  cell 2 — key:   a small golden pickup icon on a transparent background —
                  billboarded as a floating prop (unlike cells 0/1, which are
                  oriented quads tiling the floor/walls)

Run from the repo root:  python3 scripts/gen_tiles.py
"""
from PIL import Image, ImageDraw

CELL = 32
COLS = 3

# Floor checker shades (cool stone), wall brick shades (warm stone), key gold.
FLOOR_A   = (96, 100, 112, 255)
FLOOR_B   = (76, 80, 92, 255)
BRICK     = (134, 106, 88, 255)
MORTAR    = (88, 70, 60, 255)
KEY_GOLD  = (230, 190, 60, 255)
KEY_SHINE = (255, 235, 150, 255)

img = Image.new("RGBA", (COLS * CELL, CELL))
px = img.load()

# Cell 0: 8px checkerboard.
for y in range(CELL):
    for x in range(CELL):
        px[x, y] = FLOOR_A if ((x // 8) + (y // 8)) % 2 == 0 else FLOOR_B

# Cell 1: bricks — 8px courses, mortar every 8 rows and staggered verticals.
for y in range(CELL):
    for x in range(CELL):
        course = y // 8
        offset = (course % 2) * 8            # stagger alternate courses
        mortar = (y % 8 == 7) or ((x + offset) % 16 == 15)
        px[CELL + x, y] = MORTAR if mortar else BRICK

# Cell 2: a simple key — a ring near the top-left, a shaft with two teeth
# extending toward the bottom-right, gold on transparent.
draw = ImageDraw.Draw(img)
ox = 2 * CELL
draw.ellipse([ox + 6, 4, ox + 18, 16], outline=KEY_GOLD, width=3)
draw.rectangle([ox + 15, 14, ox + 19, 26], fill=KEY_GOLD)
draw.rectangle([ox + 19, 20, ox + 25, 23], fill=KEY_GOLD)
draw.rectangle([ox + 19, 24, ox + 24, 27], fill=KEY_GOLD)
draw.point([ox + 10, 8], fill=KEY_SHINE)

img.save("art/tiles.png")
print("wrote art/tiles.png (%dx%d)" % img.size)
