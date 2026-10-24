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

## 5-6 players (branch `6p-support`, 2026-10-23)
Everything below was built and smoke-tested on virtual pads only (race start, driving, quit, screenshots, logs).

**Pending tests with real controllers:**
- 2-4P regression: circuit, sprint, drift, drag, URL, Street X, Free Run; pause by P3/P4; Controls pages P3/P4
  (rebind, survives a restart); rumble per pad; finish, results, restart, quit.
- 5-6P joining: Press START for players 5/6, car select order P1..P6 and Back from every seat, Customize from seats
  5/6 and the return to the right seat.
- 5-6P racing in every race type (drag is capped at 4 players); 5P with 1 AI opponent; finish and the post-race
  scene with 6 humans (`FollowCameraForSceneCars`, untested), restart, quit. Done 2026-10-24: 6P drift results
  screen after the Hurry timer (4 real pads, 2 idle virtual pads), correct. Done 2026-10-24 (autopilot): 6P circuit
  CC-IND #1 with the players spread over a quarter lap, every view drew its cars and scenery to the results (frame
  scratch peak ~340 KB of 1 MB, no overflow).
- Pause by P5/P6; Controls pages P5/P6 (rebind, saved to `[Player5]`/`[Player6]` of `NFSU2SplitScreen_Players.ini`);
  rumble for P5/P6.
- Two or more native DualSense pads at once (`kDsMax` raised to 8; only one was ever tested); P5/P6 player LEDs and
  lightbar colours (purple, cyan).
- Free Run with 6 players spread across the city (streaming: no missing sections); memory on the largest tracks
  (peak 874 MB virtual of 2 GB measured).
- Frame rate on a display faster than 75 Hz (6 views measured 75 fps = the 75 Hz cap). 2026-10-24, 6P drift SH Drift
  Hard #1 at 3840x2160 / 60 Hz, WSF `FPSLimit = 0`: 60 fps throughout (display cap), 46-55 for ~6 s around the
  countdown and start. Frame counter: 0x870818 (0x8665B8 is the 60 Hz sim tick).

**Known bugs and limitations:**
- **More than 4 Xbox pads** don't work (XInput limit); see [input](input.md).
- A **stray white line** across P3's minimap was seen once in a 5-view debug layout (`CloneViews`); not seen in real
  5P/6P races.
- Virtual pads: a 0.15 s START press for P3..P6 sometimes didn't register; 0.3 s always did. Check with real pads that
  a quick tap pauses.

## Found by static analysis (2026-10-23, not fixed yet)
Same class as the custom gauge texture fix (35c1974): plugin work done before / around the game's step it depends on.
- **P3/P4's HUD is built before the player is registered** (confirmed; no visible effect, left as is). `CreatePlayers`
  (race_rules.cpp) sets `byIndex[idx]` only after the player ctor, which builds the HUD; its elements test
  `byIndex[2/3]` (0x4F6FF6 Turbo/NoTurbo + Drift_Angle_Backing1/3, 0x4F8014 BestDriftBacking, 0x4F9260 initial
  state) and skip those steps for P3/P4. Tried 2026-10-24: registering before the ctor also needs the racer lookups at
  0x4F7009 / 0x4F8023 redirected (`0x5ED980` has no slot for index 2: crash 0x4F701A). With both, a 3P drift race
  showed the same backing flags as without: the 2P layout has no drift angle backings and BestDriftBacking stays
  shown for everyone.
- (speculative) The extra players' custom gauge texture requests use a no-op callback; the race load only waits for
  P1's. If the request queue isn't in order, their tach textures could appear late.
