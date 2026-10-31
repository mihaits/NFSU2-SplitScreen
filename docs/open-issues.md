# Open issues and leads

Fixed 2026-10-31: **tuned headlight colours turned white**, also in the original game: ps_1_1 clamps the colour
(a tuned 0x7878B4 is 1.41, 0.94, 0.94 before the clamp). Only the mod's ps_2_0 lit shaders kept it, with another car within
250 m ([rendering](rendering.md)).

Fixed 2026-10-30: **player 5's steering too sharp at speed**, the wheel check read past its 2-slot arrays
([details](bugs/p5-steering-sharp.md)). **Crash in the track streamer's pool** (0x5DEEE1), 192 block records ran out with 4-6
players far apart ([details](bugs/streamer-pool-crash.md)).

Closed 2026-10-27, not seen since their fixes:
- Results ambient sound stutter (3P, ~2 Hz gate) and the black results background: both came from the post-race scene
  (cars it holds were reset as fallen, 25f765b; streaming followed a car the scene parked at the origin, 3744558).
- Boot hang at the splash: WidescreenFix in exclusive fullscreen, also without this plugin (1 boot in 2 to 8);
  none in 12 boots with `WindowedMode = 4` (borderless fullscreen).

## 5-6 players
Tested with real controllers on 2026-10-24: 4 native DualSense pads over Bluetooth at once plus 1-2 Xbox pads. 6P, 5P
and 3P circuit, 5P sprint, 4P drag; joining P1..P6, finish, results, the post-race scene with 5 humans, restarts,
pause by P4, the touchpad lightbar toggle. Earlier: 6P drift results after the Hurry timer, 6P circuit on autopilot
(frame scratch peak ~340 KB of 1 MB), 6P at 3840x2160 / 60 Hz holding 60 fps (46-55 around the countdown). 2026-10-26:
P5/P6 player LEDs and lightbar colours, pause menus by P5/P6, the post-race scene with 5 humans.

**Not covered yet:**
- 5-6P in URL, Street X and Free Run; low risk: 6P on a big circuit was the worst case for streaming and memory (user,
  2026-10-31, after the streamer pool fix). 5P with 1 AI opponent.
- Controls pages P5/P6 (rebind, saved to `[Player5]`/`[Player6]`); Customize from seats 5/6.
- USB speakers with 2 or more PlayStation pads on USB, in any mix (DualSense + DualShock 4 too): each should hear only
  their own car, and 3-6 on one USB 2.0 hub may run out of isochronous bandwidth (the stream then fails and the car stays
  on the PC).

**Podium lightbar** (user, 2026-10-24/25). The 3P logs of 2026-10-25 ruled out the colour going at the results screen
(it stays until state 7) and a missing finish for P3. Found: in drift the place was the order on track (P1 crossed 5th,
1st by score); now `DriftPlace` from the standings. Left: 4th-6th place turn the bar off by design; when someone
finishes, racer +0xA reads 1 for one frame (Street X: every finisher logged "place 1", then the right place); minor,
left as is.

**Known bugs and limitations:**
- **5-8 Xbox pads** were only checked with virtual 360 pads. See [input](input.md).
- **Two Xbox pads of one model can swap players on reconnect under Linux**: Wine gives no pad id, so the key is VID:PID
  ([input](input.md)). Follows from the code, not seen in a test.
- A **stray white line** across P3's minimap was seen once in a 5-view debug layout (`CloneViews`); not seen in real
  5P/6P races.

## Multiple DualSense pads, static analysis (2026-10-24)
Fixed in a93b159: Bluetooth output flood (now at most 40 reports/s per pad) and the retry stall after a failed write.
- **Slots by first free index** (fixed: slots are kept per pad by its Bluetooth address, see [input](input.md); Xbox pads
  by `NonRoamableId`, same page).
- Steam Input (PlayStation support on) can expose a DualSense as an XInput pad too: one pad, two indices. Depends on
  the Steam Input config; not worth much, native input needs no Steam Input.
- DualSense 0 (hardware 00000514) dropped with error 1167 several times on 2026-10-24, also mid-race: likely its
  Bluetooth link (range, battery).

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
