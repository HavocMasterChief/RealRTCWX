#!/usr/bin/env python3
"""
Draws the controller layout picture shown on the Xbox build's Controls screen
(misc/uwp/xbox/ui/xbox_controller.tga). It's an original, generic gamepad
drawing (no console logos), labelled with RealRTCW's default pad bindings
from main/realrtcwdefault.cfg. Re-run it if those bindings change.
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "misc", "uwp", "xbox", "ui", "xbox_controller.tga")
K = 2                       # supersample
W, H = 1024, 512
FONT_B = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
FONT_R = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"

BODY = (52, 56, 60, 255)
BODY_EDGE = (120, 126, 132, 255)
PART = (30, 32, 35, 255)
LINE = (190, 200, 170, 255)
TEXT = (240, 240, 235, 255)
KEY = (150, 210, 110, 255)      # binding name colour (menu green)


def s(v):
    return [int(x * K) for x in v]


def main():
    img = Image.new("RGBA", (W * K, H * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fb = ImageFont.truetype(FONT_B, 19 * K)
    fr = ImageFont.truetype(FONT_R, 19 * K)
    fbtn = ImageFont.truetype(FONT_B, 15 * K)

    # --- body: triggers, bumpers, grips, main shell
    for x0 in (366, 588):
        d.rounded_rectangle(s([x0, 98, x0 + 70, 132]), radius=12 * K, fill=PART, outline=BODY_EDGE, width=2 * K)
    for x0 in (348, 572):
        d.rounded_rectangle(s([x0, 128, x0 + 104, 152]), radius=10 * K, fill=PART, outline=BODY_EDGE, width=2 * K)
    shell = Image.new("L", img.size, 0)
    sd = ImageDraw.Draw(shell)
    sd.rounded_rectangle(s([330, 145, 694, 300]), radius=70 * K, fill=255)
    sd.ellipse(s([292, 190, 446, 410]), fill=255)
    sd.ellipse(s([578, 190, 732, 410]), fill=255)
    edge = shell.filter(ImageFilter.MaxFilter(5 * K + 1))
    img.paste(Image.new("RGBA", img.size, BODY_EDGE), (0, 0), edge)
    img.paste(Image.new("RGBA", img.size, BODY), (0, 0), shell)
    d = ImageDraw.Draw(img)

    # sticks
    for cx, cy in ((400, 215), (590, 292)):
        d.ellipse(s([cx - 34, cy - 34, cx + 34, cy + 34]), fill=PART, outline=BODY_EDGE, width=2 * K)
        d.ellipse(s([cx - 22, cy - 22, cx + 22, cy + 22]), fill=(70, 74, 78, 255))
    # d-pad
    cx, cy = 450, 292
    d.ellipse(s([cx - 30, cy - 30, cx + 30, cy + 30]), fill=PART)
    d.rectangle(s([cx - 9, cy - 24, cx + 9, cy + 24]), fill=(80, 84, 88, 255))
    d.rectangle(s([cx - 24, cy - 9, cx + 24, cy + 9]), fill=(80, 84, 88, 255))
    # small centre buttons + a plain home button
    for bx in (482, 542):
        d.ellipse(s([bx - 9, 206, bx + 9, 224]), fill=PART, outline=BODY_EDGE, width=1 * K)
    d.ellipse(s([497, 160, 527, 190]), fill=PART, outline=BODY_EDGE, width=2 * K)
    # face buttons
    face = {"Y": (640, 183, (200, 170, 60)), "X": (610, 214, (70, 120, 200)),
            "B": (670, 214, (190, 70, 60)), "A": (640, 245, (90, 170, 70))}
    for name, (bx, by, col) in face.items():
        d.ellipse(s([bx - 15, by - 15, bx + 15, by + 15]), fill=PART, outline=col + (255,), width=3 * K)
        tw = d.textlength(name, font=fbtn)
        d.text((bx * K - tw / 2, (by - 9) * K), name, font=fbtn, fill=col + (255,))

    # --- callouts: (button, action, target)
    left = [
        ("LT", "Aim / Alt Fire", (402, 112)),
        ("LB", "Weapon Wheel (hold)", (380, 140)),
        ("LS", "Move  |  Click: Sprint", (368, 215)),
        ("D-Pad Up", "Use Item", (450, 266)),
        ("D-Pad L / R", "Change Item", (422, 294)),
        ("D-Pad Down", "Binoculars", (450, 320)),
    ]
    right = [
        ("RT", "Fire", (622, 112)),
        ("RB", "Quick Grenade", (644, 140)),
        ("Y", "Use / Activate", (654, 178)),
        ("B", "Crouch", (685, 218)),
        ("A", "Jump", (651, 256)),
        ("X", "Reload", (603, 227)),
        ("RS", "Look  |  Click: Kick", (614, 316)),
    ]

    def column(items, side):
        n = len(items)
        top, bottom = 50, 470
        for i, (btn, act, (tx, ty)) in enumerate(items):
            y = top + (bottom - top) * i / (n - 1)
            if side == "L":
                ex = 268
                d.text((ex * K - d.textlength(btn, font=fb), (y - 22) * K), btn, font=fb, fill=KEY)
                d.text((ex * K - d.textlength(act, font=fr), (y + 1) * K), act, font=fr, fill=TEXT)
                pts = [(ex + 8, y), (300, y), (tx, ty)]
            else:
                ex = 756
                d.text((ex * K, (y - 22) * K), btn, font=fb, fill=KEY)
                d.text((ex * K, (y + 1) * K), act, font=fr, fill=TEXT)
                pts = [(ex - 8, y), (724, y), (tx, ty)]
            d.line([(px * K, py * K) for px, py in pts], fill=LINE, width=2 * K, joint="curve")
            d.ellipse(s([tx - 3, ty - 3, tx + 3, ty + 3]), fill=LINE)

    # View / Menu: labels above the controller, lines straight down
    for bx, btn, act, align in ((482, "View", "Dossier", "R"), (542, "Menu", "Pause", "L")):
        d.line(s([bx, 205, bx, 70]), fill=LINE, width=2 * K)
        d.ellipse(s([bx - 3, 202, bx + 3, 208]), fill=LINE)
        for text, font, col, yy in ((btn, fb, KEY, 22), (act, fr, TEXT, 45)):
            tw = d.textlength(text, font=font)
            x = (bx - 8) * K - tw if align == "R" else (bx + 8) * K
            d.text((x, yy * K), text, font=font, fill=col)

    column(left, "L")
    column(right, "R")

    # weapon wheel tip
    tip = "Weapon wheel: hold LB, pick with the right stick"
    d.text(((W * K - d.textlength(tip, font=fr)) / 2, 476 * K), tip, font=fr, fill=(200, 200, 195, 255))

    img = img.resize((W, H), Image.LANCZOS)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img.save(OUT)
    if os.environ.get("PREVIEW"):           # optional preview on a dark background
        prev = Image.new("RGBA", (W, H), (20, 20, 20, 255))
        prev.alpha_composite(img)
        prev.save(os.environ["PREVIEW"])
    print("wrote", OUT)


if __name__ == "__main__":
    main()
