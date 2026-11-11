# Open issues and leads

Closed bugs with their investigations are in [bugs/](bugs/); the journal is `NOTES.md`.

## Test coverage, 5-6 players
Tested with real controllers, 4 native DualSense pads over Bluetooth at once plus 1-2 Xbox pads:
- 6P, 5P and 3P circuit, 5P sprint, 4P drag; 6P drift results after the Hurry timer.
- Joining P1..P6, finish, results, the post-race scene with 5 humans, restarts, pause by P4..P6, the touchpad lightbar
  toggle, P5/P6 player LEDs and lightbar colours.
- 6P circuit on autopilot: frame scratch peak ~340 KB of 1 MB. 6P at 3840x2160 / 60 Hz holds 60 fps.

**Not covered yet:**
- 5-6P in URL, and in Street X with real pads (6P ran on autopilot, 2026-11-02); low risk: 6P on a big circuit was the worst case for streaming and memory (user,
  2026-10-31). 6P Free Run ran on autopilot only (2026-11-02), not with real pads. 5P with 1 AI opponent.
- Controls pages P5/P6 (rebind, saved to `[Player5]`/`[Player6]`); Customize from seats 5/6.
- USB speakers with 2 or more PlayStation pads on USB, in any mix (DualSense + DualShock 4 too): each should hear only
  their own car, and 3-6 on one USB 2.0 hub may run out of isochronous bandwidth (the stream then fails and the car stays
  on the PC).

## Known bugs and limitations
- **5-8 Xbox pads** were only checked with virtual 360 pads. See [input](input.md).
- **Two Xbox pads of one model can swap players on reconnect under Linux**: Wine gives no pad id, so the key is VID:PID
  ([input](input.md)). Follows from the code, not seen in a test.
- A **stray white line** across P3's minimap was seen once in a 5-view debug layout (`CloneViews`); not seen in real
  5P/6P races.
- **Podium lightbar:** 4th-6th place turn the bar off by design. When someone finishes, racer +0xA reads 1 for one frame
  (Street X: every finisher logged "place 1", then the right place); minor, left as is.
- **Boot hang at the splash:** WidescreenFix in exclusive fullscreen, also without this plugin (1 boot in 2 to 8); none in
  12 boots with `WindowedMode = 4` (borderless fullscreen).
- **Stutter at a steady 60 fps in borderless with monitors of different refresh rates:** the compositor can pace on the
  faster monitor's clock. 60 fps on a 75 Hz clock shows as 13 and 27 ms frames, 15 times a second, while the game's own
  timing stays even. Match the refresh rates or use exclusive fullscreen. `tools/screen_pacing` shows it.
- **Steam Input** (PlayStation support on) can expose a DualSense as an XInput pad too: one pad, two indices. Depends on
  the Steam Input config; not worth much, native input needs no Steam Input.
- **Cars row: player 2 picked instead of getting player 1's car once** (2026-11-06, Pick One, 2P drag, the first run
  after adding the row). Not reproduced in 10 runs since, also with `CarsMode` missing from the ini; cause unknown. The
  join logs "cars: <mode>, player N gets a car / picks" and "cars: player N gets <car>", which show the failing step
  if it happens again.
- **Traffic popping up close in plain view** (user, 2026-11-10; straight road, at least P1's view): not reproduced with
  the AI driving; leads that need a real session in [traffic-pop-in](bugs/traffic-pop-in.md).
- **A DualSense can drop with error 1167,** seen several times on one pad, also mid-race: likely its Bluetooth link (range,
  battery).
- **Controls pages of P3-P6 need room in the page table** (code review, 2026-11-08; never seen). `BuildControlsPages`
  appends the four only when the game's own page count + 4 fits the relocated table of 8; otherwise none, with no
  message. Static read: the game makes one page per device record with a joystick slot (0x4ACE00), and the only
  records with one are the keyboard's (0x5C10A3) and the two that 0x5CA690 gives slots 0 and 1, so at most 3 pages and
  the four always fit. To settle: open Controls with the keyboard, two pads and a wheel connected and read the log line
  "controls: pages 3..6" (the first number is the game's count). If it can't go above 3, the guard and this entry go.

## Missing car files hang the race load (2026-11-02)
A 2P + 4 AI race stayed on the loading screen at full frame rate, game state 5, race load step 4: the car loader
waited for `CARS\PEUGOT106\TEXTURES.BIN`, which that install doesn't have (see [streaming](streaming.md)). PEUGOT106
and CORSA are in the car table, but their folders aren't in every install, and an AI opponent got the 106: with
Unlimiter and ExtraOptions loaded, `UnlockRegionalCars` puts both in the AI list. ExtraOptions checks for the files and
skips its patch, but Unlimiter adds them anyway ([players](players.md)). Outside this mod's scope.
- Adding the two folders fixes that install, and so should `UnlockRegionalCars = 0`, which takes them out of the list.
- A stuck load was recovered live: with the files in place, set the geometry entry's state +0x5A and the texture
  entry's state +0x52 to 0, clear busy (+0x14), and run 0x63B160 once on the main thread. The race then started.

## Found by static analysis (2026-10-23)
The others are fixed (gearbox, minimap rebuild) or measured harmless (extra players' gauge textures, hud.md).
Same class as the custom gauge texture fix (35c1974): plugin work done before / around the game's step it depends on.
- **P3/P4's HUD is built before the player is registered** (confirmed; no visible effect, left as is). `CreatePlayers`
  (race_rules.cpp) sets `byIndex[idx]` only after the player ctor, which builds the HUD; its elements test
  `byIndex[2/3]` (0x4F6FF6 Turbo/NoTurbo + Drift_Angle_Backing1/3, 0x4F8014 BestDriftBacking, 0x4F9260 initial
  state) and skip those steps for P3/P4. Tried 2026-10-24: registering before the ctor also needs the racer lookups at
  0x4F7009 / 0x4F8023 redirected (`0x5ED980` has no slot for index 2: crash 0x4F701A). With both, a 3P drift race
  showed the same backing flags as without: the 2P layout has no drift angle backings and BestDriftBacking stays
  shown for everyone.
