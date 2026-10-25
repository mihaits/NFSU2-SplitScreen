# Open issues and leads

- **Light streaks without overbright** in split views ([rendering](rendering.md)).
- **Results ambient sound stutter (3P).** Continuous ~2 Hz gating, music off. Not frame hitches, sound fs handles, the
  nearest-player functions or ListenerUpdate (each was reverted live with no change). Left to check: the relocated
  listener arrays, or the original 2P behaviour.
- **Black results background**, seen once after a 2P race. If it happens again, capture state while it is still
  happening (g_currentView, views 1-4 active/rt, viewport).
- **Boot hang at the splash, about 1 boot in 5** (2026-10-23, before any split code runs). Game not responding, all
  threads waiting; 2-3 in the game's `WaitForSingleObject(INFINITE)` wrapper 0x6F6335 (from 0x6F71F7 / 0x6F5B29 ->
  0x6F57E3 -> 0x75F617), log ends at `sound fs: N free handles (opened bf:/speech/UG2_Speech...)`. Suspect: the
  plugin's `SoundFsOpen`/`SoundFsClose` critical section (threads_memory.cpp) around the game's open/close, which take
  the game's own mutex inside: lock-order inversion with a thread that holds that mutex and closes.

## 5-6 players
Tested with real controllers on 2026-10-24: 4 native DualSense pads over Bluetooth at once plus 1-2 Xbox pads. 6P, 5P
and 3P circuit, 5P sprint, 4P drag; joining P1..P6, finish, results, the post-race scene with 5 humans, restarts,
pause by P4, the touchpad lightbar toggle. Earlier: 6P drift results after the Hurry timer, 6P circuit on autopilot
(frame scratch peak ~340 KB of 1 MB), 6P at 3840x2160 / 60 Hz holding 60 fps (46-55 around the countdown).

**Not covered yet:**
- 5-6P in drift, URL, Street X and Free Run (6 players spread across the city: streaming, memory; peak 874 MB virtual
  of 2 GB measured); the post-race scene with 6 humans (`FollowCameraForSceneCars`); 5P with 1 AI opponent.
- Pause by P5/P6; Controls pages P5/P6 (rebind, saved to `[Player5]`/`[Player6]`); Customize from seats 5/6.
- P5/P6 player LEDs and lightbar colours (purple, cyan).
- Frame rate on a display faster than 75 Hz.

**Podium lightbar at 5-6P** (user, 2026-10-24: maybe not shown at the race end; not remembered which pads). Not in
the log then; `finish:` and `lightbar: pad N` lines now log it. Candidates ([input](input.md), `parts/pad_lights.cpp`):
4th-6th place turn the bar off by design (2-3 players in a 5-6P race); the colour needs race stage >= 4, so it goes
if the stage drops at the results screen; P3-P6's finish flag (player +0x50) or place (racer +0xA) may not be set
(only checked with P1/P2).

**Known bugs and limitations:**
- **More than 4 Xbox pads** don't work (XInput limit); see [input](input.md).
- A **stray white line** across P3's minimap was seen once in a 5-view debug layout (`CloneViews`); not seen in real
  5P/6P races.
- Virtual pads: a 0.15 s START press for P3..P6 sometimes didn't register; 0.3 s always did. Check with real pads that
  a quick tap pauses.

## Multiple DualSense pads, static analysis (2026-10-24)
Fixed in a93b159: Bluetooth output flood (now at most 40 reports/s per pad) and the retry stall after a failed write.
- **Slots by first free index, not by pad:** players are bound to pad index 10 + slot. Two pads that drop and
  reconnect in the other order swap players; a different pad connecting first takes the dropped player's slot; a
  Bluetooth pad plugged into USB gets a new path and slot. Fix: key slots by the MAC (BT: `HidD_GetSerialNumberString`,
  USB: feature 0x09). Xbox pads have the same issue through the XInput user index, with no id to key on.
- No rumble after a reconnect while the game holds the same value: `g_dsOut` (and `g_rumbleSent` for Xbox pads) keeps
  the old motors and only changes are sent.
- P5's player LEDs (0x1F, all five) look like the unjoined pads' flash (0x1F / 0).
- DS4Windows exposing any virtual pad closes every native pad, also the ones it doesn't handle.
- Steam Input (PlayStation support on) can expose a DualSense as an XInput pad too: one pad, two indices.
- `OpenPad`'s logged-failure list (16 paths) is never cleared.
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
