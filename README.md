# NFSU2 Split Screen

Local **split screen for 2-6 players** in *Need for Speed Underground 2* on PC.

![6-player split screen](docs/split-screen-6p.gif)

**Status: 0.10.0 alpha.** Every race mode works with 2-6 players, tested with real controllers (4 native DualSense pads
and 2 Xbox pads at once). Known issues are in [open issues](docs/open-issues.md).

## Install
1. You need `speed2.exe` **v1.2** and the [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).
   [WidescreenFix](https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/nfsu2) is recommended.
2. Download the [latest release](https://github.com/mihaits/NFSU2-SplitScreen/releases) and copy
   `NFSU2SplitScreen.asi` and `NFSU2SplitScreen.ini` into the game's `scripts` folder.
3. One controller per player. Xbox pads work directly (up to 4: the XInput limit); PlayStation DualSense pads work
   natively (USB or Bluetooth, up to 8) or through DS4Windows. For 5-6 players use native DualSense pads (DS4Windows
   off) for the players past the 4th Xbox pad, or the keyboard for one player.

## Play
1. Main menu → **Split-Screen** (next to Quick Race).
2. Pick the race type, track, the number of **Players**, **Opponents** (AI) and their **Difficulty**. **Free Run**
   (the last mode) drops you in the city with no race.
3. Each player presses **START** on their own pad and picks a car. One player can use the keyboard instead: press
   **Enter** on their Press START screen. Back undoes only your own step: on your car choice it returns to your Press
   START screen, and on the next player's Press START screen only you can go back to your car choice.

Every player has their own controls page (Options → Controls) and their own player options. Pausing opens the
options of whoever paused.

## Features
- [x] 2 to 6 players, in every race mode (Outrun is 2 players, drag up to 4): halves, quadrants, 3 + 2, 3 × 2
- [x] Each player has their own view, HUD, gauges, minimap, camera, sound and rumble
- [x] Full graphics in every view: reflections, rain, headlights, blur, light streaks, overbright, anti-aliasing
- [x] Own controls, options and last car per player
- [x] WidescreenFix support, including its free camera per player
- [x] Catch-up between players
- [x] Traffic around every player, not only players 1 and 2
- [x] AI opponents with difficulty (up to 6 racers in all, 4 in drag; none in sprint drift; 6 players leave no room)
- [x] Free Run: drive the whole city together, no race
- [x] DualSense without DS4Windows: input, rumble, player LEDs, RPM and nitrous lightbar, adaptive triggers
- [x] A controller that turns off keeps its player; press START on a spare one to take over that player
  (each player turns them on or off in the pause options)

## To do
- [ ] More than 4 Xbox pads (Windows.Gaming.Input backend)
- [ ] Session results: standings across back-to-back races
- [ ] Replays of split-screen races
- [ ] Rear-view mirror in the bumper and hood cameras, like single player
- [ ] Maybe: everyone picking a car at once; press START to join at any time
- [ ] Free Run: pick the start location; game modes like a waypoint race (see [Free Run ideas](docs/free-run.md#ideas-not-built-yet))

This is an alpha, so expect the odd bug. If you hit one, please
[open an issue](https://github.com/mihaits/NFSU2-SplitScreen/issues): fill in the [bug report template](docs/bug-report.md)
and attach `scripts\NFSU2SplitScreen.log` (set `Debug = 1` in the ini to write it).

## Settings
`NFSU2SplitScreen.ini` explains every option. The ones you might change:

| Setting | Default | |
|---|---|---|
| `Rumble` | 1 | controller rumble |
| `RumbleStrength` | 100 | rumble strength (%) |
| `AdaptiveTriggers` | 1 | DualSense triggers: resistance, grip-limit / limiter buzz, nitrous kick (default; per player in the pause options) |
| `TriggerStrength` | 100 | trigger effect strength (%) |
| `Lightbar` | 1 | DualSense lightbar shows engine rpm / nitrous while racing |
| `PlayerLeds` | 1 | DualSense player LEDs show the player number |
| `NativeDualSense` | 1 | DualSense pads without DS4Windows (off while DS4Windows's virtual pad is active) |
| `DualSenseRumbleMode` | 0 | 0 auto, 1 old firmware mode, 2 new firmware mode |
| `SinglePlayerGauges` | 1 | single-player style gauges for every player |
| `GaugeScalePercent` | 60 | gauge size |

## For developers
Build with Visual Studio 2022 Build Tools (C++, x86): run `build.bat`.
[`docs/TECHNICAL.md`](docs/TECHNICAL.md) explains how everything works, for anyone (or any AI agent) continuing the
work; [`tools/`](tools) has the testing and reverse-engineering helpers.

## License
MIT. Need for Speed Underground 2 is © Electronic Arts; this project contains no game code or data.
