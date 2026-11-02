#!/usr/bin/env python3
"""
Generates the RealRTCWX achievement icon pack: one illustrated icon per
achievement ID in the game code - an RTCW-style riveted iron plate, an
engraved scene for that achievement (parchment and ink, with blood, brass
and other accents), the inner rim coloured by group.

  python3 tools/gen_achievement_icons.py <outdir> [--sheets] [--size N]

Writes <outdir>/<ACH_ID>.png (N x N, default 256, rendered at 2x and
downsampled) and, with --sheets, contact sheets per group for review.
"""
import html
import os
import sys

# ------------------------------------------------------------------ palette
INK = "#1b130e"           # outlines
PAPER = "#eadbb8"         # main fill (aged parchment)
SHADE = "#bfa57c"         # shading
DEEP = "#7d6648"          # deep shading
BLOOD = "#8e1b12"
BLOOD_HI = "#c0392b"
BRASS = "#c9a043"
BRASS_HI = "#f0d27a"
STEEL = "#8b97a0"
STEEL_HI = "#c7d0d6"
CRYSTAL = "#9fd0e6"
NIGHT = "#2b3550"

GROUPS = {
    "chapter":  ("#8a7656", "Chapters"),
    "hard":     ("#a3241a", "Chapters on hard"),
    "combat":   ("#a3241a", "Combat & bosses"),
    "secret":   ("#c9a043", "Secrets & treasure"),
    "stealth":  ("#6f7d33", "Stealth & missions"),
    "weapon":   ("#7f8d97", "Weapons & items"),
    "survival": ("#b0571f", "Survival & Dark Army"),
    "mode":     ("#6c5a92", "Modes & challenges"),
    "malta":    ("#2f7f7f", "Malta"),
    "et":       ("#56688a", "Enemy Territory"),
}

STYLE = f"""
.o{{stroke:{INK};stroke-width:1.6;stroke-linejoin:round;stroke-linecap:round}}
.l{{fill:none;stroke:{INK};stroke-width:1.6;stroke-linejoin:round;stroke-linecap:round}}
.t{{fill:none;stroke:{INK};stroke-width:1;stroke-linecap:round}}
.p{{fill:{PAPER}}}.s{{fill:{SHADE}}}.d{{fill:{DEEP}}}.k{{fill:{INK}}}
.r{{fill:{BLOOD}}}.rh{{fill:{BLOOD_HI}}}.g{{fill:{BRASS}}}.gh{{fill:{BRASS_HI}}}
.st{{fill:{STEEL}}}.sth{{fill:{STEEL_HI}}}.c{{fill:{CRYSTAL}}}.n{{fill:{NIGHT}}}
.w{{fill:#fff8e6}}.gr{{fill:#6f7d33}}.bl{{fill:#3d6fb0}}.ye{{fill:#e2b93b}}
"""

# ------------------------------------------------------------------ elements
# Each is drawn in a 64x64 box. Classes: o = ink outline, p/s/d = parchment
# tones, r = blood, g = brass, st = steel, c = crystal, k = ink fill, l = line.
E = {
    # --- people & creatures
    "soldier": '<path class="o s" d="M22 60v-16c0-6 4-10 10-10s10 4 10 10v16z"/><path class="o p" d="M24 60V46c0-4 3-7 8-7s8 3 8 7v14z"/><circle class="o p" cx="32" cy="27" r="7"/><path class="o st" d="M22 26c0-7 4-12 10-12s10 5 10 12c-3-1-6-2-10-2s-7 1-10 2z"/><path class="o d" d="M20 27h24l-2 2H22z"/>',
    "elite": '<path class="o d" d="M22 60l2-18c1-5 4-8 8-8s7 3 8 8l2 18z"/><path class="o p" d="M27 44h10l2 16H25z"/><circle class="o p" cx="32" cy="26" r="7"/><path class="o k" d="M24 25c0-6 4-10 8-10s8 4 8 10c0 3-1 6-3 7 0-4-2-8-5-8s-5 4-5 8c-2-1-3-4-3-7z"/>',
    "loper": '<path class="o s" d="M10 44c4-12 12-18 24-18 8 0 14 4 18 10l4 2-4 4c-2 6-8 10-16 10H18z"/><path class="o p" d="M40 30c4-4 10-4 14 0l2 6-6 2"/><path class="l" d="M16 52l-4 8M24 52l-2 8M36 52l2 8M44 50l6 8"/><circle class="k" cx="50" cy="33" r="1.6"/><path class="o r" d="M30 30l4-8 4 8z"/><path class="o r" d="M22 32l3-6 3 6z"/>',
    "proto": '<path class="o st" d="M18 60l2-20c1-8 6-12 12-12s11 4 12 12l2 20z"/><path class="o sth" d="M24 40h16v8H24z"/><circle class="o st" cx="32" cy="22" r="9"/><path class="o k" d="M26 20h12v4H26z"/><path class="o d" d="M14 40l6-4v14l-6 2zM50 40l-6-4v14l6 2z"/><circle class="rh" cx="29" cy="22" r="1.4"/><circle class="rh" cx="35" cy="22" r="1.4"/>',
    "uber": '<path class="o st" d="M14 60l4-24c2-8 7-12 14-12s12 4 14 12l4 24z"/><path class="o d" d="M8 34h14v10H8z"/><path class="o d" d="M10 30h6v4h-6z"/><circle class="o st" cx="32" cy="18" r="8"/><path class="o k" d="M26 17h12v3H26z"/><path class="o sth" d="M24 40h16v12H24z"/><path class="l" d="M28 44h8M28 48h8"/>',
    "heinrich": '<path class="o n" d="M12 60l6-28c2-8 7-12 14-12s12 4 14 12l6 28z"/><path class="o st" d="M24 22l2-10 6 6 6-6 2 10z"/><circle class="o p" cx="32" cy="26" r="7"/><path class="o k" d="M28 26h2M34 26h2"/><path class="o d" d="M20 40h24l-2 6H22z"/><path class="o g" d="M30 12l2-4 2 4z"/>',
    "olaric": '<path class="o d" d="M16 60l4-22c2-8 6-12 12-12s10 4 12 12l4 22z"/><circle class="o p" cx="32" cy="22" r="8"/><path class="o s" d="M22 26c2 10 6 14 10 14s8-4 10-14c-2 4-6 6-10 6s-8-2-10-6z"/><path class="o st" d="M24 18c0-6 4-10 8-10s8 4 8 10l-4-2-4 2-4-2z"/>',
    "civilian": '<path class="o s" d="M22 60V44c0-6 4-10 10-10s10 4 10 10v16z"/><circle class="o p" cx="32" cy="26" r="7"/><path class="o d" d="M24 24c0-6 4-9 8-9s8 3 8 9c-2-3-5-4-8-4s-6 1-8 4z"/>',
    "agent": '<path class="o d" d="M20 60l2-18c1-6 5-9 10-9s9 3 10 9l2 18z"/><path class="o p" d="M29 34h6l2 26h-10z"/><path class="o r" d="M31 36h2l1 10-2 2-2-2z"/><circle class="o p" cx="32" cy="25" r="7"/><path class="o k" d="M20 22c4 1 20 1 24 0l-2 3H22z"/><path class="o k" d="M24 21c0-6 3-9 8-9s8 3 8 9z"/>',
    "dog": '<path class="o s" d="M10 46l6-10c2-4 6-6 12-6h12l6-8 4 2-2 8 6 4-4 4h-6l-4 6v12h-6V48H24l-4 10h-6l2-12z"/><circle class="k" cx="47" cy="30" r="1.4"/>',
    "priest": '<path class="o k" d="M18 60l4-22c1-6 5-10 10-10s9 4 10 10l4 22z"/><path class="o w" d="M29 30h6v4h-6z"/><circle class="o p" cx="32" cy="22" r="7"/><path class="o g" d="M30 40h4v14h-4zM26 44h12v4H26z"/>',
    "mummy": '<path class="o p" d="M20 60l2-20c1-8 5-12 10-12s9 4 10 12l2 20z"/><circle class="o p" cx="32" cy="22" r="9"/><path class="t" d="M23 18l18 4M23 24l18-2M22 44l20-4M22 50l20 2M24 56l16-4M23 34l18 2"/><circle class="k" cx="29" cy="22" r="1.4"/><circle class="k" cx="35" cy="22" r="1.4"/>',
    "ghost": '<path class="o w" d="M16 58V30a16 16 0 0 1 32 0v28l-5-4-5 4-6-4-5 4-6-4z"/><ellipse class="k" cx="26" cy="30" rx="3" ry="4"/><ellipse class="k" cx="38" cy="30" rx="3" ry="4"/><ellipse class="k" cx="32" cy="42" rx="3" ry="4"/>',
    "zombie": '<path class="o gr" d="M20 60l2-18c1-6 5-10 10-10s9 4 10 10l2 18z"/><circle class="o gr" cx="32" cy="24" r="8"/><circle class="rh" cx="29" cy="24" r="1.6"/><circle class="rh" cx="35" cy="24" r="1.6"/><path class="l" d="M28 29l2 1 2-1 2 1 2-1"/><path class="o gr" d="M42 40l12-6 2 4-12 6z"/>',
    "goat": '<path class="o s" d="M14 40c0-8 6-12 14-12h12c6 0 10 4 10 10l6 2-4 6h-6c-2 4-6 6-10 6H24l-2 10h-6l2-10c-2-2-4-6-4-12z"/><path class="o d" d="M44 26c2-6 6-8 10-8-2 2-4 6-4 10M40 26c0-6 2-10 6-12-1 3-1 7 0 10"/><path class="o p" d="M50 44l2 6-4-2z"/><circle class="k" cx="47" cy="34" r="1.4"/>',
    "firefly": '<ellipse class="o d" cx="32" cy="32" rx="5" ry="10"/><ellipse class="o ye" cx="32" cy="42" rx="4" ry="4"/><path class="o w" d="M27 28c-10-8-18-6-18-1s8 7 18 4zM37 28c10-8 18-6 18-1s-8 7-18 4z" opacity="0.9"/><path class="l" d="M30 22l-4-8M34 22l4-8"/><circle fill="#f6e27a" opacity="0.35" cx="32" cy="42" r="12"/>',
    "bartender": '<path class="o p" d="M22 60V42c0-6 4-10 10-10s10 4 10 10v18z"/><path class="o w" d="M26 40h12v20H26z"/><circle class="o p" cx="32" cy="24" r="7"/><path class="o k" d="M26 28c2 3 10 3 12 0l-1 3c-3 2-7 2-10 0z"/><path class="o k" d="M25 22c1-6 4-8 7-8s6 2 7 8c-2-2-4-3-7-3s-5 1-7 3z"/>',
    # --- weapons & gear
    "panzerfaust": '<path class="o gr" d="M4 38l40-10 2 6-40 10z"/><path class="o d" d="M42 24c6-2 12 0 14 4s-2 8-8 10l-4-8z"/><path class="o st" d="M14 40l2 8 4-1-2-8z"/><path class="o d" d="M24 36l1 4 3-1-1-4z"/>',
    "rocket": '<path class="o st" d="M32 4c9 6 11 18 9 34H23c-2-16 0-28 9-34z"/><path class="o sth" d="M32 4c5 4 7 10 7 18H32z" opacity="0.7"/><path class="o r" d="M23 38l-7 12h9zM41 38l7 12h-9z"/><path class="o d" d="M27 38h10v8H27z"/><circle class="o n" cx="32" cy="22" r="4"/><path class="o ye" d="M28 46h8l-4 14z"/>',
    "knife": '<path class="o sth" d="M48 6c4 2 6 6 4 10L26 44l-6-6L46 10z"/><path class="t" d="M44 14L26 34"/><path class="o d" d="M20 38l6 6-4 4-6-6z"/><path class="o k" d="M16 42l6 6-8 10-6-6z"/><path class="o r" d="M30 30c1 4 2 6 0 8-2-2-1-4 0-8z"/>',
    "boot": '<path class="o d" d="M20 6h18v28l16 8c3 2 4 5 4 8v6H12V40l8-6z"/><path class="o k" d="M12 52h46v6H12z"/><path class="t" d="M22 14h14M22 20h14M22 26h14"/><path class="o st" d="M12 52h46"/>',
    "rifle": '<path class="o d" d="M4 36l26-2 4 4-4 4-8 1-6 10H8l4-10H4z"/><path class="o st" d="M30 34h28v4H30z"/><path class="o k" d="M34 30h6v4h-6z"/><path class="o st" d="M24 33h6v-3h-4z"/>',
    "mosin": '<path class="o d" d="M4 34h30l2 4-2 4H22l-6 10H8l4-10H4z"/><path class="o st" d="M34 34h26v3H34z"/><path class="o k" d="M28 30h8v4h-8z"/><path class="o r" d="M44 20l2 4 4 1-3 3 1 4-4-2-4 2 1-4-3-3 4-1z"/>',
    "ppsh": '<path class="o d" d="M4 30h28v8H18l-4 12H6l4-12H4z"/><path class="o st" d="M32 29h26v6H32z"/><path class="t" d="M36 30v4M40 30v4M44 30v4M48 30v4M52 30v4"/><circle class="o k" cx="26" cy="44" r="8"/><circle class="o st" cx="26" cy="44" r="4"/><path class="o r" d="M48 14l2 4 4 1-3 3 1 4-4-2-4 2 1-4-3-3 4-1z"/>',
    "browning": '<path class="o k" d="M8 28h36v10H8z"/><path class="o st" d="M44 30h16v4H44z"/><path class="o d" d="M20 38l-4 14h-6l4-14zM30 38l4 14h6l-4-14z"/><path class="o g" d="M16 18h2v10h-2zM20 18h2v10h-2zM24 18h2v10h-2zM28 18h2v10h-2z"/><path class="o st" d="M10 24h30v4H10z"/>',
    "m7": '<path class="o d" d="M4 36h30l2 4-2 4H22l-6 10H8l4-10H4z"/><path class="o st" d="M34 36h14v4H34z"/><path class="o k" d="M46 32h12v12H46z"/><path class="o gr" d="M52 22c3 0 5 3 5 6v4h-10v-4c0-3 2-6 5-6z"/>',
    "railgun": '<path class="o st" d="M4 28h30l6 6-6 6H4z"/><path class="o k" d="M8 32h20v4H8z"/><path class="o bl" d="M40 32h20v4H40z"/><path fill="#9fd0ff" opacity="0.5" d="M40 28h22v12H40z"/><circle class="o c" cx="14" cy="34" r="3"/>',
    "tesla": '<path class="o st" d="M10 30h24v12H10z"/><circle class="o sth" cx="40" cy="36" r="8"/><path class="l" d="M14 30v-4M20 30v-4M26 30v-4"/><path stroke="#9fd0ff" stroke-width="2.4" fill="none" stroke-linejoin="round" d="M46 30l6-8 2 6 6-6M48 40l6 4 2-4 6 6M46 36h8l2-4"/>',
    "grenade": '<path class="o d" d="M22 22h20v6H22z"/><path class="o gr" d="M24 28h16v22c0 4-4 8-8 8s-8-4-8-8z"/><path class="o st" d="M28 14h8v8h-8z"/><path class="l" d="M36 16c6-2 10 0 12 4"/><path class="t" d="M24 38h16M24 46h16"/>',
    "stick": '<path class="o d" d="M30 22h4v36h-4z"/><path class="o gr" d="M24 6h16v18H24z"/><path class="t" d="M24 12h16"/>',
    "gascan": '<path class="o gr" d="M20 18h24v34c0 4-4 6-12 6s-12-2-12-6z"/><path class="o d" d="M24 10h16v8H24z"/><path class="o ye" d="M20 30h24v8H20z"/><path fill="#c7d8a0" opacity="0.7" d="M40 14c8-4 16 0 16 8 4 2 4 10-4 10-2 6-12 6-14 0z"/>',
    "gasmask": '<path class="o d" d="M32 8C20 8 14 16 14 28c0 10 6 18 18 18s18-8 18-18C50 16 44 8 32 8z"/><circle class="o c" cx="24" cy="26" r="6"/><circle class="o c" cx="40" cy="26" r="6"/><path class="o st" d="M26 38h12v12c0 3-3 5-6 5s-6-2-6-5z"/><path class="t" d="M28 44h8M28 48h8"/>',
    "plane": '<path class="o st" d="M30 4h4l2 16 22 8v6l-22-2-2 18 8 6v4l-10-2-10 2v-4l8-6-2-18-22 2v-6l22-8z"/><path class="o d" d="M30 4h4l1 10h-6z"/>',
    "smoke": '<circle class="o p" cx="24" cy="44" r="10"/><circle class="o s" cx="36" cy="34" r="12"/><circle class="o p" cx="30" cy="22" r="9"/><path class="o r" d="M28 52h8v8h-8z"/>',
    "cross": '<path class="o g" d="M27 6h10v14h14v10H37v28H27V30H13V20h14z"/><path class="gh" d="M29 8h3v48h-3z" opacity="0.6"/><circle fill="#f6e27a" opacity="0.25" cx="32" cy="25" r="22"/>',
    "emp": '<circle class="o st" cx="32" cy="32" r="18"/><circle class="o k" cx="32" cy="32" r="12"/><path fill="#9fd0ff" class="o" d="M34 18l-10 16h8l-4 14 12-18h-8z"/><path stroke="#9fd0ff" stroke-width="1.6" fill="none" d="M10 20c-4 8-4 16 0 24M54 20c4 8 4 16 0 24"/>',
    "shield": '<path class="o st" d="M32 6l20 7v16c0 13-8 22-20 28-12-6-20-15-20-28V13z"/><path class="o bl" d="M32 12l14 5v12c0 9-6 16-14 21z"/><path class="sth" d="M18 16l14-5v6l-10 4z" opacity="0.6"/>',
    "syringe": '<path class="o sth" d="M40 14l10 10-24 24-8 2 2-8z"/><path class="o r" d="M36 22l6 6-14 14-6-6z" opacity="0.85"/><path class="o d" d="M44 8l12 12-4 4-12-12z"/><path class="l" d="M14 50l-6 6"/>',
    "bandage": '<g transform="rotate(-35 32 32)"><rect class="o p" x="8" y="22" width="48" height="20" rx="10"/><rect class="o s" x="25" y="24" width="14" height="16"/><path class="t" d="M28 28v8M32 28v8M36 28v8"/></g><path class="o r" d="M30 30l4 2-2 4z"/>',
    "binocs": '<circle class="o d" cx="18" cy="42" r="12"/><circle class="o d" cx="46" cy="42" r="12"/><circle class="o c" cx="18" cy="42" r="7"/><circle class="o c" cx="46" cy="42" r="7"/><path class="o k" d="M12 30l4-16h10l2 16zM36 30l2-16h10l4 16z"/><path class="o st" d="M28 32h8v6h-8z"/>',
    "scope": '<circle class="o k" cx="32" cy="32" r="24"/><circle fill="#3b4a3a" cx="32" cy="32" r="20"/><path stroke="#e8d9b5" stroke-width="1.4" d="M32 12v40M12 32h40"/><circle class="o p" cx="32" cy="27" r="5"/><path class="o p" d="M24 46c0-6 4-10 8-10s8 4 8 10z"/><path class="o r" d="M30 24h4v4h-4z"/>',
    "orbs": '<circle class="o rh" cx="18" cy="42" r="10"/><circle class="o bl" cx="46" cy="42" r="10"/><circle class="o ye" cx="32" cy="20" r="10"/><circle class="w" cx="15" cy="39" r="3" opacity="0.6"/><circle class="w" cx="43" cy="39" r="3" opacity="0.6"/><circle class="w" cx="29" cy="17" r="3" opacity="0.6"/>',
    # --- places & objects
    "castle": '<path class="o s" d="M10 60V28h8v-6h6v6h6v-10h4v10h6v-6h6v6h8v32z"/><path class="o d" d="M28 60V46a4 4 0 0 1 8 0v14z"/><path class="o k" d="M16 36h4v6h-4zM44 36h4v6h-4z"/><path class="o r" d="M30 10l8 3-8 3z"/><path class="l" d="M30 10v10"/>',
    "crypt": '<path class="o s" d="M8 60V30l24-18 24 18v30z"/><path class="o k" d="M24 60V42a8 8 0 0 1 16 0v18z"/><path class="o p" d="M28 18h8v4h-8zM30 14h4v12h-4z"/>',
    "factory": '<path class="o s" d="M6 60V34l12-8v8l12-8v8l12-8v26h16v-30h-8v30"/><path class="o d" d="M46 30h8v30h-8z"/><path class="o k" d="M12 44h6v6h-6zM24 44h6v6h-6zM36 44h6v6h-6z"/><circle class="o p" cx="50" cy="20" r="5"/><circle class="o s" cx="54" cy="12" r="4"/>',
    "lab": '<path class="o c" d="M26 8h12M28 8v16L14 50c-2 4 0 8 4 8h28c4 0 6-4 4-8L36 24V8" opacity="0.9"/><path class="o gr" d="M18 44h28l4 8c1 3-1 6-4 6H18c-3 0-5-3-4-6z"/><circle class="w" cx="26" cy="50" r="2"/><circle class="w" cx="34" cy="46" r="1.6"/><path class="o d" d="M24 8h16v4H24z"/>',
    "chateau": '<path class="o s" d="M6 60V32l10-10 10 10v-8l6-6 6 6v8l10-10 10 10v28z"/><path class="o k" d="M28 60V48a4 4 0 0 1 8 0v12z"/><path class="o d" d="M12 38h6v6h-6zM46 38h6v6h-6zM29 30h6v6h-6z"/><path class="o n" d="M16 22l-4-8 4-6 4 6zM48 22l-4-8 4-6 4 6z"/>',
    "dark": '<circle fill="#4a1a5a" opacity="0.5" cx="32" cy="32" r="26"/><path class="o k" d="M14 60l6-26c2-8 6-12 12-12s10 4 12 12l6 26z"/><circle class="rh" cx="28" cy="32" r="2"/><circle class="rh" cx="36" cy="32" r="2"/><path class="o st" d="M22 22l2-10 8 6 8-6 2 10z"/>',
    "bridge": '<path class="o s" d="M4 30h22l4 6-4 4H4z"/><path class="o s" d="M38 34l6-4h16v10H42z"/><path class="o d" d="M8 40v20M18 40v20M50 40v20M56 40v20"/><path class="o p" d="M30 38l4-2 2 4-4 2zM34 46l3-1 1 3-3 1z"/><path class="l" d="M28 30l-2-6M40 28l2-6"/>',
    "fallman": '<circle class="o st" cx="40" cy="44" r="6"/><path class="o st" d="M32 30l10 6-4 6-10-6z"/><path class="l" d="M30 32l-6-6M34 30l-2-8M42 48l6 6M38 50l-2 8"/><path class="t" d="M44 26v-8M50 30v-6M38 22v-8"/>',
    "rocks": '<path class="o s" d="M8 60l6-14 10 4 6-10 10 8 8-6 8 18z"/><path class="o d" d="M18 28l6-4 4 6-6 4zM36 18l6-2 2 6-6 2zM44 32l5-2 2 5-5 2z"/><path class="t" d="M20 22v-6M40 12v-6M48 26v-6"/>',
    "crown": '<path class="o g" d="M10 48l4-26 10 12 8-18 8 18 10-12 4 26z"/><path class="o gh" d="M10 48h44v8H10z"/><circle class="o rh" cx="32" cy="38" r="3"/><circle class="o bl" cx="20" cy="40" r="2.4"/><circle class="o bl" cx="44" cy="40" r="2.4"/>',
    "stopwatch": '<circle class="o sth" cx="32" cy="36" r="20"/><circle class="o w" cx="32" cy="36" r="16"/><path class="o st" d="M28 8h8v6h-8z"/><path class="o st" d="M46 16l4-4 4 4-4 4z"/><path class="l" d="M32 36l8-9"/><circle class="k" cx="32" cy="36" r="2"/>',
    "keyhole": '<path class="o g" d="M32 6l22 10v18c0 12-10 20-22 24-12-4-22-12-22-24V16z"/><circle class="o k" cx="32" cy="28" r="6"/><path class="o k" d="M29 32l-3 14h12l-3-14z"/><path class="gh" d="M14 18l18-8v4l-14 6z" opacity="0.6"/>',
    "lever": '<path class="o s" d="M10 52h44v8H10z"/><path class="o d" d="M28 46h8v6h-8z"/><path class="o st" d="M31 48l-14-30 4-2 14 30z"/><circle class="o rh" cx="18" cy="16" r="5"/>',
    "gold": '<path class="o g" d="M8 52l6-10h16l6 10z"/><path class="o g" d="M28 52l6-10h16l6 10z"/><path class="o g" d="M18 40l6-10h16l6 10z"/><path class="gh" d="M10 50l4-6h14l-2 4H12zM30 50l4-6h14l-2 4H32zM20 38l4-6h14l-2 4H22z" opacity="0.7"/>',
    "chest": '<path class="o d" d="M8 30h48v26H8z"/><path class="o s" d="M8 30c0-10 10-16 24-16s24 6 24 16z"/><path class="o g" d="M8 34h48v4H8zM28 30h8v12h-8z"/><path class="o g" d="M14 22l4-6h4l-2 8zM40 16h6l4 6-6 2z"/><circle fill="#f6e27a" opacity="0.3" cx="32" cy="22" r="14"/>',
    "skullgem": '<path class="o c" d="M32 6c-12 0-20 8-20 19 0 6 3 11 8 14v9h24v-9c5-3 8-8 8-14C52 14 44 6 32 6z"/><path class="t" d="M20 18l12 6 12-6M32 24v24M14 28l10 2M50 28l-10 2"/><path class="o n" d="M22 26a4 4 0 1 0 8 0 4 4 0 1 0-8 0zM34 26a4 4 0 1 0 8 0 4 4 0 1 0-8 0z"/><path class="o n" d="M30 36l2-4 2 4z"/><path class="w" d="M20 12l6-3-2 6z" opacity="0.8"/>',
    "wine": '<path class="o r" d="M18 6h8v10c5 3 7 7 7 12v28H11V28c0-5 2-9 7-12z"/><path class="o p" d="M11 34h22v12H11z"/><path class="t" d="M14 38h16M14 42h12"/><path class="o w" d="M38 22h16c0 10-4 14-8 14s-8-4-8-14z" opacity="0.9"/><path class="o r" d="M39 26h14c-1 6-4 8-7 8s-6-2-7-8z"/><path class="l" d="M46 36v14M40 54h12"/>',
    "book": '<path class="o p" d="M8 14c8-3 16-2 24 2 8-4 16-5 24-2v38c-8-3-16-2-24 2-8-4-16-5-24-2z"/><path class="o s" d="M32 16v38"/><path class="t" d="M14 22h12M14 28h12M14 34h10M38 22h12M38 28h12M38 34h10"/><path class="o r" d="M42 10h6v16l-3-3-3 3z"/>',
    "wand": '<path class="o d" d="M10 54l30-30 4 4-30 30z"/><path class="o p" d="M40 24l4 4 4-4-4-4z"/><path class="o ye" d="M46 8l2 6 6 2-6 2-2 6-2-6-6-2 6-2z"/><path class="o ye" d="M52 30l1 3 3 1-3 1-1 3-1-3-3-1 3-1zM26 10l1 3 3 1-3 1-1 3-1-3-3-1 3-1z"/>',
    "elevator": '<path class="o st" d="M10 6h44v52H10z"/><path class="o sth" d="M14 10h17v44H14zM33 10h17v44H33z"/><path class="t" d="M14 16h17M14 22h17M14 28h17M14 34h17M14 40h17M14 46h17M33 16h17M33 22h17M33 28h17M33 34h17M33 40h17M33 46h17"/><path class="o ye" d="M28 2h8v4h-8z"/>',
    "pixel": '<path class="k" d="M20 16h24v4H20zM16 20h32v20H16zM20 40h24v4H20z"/><path class="p" d="M20 20h24v18H20z"/><path class="k" d="M24 26h4v4h-4zM36 26h4v4h-4zM28 34h8v2h-8z"/><path class="bl" d="M16 44h32v12H16z"/><path class="k" d="M30 44h4v12h-4z"/>',
    "safe": '<path class="o st" d="M8 10h48v42H8z"/><path class="o sth" d="M12 14h40v34H12z"/><circle class="o st" cx="32" cy="31" r="10"/><circle class="o k" cx="32" cy="31" r="3"/><path class="t" d="M32 21v3M32 38v3M22 31h3M39 31h3"/><path class="o d" d="M12 52h8v6h-8zM44 52h8v6h-8z"/><path class="o g" d="M48 26h4v10h-4z"/>',
    "vault": '<circle class="o st" cx="32" cy="32" r="26"/><circle class="o sth" cx="32" cy="32" r="20"/><circle class="o st" cx="32" cy="32" r="6"/><path class="o d" d="M32 12v14M32 38v14M12 32h14M38 32h14"/><path class="o g" d="M48 44l8 8-4 4-8-8z"/>',
    "bag": '<path class="o s" d="M16 22h32l6 34H10z"/><path class="o d" d="M24 22c0-8 4-12 8-12s8 4 8 12"/><path class="o g" d="M20 30h24v4H20z"/><text x="32" y="50" text-anchor="middle" font-family="DejaVu Serif" font-weight="700" font-size="14" fill="#1b130e">$</text>',
    "map": '<path class="o p" d="M6 14l16-6 20 6 16-6v42l-16 6-20-6-16 6z"/><path class="o s" d="M22 8v42l20 6V14z"/><path class="l" d="M12 40c6-6 10 4 16-4s10-2 14-10"/><path class="o r" d="M46 22l3 3 3-3-3-3zM44 20l10 10M54 20L44 30"/>',
    "waves": '<path class="o bl" d="M4 34c6-6 10 6 16 0s10 6 16 0 10 6 16 0 8 6 12 0v26H4z"/><path class="l" d="M4 44c6-6 10 6 16 0s10 6 16 0 10 6 16 0 8 6 12 0"/><path class="o s" d="M28 10l10 16H18z"/><path class="o d" d="M26 26h4v8h-4z"/>',
    "bell": '<path class="o g" d="M32 8c-10 0-14 8-14 16v14l-6 8h40l-6-8V24c0-8-4-16-14-16z"/><path class="gh" d="M22 22c0-6 4-10 8-10-3 3-4 8-4 14v12h-4z" opacity="0.6"/><circle class="o d" cx="32" cy="52" r="5"/><path class="o d" d="M28 4h8v6h-8z"/>',
    "sword": '<path class="o sth" d="M30 4h4l2 6v32h-8V10z"/><path class="t" d="M32 8v32"/><path class="o g" d="M18 42h28v5H18z"/><path class="o d" d="M29 47h6v9h-6z"/><circle class="o g" cx="32" cy="58" r="3"/>',
    "skull": '<path class="o p" d="M32 8c-12 0-20 8-20 19 0 6 3 11 8 14v9h24v-9c5-3 8-8 8-14C52 16 44 8 32 8z"/><path class="o k" d="M21 27a5 5 0 1 0 10 0 5 5 0 1 0-10 0zM33 27a5 5 0 1 0 10 0 5 5 0 1 0-10 0z"/><path class="o k" d="M29 38l3-5 3 5z"/><path class="t" d="M26 46v4M32 46v4M38 46v4"/>',
    "flag": '<path class="o d" d="M14 6h4v52h-4z"/><path class="o k" d="M18 10h34l-6 10 6 10H18z"/><path class="o p" d="M30 14c-4 0-6 3-6 6 0 2 1 4 3 5v2h6v-2c2-1 3-3 3-5 0-3-2-6-6-6z"/><path class="k" d="M28 19h2v2h-2zM32 19h2v2h-2z"/>',
    "curtain": '<path class="o r" d="M4 6h26c-2 20 0 36 4 52H4z"/><path class="o r" d="M60 6H34c2 20 0 36-4 52h30z"/><path class="t" d="M12 8v48M20 8v48M44 8v48M52 8v48"/><path class="o g" d="M4 4h56v4H4z"/>',
    "vent": '<path class="o st" d="M14 18h36v28H14z"/><path class="o k" d="M18 22h28v4H18zM18 30h28v4H18zM18 38h28v4H18z"/>',
    "crates": '<path class="o s" d="M6 34h24v24H6zM34 34h24v24H34zM20 12h24v22H20z"/><path class="t" d="M6 34l24 24M30 34L6 58M34 34l24 24M58 34L34 58M20 12l24 22M44 12L20 34"/>',
    "door": '<path class="o d" d="M16 60V10h32v50"/><path class="o s" d="M20 60V14h24v46z"/><circle class="o g" cx="40" cy="38" r="2"/><path class="l" d="M8 60h48"/>',
    "bed": '<path class="o p" d="M6 40h52v10H6z"/><path class="o s" d="M6 32h14v8H6z"/><path class="o d" d="M6 30v26M58 36v20"/><path class="o r" d="M20 34h38v6H20z"/>',
    "moon": '<path class="o p" d="M40 6a26 26 0 1 0 18 40A22 22 0 0 1 40 6z"/><path class="s" d="M24 30a4 4 0 1 0 8 0 4 4 0 1 0-8 0zM30 44a3 3 0 1 0 6 0 3 3 0 1 0-6 0z" opacity="0.8"/>',
    "haycart": '<path class="o ye" d="M8 28c4-8 44-8 48 0v10H8z"/><path class="t" d="M14 24l2 8M22 22l1 10M32 21v11M42 22l-1 10M50 24l-2 8"/><path class="o d" d="M6 38h52v6H6z"/><circle class="o d" cx="16" cy="50" r="7"/><circle class="o d" cx="48" cy="50" r="7"/><circle class="o s" cx="16" cy="50" r="2"/><circle class="o s" cx="48" cy="50" r="2"/>',
    "diver": '<path class="o p" d="M30 8l8 4-2 4-8-4z"/><circle class="o p" cx="26" cy="8" r="4"/><path class="l" d="M30 12l-8 10M34 14l8 8M36 16l2 10"/>',
    "target": '<circle class="o w" cx="32" cy="32" r="24"/><circle class="o r" cx="32" cy="32" r="17"/><circle class="o w" cx="32" cy="32" r="10"/><circle class="o r" cx="32" cy="32" r="4"/><circle class="k" cx="25" cy="27" r="1.8"/><circle class="k" cx="38" cy="36" r="1.8"/><circle class="k" cx="33" cy="31" r="1.8"/>',
    "obstacle": '<path class="o s" d="M6 58V30h16v28zM42 58V18h16v40z"/><path class="o d" d="M22 40h20v4H22z"/><path class="l" d="M46 22v32M54 22v32M46 28h8M46 36h8M46 44h8"/>',
    "radio": '<path class="o d" d="M8 22h48v32H8z"/><circle class="o s" cx="22" cy="38" r="9"/><circle class="o k" cx="22" cy="38" r="3"/><path class="o g" d="M36 30h14v4H36z"/><path class="t" d="M36 40h14M36 46h14M20 22L40 6"/><circle class="o g" cx="40" cy="6" r="2"/>',
    "pyramid": '<path class="o ye" d="M4 56L32 12l28 44z"/><path class="o s" d="M32 12l28 44H32z"/><path class="t" d="M18 34h28M11 45h42M24 23h16"/><circle class="o ye" cx="50" cy="12" r="5"/>',
    "mug": '<path class="o g" d="M14 16h28v40H14z"/><path class="o w" d="M14 12h28v8H14z"/><path class="o g" d="M42 24h6c4 0 6 3 6 8s-2 8-6 8h-6"/><path class="t" d="M20 24v26M28 24v26M36 24v26"/>',
    "counter": '<path class="o d" d="M4 46h56v14H4z"/><path class="o s" d="M2 42h60v4H2z"/>',
    "dagger": '<path class="o sth" d="M30 4h4l3 6v30h-10V10z"/><path class="o g" d="M20 40h24v5H20z"/><path class="o d" d="M29 45h6v10h-6z"/><path class="o r" d="M33 12c1 6 2 10 0 14-2-4-1-8 0-14z"/>',
    "wall": '<path class="o s" d="M4 14h56v28H4z"/><path class="t" d="M4 21h56M4 28h56M4 35h56M14 14v7M30 14v7M46 14v7M22 21v7M38 21v7M54 21v7M14 28v7M30 28v7M46 28v7M22 35v7M38 35v7"/><path class="o bl" d="M2 48c6-6 10 6 16 0s10 6 16 0 10 6 16 0 8 6 12 0v14H2z"/>',
    "radar": '<path class="o st" d="M10 46a26 26 0 0 1 28-36z"/><path class="o sth" d="M14 42a20 20 0 0 1 20-28z"/><path class="l" d="M24 32l14-14"/><circle class="o rh" cx="38" cy="18" r="3"/><path class="o d" d="M28 46h8v8h8v4H20v-4h8z"/><path stroke="#9fd0ff" stroke-width="1.6" fill="none" d="M44 12c4 2 6 6 6 10M48 8c6 3 9 9 9 14"/>',
    "fuelcan": '<path class="o r" d="M14 14h26l10 10v34H14z"/><path class="o d" d="M40 14v10h10"/><path class="o st" d="M20 8h14v6H20z"/><path class="o k" d="M22 30l16 16M38 30L22 46"/>',
    "battery": '<path class="o st" d="M16 14h32v44H16z"/><path class="o d" d="M26 8h12v6H26z"/><path class="o ye" d="M20 18h24v36H20z" opacity="0.9"/><path class="o k" d="M34 22l-10 16h8l-4 12 12-18h-8z"/>',
    "tank": '<path class="o gr" d="M6 42h52l-4 12H10z"/><path class="o gr" d="M16 42v-10h24v10z"/><path class="o d" d="M40 34h20v4H40z"/><circle class="o d" cx="16" cy="48" r="3"/><circle class="o d" cx="26" cy="48" r="3"/><circle class="o d" cx="36" cy="48" r="3"/><circle class="o d" cx="46" cy="48" r="3"/><path class="o k" d="M22 26h12v6H22z"/>',
    "train": '<path class="o d" d="M8 22h40v24H8z"/><path class="o k" d="M48 30h8v16h-8z"/><path class="o st" d="M14 14h8v8h-8z"/><circle class="o k" cx="16" cy="50" r="5"/><circle class="o k" cx="30" cy="50" r="5"/><circle class="o k" cx="44" cy="50" r="5"/><path class="o s" d="M12 26h12v8H12z"/><path class="l" d="M2 58h60"/>',
    "cannon": '<path class="o st" d="M4 34l44-16 4 8-44 16z"/><path class="o d" d="M12 40h40l4 8H8z"/><circle class="o k" cx="16" cy="52" r="5"/><circle class="o k" cx="28" cy="52" r="5"/><circle class="o k" cx="40" cy="52" r="5"/><path class="l" d="M2 58h60"/>',
    "tower": '<path class="o st" d="M24 58l6-48h4l6 48z"/><path class="t" d="M26 46h12M27 36h10M28 26h8M24 58l12-24M40 58L28 34"/><path stroke="#9fd0ff" stroke-width="1.6" fill="none" d="M24 10c-4 2-6 6-6 10M40 10c4 2 6 6 6 10"/><circle class="o rh" cx="32" cy="8" r="3"/>',
    "bomber": '<path class="o st" d="M10 20h44l-4 4H14z"/><path class="o st" d="M28 14h8l2 16h-12z"/><path class="o d" d="M26 30h12v4H26z"/>',
    "snow": '<path stroke="#e8f2f8" stroke-width="3" fill="none" stroke-linecap="round" d="M32 6v52M9 19l46 26M9 45l46-26M26 10l6 6 6-6M26 54l6-6 6 6M10 26l8 2-2 8M54 38l-8-2 2-8M10 38l8-2-2-8M54 26l-8 2 2 8"/><path stroke="#1b130e" stroke-width="1" fill="none" d="M32 6v52M9 19l46 26M9 45l46-26"/>',
    "chair": '<path class="o d" d="M20 10h4v26h-4zM40 10h4v26h-4z"/><path class="o s" d="M18 34h28v6H18z"/><path class="o d" d="M20 40v18M44 40v18M22 14h20v4H22z"/>',
    "c4": '<path class="o s" d="M14 28h36v20H14z"/><path class="t" d="M14 34h36M14 40h36"/><path class="o k" d="M38 22h10v8H38z"/><path stroke="#c0392b" stroke-width="1.6" fill="none" d="M40 22c0-6 6-8 10-4"/><circle class="rh" cx="44" cy="26" r="1.6"/>',
    "helmet": '<path class="o gr" d="M8 40c0-16 10-28 24-28s24 12 24 28z"/><path class="o d" d="M4 40h56l-4 6H8z"/><path class="sth" d="M16 30c2-8 8-12 14-14-6 4-9 9-10 16z" opacity="0.4"/>',
    "anvil": '<path class="o st" d="M6 20h38c0 6 6 10 14 10-4 5-10 7-18 7l-2 6h6v10H20V43h6l-2-6C14 37 6 31 6 20z"/><path class="sth" d="M8 21h34c-1 3-1 4 0 6H12z" opacity="0.5"/>',
    "tree": '<path class="o gr" d="M32 4L14 30h8L8 48h48L42 30h8z"/><path class="o d" d="M28 48h8v10h-8z"/><path class="l" d="M2 58h60"/>',
    "sun": '<circle class="o ye" cx="32" cy="32" r="12"/><path stroke="#e2b93b" stroke-width="3" stroke-linecap="round" d="M32 6v8M32 50v8M6 32h8M50 32h8M14 14l6 6M44 44l6 6M14 50l6-6M44 20l6-6"/>',
    "arena": '<ellipse class="o s" cx="32" cy="42" rx="28" ry="12"/><ellipse class="o d" cx="32" cy="42" rx="18" ry="7"/><path class="o s" d="M4 42V30c0-6 12-12 28-12s28 6 28 12v12"/><path class="t" d="M14 22v10M24 19v10M32 18v10M40 19v10M50 22v10"/>',
    "dice": '<path class="o w" d="M10 22l22-12 22 12v24L32 58 10 46z"/><path class="o p" d="M10 22l22 12v24L10 46z"/><path class="o s" d="M54 22L32 34v24l22-12z"/><circle class="k" cx="32" cy="22" r="2"/><circle class="k" cx="18" cy="34" r="2"/><circle class="k" cx="24" cy="44" r="2"/><circle class="k" cx="40" cy="38" r="2"/><circle class="k" cx="46" cy="46" r="2"/><circle class="k" cx="43" cy="42" r="2"/>',
    "coin": '<circle class="o g" cx="32" cy="32" r="22"/><circle class="o gh" cx="32" cy="32" r="16"/><text x="32" y="40" text-anchor="middle" font-family="DejaVu Serif" font-weight="700" font-size="22" fill="#7d6648">$</text>',
    "ban": '<circle fill="none" stroke="#c0392b" stroke-width="6" cx="32" cy="32" r="26"/><path stroke="#c0392b" stroke-width="6" d="M14 50L50 14"/>',
    "heart": '<path class="o r" d="M32 56C14 42 6 32 6 23a13 13 0 0 1 26-5 13 13 0 0 1 26 5c0 9-8 19-26 33z"/><path class="w" d="M28 24h8v6h6v8h-6v6h-8v-6h-6v-8h6z"/>',
    "infinity": '<path fill="none" stroke="#1b130e" stroke-width="8" stroke-linecap="round" d="M32 32c-6-9-11-11-15-11a11 11 0 0 0 0 22c4 0 9-2 15-11s11-11 15-11a11 11 0 0 1 0 22c-4 0-9-2-15-11z"/><path fill="none" stroke="#c9a043" stroke-width="4" stroke-linecap="round" d="M32 32c-6-9-11-11-15-11a11 11 0 0 0 0 22c4 0 9-2 15-11s11-11 15-11a11 11 0 0 1 0 22c-4 0-9-2-15-11z"/>',
    "flask": '<path class="o c" d="M26 6h12M28 6v16L14 50c-2 4 0 8 4 8h28c4 0 6-4 4-8L36 22V6" opacity="0.9"/><path class="o rh" d="M18 44h28l4 8c1 3-1 6-4 6H18c-3 0-5-3-4-6z"/><circle class="w" cx="26" cy="50" r="2"/><circle class="w" cx="36" cy="47" r="1.6"/><path fill="#c7d8a0" opacity="0.6" d="M30 4c4-4 10-2 10 2-4 0-6 2-10-2z"/>',
    "laurel": '<path fill="none" stroke="#6f7d33" stroke-width="3" d="M22 56c-12-8-14-26-6-40M42 56c12-8 14-26 6-40"/><path class="o gr" d="M15 28c-5 0-8-3-8-7 5 0 8 3 8 7zM14 38c-5 1-8-1-9-5 5-1 8 1 9 5zM18 47c-4 2-8 1-9-3 4-2 8-1 9 3zM16 18c-4-2-5-6-3-9 4 2 5 6 3 9zM49 28c5 0 8-3 8-7-5 0-8 3-8 7zM50 38c5 1 8-1 9-5-5-1-8 1-9 5zM46 47c4 2 8 1 9-3-4-2-8-1-9 3zM48 18c4-2 5-6 3-9-4 2-5 6-3 9z"/>',
    "star": '<path class="o g" d="M32 4l8 17 18 2-13 12 4 18-17-9-17 9 4-18L6 23l18-2z"/><path class="gh" d="M32 10l5 12-5 1z" opacity="0.6"/>',
    "medal": '<path class="o r" d="M22 4h8l4 18h-8zM34 4h8l-4 18h-8z"/><circle class="o g" cx="32" cy="40" r="16"/><circle class="o gh" cx="32" cy="40" r="11"/><path class="o g" d="M32 32l2.4 5 5.6.6-4 3.8 1 5.6-5-2.8-5 2.8 1-5.6-4-3.8 5.6-.6z"/>',
    "figure8": '',
}

# ------------------------------------------------------------------ compositions
# id -> (label, group, [elements]) ; element = (name, x, y, scale[, rotate])
# or ("text", text, x, y, size[, colour])
C = {}
def add(i, label, group, *parts, badge=None):
    C[i] = (label, group, list(parts), badge)

CH_SCENE = {1: "castle", 2: "crypt", 3: "rocket", 4: "factory", 5: "lab", 6: "chateau", 7: "dark"}
CH_NAME = {1: "Ominous Rumors", 2: "Dark Secret", 3: "Weapons of Vengeance", 4: "Deadly Designs",
           5: "Deathshead's Playground", 6: "Return Engagement", 7: "Operation Resurrection"}
ROMAN = ["", "I", "II", "III", "IV", "V", "VI", "VII"]
for n in range(1, 8):
    add(f"ACH_CHAPTER_{n}", CH_NAME[n], "chapter", (CH_SCENE[n], 4, 2, 0.88),
        ("plaque", ROMAN[n]))
    add(f"ACH_CHAPTER_{n}_HARD", CH_NAME[n] + " (hard)", "hard", (CH_SCENE[n], 4, 2, 0.88),
        ("plaque", ROMAN[n]), ("skull", 40, 38, 0.38))

add("ACH_BOSS1", "Ancient Weaponsmith (Olaric)", "combat", ("olaric", 2, 4, 0.92), ("sword", 30, 4, 0.9, 25))
add("ACH_BOSS2", "Ultimate Killing Machine (Uber Soldat)", "combat", ("uber", 2, 2, 0.95))
add("ACH_BOSS3", "The Dark King (Heinrich)", "combat", ("heinrich", 2, 4, 0.95))
add("ACH_HEIN_NOSHOT", "Henry the Fowler (cave trap)", "combat", ("heinrich", 8, 14, 0.78), ("rocks", 0, -10, 1.0))
add("ACH_LOPER_ROCKET", "Damn, You're Ugly! (Loper, Panzerfaust)", "combat", ("loper", 0, 14, 0.85), ("panzerfaust", 10, -4, 0.75))
add("ACH_PROTO_KNIFE", "Superiority of Flesh and Blood", "combat", ("proto", -2, 4, 0.9), ("knife", 26, 22, 0.6))
add("ACH_PROTO_FALL", "Hasta la Vista (Proto off the bridge)", "combat", ("bridge", 0, 4, 1.0), ("fallman", 4, 18, 0.7))
add("ACH_ELITE_FOOT", "Foot Fetishism (Elite Guard, kick)", "combat", ("elite", 18, 4, 0.85), ("boot", -4, 20, 0.6, -30))
add("ACH_WINTERSTEIN_TESLA", "Tesla kill (Winterstein)", "combat", ("tesla", 2, 2, 1.0))
add("ACH_KILL_CIVILIAN", "Civilian casualty", "combat", ("civilian", 2, 2, 0.95), ("ban", 6, 6, 0.8))
add("ACH_SECOND_CHANCE", "Second chance", "combat", ("heart", 4, 4, 0.9))
add("ACH_SECRET", "Hidden Treasure (first secret)", "secret", ("keyhole", 4, 4, 0.9))
add("ACH_SECRET_CRYPT2", "Crypt secret", "secret", ("crypt", 4, 4, 0.9), ("keyhole", 34, 34, 0.42))
add("ACH_GOLDRUSH", "Gold Rush (crypt lever puzzle)", "secret", ("lever", 22, -2, 0.7), ("gold", 0, 18, 0.8))
add("ACH_CRYSTALSKULL", "Dr. Jones? (crystal skull)", "secret", ("skullgem", 4, 4, 0.9))
add("ACH_WINE", "Valuable Property of the Reich (wine)", "secret", ("wine", 2, 2, 0.95))
add("ACH_READ_BOOK", "The book", "secret", ("book", 2, 4, 0.95))
add("ACH_MAGIC", "Magic", "secret", ("wand", 4, 4, 0.9))
add("ACH_W3D_1", "Get Psyched I", "secret", ("elevator", 4, 4, 0.9))
add("ACH_W3D_2", "Get Psyched II", "secret", ("pixel", 4, 4, 0.9))
add("ACH_SAFE", "Cracked the safe", "secret", ("safe", 4, 4, 0.9))
add("ACH_CHESTSIWA", "Siwa treasure chest", "secret", ("chest", 4, 6, 0.9))
add("ACH_ALLGOLDSIWA", "All the gold (Siwa)", "secret", ("pyramid", 14, -2, 0.6), ("gold", 2, 18, 0.8))
add("ACH_ALLGOLDFUEL", "All the gold (fuel depot)", "secret", ("fuelcan", 22, -4, 0.55), ("gold", 2, 18, 0.8))
add("ACH_MANSIONGOLD", "Mansion gold", "secret", ("chateau", 14, -4, 0.6), ("gold", 2, 18, 0.8))
add("ACH_KELLYS", "Kelly's Heroes (9 gold bars)", "secret", ("gold", 2, 22, 0.75), ("gold", 6, 8, 0.75), ("gold", 10, -6, 0.75))
add("ACH_EE_SECRETS", "All secrets", "secret", ("keyhole", 4, 4, 0.9), ("star", 34, 34, 0.42))
add("ACH_STEALTH_1", "You're Pretty Good!", "stealth", ("agent", 2, 2, 0.95))
add("ACH_STEALTH_2", "Ghost of Paderborn", "stealth", ("ghost", 4, 4, 0.9))
add("ACH_ROCKET_STEALTH", "Infiltrator (rocket base)", "stealth", ("rocket", 14, 0, 0.8), ("agent", -6, 18, 0.65))
add("ACH_ESCAPE_SPEEDRUN", "Escape! (under 90 seconds)", "stealth", ("stopwatch", 2, 0, 0.95), ("text", "90", 32, 40, 13))
add("ACH_AGENT1", "Agent One", "stealth", ("agent", 2, 2, 0.95), ("text", "1", 48, 18, 14, "#c9a043"))
add("ACH_SIWA", "Siwa oasis", "stealth", ("pyramid", 2, 4, 0.95))
add("ACH_SEAWALL", "The seawall", "stealth", ("wall", 2, 4, 0.95))
add("ACH_RADAR", "The radar", "stealth", ("radar", 2, 2, 0.95))
add("ACH_FUEL", "Fuel depot", "stealth", ("fuelcan", 4, 2, 0.9))
add("ACH_FUEL1", "Fuel depot, part one", "stealth", ("fuelcan", 4, 2, 0.9), ("text", "1", 48, 50, 14, "#c9a043"))
add("ACH_SPEEDBATTERY", "Battery, against the clock", "stealth", ("battery", -2, 4, 0.85), ("stopwatch", 28, 28, 0.5))
add("ACH_ROOMBATTERY", "The battery room", "stealth", ("battery", 4, 2, 0.9))
add("ACH_PANTHER", "The Panther", "stealth", ("tank", 2, 2, 0.95))
add("ACH_DEPOT", "The depot", "stealth", ("train", 2, 2, 0.95))
add("ACH_DORA", "Dora (railway gun)", "stealth", ("cannon", 2, 2, 0.95))
add("ACH_BINOCS", "Binoculars", "stealth", ("binocs", 2, 2, 0.95))
add("ACH_WARBELL_MAP", "Warbell: the map", "stealth", ("map", 2, 4, 0.95))
add("ACH_WARBELL_WATERS", "Warbell: the waters", "stealth", ("waves", 2, 2, 0.95))
add("ACH_WARBELL_BLAV", "Warbell: Blavatsky", "stealth", ("book", 2, 8, 0.8), ("bell", 30, -2, 0.5))
add("ACH_WARBELL_OLARIC", "Warbell: Olaric", "combat", ("olaric", 0, 6, 0.85), ("bell", 32, 0, 0.5))
add("ACH_WARBELL_HEIN", "Warbell: Heinrich", "combat", ("heinrich", 0, 6, 0.85), ("bell", 32, 0, 0.5))
add("ACH_BROWNING", "Browning", "weapon", ("browning", 2, 4, 0.95))
add("ACH_MOSIN", "Reverse Lend Lease II (Mosin)", "weapon", ("mosin", 2, 6, 0.95))
add("ACH_PPSH", "Reverse Lend Lease I (PPSh)", "weapon", ("ppsh", 2, 6, 0.95))
add("ACH_GL", "Noobtube (M7 grenade launcher)", "weapon", ("m7", 2, 6, 0.95))
add("ACH_RAILGUN", "Railgun", "weapon", ("railgun", 2, 4, 0.95))
add("ACH_GAS", "Unconventional Weapon (poison gas)", "weapon", ("gascan", 4, 4, 0.9))
add("ACH_AIR", "Affirmative, On My Way! (airstrike)", "weapon", ("plane", 14, -2, 0.62), ("smoke", -4, 18, 0.7))
add("ACH_ITEM_CROSS", "The holy cross", "weapon", ("cross", 4, 4, 0.9))
add("ACH_ITEM_EMP", "EMP", "weapon", ("emp", 2, 2, 0.95))
add("ACH_ITEM_XSHIELD", "X-shield", "weapon", ("shield", 4, 4, 0.9))
add("ACH_ADRENALINE", "Adrenaline", "weapon", ("syringe", 4, 4, 0.9))
add("ACH_BANDAGES", "Bandages", "weapon", ("bandage", 4, 4, 0.9))
add("ACH_UBER_PW", "Super Soldier (three power-ups)", "weapon", ("orbs", 2, 2, 0.95))
add("ACH_6PERKS", "Six perks", "survival", ("dice", 4, 4, 0.9))
add("ACH_999", "999", "survival", ("text", "999", 32, 40, 20, "#c9a043"))
add("ACH_NO_BUY", "Non-Monetary Economy", "survival", ("coin", 4, 4, 0.9), ("ban", 4, 4, 0.9))
add("ACH_NO_CLASS", "Classless Society", "survival", ("helmet", 4, 6, 0.9), ("ban", 4, 4, 0.9))
add("ACH_TDA_DAY", "The Dark Army: day", "survival", ("zombie", 0, 8, 0.85), ("sun", 32, -2, 0.5))
add("ACH_TDA_NIGHT", "The Dark Army: night", "survival", ("zombie", 0, 8, 0.85), ("moon", 34, -2, 0.45))
add("ACH_TDA_DARK", "Dark Realm Master", "survival", ("dark", 2, 2, 0.95))
add("ACH_TDA_PLUS", "The Dark Army+", "survival", ("ghost", 2, 4, 0.85), ("zombie", 26, 22, 0.55))
add("ACH_TDA_ARENA", "The Dark Army: arena", "survival", ("arena", 2, 4, 0.95))
add("ACH_EE_SURV", "Survivor", "survival", ("shield", 4, 4, 0.9), ("skull", 18, 18, 0.45))
add("ACH_EE_SURV_DOG", "Survivor: the hounds", "survival", ("dog", 2, 4, 0.95))
add("ACH_EE_SURV_PRIEST", "Survivor: the priest", "survival", ("priest", 2, 2, 0.95))
add("ACH_EE_SURV_SPECIAL", "Survivor: special wave", "survival", ("zombie", 2, 6, 0.9), ("star", 36, 0, 0.4))
add("ACH_EE_CORPS", "The corps", "survival", ("helmet", 2, 8, 0.8), ("skull", 18, 22, 0.5))
add("ACH_EE_GREN", "Grenadier", "survival", ("stick", -2, 2, 0.85, -20), ("grenade", 22, 8, 0.75))
add("ACH_EE_INF", "Endless", "survival", ("infinity", 2, 2, 0.95))
add("ACH_EE_MAD", "Mad science", "survival", ("flask", 4, 2, 0.9))
add("ACH_EE_MERCS", "Mercenaries", "survival", ("soldier", 2, 2, 0.95), ("coin", 36, 36, 0.38))
add("ACH_EE_MOMMY", "Mommy", "survival", ("mummy", -6, 4, 0.82), ("ghost", 28, 18, 0.55))
add("ACH_EE_MUMMY", "The mummy", "survival", ("mummy", 2, 2, 0.95))
add("ACH_EE_CHALLENGE", "The challenge", "mode", ("medal", 4, 4, 0.9))
add("ACH_EE_COMPLETED", "Completed", "mode", ("laurel", 2, 2, 0.95), ("star", 18, 16, 0.45))
add("ACH_EE_COMPLETED_HARD", "Completed on hard", "mode", ("laurel", 2, 2, 0.95), ("skull", 18, 16, 0.45))
add("ACH_HARDCORE", "Average Hitscan Enjoyer (hardcore)", "mode", ("helmet", 2, 4, 0.95), ("skull", 20, 22, 0.4))
add("ACH_IRONMAN", "No Pressure (ironman)", "mode", ("anvil", 2, 6, 0.95))
add("ACH_NIGHTMARE", "The 2000 Yard Stare (nightmare)", "mode", ("moon", 2, 2, 0.9), ("skull", 26, 26, 0.5))
add("ACH_WALKINTHEPARK", "Didn't Even Break a Sweat", "mode", ("tree", 2, 2, 0.95))
add("ACH_TRAINING", "Firing Range", "mode", ("target", 2, 2, 0.95))
add("ACH_MALTA_NIGHTMARE", "Whatever You Do, Don't Fall Asleep", "malta", ("bed", 2, 16, 0.95), ("moon", 30, -2, 0.42))
add("ACH_MALTA_LEAP", "Airborne Training", "malta", ("haycart", 2, 22, 0.9), ("diver", 6, -2, 0.9))
add("ACH_MALTA_OSA", "The OSA", "malta", ("agent", 2, 2, 0.95), ("star", 38, 2, 0.36))
add("ACH_MALTA_GOAT", "The goat", "malta", ("goat", 0, 2, 1.0))
add("ACH_MALTA_COURSE", "How the Muppet Like You Passed Selection?", "malta", ("obstacle", 2, 6, 0.95))
add("ACH_MALTA_RADIO", "The radio", "malta", ("radio", 2, 4, 0.95))
add("ACH_MALTA_EGYPT", "Egypt", "malta", ("pyramid", 2, 4, 0.95))
add("ACH_MALTA_WIDE", "Wide", "malta", ("scope", 2, 2, 0.95))
add("ACH_MALTA_AGENT2", "Agent Two", "malta", ("agent", 2, 2, 0.95), ("text", "2", 48, 18, 14, "#c9a043"))
add("ACH_MALTA_BARTENDER", "You Are Not Supposed to Be Here!", "malta", ("bartender", 2, -6, 0.8), ("counter", 0, 6, 1.0), ("mug", 40, 30, 0.3))
add("ACH_MALTA_BETRAYER", "The betrayer", "malta", ("dagger", 4, 4, 0.9))
add("ACH_MALTA_FIREFLY", "Firefly", "malta", ("firefly", 2, 2, 0.95))
add("ACH_MALTA_HIDEOUT", "Black Market", "malta", ("crates", 2, 4, 0.95))
add("ACH_MALTA_LAIR", "Pirates! (the vent)", "malta", ("curtain", 2, 2, 0.95), ("flag", 14, 14, 0.55))
add("ACH_ETBONUS", "Enemy Territory bonus", "et", ("medal", 4, 4, 0.9), ("text", "ET", 32, 44, 9, "#1b130e"))
add("ACH_HEIST", "Perfect Heist", "et", ("vault", 2, 2, 0.95))
add("ACH_ICE_BEAT", "Ice: mission complete", "et", ("snow", 4, 4, 0.9))
add("ACH_ICE_DEFENSE", "Sky Melting (protect the transmitter)", "et", ("tower", 4, 8, 0.85), ("bomber", 18, -6, 0.6))
add("ACH_ICE_DH", "Death's Dead (snipe Deathshead)", "et", ("scope", 2, 2, 0.95))
add("ACH_ICE_EE", "Ice: hidden", "et", ("snow", 0, 0, 0.75), ("star", 30, 30, 0.45))
add("ACH_ICE_SECRET", "Ice: secret", "et", ("snow", 0, 0, 0.75), ("keyhole", 30, 30, 0.45))
add("ACH_ICE_STEALTH", "Ice: unseen", "et", ("snow", 0, 0, 0.75), ("agent", 26, 24, 0.55))
add("ACH_SEAWALL", "The seawall", "stealth", ("wall", 2, 4, 0.95))
add("ACH_EE_SURV", "Survivor", "survival", ("shield", 4, 4, 0.9), ("skull", 18, 18, 0.45))
# the C4 / chair one in Enemy Territory isn't in this code's list; the rest are above


# short descriptions where the achievement is known (shown under the name)
DESC = {
    "ACH_HARDCORE": "Complete a chapter on Hardcore.",
    "ACH_WALKINTHEPARK": "Complete a chapter on Walk in the Park.",
    "ACH_IRONMAN": "Complete a chapter on Ironman.",
    "ACH_NIGHTMARE": "Complete Nightmare mode.",
    "ACH_W3D_1": "Find the hidden bonus stage and its secret elevator.",
    "ACH_W3D_2": "Find the second hidden bonus stage and its secret elevator.",
    "ACH_BOSS1": "Defeat Olaric.",
    "ACH_BOSS2": "Defeat the Uber Soldat.",
    "ACH_BOSS3": "Defeat Heinrich.",
    "ACH_MALTA_LEAP": "Jump from the tower into the hay cart.",
    "ACH_TRAINING": "Find the firing range near the docks.",
    "ACH_MALTA_COURSE": "Complete the training course.",
    "ACH_MALTA_NIGHTMARE": "Sleep in the hotel and survive the nightmare.",
    "ACH_MALTA_LAIR": "Find the vent behind the red curtains.",
    "ACH_MALTA_BARTENDER": "Stand on the bar long enough to annoy the bartender.",
    "ACH_MALTA_HIDEOUT": "Find the smugglers' hideout near the port.",
    "ACH_ESCAPE_SPEEDRUN": "Finish the opening level in under 90 seconds.",
    "ACH_SECRET": "Find your first secret area.",
    "ACH_WINE": "Find the hidden wine in the cellar and drink it.",
    "ACH_GOLDRUSH": "Solve the crypt lever puzzle.",
    "ACH_CRYSTALSKULL": "Find the crystal skull.",
    "ACH_ELITE_FOOT": "Kill an Elite Guard with only your kick.",
    "ACH_STEALTH_1": "Complete the forest mission without failing.",
    "ACH_ROCKET_STEALTH": "Get through the rocket base unseen.",
    "ACH_PPSH": "Find the hidden PPSh.",
    "ACH_LOPER_ROCKET": "Kill the Loper with a Panzerfaust.",
    "ACH_PROTO_KNIFE": "Finish the Proto Soldier with your knife.",
    "ACH_STEALTH_2": "Complete Paderborn without failing or reloading.",
    "ACH_MOSIN": "Find the hidden Mosin behind the portrait.",
    "ACH_PROTO_FALL": "Drop the bridge with the Proto Soldier on it.",
    "ACH_HEIN_NOSHOT": "Kill Heinrich with the cave trap.",
    "ACH_NO_CLASS": "Reach wave 15 without picking a class.",
    "ACH_NO_BUY": "Reach wave 10 without buying anything.",
    "ACH_UBER_PW": "Have all three power-ups active at once.",
    "ACH_TDA_DARK": "Complete the Dark Realm.",
    "ACH_GL": "Kill an enemy with the M7 grenade launcher.",
    "ACH_AIR": "Kill an enemy with an airstrike.",
    "ACH_GAS": "Kill an enemy with poison gas.",
    "ACH_HEIST": "Get through the bank undetected.",
    "ACH_KELLYS": "Collect all 9 gold bars.",
    "ACH_ICE_DH": "Snipe Deathshead before he escapes.",
    "ACH_ICE_DEFENSE": "Protect the transmitter from the bombers.",
}
for n in range(1, 8):
    DESC[f"ACH_CHAPTER_{n}"] = f"Complete chapter {n}."
    DESC[f"ACH_CHAPTER_{n}_HARD"] = f"Complete chapter {n} on a hard difficulty."


def plate_svg():
    """The pop-up's iron plate (560x112 at 2x), with the blood line along
    the bottom; drips are drawn separately by cgame."""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="1120" height="232" viewBox="0 0 560 116">
<defs><linearGradient id="pl" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2b211c"/><stop offset="1" stop-color="#18120f"/></linearGradient>
<filter id="grain" x="0" y="0" width="1" height="1"><feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="2" seed="11"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 0.45 0"/><feComposite in2="SourceGraphic" operator="in"/></filter>
<radialGradient id="rv" cx="0.35" cy="0.35" r="0.7"><stop offset="0" stop-color="#8a7766"/><stop offset="1" stop-color="#2a201a"/></radialGradient></defs>
<path d="M0 0h560v108H0z" fill="url(#pl)"/>
<path d="M0 0h560v108H0z" fill="#000" filter="url(#grain)" opacity="0.6"/>
<path d="M1 0v107h558V0" fill="none" stroke="#3d2f28" stroke-width="2"/>
<path d="M2 2h556" stroke="#fff" stroke-opacity="0.05" stroke-width="2"/>
<circle cx="12" cy="12" r="4" fill="url(#rv)"/><circle cx="548" cy="12" r="4" fill="url(#rv)"/>
<path d="M0 106h560v4H0z" fill="#5e0b0b"/>
<path d="M0 108c40 2 80-1 120 1s90 3 140 0 100 2 150 1 100-2 150 0v3H0z" fill="#5e0b0b"/>
</svg>'''


def drip_svg():
    return '''<svg xmlns="http://www.w3.org/2000/svg" width="32" height="256" viewBox="0 0 16 128">
<path d="M5 0h6v110a3 3 0 0 1-6 0z" fill="#5e0b0b"/>
<circle cx="8" cy="116" r="7" fill="#5e0b0b"/>
<path d="M6.5 2h1.5v100H6.5z" fill="#8e1b12" opacity="0.6"/>
</svg>'''


def part_svg(p):
    if p[0] == "text":
        _, t, x, y, size = p[:5]
        col = p[5] if len(p) > 5 else INK
        return (f'<text x="{x}" y="{y}" text-anchor="middle" font-family="DejaVu Serif, Georgia, serif" '
                f'font-weight="700" font-size="{size}" fill="{col}" stroke="{INK if col != INK else "none"}" '
                f'stroke-width="0.8" paint-order="stroke">{html.escape(t)}</text>')
    if p[0] == "plaque":
        t = p[1]
        return (f'<g><path class="o g" d="M18 48h28l4 6-4 6H18l-4-6z"/><path class="gh" d="M19 49h26l2 3H17z" opacity="0.6"/>'
                f'<text x="32" y="58.5" text-anchor="middle" font-family="DejaVu Serif, Georgia, serif" font-weight="700" '
                f'font-size="9" fill="{INK}">{t}</text></g>')
    name, x, y, sc = p[:4]
    rot = p[4] if len(p) > 4 else 0
    tr = f"translate({x} {y}) scale({sc})"
    if rot:
        tr += f" rotate({rot} 32 32)"
    return f'<g transform="{tr}">{E[name]}</g>'


def icon_svg(aid, px=512):
    label, group, parts, badge = C[aid]
    rim = GROUPS[group][0]
    art = "".join(part_svg(p) for p in parts)
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{px}" height="{px}" viewBox="0 0 256 256">
<defs>
<style>{STYLE}</style>
<linearGradient id="iron" x1="0" y1="0" x2="0.3" y2="1"><stop offset="0" stop-color="#3a2e27"/><stop offset="0.5" stop-color="#241b17"/><stop offset="1" stop-color="#130e0c"/></linearGradient>
<linearGradient id="rim" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="{rim}" stop-opacity="1"/><stop offset="0.5" stop-color="{rim}" stop-opacity="0.65"/><stop offset="1" stop-color="{rim}" stop-opacity="1"/></linearGradient>
<radialGradient id="fld" cx="0.5" cy="0.42" r="0.75"><stop offset="0" stop-color="#3b2f25"/><stop offset="0.55" stop-color="#211913"/><stop offset="1" stop-color="#0c0908"/></radialGradient>
<radialGradient id="rv" cx="0.35" cy="0.35" r="0.7"><stop offset="0" stop-color="#8a7766"/><stop offset="1" stop-color="#2a201a"/></radialGradient>
<filter id="grain" x="0" y="0" width="1" height="1"><feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="2" seed="7"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 0.5 0"/><feComposite in2="SourceGraphic" operator="in"/></filter>
<filter id="paper" x="-10%" y="-10%" width="120%" height="120%"><feTurbulence type="fractalNoise" baseFrequency="1.4" numOctaves="2" seed="3" result="n"/><feColorMatrix in="n" values="0 0 0 0 0.35  0 0 0 0 0.25  0 0 0 0 0.15  0 0 0 0.35 0" result="t"/><feComposite in="t" in2="SourceGraphic" operator="in" result="tt"/><feMerge><feMergeNode in="SourceGraphic"/><feMergeNode in="tt"/></feMerge></filter>
<filter id="shadow" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="3" stdDeviation="3" flood-color="#000" flood-opacity="0.75"/></filter>
<clipPath id="fc"><rect x="40" y="40" width="176" height="176" rx="8"/></clipPath>
</defs>
<rect x="6" y="6" width="244" height="244" rx="18" fill="url(#iron)" stroke="#5a483b" stroke-width="5"/>
<rect x="6" y="6" width="244" height="244" rx="18" fill="#000" filter="url(#grain)" opacity="0.55"/>
<path d="M16 14h224" stroke="#fff" stroke-opacity="0.08" stroke-width="3"/>
<path d="M30 60l18 6M200 34l14 10M220 190l-12 10M48 214l20-4" stroke="#000" stroke-opacity="0.35" stroke-width="1.4"/>
<g><circle cx="24" cy="24" r="8" fill="url(#rv)" stroke="#120d0b" stroke-width="1.5"/><circle cx="232" cy="24" r="8" fill="url(#rv)" stroke="#120d0b" stroke-width="1.5"/>
<circle cx="24" cy="232" r="8" fill="url(#rv)" stroke="#120d0b" stroke-width="1.5"/><circle cx="232" cy="232" r="8" fill="url(#rv)" stroke="#120d0b" stroke-width="1.5"/></g>
<rect x="36" y="36" width="184" height="184" rx="11" fill="url(#rim)" stroke="#0c0908" stroke-width="2"/>
<rect x="40" y="40" width="176" height="176" rx="8" fill="url(#fld)"/>
<g clip-path="url(#fc)">
<rect x="40" y="40" width="176" height="176" fill="#000" filter="url(#grain)" opacity="0.35"/>
<g filter="url(#shadow)"><g filter="url(#paper)" transform="translate(46 44) scale(2.56)">{art}</g></g>
<rect x="40" y="40" width="176" height="176" fill="none" stroke="#000" stroke-opacity="0.6" stroke-width="8"/>
</g>
</svg>'''


def main():
    out = sys.argv[1]
    sheets = "--sheets" in sys.argv
    size = 256
    if "--size" in sys.argv:
        size = int(sys.argv[sys.argv.index("--size") + 1])
    os.makedirs(out, exist_ok=True)
    from playwright.sync_api import sync_playwright
    from PIL import Image
    ids = list(C.keys())
    with sync_playwright() as p:
        b = p.chromium.launch()
        pg = b.new_page(viewport={"width": 512, "height": 512})
        for aid in ids:
            pg.set_content(f'<html><body style="margin:0;background:transparent">{icon_svg(aid)}</body></html>')
            path = os.path.join(out, aid + ".png")
            pg.locator("svg").screenshot(path=path, omit_background=True)
            if size != 512:
                Image.open(path).resize((size, size), Image.LANCZOS).save(path)
        # pop-up art and the name list for cgame
        for name, svg, w, h in (("plate", plate_svg(), 1120, 232), ("drip", drip_svg(), 32, 256)):
            pg.set_viewport_size({"width": w, "height": h})
            pg.set_content(f'<html><body style="margin:0;background:transparent">{svg}</body></html>')
            pg.locator("svg").screenshot(path=os.path.join(out, name + ".png"), omit_background=True)
        pg.set_viewport_size({"width": 512, "height": 512})
        with open(os.path.join(out, "list.txt"), "w", encoding="utf-8") as f:
            f.write("// RealRTCWX achievements: ID <tab> name <tab> description (generated by tools/gen_achievement_icons.py)\n")
            for aid in ids:
                name = C[aid][0].replace("\t", " ")
                if aid in DESC and " (" in name and not name.endswith("(hard)"):
                    name = name[:name.index(" (")]     # the description says it
                f.write(f"{aid}\t{name}\t{DESC.get(aid, '')}\n")
        if sheets:
            for g, (_, title) in GROUPS.items():
                items = [a for a in ids if C[a][1] == g]
                if not items:
                    continue
                cells = "".join(
                    f'<div style="width:176px;display:flex;flex-direction:column;align-items:center;gap:10px">'
                    f'<img src="file://{os.path.abspath(os.path.join(out, a + ".png"))}" width="160" height="160">'
                    f'<div style="font:13px/1.3 DejaVu Sans,sans-serif;color:#d8c9a8;text-align:center">{html.escape(C[a][0])}</div></div>'
                    for a in items)
                doc = (f'<html><body style="margin:0;background:#14100e;width:1240px">'
                       f'<div style="padding:28px 26px"><div style="font:700 26px DejaVu Serif,serif;color:#efe3c6;margin-bottom:22px">{title} '
                       f'<span style="font:400 16px DejaVu Sans;color:#8f7d63">({len(items)})</span></div>'
                       f'<div style="display:flex;flex-wrap:wrap;gap:24px 0">{cells}</div></div></body></html>')
                tmp = os.path.join(out, "_sheet.html")
                open(tmp, "w").write(doc)
                pg.set_viewport_size({"width": 1240, "height": 400})
                pg.goto("file://" + os.path.abspath(tmp))
                pg.screenshot(path=os.path.join(out, f"sheet_{g}.png"), full_page=True)
                pg.set_viewport_size({"width": 512, "height": 512})
            os.remove(os.path.join(out, "_sheet.html"))
        b.close()
    print(len(ids), "icons")


if __name__ == "__main__":
    main()
