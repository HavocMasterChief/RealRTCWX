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
from gen_xbox_menus import split_items, skulls_window  # noqa: E402


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


def survival_menus():
    """Survival, like the campaign: pick a map, then the options (with
    cheats and skulls), then Begin."""
    src = os.path.join(ROOT, "main", "ui", "survival_menu.menu")
    its = items(open(src, encoding="latin-1").read())
    by_name = lambda n: [i for i in its if field(i, 'name') == n]
    background = by_name("realrtcw_background")[0] + "\n" + by_name("realrtcw_logo")[0] + "\n"
    previews = "\n".join(i.replace('{', '{\n\t\tdecoration', 1) for i in by_name("mappreview"))
    maplist = by_name("maplist")[0]

    # --- 1. map selection
    ml = set_rect(re.sub(r'\brect\s+[-\d.]+\s+[-\d.]+\s+[\d.]+\s+[\d.]+', 'rect 40 140 280 290', maplist, count=1))
    ml = set_field(ml, "elementheight", "18")
    ml = set_field(ml, "textscale", ".26")
    ml = set_field(ml, "visible", "1")
    ml = re.sub(r'^(\s*)name\s+\S+', lambda m: m.group(1) + 'name padList', ml, count=1, flags=re.M)
    # the three previews, moved to the right of the list
    prev = previews
    prev = re.sub(r'\brect\s+29\s+146\s+151\s+114', 'rect 341 141 258 154', prev)
    prev = re.sub(r'\brect\s+28\s+145\s+152\s+115', 'rect 340 140 260 156', prev)
    prev = re.sub(r'\brect\s+186\s+146\s+72\s+54', 'rect 341 301 127 74', prev)
    prev = re.sub(r'\brect\s+185\s+145\s+73\s+55', 'rect 340 300 129 76', prev)
    prev = re.sub(r'\brect\s+186\s+206\s+72\s+54', 'rect 472 301 127 74', prev)
    prev = re.sub(r'\brect\s+185\s+205\s+73\s+55', 'rect 471 300 129 76', prev)
    prev = prev.replace("visible\t\t\t\t1", "visible 1").replace("visible\t\t\t1", "visible 1")
    filt = by_name("enemies_type")[0]
    filt = re.sub(r'^(\s*)name\s+\S+', lambda m: m.group(1) + 'name padCycle', filt, count=1, flags=re.M)
    filt = re.sub(r'\brect\s+[-\d.]+\s+[-\d.]+\s+290\s+12', 'rect 340 386 260 14', filt, count=1)
    filt = set_field(filt, "textalignx", "100")
    filt = set_field(filt, "textscale", ".24")
    filt = set_field(filt, "visible", "1")
    filt = filt.replace('{', '{\n\t\tdecoration', 1)
    body = background + label("SURVIVAL", 40, 104, 300, scale=".4", color="1 1 1 1", align="ITEM_ALIGN_LEFT", h=30)
    body += ml + prev + filt
    body += label("LB / RB: map filter", 340, 404, 260, scale=".2", color="1 1 1 .6")
    body += '''
	itemDef {
		name padA
		visible 0
		decoration
		action { close survival_menu ; open xbox_survival_opts }
	}
'''
    body += hints("A  Select map          LB / RB  Filter          B  Back")
    out = menu("survival_menu", body,
               on_open="uiScript loadArenas ; uiScript skill_surv",
               on_esc="close survival_menu ; open main")

    # --- 2. options + cheats
    opts = [i for i in its if field(i, 'group') == "srvCreate" and field(i, 'name') in ("window", "player_class", "soldier_desc", "gameplay")]
    opts = [set_field(i, "visible", "1") if field(i, 'name') != "soldier_desc" else i for i in opts]
    # descriptions show by cvarTest; the stock ones were shown with the group
    opts = [i.replace("visible 0", "visible 1") for i in opts]
    shifted = [set_rect(i, dx=-250) for i in opts]
    # class descriptions wrap inside the left panel
    shifted = [re.sub(r'\brect\s+50\s+185\s+500\s+500', 'rect 50 185 280 62', i) for i in shifted]
    b = background + label("SURVIVAL", 40, 104, 300, scale=".4", color="1 1 1 1", align="ITEM_ALIGN_LEFT", h=30)
    b += panel(40, 140, 300, 266)
    b += "\n".join(shifted)
    # cheats, on the right
    b += panel(360, 140, 240, 266, "Cheats")
    b += f'''
	itemDef {{
		name xbsurvcheats
		type ITEM_TYPE_YESNO
		text "Use cheats:"
		cvar "ui_xbSurvCheats"
		textfont UI_FONT_BIG
		textscale .22
		textalign ITEM_ALIGN_RIGHT
		textalignx 110
		textaligny 10
		rect 366 172 228 12
		style WINDOW_STYLE_FILLED
		backcolor 1 1 1 .07
		forecolor 1 1 1 1
		visible 1
	}}
'''
    b += label("Achievements are off for a run with cheats.", 366, 190, 228, scale=".18", color=".8 .72 .58 1")
    y = 210
    for cmd in ("god", "ammo", "onehit", "noclip"):
        b += f'''
	itemDef {{
		name xbsc_{cmd}
		type ITEM_TYPE_BUTTON
		cvar "ui_xbc_{cmd}"
		cvarTest "ui_xbSurvCheats"
		showCvar {{ "1" }}
		textfont UI_FONT_BIG
		textscale .22
		textalign ITEM_ALIGN_LEFT
		textalignx 10
		textaligny 13
		rect 366 {y} 228 18
		style WINDOW_STYLE_FILLED
		backcolor 1 1 1 .07
		forecolor 1 1 1 1
		visible 1
		action {{ uiScript xbCheat {cmd} }}
	}}
'''
        y += 22
    b += f'''
	itemDef {{
		name xbsc_skulls
		type ITEM_TYPE_BUTTON
		text "Skulls..."
		cvarTest "ui_xbSurvCheats"
		showCvar {{ "1" }}
		textfont UI_FONT_BIG
		textscale .22
		textalign ITEM_ALIGN_LEFT
		textalignx 10
		textaligny 13
		rect 366 {y + 6} 228 18
		style WINDOW_STYLE_FILLED
		backcolor 1 1 1 .07
		forecolor .9 .85 1 1
		visible 1
		action {{ open xbox_surv_skulls }}
	}}
'''
    b += button("xbsurvbegin", "BEGIN", 470, 413, 130, 24, "uiScript xbSurvBegin", scale=".3")
    b += '''
	itemDef {
		name padY
		visible 0
		decoration
		action { uiScript xbSurvBegin }
	}
'''
    b += hints("D-Pad  Change          A  Select          Y  Begin          B  Back")
    out += menu("xbox_survival_opts", b, on_open="uiScript xbCheat refresh ; uiScript xbSkull refresh",
                on_esc="close xbox_survival_opts ; open survival_menu")
    out += skulls_window("xbox_surv_skulls", "close xbox_surv_skulls")
    return out


def load_screen(name, background, ingame):
    """Saved games, newest first: name, mission, date, and a CHEATS tag."""
    close = f"close {name}"
    load = "uiScript Loadgame ; uiScript closeingame" if ingame else f"{close} ; uiScript Loadgame"
    back = f"{close} ; open ingame" if ingame else f"{close} ; open main"
    body = background
    body += label("LOAD GAME", 40, 92, 300, scale=".4", color="1 1 1 1", align="ITEM_ALIGN_LEFT", h=30)
    body += panel(36, 128, 568, 214)
    # column headings
    for text, x in (("NAME", 46), ("MISSION", 214), ("DATE", 384), ("", 540)):
        body += label(text, x, 132, 150, scale=".2", color=".85 .78 .64 1", align="ITEM_ALIGN_LEFT", h=12)
    # columns are in feeder order: 0 name, 1 date, 2 mission, 3 cheats
    body += f'''
	itemDef {{
		name padList
		rect 40 148 560 190
		type ITEM_TYPE_LISTBOX
		style WINDOW_STYLE_FILLED
		elementwidth 560
		elementheight 19
		textfont UI_FONT_NORMAL
		textscale .25
		elementtype LISTBOX_TEXT
		feeder FEEDER_SAVEGAMES
		columns 4  6 160 22  344 150 22  174 165 22  500 56 8
		textaligny 0
		border 1
		bordercolor .5 .5 .5 .3
		forecolor 1 1 1 1
		backcolor 0 0 0 0
		outlinecolor .3 .6 .2 .55
		visible 1
		doubleclick {{ {load} }}
	}}
	itemDef {{
		name sshot
		ownerdraw UI_SAVEGAME_SHOT
		rect 40 352 128 80
		cvarTest "ui_xbHasSave"
		showCvar {{ "1" }}
		visible 1
		decoration
	}}
	itemDef {{
		name info
		textsavegame
		autowrapped
		rect 180 352 420 80
		textalign ITEM_ALIGN_LEFT
		textscale .22
		textfont UI_FONT_NORMAL
		textaligny 14
		forecolor .9 .9 .9 1
		visible 1
		decoration
	}}
	itemDef {{
		name padA
		visible 0
		decoration
		action {{ {load} }}
	}}
	itemDef {{
		name padX
		visible 0
		decoration
		action {{ open {name}_delete }}
	}}
'''
    body += hints("A  Load          X  Delete          B  Back")
    out = menu(name, body, on_open="uiScript xbLoadSaves", on_esc=back)

    # delete confirmation
    d = panel(170, 180, 300, 110, "Delete this save?")
    d += label("It can't be brought back.", 170, 214, 300, scale=".22")
    d += button(f"{name}_del_no", "Keep it", 190, 236, 120, 24, f"close {name}_delete", scale=".24")
    d += button(f"{name}_del_yes", "Delete", 330, 236, 120, 24,
                f"uiScript DelSavegame ; uiScript xbLoadSaves ; close {name}_delete", scale=".24")
    out += menu(f"{name}_delete", d, on_esc=f"close {name}_delete", fullscreen=0)

    return out


def save_screen(name, background):
    """In-game Save: the same list as Load. Y makes a new save (named on an
    on-screen keyboard, the mission and time to start with), A overwrites
    the highlighted one after a check."""
    close = f"close {name}"
    back = f"{close} ; open ingame"
    body = background
    body += label("SAVE GAME", 40, 92, 300, scale=".4", color="1 1 1 1", align="ITEM_ALIGN_LEFT", h=30)
    body += panel(36, 128, 568, 214)
    for text, x in (("NAME", 46), ("MISSION", 214), ("DATE", 384), ("", 540)):
        body += label(text, x, 132, 150, scale=".2", color=".85 .78 .64 1", align="ITEM_ALIGN_LEFT", h=12)
    body += f'''
	itemDef {{
		name padList
		rect 40 148 560 190
		type ITEM_TYPE_LISTBOX
		style WINDOW_STYLE_FILLED
		elementwidth 560
		elementheight 19
		textfont UI_FONT_NORMAL
		textscale .25
		elementtype LISTBOX_TEXT
		feeder FEEDER_SAVEGAMES
		columns 4  6 160 22  344 150 22  174 165 22  500 56 8
		textaligny 0
		border 1
		bordercolor .5 .5 .5 .3
		forecolor 1 1 1 1
		backcolor 0 0 0 0
		outlinecolor .3 .6 .2 .55
		visible 1
		doubleclick {{ open {name}_over }}
	}}
	itemDef {{
		name sshot
		ownerdraw UI_SAVEGAME_SHOT
		rect 40 352 128 80
		cvarTest "ui_xbHasSave"
		showCvar {{ "1" }}
		visible 1
		decoration
	}}
	itemDef {{
		name info
		textsavegame
		autowrapped
		rect 180 352 420 80
		textalign ITEM_ALIGN_LEFT
		textscale .22
		textfont UI_FONT_NORMAL
		textaligny 14
		forecolor .9 .9 .9 1
		visible 1
		decoration
	}}
	itemDef {{
		name padY
		visible 0
		decoration
		action {{ uiScript xbSaveNameOpen }}
	}}
	itemDef {{
		name padA
		visible 0
		decoration
		action {{ uiScript xbSaveOverAsk }}
	}}
	itemDef {{
		name padX
		visible 0
		decoration
		action {{ uiScript xbSaveDelAsk }}
	}}
'''
    body += label("Y  NEW SAVE", 380, 98, 220, scale=".3", color=".95 .85 .55 1", align="ITEM_ALIGN_RIGHT", h=24)
    body += hints("Y  New Save          A  Overwrite          X  Delete          B  Back")
    out = menu(name, body, on_open="uiScript xbLoadSaves", on_esc=back)

    o = panel(170, 180, 300, 110, "Overwrite this save?")
    o += label("The old one will be replaced.", 170, 214, 300, scale=".22")
    o += button(f"{name}_over_no", "Keep it", 190, 236, 120, 24, f"close {name}_over", scale=".24")
    o += button(f"{name}_over_yes", "Overwrite", 330, 236, 120, 24,
                f"close {name}_over ; uiScript xbSaveOver", scale=".24")
    out += menu(f"{name}_over", o, on_esc=f"close {name}_over", fullscreen=0)

    d = panel(170, 180, 300, 110, "Delete this save?")
    d += label("It can't be brought back.", 170, 214, 300, scale=".22")
    d += button(f"{name}_del_no", "Keep it", 190, 236, 120, 24, f"close {name}_delete", scale=".24")
    d += button(f"{name}_del_yes", "Delete", 330, 236, 120, 24,
                f"uiScript DelSavegame ; uiScript xbLoadSaves ; close {name}_delete", scale=".24")
    out += menu(f"{name}_delete", d, on_esc=f"close {name}_delete", fullscreen=0)

    # naming a new save: the keyboard is an ownerdraw (ui_main.c, UI_XbKb_*)
    k = panel(60, 96, 520, 282, "Name your save")
    k += '''
	itemDef {
		name padGrid
		ownerdraw UI_XBOX_KEYBOARD
		rect 80 128 480 240
		forecolor 1 1 1 1
		visible 1
		decoration
	}
'''
    k += hints("A  Type     B  Delete     X  Space     Y  Shift     START  Save     VIEW  Cancel")
    out += menu(f"{name}_name", k, on_esc=f"close {name}_name", fullscreen=0)
    return out


def write_menu(fname, text, src):
    path = os.path.join(ROOT, "misc", "uwp", "xbox", "ui", fname)
    header = (f"// Xbox build override of ui/{fname}, generated by tools/gen_xbox_campaign.py\n"
              f"// (replaces {src}). Edit the generator, not this file.\n"
              '#include "ui/menudef.h"\n\n{\n')
    with open(path, "w", encoding="latin-1") as f:
        f.write(header + text + "\n}\n")
    print("wrote", path)


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
    # CONTINUE (newest save) above NEW GAME when there is a save; the
    # buttons below move down to make room (ui_xbHasSave, set on open)
    def when(item, has):
        test = f'\t\tcvarTest "ui_xbHasSave"\n\t\t{"showCvar" if has else "hideCvar"} {{ "1" }}\n'
        return item.replace("\t\tvisible 1\n", test + "\t\tvisible 1\n", 1)
    campaign += when(button("xb_continue", "CONTINUE", 40, 170, 240, 32,
                            "close campaign_menu ; uiScript xbContinue", scale=".34", align="ITEM_ALIGN_LEFT"), True)
    for has, y in ((False, 170), (True, 212)):
        sfx = "_s" if has else ""
        campaign += when(button("xb_newgame" + sfx, "NEW GAME", 40, y, 240, 32,
                                "setcvar ui_camp_chapter 0 ; uiScript loadCampaignArenas ; "
                                "close campaign_menu ; open xbox_difficulty_new", scale=".34", align="ITEM_ALIGN_LEFT"), has)
        campaign += when(button("xb_mission" + sfx, "CHOOSE MISSION", 40, y + 42, 240, 32,
                                "close campaign_menu ; open xbox_missions", scale=".34", align="ITEM_ALIGN_LEFT"), has)
    campaign += hints("A  Select          B  Back")
    out = menu("campaign_menu", campaign,
               on_open="setcvar g_gameskill 2 ; uiScript loadCampaignArenas ; uiScript xbLoadSaves",
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
                           f"setcvar g_cheatedRun 0 ; setcvar g_xbcheats 0 ; setcvar g_xbskulls 0 ; uiScript StartCampaign", scale=".25")
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

    write_menu("load.menu", load_screen("load_menu", background, False), "main/ui/load.menu")
    write_menu("survival_menu.menu", survival_menus(), "main/ui/survival_menu.menu")
    write_menu("ingame_load.menu", load_screen("ingame_load", background, True), "main/ui/ingame_load.menu")
    write_menu("ingame_save.menu", save_screen("ingame_save", background), "main/ui/ingame_save.menu")


if __name__ == "__main__":
    main()
