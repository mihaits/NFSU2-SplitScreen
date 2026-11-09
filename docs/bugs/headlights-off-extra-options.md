# No headlight projectors (Extra Options' headlights off)
Status:       closed 2026-11-09, not a plugin bug: Extra Options' `HeadlightsMode = 0` (its G hotkey) turns them off
Report:       user, while testing issue #5: no lit patch on the road from the headlights, in single player and split
              screen.
Repro:        `HeadlightsMode = 0` in `scripts/NFSU2ExtraOptionsSettings.ini`, any race.
Area:         rendering.md (headlights)
Known:
- Missing with the current build, with the 0.14.0 release plugin, with and without the CrashingForeignCars damage mod,
  and with this plugin disabled. The game's own options were on (`OPT_CAR_HEADLIGHTS` 0x870CC4 = 1).
- Extra Options (ExOptsTeam/NFSU2ExOpts, `ExtraOptionsStuff.h` / `HotkeyStuff.h`): `HeadlightsMode` 0 off, 1 low beam,
  2 high beam (default). Mode 0 writes 0.0 to the float at 0x615950 (the headlight amount) and 0 to the byte at
  0x7A3658 (the light flare). The hotkey (`Headlights = 71`, the G key) cycles the mode while racing
  (game flow 6, window focused) and writes it back to the ini, so the setting survives restarts.
Fix:
- `HeadlightsMode = 2` in the ini (or G until the beams are back). Projectors back in a race (user, 2026-11-09).
- In split screen the setting is global: one G press switches every player's headlights.
