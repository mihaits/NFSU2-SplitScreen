# Plugin architecture

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
  check for out-of-bounds use when views 5/6 or players 3/4 reach new code.
