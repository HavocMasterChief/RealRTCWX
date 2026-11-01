#!/usr/bin/env python3
"""
Generates the gore textures in misc/uwp/xbox/gore/ (see code/cgame/cg_gore.c
and misc/uwp/xbox/scripts/gore.shader).

Pools and runs are drawn with a multiply-style blend
(blendFunc GL_ZERO GL_ONE_MINUS_SRC_COLOR): each texel says how much light
the blood absorbs, so the blood darkens whatever surface it's on and is lit
by the level like the game's own blood decals. Black = no blood. Every
texture's outer rows/columns are black, because the effects "grow" by
stretching the texture with clamped coordinates.
"""
import math
import os
import random
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "misc", "uwp", "xbox", "gore")

# fraction of light the blood lets through, per channel (fully soaked)
RED = (0.30, 0.03, 0.04)
GREEN = (0.07, 0.32, 0.04)


def save(density, name):
    """density: L image (0..255). Writes red and green absorbance versions."""
    w, h = density.size
    d = density.load()
    for suffix, col in (("", RED), ("_green", GREEN)):
        img = Image.new("RGB", (w, h))
        p = img.load()
        for y in range(h):
            for x in range(w):
                v = d[x, y] / 255.0
                p[x, y] = tuple(int(255 * v * (1.0 - c)) for c in col)
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


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    pool(1)
    pool(2)
    run()
