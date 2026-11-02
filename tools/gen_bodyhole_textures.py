#!/usr/bin/env python3
"""
Bullet holes on characters (misc/uwp/xbox/gore/bodyhole*.tga), drawn by the
renderer on the skin where a bullet hit (see R_ApplyWounds, tr_animation.c).

Absorption textures like the other gore (dst * (1 - tex)): the hole is lit
like the skin or cloth under it. A dark, almost black puncture with a ragged
blood-soaked ring around it; a few variants so holes don't look stamped.
"""
import math
import os
import random
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "misc", "uwp", "xbox", "gore")
SIZE = 64

# fraction of each channel taken away: hole core, then the bloody ring
CORE = {"": (0.92, 0.97, 0.97), "_green": (0.97, 0.90, 0.97), "_dust": (0.85, 0.85, 0.85)}
RING = {"": (0.35, 0.90, 0.88), "_green": (0.88, 0.45, 0.90), "_dust": (0.45, 0.50, 0.55)}


def make(seed):
    rnd = random.Random(seed)
    core = Image.new("L", (SIZE, SIZE))
    ring = Image.new("L", (SIZE, SIZE))
    c, r = core.load(), ring.load()
    lobes = [(rnd.uniform(0, 6.28), rnd.uniform(0.03, 0.10), rnd.randint(3, 9)) for _ in range(4)]
    for y in range(SIZE):
        for x in range(SIZE):
            dx, dy = (x - SIZE / 2 + 0.5) / (SIZE / 2), (y - SIZE / 2 + 0.5) / (SIZE / 2)
            d = math.hypot(dx, dy)
            a = math.atan2(dy, dx)
            wob = 1 + sum(amp * math.sin(n * a + ph) for ph, amp, n in lobes) * 0.5
            # puncture: hard-edged, slightly torn
            cr = 0.16 * (1 + 0.15 * math.sin(7 * a + seed))
            c[x, y] = 255 if d < cr else int(255 * max(0, 1 - (d - cr) / 0.06))
            # soaked ring: soft, ragged, fading out
            rr = 0.55 * wob
            v = max(0.0, 1 - d / rr) ** 0.9
            v = min(1.0, v * 1.4) * ( 0.8 + 0.2 * rnd.random() )
            # darker bruised ring right around the puncture
            v = min(1.0, v + 0.5 * math.exp(-((d - 0.22) / 0.07) ** 2))
            r[x, y] = int(255 * min(1, v))
    ring = ring.filter(ImageFilter.GaussianBlur(1.2))
    core = core.filter(ImageFilter.GaussianBlur(0.6))
    return core, ring


def main():
    os.makedirs(OUT, exist_ok=True)
    for i in range(3):
        core, ring = make(17 + i * 31)
        cp, rp = core.load(), ring.load()
        for suffix in CORE:
            img = Image.new("RGBA", (SIZE, SIZE))
            p = img.load()
            for y in range(SIZE):
                for x in range(SIZE):
                    k, g = cp[x, y] / 255, rp[x, y] / 255
                    # border pixels clear (clampmap)
                    if x in (0, SIZE - 1) or y in (0, SIZE - 1):
                        k = g = 0
                    px = [min(1, CORE[suffix][ch] * k + RING[suffix][ch] * g * (1 - k)) for ch in range(3)]
                    p[x, y] = tuple(int(255 * v) for v in px) + (255,)
            img.save(os.path.join(OUT, f"bodyhole{i + 1}{suffix}.tga"), compression="tga_rle")
    print("wrote bodyhole textures")


if __name__ == "__main__":
    main()
