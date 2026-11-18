#!/usr/bin/env python3
"""gen_ui.py — generates art/ui.png, the pieces the screen overlay is built from.

Standard library only (the PNG is written by hand with zlib).

Layout: 8 cols x 2 rows of 16x16 cells, flat index = row * 8 + col.

   0  1  2     a window frame cut in nine: corners keep their size, edges and
   3  4  5     the centre stretch (they are flat along the stretched axis), so
   6  7  8     one set of cells draws a window of any size.
   9           solid white (tint it for bars and backdrops)
  10           cursor: a triangle pointing right
  11           "more" marker: a triangle pointing down
  12           empty (pairs with 11 as a two-frame blink)

Run from the repo root:  python3 scripts/gen_ui.py
"""
import struct
import zlib

CELL = 16
COLS = 8
ROWS = 2
W, H = COLS * CELL, ROWS * CELL

OUTLINE = (10, 12, 30, 255)
BORDER = (232, 238, 255, 255)
BORDER_LO = (150, 164, 220, 255)
FILL = (22, 32, 88, 238)
CLEAR = (0, 0, 0, 0)

img = [[CLEAR] * W for _ in range(H)]


def put(cell, x, y, color):
    img[(cell // COLS) * CELL + y][(cell % COLS) * CELL + x] = color


# --- the window frame: draw one 48x48 rounded window, cut it in nine ----------
SIZE = 3 * CELL
RADIUS = 6


def inside(x, y, inset):
    """Is pixel (x, y) inside the rounded rectangle shrunk by `inset`?"""
    lo, hi = inset, SIZE - 1 - inset
    if x < lo or y < lo or x > hi or y > hi:
        return False
    r = max(RADIUS - inset, 0)
    cx = min(max(x, lo + r), hi - r)
    cy = min(max(y, lo + r), hi - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r + 1


for y in range(SIZE):
    for x in range(SIZE):
        if not inside(x, y, 0):
            color = CLEAR
        elif not inside(x, y, 1):
            color = OUTLINE
        elif not inside(x, y, 3):
            # lit from the top-left: the lower/right half of the rim is dimmer
            color = BORDER if (x + y) < SIZE else BORDER_LO
        else:
            color = FILL
        put((y // CELL) * 3 + (x // CELL), x % CELL, y % CELL, color)

# --- solid white ---------------------------------------------------------------
for y in range(CELL):
    for x in range(CELL):
        put(9, x, y, (255, 255, 255, 255))

# --- cursor (points right) and "more" marker (points down) ---------------------
for y in range(3, 13):
    half = 5 - abs(y - 7.5)          # widest in the middle row
    for x in range(4, 4 + int(half * 2)):
        put(10, x, y, (255, 255, 255, 255))
for x in range(3, 13):
    half = 5 - abs(x - 7.5)
    for y in range(5, 5 + int(half * 2)):
        put(11, x, y, (255, 255, 255, 255))


def write_png(path):
    raw = bytearray()
    for row in img:
        raw.append(0)   # filter: none
        for r, g, b, a in row:
            raw += bytes((r, g, b, a))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


write_png("art/ui.png")
print("wrote art/ui.png (%dx%d, %dx%d cells)" % (W, H, CELL, CELL))
