#!/usr/bin/env python3
"""
Generates the gore textures in misc/uwp/xbox/gore/ (see code/cgame/cg_gore.c
and misc/uwp/xbox/scripts/gore.shader).

Each effect is an absorption texture (see save()), one per blood colour. Every
texture's outer rows/columns are transparent, because the effects "grow"
by stretching the texture with clamped coordinates.
"""
import math
import os
import random
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "misc", "uwp", "xbox", "gore")



# How much light thick blood lets through, per channel (red, green, blue).
# The blood is drawn as a filter over the surface (blendFunc GL_ZERO
# GL_ONE_MINUS_SRC_COLOR), so it's lit exactly like the floor or wall under
# it: dark red in daylight, nearly black in shadow, never brighter than the
# surface itself.
TRANSMIT = {
    "": (0.40, 0.035, 0.03),          # red
    "_green": (0.06, 0.30, 0.04),     # green blood option
}


def save(density, name):
    """density: L image (0..255). Writes an "absorption" texture per blood
    colour: RGB = how much of each channel the blood takes away (0 = clear,
    255 = black). cg_gore.c fades effects by scaling it with vertex colours."""
    w, h = density.size
    d = density.load()
    for suffix, tr in TRANSMIT.items():
        img = Image.new("RGBA", (w, h))
        p = img.load()
        for y in range(h):
            for x in range(w):
                v = min(1.0, d[x, y] / 255.0 * 1.05)
                # thickest parts a little darker still
                k = [1.0 - c * (1.0 - 0.35 * v) for c in tr]
                p[x, y] = tuple(int(255 * v * kk) for kk in k) + (int(255 * v),)
        img.save(os.path.join(OUT, f"{name}{suffix}.tga"))
        print("wrote", f"{name}{suffix}.tga")


def clear_border(img, n=2):
    w, h = img.size
    p = img.load()
    for y in range(h):
        for x in range(w):
            if x < n or y < n or x >= w - n or y >= h - n:
                p[x, y] = 0
    return img


def pool(seed, size=128):
    random.seed(seed)
    c = size / 2
    # irregular outline: radius varies with angle (a few low harmonics)
    harm = [(random.uniform(0.02, 0.05), random.randint(2, 4), random.uniform(0, 6.28)) for _ in range(3)] + [(0.015, random.randint(7, 11), random.uniform(0, 6.28))]
    blobs = [(random.uniform(0, 6.28), random.uniform(0.55, 0.85), random.uniform(0.05, 0.09)) for _ in range(4)]
    img = Image.new("L", (size, size), 0)
    p = img.load()
    R = size * 0.36
    for y in range(size):
        for x in range(size):
            dx, dy = x - c, y - c
            r = math.hypot(dx, dy)
            a = math.atan2(dy, dx)
            edge = R * (1 + sum(amp * math.sin(k * a + ph) for amp, k, ph in harm))
            v = 0.0
            if r < edge:
                t = r / edge
                v = 1.0 if t < 0.8 else 1.0 - (t - 0.8) / 0.2 * 0.35   # soaked middle, thinner rim
            # satellite blobs that merged into the edge
            for ba, bd, br in blobs:
                bx, by = c + math.cos(ba) * R * bd * 1.25, c + math.sin(ba) * R * bd * 1.25
                if math.hypot(x - bx, y - by) < size * br:
                    v = max(v, 0.85)
            p[x, y] = int(255 * v)
    img = img.filter(ImageFilter.GaussianBlur(1.4))
    save(clear_border(img), f"bloodpool{seed}")


def run(size_w=32, size_h=128):
    """A drip running down: thin stream that wanders slightly, a bead at the tip."""
    random.seed(7)
    img = Image.new("L", (size_w, size_h), 0)
    p = img.load()
    cx = size_w / 2
    bead_y = size_h * 0.88
    for y in range(size_h):
        t = y / size_h
        # stream centre wanders a little
        x0 = cx + math.sin(t * 7.0) * 1.2 + math.sin(t * 17.0) * 0.5
        # wider where it started, thinning as it runs
        half = 5.5 - 3.2 * min(1.0, t / 0.6)
        for x in range(size_w):
            dx = abs(x - x0)
            v = 0.0
            if y < bead_y and dx < half:
                v = 1.0 - (dx / half) ** 2 * 0.45       # thicker (darker) down the middle
            # bead at the tip
            bd = math.hypot((x - x0) / 1.0, (y - bead_y) / 1.25)
            if bd < 4.6:
                v = max(v, 1.0 - (bd / 4.6) ** 3 * 0.4)
            p[x, y] = int(255 * min(1.0, v))
    # the top blends into the splat it came from
    for y in range(6):
        for x in range(size_w):
            p[x, y] = int(p[x, y] * (y / 6.0))
    img = img.filter(ImageFilter.GaussianBlur(0.7))
    save(clear_border(img, 1), "bloodrun")


def footprint(w=32, h=64):
    """A boot print: sole at the top (toe forward = up), heel at the bottom,
    with a little tread texture so it doesn't read as a solid blob."""
    random.seed(11)
    img = Image.new("L", (w, h), 0)
    p = img.load()
    for y in range(h):
        for x in range(w):
            v = 0.0
            # sole: rounded, slightly narrower at the toe
            sy = (y - 20) / 17.0
            sx = (x - 16) / (9.5 - 1.5 * max(0.0, -sy))
            if sx * sx + sy * sy < 1.0:
                v = 1.0
            # heel
            hy = (y - 50) / 9.0
            hx = (x - 16) / 8.0
            if hx * hx + hy * hy < 1.0 or (abs(x - 16) < 8 and 44 <= y <= 55):
                v = 1.0
            # tread: horizontal grooves
            if v and (y % 6 in (0, 1)):
                v = 0.45
            if v:
                v *= 0.75 + 0.25 * random.random()
            p[x, y] = int(255 * v)
    img = img.filter(ImageFilter.GaussianBlur(0.8))
    save(clear_border(img, 1), "footprint")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    pool(1)
    pool(2)
    run()
    footprint()
