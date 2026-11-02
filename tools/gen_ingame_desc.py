#!/usr/bin/env python3
"""
In-game menu: the stock highlight pictures (ui/assets/realrtcw_ingame_*.tga)
carry each button's description baked in near the bottom of the screen, where
it runs into the Xbox CHEATS / ACHIEVEMENTS buttons. This writes Xbox
overrides of those pictures without the description, and the description on
its own (ui/assets/xbox_desc_*.tga) for gen_xbox_menus.py to place to the
right of the buttons. Source: sdk/buttons templates/.
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "sdk", "buttons templates")
DST = os.path.join(ROOT, "misc", "uwp", "xbox", "ui", "assets")
NAMES = ["save", "load", "controls", "game", "system", "main", "quit"]
CUT = 1650      # template rows below this are the description


def main():
    os.makedirs(DST, exist_ok=True)
    for n in NAMES:
        im = Image.open(os.path.join(SRC, f"realrtcw_ingame_{n}.png")).convert("RGBA")
        top = im.copy()
        top.paste((0, 0, 0, 0), (0, CUT, im.width, im.height))
        top.resize((1024, 1024), Image.LANCZOS).save(os.path.join(DST, f"realrtcw_ingame_{n}.tga"), compression="tga_rle")

        desc = im.crop((0, CUT, im.width, im.height))
        x0, y0, x1, y1 = desc.split()[-1].getbbox()
        desc = desc.crop((x0 - 4, y0 - 4, x1 + 8, y1 + 8))
        h = 32
        w = round(desc.width * h / desc.height)
        out = Image.new("RGBA", (512, 32), (0, 0, 0, 0))
        out.paste(desc.resize((min(w, 512), h), Image.LANCZOS), (0, 0))
        out.save(os.path.join(DST, f"xbox_desc_{n}.tga"), compression="tga_rle")
        print(n, w)


if __name__ == "__main__":
    main()
