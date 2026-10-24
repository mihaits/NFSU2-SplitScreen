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

## Found by static analysis (2026-10-23, not fixed yet)
Same class as the custom gauge texture fix (35c1974): plugin work done before / around the game's step it depends on.
- **P3/P4's HUD is built before the player is registered** (confirmed; no visible effect, left as is). `CreatePlayers`
  (race_rules.cpp) sets `byIndex[idx]` only after the player ctor, which builds the HUD; its elements test
  `byIndex[2/3]` (0x4F6FF6 Turbo/NoTurbo + Drift_Angle_Backing1/3, 0x4F8014 BestDriftBacking, 0x4F9260 initial
  state) and skip those steps for P3/P4. Tried 2026-10-24: registering before the ctor also needs the racer lookups at
  0x4F7009 / 0x4F8023 redirected (`0x5ED980` has no slot for index 2: crash 0x4F701A). With both, a 3P drift race
  showed the same backing flags as without: the 2P layout has no drift angle backings and BestDriftBacking stays
  shown for everyone.
- **P3/P4's car gets P1's / P2's settings +0x15/+0x16** (confirmed). The ctor copies them (0x5F17E9 / 0x5F17F2,
  redirected reader -> `g_settings[idx & 1]`) into player +0x20/+0x21, which the attach 0x5F3E10 applies to the car
  (+0x20 is forced to 1 in drag: probably the transmission). Expected: P3 picks Manual, but races with P1's choice.
  Fix: swap `g_settings[idx]` in around the ctor, or write +0x20/+0x21 from `g_settings[idx]` before the attach.
- **Minimap manager rebuilt without the hook** (likely). 0x4F92A0 destroys the manager through an unhooked call at
  0x4F92B1, empties the marker pool and builds a new one, but `g_mmExtendedMgr` isn't reset, so a new manager at the
  freed address is never extended: P3+ markers (and their minimaps' centre entities) would vanish. In a race it runs
  on race-state message 4 (0x606B32 -> 0x5EA980) when 0x89E7B0 != 0; which event sends it is unknown. Fix: hook the
  call at 0x4F92B1 with `DestroyMinimapManager` as well.
- (speculative) The extra players' custom gauge texture requests use a no-op callback; the race load only waits for
  P1's. If the request queue isn't in order, their tach textures could appear late.
