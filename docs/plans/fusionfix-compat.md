# Plan: compatibility with NFSU2 FusionFix (when it is released)

Status (2026-11-07): ThirteenAG's WidescreenFixesPack renamed every widescreen fix to "Fusion Fix" in its source on
2026-11-04 (`source/NFSUnderground2.FusionFix`). The only NFSU2 release is still `NFSUnderground2.WidescreenFix.zip`
(tag `nfsu2`, asset updated 2026-09-12), built from `source/NFSUnderground2.WidescreenFix` (commit `dbe2c0eb`), and
the README recommends it. Start this plan once a FusionFix build for NFSU2 is released, from its released source.

## Goal
Split screen works with the released FusionFix at least as it does with WidescreenFix today: no crash, every feature
below still right in 2-6 players, the README's Mod compatibility row updated (recommended or not, and why). Where
FusionFix now does something the plugin works around, prefer the simpler combined result over keeping both.

## How FusionFix differs (from its unreleased source; check again against the release)
- Modules: `Input` replaces `Controller` (front-end prompts as button images from `$BUTTON_*$` tokens, its own pad
  reading, icon style by controller), `Compat` is gone; new `Options` (a "Plugins" category in the options menu and in
  the pause menu's options, rows built on the game's options screens), `RawMouse`, `TexturePack`, `StaticText`,
  `AntiAliasing`, `UpdateNotice`; `Camera`, `Rendering`, `Resolution`, `Windowed`, `Misc`, `Frontend` reworked.
- Settings: `settings.json` describes them (ids like `MAIN.ConfineCursor`, `CAMERA.FreeLook`) and the in-game Plugins
  page edits them; the ini name and keys may differ from `NFSUnderground2.WidescreenFix.ini`.
- Module name: probably `NFSUnderground2.FusionFix.asi`; code that looks for the WidescreenFix module or ini won't
  find it.

## Where the plugin depends on WidescreenFix today (check each)
| Area | Plugin code / doc | What to check with FusionFix |
|---|---|---|
| Free camera per player | `parts/camera.cpp` (chains in front of WSF's patched eCreateLookAtMatrix call; reads `NFSUnderground2.WidescreenFix.ini` `[CAMERA]`) | where FusionFix hooks the camera now, its setting names, per player stick and mouse still right |
| 2P HUD widening | `parts/hud_split.cpp`, `docs/hud.md` (WSF moves 1P HUD objects by exact FE position) | HUD positions in every layout and aspect ratio, FusionFix's HUD aspect option |
| Drag heat bar | `parts/gauges.cpp`, `docs/bugs/drag-heat-bar.md` (undoes WSF's inline hook at 0x4C6753 / transform hook 0x51B190) | bar on the tach in split drag, single player unchanged |
| Input | `parts/input.cpp` (`RepairOldWidescreenFixPoll` for old builds; FE action tables 1..3) | FusionFix's own pad reading and FE remaps against per-player pads, joins (START), Rematch (Y), Random (Y), takeover, deadzones |
| Help bar Random button | `parts/seats.cpp` `RandomButtonText`, `docs/frontend.md` | `$BUTTON_Y$ Random` should show the Y image (texts starting with `$` are left to its icon drawing); PlayStation icon style (`BUTTON_TRIANGLE`); detect the FusionFix module |
| Options and pause menus | `parts/seats.cpp` rows (Players, Cars, Opponents, Difficulty), per-player pause options | FusionFix's Plugins category and display rows on the same screens (UI_Options / UI_PauseOptions): row lists, the pause options of whoever paused |
| Resolution, window, rendering | `parts/core.cpp` (device size, `[DEBUG] ForceScreenW/H`), render parts, envmap, headlights, weather | window modes, MSAA / anti-aliasing, post FX per view, FOV option in split views |
| Textures | `SwapSplitMenuIcon` (HD menu icon) | FusionFix's texture pack loading next to the swap |

## Steps
1. Read the release's source; run `python -I tools/mod_overlap.py ThirteenAG/WidescreenFixesPack` pointed at
   `source/NFSUnderground2.FusionFix` and read every overlap, nearby patch and patch on an address the plugin reads.
2. Install FusionFix in place of WidescreenFix (keep the WidescreenFix files to switch back) with default settings.
3. Go through the table above: fix what breaks, and remove workarounds FusionFix makes unneeded.
4. Automated runs: `start_matrix.sh`, `cars_check.sh`, `next_race_check.sh 2`, a Free Run start, a drag race to the
   results (`drag_blow.py`), the free camera; screenshots of every layout (2-6 players), the options and pause menus
   and the track select help bar.
5. Ask the user only what needs a person: camera feel with the stick and mouse, how menus and HUD look.

## Done when
- Every row of the table is checked and fixed or documented; no crash or regression in the automated runs.
- README Mod compatibility names FusionFix with its status; the subsystem docs describe how the plugin works with it.
- This plan is deleted in the commit that finishes it.
