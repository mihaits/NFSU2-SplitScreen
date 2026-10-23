# Plugin architecture

## Source layout
`src/NFSU2SplitScreen.cpp` holds the includes, `Install()` and `DllMain`, and `#include`s the files of `src/parts/` in a
fixed order: one translation unit, so a part can use the static state and helpers of the parts before it. Add new code
to the part of its subsystem (or a new part, included where its dependencies are defined). `build.bat` only compiles
`NFSU2SplitScreen.cpp` and `XInputDevices.cpp`.

| part | contents |
|---|---|
| core | game addresses, state, patch helpers |
| frontend, garage | main menu entry, Players row, garage backup, help bar; shared My Cars |
| log, debug_tools | debug log; hardware write watch and command mailbox |
| seats | players 3..6 data, controller assignment, car select seats |
| input, input_ports | input merge, P3/P4 joypad bindings; 4 input port tables |
| render_core, render_passes, render_weather, render_envmap | layout and device hooks; streaks, blur, headlights; rain, weather, road reflections, overbright; car reflections, projectors, projection |
| local_players, racers | 6-player arrays; extra local players, standings, results, post-race scene, names, skins, AI cars |
| hud_split, hud_players, gauges, minimap_players, hud_gap, player_settings | HUD widening, quad HUD, minimaps; P3/P4 HUD packages; single-player gauges; minimap markers; HUD gap; per-player settings and HUD state |
| sound, threads_memory, streaming | listeners; audio fs and pool locks, streaming pools; streaming for P3/P4 |
| intro, free_run, race_rules, camera | intro scenes and grid; Free Run; Hurry timer and catch-up; start zoom, free camera |

## Plugin architecture

- `DllMain` → `Install()`. Install first runs `IsSupportedExe()`, which compares the original bytes at the patched
  sites; on a mismatch it shows a message box and installs nothing. Then it reads `NFSU2SplitScreen.ini`
  (`LoadConfig`) and patches.
- Patch helpers: `WriteCall(at, fn)` replaces a `call rel32`; `WriteJump(at, fn, len)` writes a `jmp` and pads with NOPs;
  `MakeTrampoline(at, len)` copies whole instructions out to a callable stub. Naked asm hooks return with
  `push RET; ret`. Some patches rewrite immediates or displacements directly.
- **Per-frame entry**: `LayoutApplyHook`, a trampoline on the per-frame layout function 0x5C21E0 (called from
  0x5D2850). Each frame it runs: XInput update, HUD widening state, results counters, per-player settings sync, quad HUD
  mapping, gauge maps, seat logic, the debug mailbox and the split layout (`ApplySplitLayout`).
- **Mode predicates**:
  - `SplitMode()`: game mode 0x850078 == 4. That is the console split-screen mode, which also covers its menus.
  - `SplitRacing()`: SplitMode, NUM_PLAYERS 0x89E7C0 == 2 and LAYOUT 0x8707E0 == 3 (two player cameras).
- **Players 3/4 are "extra players"**: the game itself stays in 2-player mode (NUM_PLAYERS = 2), so all of its
  `== 2` logic keeps working. Players 3/4 (`g_extraPlayers` = players - 2) are added on top. Arrays that were sized
  for 2 are relocated, and loops/branches that only handled 2 players are hooked.
- `XInputDevices.cpp` provides virtual DirectInput gamepads backed by XInput ([input](input.md)).

## Thread safety

- **Game memory pools.** 0x440390 (alloc) and 0x440540 (free) link blocks into the pool's list outside the pool lock,
  and the sound thread uses them too. This caused list corruption and crashes, which got more likely with 3-4 cars
  loading banks. Both are wrapped in one critical section (`PoolAllocLocked` / `PoolFreeLocked`).
- **Audio file system.** Open and close (vtable 0x7C2E2C: +0xC open, +0x10 close; `CreateSoundFs` hooks the creation)
  are called from two non-main threads, and they share an unsynchronized free-handle list. `SoundFsOpen` /
  `SoundFsClose` serialize them.

## Gotchas

- **Inline asm that calls game code must be in a `__declspec(noinline)` function.** If it gets inlined, the compiler
  keeps SSE values in xmm registers across the call (it can't see the call), and the game clobbers them.
- **Preserve ECX** around 0x5370A0 (render context).
- **Game malloc (0x575620) isn't zeroed.** Memset before calling ctors that assume zeroed memory.
- **Do not write guessed fields in a live game.** A test write into what looked like a float field was a pointer, and
  it crashed.
- Display-related global caches (render targets, reflection textures) are invalid after a device reset; re-copy or
  recreate them.
- `SetRenderTarget` resets the viewport and scissor without calling SetViewport.
- `0x50CA00` / `0x50CA50` recurse into groups.
- Patch order matters: hooks inside code that `RelocateLocalPlayers` rewrites (e.g. 0x5D778D) are installed after it.
- 2-entry tables indexed by `view id - 1` or `player index` are common (weather caches, tint, settings, FF channels):
  check for out-of-bounds use when views 5..8 or players 3..6 reach new code.
