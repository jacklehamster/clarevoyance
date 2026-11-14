#!/usr/bin/env python3
"""gen_terrain.py — generates art/terrain.png, the world tile + plant sheet.

Placeholder art, drawn by code so it is reproducible on any machine: no image
library needed, just the Python standard library (the PNG is written by hand
with zlib). Every cell is seeded, so re-running produces the identical file.

Layout: 8 cols x 4 rows of 64x64 cells, flat index = row * 8 + col.

  Ground (opaque, tiles the floor):
     0 grass A        1 grass B         2 grass + flowers   3 forest floor A
     4 forest floor B 5 sand A          6 sand B (pebbles)  7 dirt road
     8 flagstones     9 stone wall     10 wall top         11 hedge side
    12 hedge top     13 water (1/2)    14 water (2/2)      15 wood planks
  Plants and rocks (transparent background, stand on the cell's bottom edge):
    16 grass tuft    17 flowers        18 bush             19 round tree
    20 pine          21 cactus         22 rock             23 mushroom
    24 fern          25 dry shrub
  More wall:
    26 stone wall, mossy foot (ground storey)

Run from the repo root:  python3 scripts/gen_terrain.py
"""
import math
import random
import struct
import zlib

CELL = 64
COLS = 8
ROWS = 4
W, H = COLS * CELL, ROWS * CELL

img = [[(0, 0, 0, 0)] * W for _ in range(H)]


# --- tiny drawing kit ---------------------------------------------------------

class Cell:
    """Draws into one cell of the sheet, clipped to it."""

    def __init__(self, index):
        self.ox = (index % COLS) * CELL
        self.oy = (index // COLS) * CELL
        self.rng = random.Random(1000 + index)

    def put(self, x, y, color):
        x, y = int(x), int(y)
        if 0 <= x < CELL and 0 <= y < CELL:
            if len(color) == 3:
                color = color + (255,)
            img[self.oy + y][self.ox + x] = color

    def get(self, x, y):
        if 0 <= x < CELL and 0 <= y < CELL:
            return img[self.oy + y][self.ox + x]
        return (0, 0, 0, 0)

    def fill(self, color, jitter=0, block=1):
        """Flat colour with per-block brightness noise."""
        for by in range(0, CELL, block):
            for bx in range(0, CELL, block):
                d = self.rng.randint(-jitter, jitter)
                c = shade(color, d)
                for y in range(by, by + block):
                    for x in range(bx, bx + block):
                        self.put(x, y, c)

    def rect(self, x0, y0, x1, y1, color):
        for y in range(int(y0), int(y1) + 1):
            for x in range(int(x0), int(x1) + 1):
                self.put(x, y, color)

    def disc(self, cx, cy, rx, ry, color):
        for y in range(int(cy - ry), int(cy + ry) + 1):
            for x in range(int(cx - rx), int(cx + rx) + 1):
                if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                    self.put(x, y, color)

    def line(self, x0, y0, x1, y1, color, width=1):
        steps = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
        for i in range(steps + 1):
            t = i / steps
            x, y = x0 + (x1 - x0) * t, y0 + (y1 - y0) * t
            for dy in range(width):
                for dx in range(width):
                    self.put(x + dx - width // 2, y + dy - width // 2, color)

    def speckle(self, count, colors, size=1):
        for _ in range(count):
            x, y = self.rng.randrange(CELL), self.rng.randrange(CELL)
            c = self.rng.choice(colors)
            self.rect(x, y, x + size - 1, y + size - 1, c)

    def outline(self, color=(24, 28, 22, 255)):
        """Dark rim around everything opaque — matches the penguins' ink line."""
        edge = []
        for y in range(CELL):
            for x in range(CELL):
                if self.get(x, y)[3] == 0:
                    if any(self.get(x + dx, y + dy)[3] != 0
                           for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                        edge.append((x, y))
        for x, y in edge:
            self.put(x, y, color)


def shade(color, d):
    return tuple(max(0, min(255, c + d)) for c in color[:3]) + (255,)


# --- ground -------------------------------------------------------------------

def grass(index, base, blades, flowers=None):
    c = Cell(index)
    c.fill(base, jitter=5, block=2)
    # soft darker/lighter patches
    for _ in range(9):
        x, y = c.rng.randrange(CELL), c.rng.randrange(CELL)
        d = c.rng.choice((-9, -6, 6, 8))
        r = c.rng.randint(4, 9)
        for yy in range(y - r, y + r + 1):
            for xx in range(x - r, x + r + 1):
                if (xx - x) ** 2 + (yy - y) ** 2 <= r * r and 0 <= xx < CELL and 0 <= yy < CELL:
                    c.put(xx, yy, shade(c.get(xx, yy), d))
    # short blades in every direction (the tile gets rotated at random)
    for _ in range(70):
        x, y = c.rng.randrange(CELL), c.rng.randrange(CELL)
        a = c.rng.uniform(0, math.tau)
        ln = c.rng.uniform(2, 4.5)
        c.line(x, y, x + math.cos(a) * ln, y + math.sin(a) * ln, c.rng.choice(blades))
    if flowers:
        for _ in range(11):
            x, y = c.rng.randrange(2, CELL - 3), c.rng.randrange(2, CELL - 3)
            col = c.rng.choice(flowers)
            c.rect(x, y, x + 1, y + 1, col)
            c.put(x - 1, y, shade(col, -30))
            c.put(x + 2, y + 1, shade(col, 25))


def sand(index, pebbles):
    c = Cell(index)
    c.fill((214, 190, 132), jitter=5, block=2)
    for _ in range(7):   # wind ripples
        y = c.rng.randrange(CELL)
        x = c.rng.randrange(CELL)
        ln = c.rng.randint(10, 26)
        for i in range(ln):
            yy = y + int(math.sin(i / 5.0) * 1.5)
            c.put((x + i) % CELL, yy % CELL, (198, 172, 116))
    c.speckle(40, [(226, 204, 150), (196, 170, 112)])
    if pebbles:
        for _ in range(9):
            x, y = c.rng.randrange(3, CELL - 4), c.rng.randrange(3, CELL - 4)
            c.disc(x, y, c.rng.uniform(1.2, 2.6), c.rng.uniform(1.0, 2.0), (150, 140, 124))
            c.put(x, y - 1, (186, 178, 162))


def road(index):
    c = Cell(index)
    c.fill((150, 118, 82), jitter=6, block=2)
    c.speckle(60, [(132, 102, 70), (166, 134, 96)], size=2)
    for _ in range(8):
        x, y = c.rng.randrange(3, CELL - 4), c.rng.randrange(3, CELL - 4)
        c.disc(x, y, 2, 1.5, (118, 104, 92))
        c.put(x, y - 1, (160, 148, 134))


def flagstones(index):
    c = Cell(index)
    c.fill((112, 114, 124), jitter=4, block=2)
    half = CELL // 2
    for sy in range(2):
        for sx in range(2):
            d = c.rng.randint(-10, 10)
            for y in range(sy * half + 1, (sy + 1) * half - 1):
                for x in range(sx * half + 1, (sx + 1) * half - 1):
                    c.put(x, y, shade(c.get(x, y), d))
            # highlight on the top-left edge, shadow bottom-right
            c.line(sx * half + 1, sy * half + 1, (sx + 1) * half - 2, sy * half + 1, shade((112, 114, 124), d + 16))
            c.line(sx * half + 1, (sy + 1) * half - 2, (sx + 1) * half - 2, (sy + 1) * half - 2, shade((112, 114, 124), d - 16))
    for i in range(CELL):   # seams
        for k in (0, half - 1, half, CELL - 1):
            c.put(i, k, (62, 62, 72))
            c.put(k, i, (62, 62, 72))
    c.speckle(18, [(92, 94, 104), (130, 132, 142)])


def stone_wall(index, moss):
    c = Cell(index)
    c.rng = random.Random(1009)   # same masonry for both variants
    mortar = (58, 54, 60)
    c.fill(mortar)
    course = 16
    for row in range(CELL // course):
        offset = (row % 2) * 16
        for col in range(-1, 3):
            x0 = col * 32 + offset
            d = c.rng.randint(-12, 12)
            base = shade((140, 134, 128), d)
            for y in range(row * course + 1, (row + 1) * course - 1):
                for x in range(x0 + 1, x0 + 31):
                    c.put(x, y, shade(base, c.rng.randint(-4, 4)))
            c.line(x0 + 1, row * course + 1, x0 + 30, row * course + 1, shade(base, 22))
            c.line(x0 + 1, (row + 1) * course - 2, x0 + 30, (row + 1) * course - 2, shade(base, -24))
    # moss creeping up from the bottom (ground storey only)
    for _ in range(26 if moss else 0):
        x = c.rng.randrange(CELL)
        y = CELL - 1 - int(abs(c.rng.gauss(0, 5)))
        c.rect(x, y, x + 1, y + 1, c.rng.choice([(78, 118, 62), (64, 100, 54)]))


def wall_top(index):
    c = Cell(index)
    c.fill((86, 84, 92), jitter=5, block=2)
    c.speckle(30, [(72, 70, 78), (104, 102, 110)], size=2)
    for i in range(CELL):
        for k in (0, 1):
            c.put(i, k, (120, 118, 126)); c.put(k, i, (120, 118, 126))
        for k in (CELL - 2, CELL - 1):
            c.put(i, k, (58, 56, 64)); c.put(k, i, (58, 56, 64))


def hedge(index, top):
    c = Cell(index)
    c.fill((44, 92, 46), jitter=6, block=2)
    for _ in range(46):
        x, y = c.rng.randrange(CELL), c.rng.randrange(CELL)
        col = c.rng.choice([(66, 128, 60), (58, 112, 52), (34, 74, 38), (84, 148, 70)])
        c.disc(x, y, c.rng.uniform(2, 4), c.rng.uniform(1.5, 3), col)
    if not top:
        for x in range(CELL):   # shadow under the crown, darker foot
            for y in range(CELL - 6, CELL):
                c.put(x, y, shade(c.get(x, y), -22))


def water(index, phase):
    c = Cell(index)
    c.fill((52, 116, 190), jitter=3, block=2)
    for row in range(5):
        y = 6 + row * 13
        for x in range(CELL):
            yy = y + math.sin((x + phase * 32 + row * 11) / CELL * math.tau * 2) * 2.2
            if (x + row * 9 + int(phase * 16)) % 21 < 13:
                c.put(x, yy, (126, 180, 232))
                c.put(x, yy + 1, (40, 96, 168))


def planks(index):
    c = Cell(index)
    for p in range(4):
        d = c.rng.randint(-10, 10)
        base = shade((152, 110, 66), d)
        for x in range(p * 16, p * 16 + 16):
            for y in range(CELL):
                c.put(x, y, shade(base, c.rng.randint(-3, 3)))
        for _ in range(5):   # grain
            y = c.rng.randrange(CELL)
            c.line(p * 16 + 2, y, p * 16 + 13, y + c.rng.randint(-1, 1), shade(base, -16))
        for y in range(CELL):
            c.put(p * 16, y, (86, 58, 34))
        c.rect(p * 16 + 7, 4, p * 16 + 8, 5, (70, 64, 62))
        c.rect(p * 16 + 7, CELL - 6, p * 16 + 8, CELL - 5, (70, 64, 62))


# --- plants -------------------------------------------------------------------

BOTTOM = CELL - 2   # leave one row for the outline


def tuft(index):
    c = Cell(index)
    for i in range(11):
        a = -math.pi / 2 + (i - 5) * 0.2 + c.rng.uniform(-0.08, 0.08)
        ln = c.rng.uniform(14, 26)
        col = c.rng.choice([(96, 168, 70), (70, 138, 56), (118, 186, 84)])
        c.line(32, BOTTOM, 32 + math.cos(a) * ln, BOTTOM + math.sin(a) * ln, col, width=2)
    c.outline()


def flowers(index):
    c = Cell(index)
    for i, (x, h, col) in enumerate(((16, 30, (232, 70, 80)), (30, 40, (250, 214, 70)),
                                     (44, 26, (236, 120, 200)), (38, 18, (250, 250, 250)))):
        c.line(x, BOTTOM, x + (i % 2) * 2 - 1, BOTTOM - h, (70, 138, 56), width=2)
        c.line(x, BOTTOM - h // 2, x + 6, BOTTOM - h // 2 - 4, (96, 168, 70), width=2)
        c.disc(x, BOTTOM - h, 5, 5, col)
        c.disc(x, BOTTOM - h, 2, 2, (250, 190, 60) if col != (250, 214, 70) else (200, 110, 40))
    c.outline()


def bush(index):
    c = Cell(index)
    for cx, cy, r in ((20, 44, 13), (42, 44, 14), (31, 32, 15)):
        c.disc(cx, cy, r, r * 0.9, (52, 110, 54))
    for cx, cy, r in ((18, 40, 8), (40, 39, 9), (30, 27, 9)):
        c.disc(cx, cy, r, r * 0.8, (78, 144, 68))
    for _ in range(12):
        c.disc(c.rng.randint(12, 50), c.rng.randint(22, 52), 2, 1.5, (112, 176, 86))
    for _ in range(5):
        c.disc(c.rng.randint(14, 48), c.rng.randint(26, 50), 2, 2, (214, 60, 70))   # berries
    c.disc(31, 56, 22, 7, (40, 88, 44))   # shaded skirt, down to the ground
    for y in range(BOTTOM + 1, CELL):
        for x in range(CELL):
            c.put(x, y, (0, 0, 0, 0))
    c.outline()


def round_tree(index):
    c = Cell(index)
    c.rect(28, 38, 35, BOTTOM, (112, 78, 48))
    c.rect(28, 38, 30, BOTTOM, (136, 98, 62))
    for cx, cy, r in ((32, 22, 20), (18, 28, 12), (46, 28, 12)):
        c.disc(cx, cy, r, r * 0.92, (46, 104, 50))
    for cx, cy, r in ((28, 17, 12), (16, 24, 7), (44, 23, 7)):
        c.disc(cx, cy, r, r * 0.85, (72, 138, 64))
    for _ in range(14):
        c.disc(c.rng.randint(12, 52), c.rng.randint(6, 36), 2, 1.5, (104, 170, 82))
    c.outline()


def pine(index):
    c = Cell(index)
    c.rect(29, 50, 34, BOTTOM, (104, 72, 44))
    for top, bottom, half in ((2, 22, 12), (14, 36, 17), (28, 52, 22)):
        for y in range(top, bottom + 1):
            w = half * (y - top) / (bottom - top)
            for x in range(int(32 - w), int(32 + w) + 1):
                c.put(x, y, (38, 92, 62) if x > 32 else (58, 122, 78))
    for _ in range(10):
        c.put(c.rng.randint(22, 42), c.rng.randint(10, 48), (96, 160, 110))
    c.outline()


def cactus(index):
    c = Cell(index)
    body, lit, dark = (70, 142, 86), (104, 176, 108), (48, 108, 66)
    c.disc(32, 16, 8, 8, body)
    c.rect(24, 16, 40, BOTTOM, body)
    c.rect(12, 30, 24, 37, body); c.rect(12, 20, 19, 37, body); c.disc(15.5, 20, 4, 4, body)
    c.rect(40, 36, 52, 43, body); c.rect(45, 26, 52, 43, body); c.disc(48.5, 26, 4, 4, body)
    c.rect(26, 14, 28, BOTTOM, lit); c.rect(13, 21, 14, 34, lit); c.rect(46, 27, 47, 40, lit)
    c.rect(36, 14, 38, BOTTOM, dark)
    for y in range(14, BOTTOM, 7):   # spines
        c.put(31, y, (236, 236, 210)); c.put(34, y + 3, (236, 236, 210))
    c.disc(32, 8, 3, 3, (240, 110, 150))   # flower
    c.outline()


def rock(index):
    c = Cell(index)
    c.disc(30, 48, 22, 16, (128, 126, 130))
    c.disc(42, 46, 15, 15, (112, 110, 116))
    c.disc(24, 41, 12, 8, (160, 158, 162))
    c.disc(22, 39, 5, 3, (188, 186, 190))
    for _ in range(14):   # pits, only on the rock itself
        x, y = c.rng.randint(12, 52), c.rng.randint(36, 58)
        if c.get(x, y)[3]:
            c.put(x, y, (96, 94, 100))
    for y in range(BOTTOM + 1, CELL):   # flat foot on the ground line
        for x in range(CELL):
            c.put(x, y, (0, 0, 0, 0))
    c.outline()


def mushroom(index):
    c = Cell(index)
    c.disc(32, 34, 20, 15, (206, 54, 54))
    c.rect(12, 35, 52, 50, (0, 0, 0, 0))   # keep the top half: a domed cap
    c.rect(14, 33, 50, 36, (176, 40, 44))
    c.rect(27, 37, 37, BOTTOM, (236, 226, 204))
    c.rect(27, 37, 30, BOTTOM, (250, 244, 228))
    for x, y, r in ((24, 26, 3), (38, 24, 4), (32, 31, 2), (44, 31, 2)):
        c.disc(x, y, r, r, (250, 246, 236))
    c.outline()


def fern(index):
    c = Cell(index)
    for i in range(7):
        a = -math.pi / 2 + (i - 3) * 0.36
        ln = 30 - abs(i - 3) * 3
        ex, ey = 32 + math.cos(a) * ln, BOTTOM + math.sin(a) * ln
        c.line(32, BOTTOM, ex, ey, (48, 112, 64), width=2)
        for k in range(3, int(ln), 4):   # leaflets
            px, py = 32 + math.cos(a) * k, BOTTOM + math.sin(a) * k
            c.line(px, py, px + math.cos(a + 1.3) * 5, py + math.sin(a + 1.3) * 5, (84, 156, 88))
            c.line(px, py, px + math.cos(a - 1.3) * 5, py + math.sin(a - 1.3) * 5, (66, 134, 74))
    c.outline()


def dry_shrub(index):
    c = Cell(index)

    def branch(x, y, a, ln, depth):
        ex, ey = x + math.cos(a) * ln, y + math.sin(a) * ln
        c.line(x, y, ex, ey, (132, 98, 62) if depth > 1 else (164, 130, 86),
               width=2 if depth > 2 else 1)
        if depth > 0:
            branch(ex, ey, a - c.rng.uniform(0.3, 0.7), ln * 0.72, depth - 1)
            branch(ex, ey, a + c.rng.uniform(0.3, 0.7), ln * 0.72, depth - 1)

    for a in (-1.95, -1.57, -1.2):
        branch(32, BOTTOM, a, 15, 3)
    c.outline()


# --- build the sheet ----------------------------------------------------------

grass(0, (88, 152, 62), [(104, 170, 72), (72, 132, 52), (120, 184, 84)])
grass(1, (84, 148, 60), [(108, 174, 76), (68, 126, 50), (96, 160, 66)])
grass(2, (90, 154, 64), [(104, 170, 72), (72, 132, 52)],
      flowers=[(250, 240, 120), (244, 244, 244), (240, 130, 150)])
grass(3, (58, 110, 58), [(72, 128, 66), (44, 90, 48), (82, 138, 72)])
grass(4, (56, 106, 56), [(70, 124, 64), (42, 86, 46)],
      flowers=[(150, 104, 60), (176, 128, 70)])   # fallen leaves
sand(5, pebbles=False)
sand(6, pebbles=True)
road(7)
flagstones(8)
stone_wall(9, moss=False)
stone_wall(26, moss=True)
wall_top(10)
hedge(11, top=False)
hedge(12, top=True)
water(13, 0.0)
water(14, 0.5)
planks(15)
tuft(16)
flowers(17)
bush(18)
round_tree(19)
pine(20)
cactus(21)
rock(22)
mushroom(23)
fern(24)
dry_shrub(25)


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


write_png("art/terrain.png")
print("wrote art/terrain.png (%dx%d, %d cells of %dpx)" % (W, H, COLS * ROWS, CELL))
