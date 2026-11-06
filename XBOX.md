# RealRTCWX — RealRTCW on Xbox

RealRTCWX is a port of [RealRTCW](https://github.com/wolfetplayer/RealRTCW), the
Return to Castle Wolfenstein overhaul by WolfETPlayer, to **Xbox Series X|S in
Developer Mode**. It plays the full campaign on a controller, with additions
made for this port:

- Ragdoll deaths, with each weapon's own force, hits that move the part they
  struck, and burning bodies that roll and scream
- Blood that sprays, drips, pools and paints walls, plus dismemberment and
  gibbing on the Extreme blood setting, and a Max Blood option for how long
  it stays
- Achievements, with an in-game browser
- Named saves, typed on an on-screen keyboard
- An HL2-style flashlight
- Crouch stealth
- Quick save and quick load on the controller
- Quality (60 FPS) and Performance (up to 120 FPS) modes

The game data is **not** included. You need your own copy of Return to Castle
Wolfenstein with RealRTCW installed.

---

## What you need

1. **An Xbox Series X or S in Developer Mode.** Retail mode can't run apps
   that aren't from the Microsoft Store. Developer Mode is Microsoft's official
   way to run your own apps, and individual developer accounts are free.
   Microsoft's guide:
   [Xbox Developer Mode activation](https://learn.microsoft.com/windows/uwp/xbox-apps/devkit-activation).
2. **A PC** on the same network, with a web browser.
3. **Your own game files.** Install Return to Castle Wolfenstein from Steam or
   GOG on a PC, then install RealRTCW over it. You'll need the game's
   **`main`** folder, the one with `pak0.pk3` and RealRTCW's `z_*.pk3` files.
   That's about 1.5 GB.

## 1. Download

Get the latest `RealRTCWX-Xbox-….zip` from this repository's
[Releases](../../releases) page and unzip it on your PC. Inside are:

- `RealRTCW_1.0.x.0_x64.msix`: the app
- `Dependencies\Microsoft.VCLibs.x64.14.00.appx`: a Microsoft library the
  app needs
- `RealRTCWX.cer` and `INSTALL.txt`

## 2. Install

1. On the Xbox, in Developer Mode, open **Dev Home**. Note the
   **Remote Access** address (for example `https://192.168.0.50:11443`) and
   turn on Device Portal if it's off.
2. Open that address in your PC's browser. Accept the certificate warning
   (the Xbox uses its own certificate) and sign in if you set credentials.
3. Go to **Home → Add**, choose the `.msix` file, and continue.
4. When it asks for dependencies, add
   `Dependencies\Microsoft.VCLibs.x64.14.00.appx`, then click **Start**.
5. Optional: in Dev Home's list of installed apps, you can open RealRTCW's
   details and set its type to **Game**. It runs fine either way.

## 3. Add the game files

Pick **one** of these.

### Option A: USB drive (easiest)

1. On your PC, create a folder named `RealRTCW` on a USB drive, and copy the
   game's `main` folder into it. The drive should end up with
   `RealRTCW\main\pak0.pk3`. `RTCW\main\…` or just `main\…` at the top of the
   drive also work.
2. Plug the drive into the Xbox and start RealRTCW. It finds the files by
   itself.

Keep the drive plugged in while you play. Your saves and settings are kept
on the Xbox, not on the drive.

### Option B: copy them onto the Xbox

1. Start RealRTCW once, so its folders get created, then quit it.
2. In Device Portal, open **File explorer → LocalAppData →
   RealRTCWX_… → LocalState**, create a folder named `main` if there isn't one,
   and upload every file from your game's `main` folder into it.

## Controls

The full layout is under **Controls** in the menu. A few combinations are
specific to this port:

| | |
|---|---|
| **View + D-pad Up** | Quick save |
| **View + D-pad Down** | Quick load |
| **Y** on the save screen | New save (opens the keyboard: A type, B delete, X space, Y shift, Start save) |
| **LB / RB** in the achievements browser | Previous / next page |

## Settings worth knowing

- **Game Options → View → Blood**: Default, Green, Disabled or Extreme
  (Extreme adds dismemberment and gibbing). **Max Blood**, just below it,
  sets how much blood stays and for how long: Low, Medium, High or
  Unlimited.
- **System → Graphics**: Quality / Performance mode, bloom, ragdolls.
- **System → Graphics → Diagnostic Logging**: turn this on before
  reproducing a bug you want to report (see below).

## Problems

- **The game won't start, or says it can't find pak0.pk3.** The game files
  aren't where it looks. Check that the USB drive has `RealRTCW\main\pak0.pk3`,
  or that the files are in `LocalState\main`.
- **Reporting a bug.** Turn on **Diagnostic Logging**, reproduce the
  problem, then grab these files from `LocalState` with Device Portal's file
  explorer and attach them to an issue on this repository:
  - `main\rtcwconsole.log`
  - `crash_info.txt`
  - `crashlog.txt`
  - `main\gore_log.txt`, for blood and ragdoll issues

## Credits and license

- [RealRTCW](https://github.com/wolfetplayer/RealRTCW) by WolfETPlayer and
  contributors.
- Built on [iortcw](https://github.com/iortcw/iortcw) and id Software's Return
  to Castle Wolfenstein source release.
- Xbox libraries: SDL3 for UWP and Mesa for UWP (OpenGL on Direct3D 12),
  from the [Xbox Emulation Hub](https://github.com/XboxEmulationHub) projects.

The code is licensed under the GNU GPL v3 (see `COPYING.txt`), and the full
source for every release is in this repository. Return to Castle
Wolfenstein's game data is the property of id Software and Bethesda, and it
is not distributed here.
