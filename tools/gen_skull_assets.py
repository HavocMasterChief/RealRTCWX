#!/usr/bin/env python3
"""
Assets for the Skulls (code/cgame/cg_xbskulls.c):
  misc/uwp/xbox/gfx/xbox/newsreel_grain.tga     film grain (tiles)
  misc/uwp/xbox/gfx/xbox/newsreel_scratch.tga   a vertical film scratch
  misc/uwp/xbox/gfx/xbox/newsreel_vignette.tga  dark corners
  misc/uwp/xbox/sound/xbox/party_horn.wav       party blower (Party Pooper)
  misc/uwp/xbox/sound/xbox/projector.wav        projector rattle loop (Newsreel)
"""
import math
import os
import random
import struct
import wave
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GFX = os.path.join(ROOT, "misc", "uwp", "xbox", "gfx", "xbox")
SND = os.path.join(ROOT, "misc", "uwp", "xbox", "sound", "xbox")
RATE = 22050


def textures():
    os.makedirs(GFX, exist_ok=True)
    rnd = random.Random(5)
    # grain: dark and light specks (alpha), mostly clear
    g = Image.new("RGBA", (256, 256))
    p = g.load()
    for y in range(256):
        for x in range(256):
            v = rnd.gauss(0, 1)
            if v > 1.6:
                p[x, y] = (235, 225, 200, int(min(255, (v - 1.6) * 140)))
            elif v < -1.4:
                p[x, y] = (10, 8, 5, int(min(255, (-v - 1.4) * 150)))
            else:
                p[x, y] = (0, 0, 0, 0)
    g.save(os.path.join(GFX, "newsreel_grain.tga"), compression="tga_rle")
    # scratch: a thin wavering bright line
    s = Image.new("RGBA", (16, 256))
    p = s.load()
    for y in range(256):
        cx = 8 + 1.5 * math.sin(y * 0.05)
        for x in range(16):
            a = max(0.0, 1 - abs(x - cx) / 1.6) * (0.6 + 0.4 * rnd.random())
            p[x, y] = (240, 235, 220, int(255 * a))
    s.save(os.path.join(GFX, "newsreel_scratch.tga"), compression="tga_rle")
    # vignette: black, alpha growing to the corners
    v = Image.new("RGBA", (256, 256))
    p = v.load()
    for y in range(256):
        for x in range(256):
            d = math.hypot((x - 127.5) / 128, (y - 127.5) / 128)
            a = max(0.0, min(1.0, (d - 0.55) / 0.75)) ** 1.6
            p[x, y] = (0, 0, 0, int(255 * a))
    v.save(os.path.join(GFX, "newsreel_vignette.tga"), compression="tga_rle")


def write(name, samples):
    os.makedirs(SND, exist_ok=True)
    peak = max(1e-6, max(abs(s) for s in samples))
    with wave.open(os.path.join(SND, name), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(32000 * 0.85 * s / peak)) for s in samples))


def party_horn():
    # a paper blower: a reedy buzz that rises as it unrolls, then a squeak
    out = []
    n = int(RATE * 1.1)
    ph = 0.0
    rnd = random.Random(3)
    for i in range(n):
        t = i / RATE
        f = 330 + 160 * min(1, t / 0.25) + 40 * math.sin(t * 30)
        if t > 0.85:
            f += (t - 0.85) * 1400
        ph += 2 * math.pi * f / RATE
        saw = 2 * ((ph / (2 * math.pi)) % 1) - 1
        buzz = math.copysign(abs(math.sin(ph)) ** 0.3, math.sin(ph))
        env = min(1, t / 0.04) * (1 if t < 0.9 else max(0, 1 - (t - 0.9) / 0.2))
        out.append(env * (0.5 * saw + 0.5 * buzz) + 0.03 * rnd.uniform(-1, 1) * env)
    return out


def projector():
    # 2 s seamless loop: 24 shutter clicks a second over a motor hum
    n = RATE * 2
    out = [0.0] * n
    rnd = random.Random(9)
    for i in range(n):
        t = i / RATE
        out[i] = 0.10 * math.sin(2 * math.pi * 60 * t) + 0.05 * math.sin(2 * math.pi * 120 * t) + 0.04 * rnd.uniform(-1, 1)
    for k in range(48):
        start = int(k * n / 48)
        amp = 0.6 + 0.2 * rnd.random()
        for j in range(int(RATE * 0.006)):
            out[(start + j) % n] += amp * rnd.uniform(-1, 1) * (1 - j / (RATE * 0.006))
    return out


if __name__ == "__main__":
    textures()
    write("party_horn.wav", party_horn())
    write("projector.wav", projector())
    print("skull assets written")
