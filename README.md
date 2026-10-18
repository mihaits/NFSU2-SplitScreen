# NFSU2 Split Screen

An ASI plugin that adds native **2-4 player split screen** to *Need for Speed Underground 2* on PC.

The PC version still contains the console's two-player split-screen code, but its menu entry, the renderer's split path
and the per-player input were cut. This plugin restores all of that and extends the game to four local players. Each
player gets their own view, HUD, minimap, camera, sound, reflections and effects.

![4-player split screen](docs/screenshot-4p.png)

**Status: 0.9 alpha.** All race modes are playable with 2-4 players. See the checklists below.

## Requirements
- `speed2.exe` **v1.2**. The plugin checks the code it patches and disables itself on other versions.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`dinput8.dll` in the game folder).
- Recommended: [NFSU2 WidescreenFix](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/nfsu2). The split-screen HUD is
  widened to match it.
- One XInput controller per player (Xbox pads). PlayStation pads work through DS4Windows or any other XInput wrapper.

## Install
Copy `NFSU2SplitScreen.asi` and `NFSU2SplitScreen.ini` from the release into the game's `scripts` folder.

## Playing
1. Open the main menu and choose **2 Player Split-Screen**, right after Quick Race.
2. Pick a race type and track. On the race options screen, set **Players** (2-4).
3. On the car select, each player presses **START** on their own pad when prompted, then picks a car and gearbox.
   Back returns to the previous player.
4. Any player can pause.

Players 1 and 2 have their own joypad pages under Options > Controls (**Joypad Configuration - P1 / P2**). Players 3
and 4 use player 1's bindings. Outside split screen, the pad you used last drives and can edit either page.

## Settings (`NFSU2SplitScreen.ini`)
| key | default | |
|---|---|---|
| `[INPUT] XInputPads` | 1 | XInput pads are the game's gamepads |
| `[INPUT] HideDirectInputControllers` | 1 | hide raw DirectInput controllers so they don't take controller slots |
| `[HUD] SinglePlayerGauges` | 1 | each player's single-player gauges (custom tachs, drag HUD) |
| `[HUD] GaugeScalePercent` | 60 | gauge size |
| `[MAIN] Debug` | 0 | write `NFSU2SplitScreen.log` and enable the debug mailbox |

## Feature checklist

### Working
**Menus and input**
- [x] Main menu entry "2 Player Split-Screen", which runs Quick Race in the console's split-screen mode
- [x] Players row (2-4) on the race options screen
- [x] Console-style "Player N Press START" joining on each player's own pad, with Back to the previous seat
- [x] Per-player input from XInput pads (virtual DirectInput gamepads); disconnect and reconnect work mid-race and in menus
- [x] Joypad pages per player for P1/P2 in Options > Controls; rebinding works
- [x] Shared garage ("My Cars") for every player; Customize from any seat on the split car select
- [x] Pause from any player's pad, including P3/P4
- [x] Clean return to single player (garage backup, frontend state reset)

**Rendering**
- [x] Layouts: 2P top/bottom, 3P (player 3 centred at the bottom, side bars black), 4P quadrants
- [x] Per-view projection and aspect ratio
- [x] Full single-player track sections for every player; streaming covers all players; streaming pool scaled by
  player count
- [x] Sky, headlights (projector and light pools on the road for every car) and car light glares in every view
- [x] Road reflections and car (environment map) reflections per player
- [x] Weather per view: falling rain, splashes and lens droplets
- [x] Speed blur per view
- [x] Overbright per view, including each player's own nitrous intensity
- [x] Light glows with anti-aliasing
- [x] Particles and effects (tyre smoke, nitrous flames) for every player
- [x] Alt-tab / device reset

**HUD**
- [x] 2P HUD widened to the screen edges in widescreen
- [x] Full HUDs for players 3/4 (private copies of the HUD packages), with a consistent quadrant layout
- [x] Per-player minimaps showing every car's marker
- [x] Single-player gauges for every player, using each car's custom tachometer, boost and nitrous gauges
- [x] Single-player drag HUD in drag races
- [x] Live leaderboard gap (circuit, sprint, Street X, URL) and drift score gap for every player

**Races**
- [x] Race types tested with 3-4 players: circuit, sprint, drag, drift, Street X, URL. Outrun works with 2 players.
- [x] Race intros, with a fallback to the single-player scenes on tracks without a 2-player scene; intro sounds
- [x] Results and standings with 3-4 players, restart, quit, several races in a row
- [x] Per-player camera settings (drift camera included)
- [x] Per-player sound listeners, so every player hears their own surroundings
- [x] Names "Player 3" / "Player 4" for the extra players
- [x] Stability fixes for audio-thread races in the game's memory pools and sound file system

### To do
- [ ] Force feedback / rumble. It has never worked through the virtual pads; P3/P4 game-side rumble is blocked.
- [ ] Controls pages for players 3/4. They copy player 1's page at start-up, so later rebinds don't reach them.
  Undecided: always 4 pages, or one page per connected pad.
- [ ] Remember the last car per seat (P3/P4 start on P1/P2's car)
- [ ] Limit Outrun to 2 players in the Players row
- [ ] Check catch-up (rubber banding) with 3-4 players
- [ ] Remaining anti-aliasing checks (reflections, overbright, results screen)
- [ ] Light streaks without overbright in split screen (currently off in split views)
- [ ] Streaming with 4 real streamer positions. P3/P4's sections ride on positions 0/1, which is enough in testing.
- [ ] Known issues: ambient sound stutter on the results screen (3P); a black results background was seen once
  (not reproduced); a rare 4P quit-to-menu crash in a HUD binding list (mitigated, root cause unknown)
- [ ] Session results: standings across back-to-back races (design to be discussed)
- [ ] Stretch goals: simultaneous split-screen car select; dynamic join (press START to join, layout updates live)

## Building
Visual Studio 2022 Build Tools (C++ workload), x86. Run `build.bat`; the output is `build\NFSU2SplitScreen.asi`.

## Documentation
[`docs/TECHNICAL.md`](docs/TECHNICAL.md) explains how everything works, with the game addresses and functions
involved and the tooling. It is written so other developers (or AI agents) can continue the work.
[`tools/`](tools) contains the Python/shell helpers used for testing and reverse engineering.

## License
MIT. Need for Speed Underground 2 is © Electronic Arts; this project contains no game code or data.
