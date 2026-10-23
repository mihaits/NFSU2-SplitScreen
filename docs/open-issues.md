# Open issues and leads

- **Light streaks without overbright** in split views ([rendering](rendering.md)).
- **Results ambient sound stutter (3P).** Continuous ~2 Hz gating, music off. Not frame hitches, sound fs handles, the
  nearest-player functions or ListenerUpdate (each was reverted live with no change). Left to check: the relocated
  listener arrays, or the original 2P behaviour.
- **Black results background**, seen once after a 2P race. If it happens again, capture state while it is still
  happening (g_currentView, views 1-4 active/rt, viewport).

## Found by static analysis (2026-10-23, not fixed yet)
Same class as the custom gauge texture fix (35c1974): plugin work done before / around the game's step it depends on.
- **P3/P4's HUD is built before the player is registered** (confirmed in the disassembly). `CreatePlayers`
  (race_rules.cpp) builds P3/P4 with the ctor as player `idx & 1` and sets `byIndex[idx]` only after it returns; the
  ctor writes `byIndex[0/1]` (0x5F1725) and builds the HUD (0x5F1A0C), whose elements read `byIndex[2/3]` (null): the
  element at 0x4F6C00 skips the per-car Turbo/NoTurbo script and the Drift_Angle_Backing1/3 binding (check 0x4F6FF6),
  0x4F7AD0 skips hiding BestDriftBacking (0x4F8014), and the initial state 0x4F9260 runs on a null player. Expected:
  wrong turbo backing and a stray best-drift backing on P3/P4's drift HUD. Fix: set `byIndex[idx]` before the ctor.
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
