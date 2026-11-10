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
| crash | crash report: `scripts\NFSU2SplitScreen_crash.txt` (+ log) and a minidump per crash, always on |
| seats, next_race, standings | players 3..6 data, controller assignment, car select seats, Cars row, rematch; Next Race and Random; session points |
| input, input_ports | input merge, P3-P6 joypad bindings; 6 input port tables |
| render_core, render_passes, render_weather, render_envmap | layout and device hooks; streaks, blur, headlights; rain, weather, road reflections, overbright; car reflections, projectors, projection |
| local_players, racers | 6-player arrays; extra local players, standings, results, post-race scene, names, skins, AI cars |
| hud_split, hud_players, gauges, minimap_players, hud_gap, player_settings | HUD widening, quad HUD, minimaps; P3-P6 HUD packages; single-player gauges; minimap markers; HUD gap; per-player settings and HUD state |
| sound, threads_memory, streaming | listeners; audio fs and pool locks, streaming pools; streaming for P3-P6 |
| intro, free_run, race_rules, camera | intro scenes and grid; Free Run; Hurry timer and catch-up; start zoom, free camera |

## Plugin architecture

- `DllMain` → `Install()`. Install first runs `IsSupportedExe()`, which compares the original bytes at a few patched
  sites (a call another plugin redirected passes); on a mismatch it shows a message box and installs nothing. Then it
  reads `NFSU2SplitScreen.ini` (`LoadConfig`) and patches.
- Patch helpers: `WriteCall(at, fn)` replaces a `call rel32`; `WriteJump(at, fn, len)` writes a `jmp` and pads with NOPs;
  `MakeTrampoline(at, len)` copies whole instructions out to a callable stub. Naked asm hooks return with
  `push RET; ret`. Some patches rewrite immediates or displacements directly.
- Other plugins on the same sites ([bugs/foreign-hook-trampoline.md](bugs/foreign-hook-trampoline.md)). The ASI loader
  loads plugins alphabetically, so a mod named before this one has patched its sites already. Its code stays in the
  chain:
  - Function starts: when `at` already holds another plugin's `jmp rel32`, `MakeTrampoline`'s stub is a jump to that
    plugin's hook, and `WriteJump` on that site writes only 5 bytes, because the other plugin's trampoline returns to
    `at + 5`. A leading `call rel32` is re-aimed. Installs that check a function's original bytes use `IsCodeAt`,
    which also accepts such a jump.
  - Calls: `WriteCall` records the target each call had (`g_callSites`) and logs the ones outside the game
    (`call at X: chained to another plugin's function Y`). A hook that runs the original calls
    `CallPrev(site, FN_...)`, which is that target, or the game function when the site had no other plugin. Hooks
    shared by several sites pass `_ReturnAddress()`, asm hooks read a pointer set right after their `WriteCall`.
    Installs that check a call's target use `IsCallTo`. Hooks that replace a call instead of wrapping it (the view
    getters, the car select's seat switches, the time limit option) drop the other plugin's function there.
  - With Debug = 1 the log lists every call site (`call sites:`), for `tools/chain_test.sh`.
- **Per-frame entry**: `LayoutApplyHook`, a trampoline on the per-frame layout function 0x5C21E0 (called from
  0x5D2850). Each frame it runs: XInput update, HUD widening state, results counters, per-player settings sync, quad HUD
  mapping, gauge maps, seat logic, the debug mailbox and the split layout (`ApplySplitLayout`).
- **Mode predicates**:
  - `SplitMode()`: game mode 0x850078 == 4. That is the console split-screen mode, which also covers its menus.
  - `SplitRacing()`: SplitMode, NUM_PLAYERS 0x89E7C0 == 2 and LAYOUT 0x8707E0 == 3 (two player cameras).
- **Players 3-6 are "extra players"**: the game itself stays in 2-player mode (NUM_PLAYERS = 2), so all of its
  `== 2` logic keeps working. Players 3-6 (`g_extraPlayers` = players - 2) are added on top. Arrays that were sized
  for 2 are relocated, and loops/branches that only handled 2 players are hooked.
- `XInputDevices.cpp` provides virtual DirectInput gamepads backed by XInput ([input](input.md)).

## Thread safety

- **Game memory pools.** 0x440390 (alloc) and 0x440540 (free) link blocks into the pool's list outside the pool lock,
  and the sound thread uses them too. This caused list corruption and crashes, which got more likely with 3-4 cars
  loading banks. Both are wrapped in one critical section (`PoolAllocLocked` / `PoolFreeLocked`).
- **Audio file system.** Open and close (vtable 0x7C2E2C: +0xC open, +0x10 close; `CreateSoundFs` hooks the creation)
  are called from two non-main threads, and they share an unsynchronized free-handle list. `SoundFsOpen` /
  `SoundFsClose` serialize them. `SoundFsPopHook` (0x6F6AEC) makes an open with no free handle return -1 instead of
  crashing at 0x6F6AF9, and the failure is logged. Unlimiter (`SerializeBigFileVFS`) loads later and writes its own
  lock and guard over the same vtable entries and 0x6F6AEC without calling ours: the same protection, only without
  our "sound fs:" log lines.
- **Startup checks.** `Install` refuses any exe but v1.2 (entry point) and code other mods already patched
  (`IsSupportedExe`). With `Debug = 1` it logs every `.asi` in the scripts folder. It warns when Extra Options'
  `ExperimentalSplitScreenFix` is on, read from that mod's ini so the load order doesn't matter.

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
- The table relocations (`RelocateTextRefs`: port tables, Controls pages, local players, listeners, one walk of .text) run before any hook is installed, so hooks and trampolines only ever see relocated code.
- 2-entry tables indexed by `view id - 1` or `player index` are common (weather caches, tint, settings, FF channels):
  check for out-of-bounds use when views 5..8 or players 3..6 reach new code.
