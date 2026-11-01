#!/usr/bin/env python3
"""
Generates the Xbox campaign flow (misc/uwp/xbox/ui/campaign_menu.menu),
replacing the PC "Campaign" screen with controller-friendly steps:

  Campaign:      New Game | Choose Mission
  New Game    -> Difficulty (Y: Campaign Options) -> starts the campaign
  Choose Mission -> Mission list (LB/RB: chapter) -> Difficulty -> starts it

The options themselves (and Content Packs) are taken from the stock
main/ui/campaign_menu.menu, so they stay in step with RealRTCW.
B backs out one step everywhere (menu onEsc scripts).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_xbox_menus import split_items  # noqa: E402


def field(item, key):
    """Value of 'key' in an itemDef, quoted or not."""
    m = re.search(r'^\s*' + key + r'\s+(?:"([^"]*)"|(\S+))', item, re.M)
    return (m.group(1) if m.group(1) is not None else m.group(2)) if m else None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "main", "ui", "campaign_menu.menu")
DST = os.path.join(ROOT, "misc", "uwp", "xbox", "ui", "campaign_menu.menu")

GREEN = ".3 .6 .2 .55"
ROW = "1 1 1 .07"


def items(text):
    return [c for k, c in split_items(text) if k == 'item']


def set_rect(item, dx=0, dy=0):
    def f(m):
        x, y = float(m.group(1)) + dx, float(m.group(2)) + dy
        return 'rect %g %g%s' % (x, y, m.group(3))
    return re.sub(r'\brect\s+(-?[\d.]+)\s+(-?[\d.]+)(\s)', f, item, count=1)


def set_field(item, key, value):
    if re.search(r'^\s*' + key + r'\b', item, re.M):
        return re.sub(r'^(\s*)' + key + r'\b.*$', lambda m: m.group(1) + key + ' ' + value, item, count=1, flags=re.M)
    return item.replace('{', '{\n\t\t' + key + ' ' + value, 1)


def button(name, text, x, y, w, h, action, scale=".26", align="ITEM_ALIGN_CENTER"):
    ax = w // 2 if align == "ITEM_ALIGN_CENTER" else 10
    return f'''
	itemDef {{
		name {name}
		text "{text}"
		type ITEM_TYPE_BUTTON
		textfont UI_FONT_BIG
		textscale {scale}
		textalign {align}
		textalignx {ax}
		textaligny {int(h * 0.75)}
		rect {x} {y} {w} {h}
		style WINDOW_STYLE_FILLED
		border 1
		bordercolor .5 .5 .5 .3
		backcolor {ROW}
		forecolor 1 1 1 1
		visible 1
		mouseEnter {{ setitemcolor {name} backcolor {GREEN} }}
		mouseExit {{ setitemcolor {name} backcolor {ROW} }}
		action {{ {action} }}
	}}
'''


def label(text, x, y, w, scale=".22", color="1 1 1 .8", align="ITEM_ALIGN_CENTER", h=14):
    ax = w // 2 if align == "ITEM_ALIGN_CENTER" else 0
    return f'''
	itemDef {{
		text "{text}"
		textfont UI_FONT_BIG
		textscale {scale}
		textalign {align}
		textalignx {ax}
		textaligny {int(h * 0.8)}
		rect {x} {y} {w} {h}
		forecolor {color}
		visible 1
		decoration
	}}
'''


def panel(x, y, w, h, title=None):
    s = f'''
	itemDef {{
		rect {x} {y} {w} {h}
		style WINDOW_STYLE_FILLED
		border 1
		bordercolor .5 .5 .5 .5
		backcolor 0 0 0 .8
		visible 1
		decoration
	}}
'''
    if title:
        s += f'''
	itemDef {{
		text "{title}"
		textfont UI_FONT_NORMAL
		textscale .25
		textalign ITEM_ALIGN_CENTER
		textalignx {(w - 12) // 2}
		textaligny 14
		rect {x + 6} {y + 5} {w - 12} 20
		style WINDOW_STYLE_FILLED
		border 1
		bordercolor .5 .5 .5 .5
		backcolor .3 0.5 0.2 .25
		forecolor 1 1 1 1
		visible 1
		decoration
	}}
'''
    return s


def hints(text):
    return f'''
	itemDef {{
		rect 0 445 640 20
		style WINDOW_STYLE_FILLED
		backcolor 0 0 0 .6
		visible 1
		decoration
	}}
''' + label(text, 0, 448, 640, scale=".22", color="1 1 1 .9")


def menu(name, body, on_open="", on_esc="", fullscreen=1):
    return f'''
menuDef {{
	name "{name}"
	visible 0
	fullscreen {fullscreen}
	rect 0 0 640 480
	focusColor .9 .9 .9 1
	style WINDOW_STYLE_FILLED
	forecolor 0 0 0 0
	backcolor 0 0 0 0
	onOpen {{ {on_open} }}
	onEsc {{ {on_esc} }}
{body}
}}
'''


def main():
    stock = open(SRC, encoding="latin-1").read()
    its = items(stock)
    by_name = lambda n: [i for i in its if field(i, 'name') == n]

    background = by_name("realrtcw_background")[0] + "\n" + by_name("realrtcw_logo")[0] + "\n"
    preview = "\n".join(i.replace('{', '{\n\t\tdecoration', 1) for i in by_name("mappreview"))
    maplist = by_name("maplist")[0]
    chapter = by_name("chapter_type")[0]

    # --- 1. Campaign: New Game / Choose Mission
    hidden_list = set_field(maplist, "visible", "0")        # keeps the map selection in step
    campaign = background + hidden_list
    campaign += label("CAMPAIGN", 40, 120, 240, scale=".4", color="1 1 1 1", align="ITEM_ALIGN_LEFT", h=30)
    campaign += button("xb_newgame", "NEW GAME", 40, 170, 240, 32,
                       "setcvar ui_camp_chapter 0 ; uiScript loadCampaignArenas ; "
                       "close campaign_menu ; open xbox_difficulty_new", scale=".34", align="ITEM_ALIGN_LEFT")
    campaign += button("xb_mission", "CHOOSE MISSION", 40, 212, 240, 32,
                       "close campaign_menu ; open xbox_missions", scale=".34", align="ITEM_ALIGN_LEFT")
    campaign += hints("A  Select          B  Back")
    out = menu("campaign_menu", campaign,
               on_open="setcvar g_gameskill 2 ; uiScript loadCampaignArenas",
               on_esc="close campaign_menu ; open play_menu")

    # --- 2. Mission list
    ml = maplist
    ml = set_rect(re.sub(r'\brect\s+[-\d.]+\s+[-\d.]+\s+[\d.]+\s+[\d.]+', 'rect 40 140 280 290', ml, count=1))
    ml = set_field(ml, "elementheight", "18")
    ml = set_field(ml, "textscale", ".26")
    ml = set_field(ml, "visible", "1")
    # up/down always scroll it and A always picks the highlighted mission,
    # whether or not the list has focus (see padList / padA in ui_shared.c)
    ml = re.sub(r'^(\s*)name\s+\S+', lambda m: m.group(1) + 'name padList', ml, count=1, flags=re.M)
    prev = re.sub(r'\brect\s+29\s+146\s+229\s+114', 'rect 341 141 258 154', preview)
    prev = re.sub(r'\brect\s+28\s+145\s+230\s+115', 'rect 340 140 260 156', prev)
    prev = prev.replace("visible\t\t\t\t1", "visible 1")
    chap = re.sub(r'^(\s*)name\s+\S+', lambda m: m.group(1) + 'name padCycle', chapter, count=1, flags=re.M)
    chap = re.sub(r'\brect\s+[-\d.]+\s+[-\d.]+\s+290\s+12', 'rect 340 310 260 14', chap, count=1)
    chap = set_field(chap, "textalignx", "100")
    chap = set_field(chap, "textscale", ".24")
    chap = set_field(chap, "visible", "1")
    chap = chap.replace('{', '{\n\t\tdecoration', 1)        # changed with LB/RB, not focused
    missions = background + ml + prev + chap
    missions += label("LB / RB: change chapter", 340, 330, 260, scale=".2", color="1 1 1 .6")
    missions += '''
	itemDef {
		name padA
		visible 0
		decoration
		action { close xbox_missions ; open xbox_difficulty_mission }
	}
'''
    missions += hints("A  Select mission          LB / RB  Chapter          B  Back")
    out += menu("xbox_missions", missions, on_esc="close xbox_missions ; open campaign_menu")

    # --- 3. Difficulty (two copies so B returns to the right screen)
    skills = re.search(r'cvarFloatList\s*\{([^}]*)\}', [i for i in its if field(i, 'cvar') == 'g_gameskill'][0]).group(1)
    skills = re.findall(r'"([^"]+)"\s+(\d+)', skills)
    for kind, title, back in (("new", "NEW GAME", "close xbox_difficulty_new ; open campaign_menu"),
                              ("mission", "MISSION", "close xbox_difficulty_mission ; open xbox_missions")):
        mname = "xbox_difficulty_" + kind
        body = background
        body += preview.replace("visible\t\t\t\t1", "visible 1")
        body += label(title, 28, 268, 230, scale=".3", color="1 1 1 1")
        body += panel(300, 140, 300, 40 + 28 * len(skills), "Select Difficulty")
        for n, (text, val) in enumerate(skills):
            body += button(f"xb_skill{val}", text, 312, 172 + 28 * n, 276, 22,
                           f"close {mname} ; setcvar g_gameskill {val} ; setcvar g_level_was_selected 1 ; "
                           f"uiScript StartCampaign", scale=".25")
        body += f'''
	itemDef {{
		name padY
		visible 0
		decoration
		action {{ open xbox_campopts_{kind} }}
	}}
'''
        body += hints("A  Start          Y  Campaign Options          B  Back")
        out += menu(mname, body, on_esc=back)

    # --- 4. Campaign Options (Y), taken from the stock panel
    opts = [i for i in its if field(i, 'group') in ("campOptions", "campPacks") and field(i, 'cvar') != 'g_gameskill']
    shifted = []
    for i in opts:
        g = field(i, 'group')
        m = re.search(r'\brect\s+[-\d.]+\s+([-\d.]+)', i)
        dy = -15 if (g == "campOptions" and m and float(m.group(1)) > 170) else 0   # close the difficulty gap
        shifted.append(set_rect(i, dx=-120, dy=dy))
    window = by_name("campwindow1")[0]
    window = set_rect(re.sub(r'^(\s*)group\s+\S+', lambda m: m.group(1) + 'group campWin', window, count=1, flags=re.M), dx=-120)
    window = set_field(window, "backcolor", "0 0 0 .92")
    for kind in ("new", "mission"):
        body = window + "\n".join(shifted)
        body += hints("D-Pad  Change          B  Back")
        out += menu("xbox_campopts_" + kind, body,
                    on_open="show campWin ; show campOptions ; hide campPacks",
                    on_esc=f"close xbox_campopts_{kind} ; open xbox_difficulty_{kind}", fullscreen=0)

    header = ("// Xbox build override of ui/campaign_menu.menu, generated by tools/gen_xbox_campaign.py\n"
              "// from main/ui/campaign_menu.menu. Edit the generator, not this file.\n"
              '#include "ui/menudef.h"\n\n{\n')
    with open(DST, "w", encoding="latin-1") as f:
        f.write(header + out + "\n}\n")
    print("wrote", DST)


if __name__ == "__main__":
    main()
