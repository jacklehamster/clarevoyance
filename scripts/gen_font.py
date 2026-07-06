#!/usr/bin/env python3
"""gen_font.py — generates art/font.png, the bitmap font sheet used by the
game-layer text helper (src/game/text.{h,cpp}).

Layout: 16 cols x 6 rows of CELL x CELL cells (96 cells total), covering ASCII
32 ("space") through 127 (DEL, blank) in order — cell index = ascii - 32, same
convention as the sprite sheets (art/penguin.png, art/tiles.png): flat cell
index, row-major, first row first.

Glyphs are white (so a tint multiplier can recolor them) on a transparent
background, drawn with PIL's built-in default font (no external font file
needed — keeps this reproducible on any machine with just Pillow installed).

Run from the repo root:  python3 scripts/gen_font.py
"""
from PIL import Image, ImageDraw, ImageFont

COLS = 16
ROWS = 6
CELL = 16
FIRST_ASCII = 32
NUM_GLYPHS = COLS * ROWS   # 96 → ASCII 32..127 inclusive

img = Image.new("RGBA", (COLS * CELL, ROWS * CELL), (0, 0, 0, 0))
draw = ImageDraw.Draw(img)

# A small built-in bitmap font upscaled slightly still reads fine at 16x16;
# load_default(size=N) (Pillow >= 10.1) gives a scalable version of the same.
try:
    font = ImageFont.load_default(size=12)
except TypeError:
    # Older Pillow: no size param — fall back to the fixed built-in bitmap font.
    font = ImageFont.load_default()

for i in range(NUM_GLYPHS):
    ascii_code = FIRST_ASCII + i
    ch = chr(ascii_code)
    col = i % COLS
    row = i // COLS
    cx = col * CELL
    cy = row * CELL
    if ch.isprintable() or ch == " ":
        bbox = draw.textbbox((0, 0), ch, font=font)
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        x = cx + (CELL - w) // 2 - bbox[0]
        y = cy + (CELL - h) // 2 - bbox[1]
        draw.text((x, y), ch, font=font, fill=(255, 255, 255, 255))

img.save("art/font.png")
print("wrote art/font.png (%dx%d, %d glyphs, %dx%d cells)"
      % (img.size[0], img.size[1], NUM_GLYPHS, CELL, CELL))
