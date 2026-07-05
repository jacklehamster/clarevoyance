#!/usr/bin/env python3
"""gen_tiles.py — generates art/tiles.png, the environment tile sheet.

Layout: 2 cols x 1 row of 32x32 cells.
  cell 0 — floor: two-tone checker of 8px blocks
  cell 1 — wall:  brick courses with mortar lines

Run from the repo root:  python3 scripts/gen_tiles.py
"""
from PIL import Image

CELL = 32

# Floor checker shades (cool stone) and wall brick shades (warm stone).
FLOOR_A = (96, 100, 112, 255)
FLOOR_B = (76, 80, 92, 255)
BRICK   = (134, 106, 88, 255)
MORTAR  = (88, 70, 60, 255)

img = Image.new("RGBA", (2 * CELL, CELL))
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

img.save("art/tiles.png")
print("wrote art/tiles.png (%dx%d)" % img.size)
