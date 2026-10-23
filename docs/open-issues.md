# Open issues and leads

- **Light streaks without overbright** in split views ([rendering](rendering.md)).
- **Results ambient sound stutter (3P).** Continuous ~2 Hz gating, music off. Not frame hitches, sound fs handles, the
  nearest-player functions or ListenerUpdate (each was reverted live with no change). Left to check: the relocated
  listener arrays, or the original 2P behaviour.
- **Black results background**, seen once after a 2P race. If it happens again, capture state while it is still
  happening (g_currentView, views 1-4 active/rt, viewport).

## 5-6 players (branch `6p-support`, 2026-10-23)
Everything below was built and smoke-tested on virtual pads only (race start, driving, quit, screenshots, logs).

**Pending tests with real controllers:**
- 2-4P regression: circuit, sprint, drift, drag, URL, Street X, Free Run; pause by P3/P4; Controls pages P3/P4
  (rebind, survives a restart); rumble per pad; finish, results, restart, quit.
- 5-6P joining: Press START for players 5/6, car select order P1..P6 and Back from every seat, Customize from seats
  5/6 and the return to the right seat.
- 5-6P racing in every race type (drag is capped at 4 players); 5P with 1 AI opponent; finish, results and the post-race
  scene with 6 humans (`FollowCameraForSceneCars`, untested), restart, quit.
- Pause by P5/P6; Controls pages P5/P6 (rebind, saved to `[Player5]`/`[Player6]` of `NFSU2SplitScreen_Players.ini`);
  rumble for P5/P6.
- Two or more native DualSense pads at once (`kDsMax` raised to 8; only one was ever tested); P5/P6 player LEDs and
  lightbar colours (purple, cyan).
- Free Run with 6 players spread across the city (streaming: no missing sections); memory on the largest tracks
  (peak 874 MB virtual of 2 GB measured).
- Frame rate on a display faster than 75 Hz (6 views measured 75 fps = the 75 Hz cap).

**Known bugs and limitations:**
- **Boot hang at the splash, about 1 boot in 5** (not related to 6P: happens before any split code). Game not
  responding, all threads waiting; 2-3 in the game's `WaitForSingleObject(INFINITE)` wrapper 0x6F6335 (from 0x6F71F7 /
  0x6F5B29 -> 0x6F57E3 -> 0x75F617), log ends at `sound fs: N free handles (opened bf:/speech/UG2_Speech...)`.
  Suspect: the plugin's `SoundFsOpen`/`SoundFsClose` critical section (threads_memory.cpp) around the game's open/close,
  which take the game's own mutex inside: lock-order inversion with a thread that holds that mutex and closes.
- **Pause opened by P2/P4/P6 doesn't close with START**, only with Back / Continue (the game's own P2 pause behaves the
  same: frontend slot 1).
- **More than 4 Xbox pads** don't work (XInput limit); see [input](input.md).
- **P5's LED pattern** (all 5 LEDs on) looks like the flashing "this pad doesn't drive the game" pattern.
- A **stray white line** across P3's minimap was seen once in a 5-view debug layout (`CloneViews`); not seen in real
  5P/6P races.
- Virtual pads: a 0.15 s START press for P3..P6 sometimes didn't register; 0.3 s always did. Check with real pads that
  a quick tap pauses.

