# next-race-options-fallback
Status:       fixed (2026-11-08)
Report:       code review, 2026-11-08: Next Race skips the options screen and a new mode takes the last race's options,
              but only when the last round starts again by itself.
Repro:        a split race, Post Race → Next Race, Back on the track select, another mode on the mode select, a track;
              one of the last round's pads off (or a car gone from the garage), so player 1's popup can't take the last
              round. The new mode raced on its own stored settings (laps, direction, traffic, catch-up, difficulty from
              whenever it was last set), and no options screen came up to change them. A random series (it always
              changes the mode) with a Bluetooth pad that went to sleep between races does the same.
Area:         [frontend.md](../frontend.md) (Next Race).

## Cause
`CarryRaceOptions` ran inside the branch where player 1's popup takes the last round. When `LastRoundReady` failed,
`PressStartFilter` cleared `g_nextRace` and the players joined as usual, but the options screen had already been
skipped (`QrTrackNextHook`), so nothing gave the new mode the last race's options.

## Fix
Player 1's popup carries the options whenever Next Race reached it (`g_nextRace`), before deciding whether the last
round starts again. Tested 2026-11-08 with `tools/next_race_options_check.sh`: 3P circuit, Next Race, Back, Sprint
(its difficulty, traffic and catch-up set apart from the circuit's first), P2's virtual pad off, a track: the popup
joined as usual and Sprint had the circuit's direction, traffic, catch-up and difficulty.
