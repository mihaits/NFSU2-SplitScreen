# Mods that hook the same game code first: crash, and their code skipped (CrashingForeignCars, issue #5)
Status:       fixed 2026-11-09. Crash: `MakeTrampoline` chains a foreign `jmp`/`call rel32` (commit 53e3c11).
              No health bar: `WriteCall` records each call's previous target and hooks call it (`CallPrev`), and
              the game's local player block is kept as a copy of the relocated one (`MirrorLocalPlayers`).
Report:       [issue #5](https://github.com/mihaits/NFSU2-SplitScreen/issues/5), 0.13.0 with CrashingForeignCars 1.0.0
              (FRS Mods' damage mod) and nine other ASI mods. Crashed in the frontend, 6 s after load.
Repro:        `sh tools/chain_test.sh`: a test plugin that loads first hooks every trampoline site and redirects every
              `WriteCall` site. 0.14.0 crashes at load (eip in unmapped memory); the fixed build boots, and `RACE=1`
              starts a 2P split race. With the damage mod itself (installed by hand, read-only inspected first): damage
              on every car in split screen and Repair button in the garage (user), health bar in single player
              (screenshot, its log's `hud: health bar backing added`), 2026-11-09.
Area:         architecture.md (patch helpers, other plugins on the same sites), tooling.md (`chain_test.sh`)
Known:
- Crash report: access violation executing 0x71095850 on the main thread, mode 2 state 3; the value at esp was
  asi+B760, the return address of `call [g_origHelpBarSet]` in `HelpBarSet` (0x54E6E0, the help bar's set buttons).
  The minidump held `g_origHelpBarSet` = 0x03550000 (our trampoline); 0x71095850 isn't in any module.
- CrashingForeignCars hooks 12 game functions at their entry with a 5-byte `jmp rel32` (its installer copies whole
  instructions to its own trampoline and NOPs the rest): 0x4B0140, 0x5289F0, 0x547570, 0x54E6E0 (its "Repair" button
  in the button bar), 0x558790, 0x55B5C0, 0x55C230, 0x5FFE70, 0x60D7F0, 0x620AE0, 0x620C2B, 0x637D70. It redirects
  11 calls after checking each is an `E8`: 0x4A9931, 0x4E8F52, 0x4F38F1, 0x55DF22, 0x581493, 0x5B732D, 0x6005DB (the
  HUD update: its health bar), 0x60A1F5, 0x624499, 0x624C52, 0x625A27. Ours among them: 0x54E6E0 and 0x6005DB. The ASI
  loader loads plugins alphabetically, so its patches are there when ours install.
- With the crash fixed, the damage worked on every car in split screen and the Repair button showed in the garage,
  but the race health bar was missing in single player as well. Without our plugin the bar was there.
- With the calls chained the bar was still missing: its HUD function (0x1000D960 in the mod) ran, then drew only when
  `[0x8900B4] == player` (the game's first local player in creation order) and `[0x8900BC] == 1` (local player
  count). Both stayed 0: that block is the one `RelocateLocalPlayers` moves, so the game no longer writes it.
- Its bar is single-player only by its own design, so split screen has no health bar (the damage works on every
  car). Its HUD function draws only when the local player count is 1, for the first local player's car, and it finds
  the HUD by package name in the active package list (0x52CEF0) from a table of the 1P packages `HUD_SingleRace`,
  `HUD_Short_Track`, `HUD_Drift`, `HUD_Drag` (with a bar x for each). Split screen runs the 2P packages and our
  per-player copies (`HUD_SingleRac1.fng`, ...), and one bar at a fixed spot can't serve several players. Nothing on
  our side fixes that without lying to every mod (count 1) or renaming packages the frontend looks up: a per-player
  bar needs the damage mod to loop over the local players and take each player's HUD from the player.
- The headlight projectors were missing in the same tests, also with 0.14.0 and without the damage mod: not part of
  this bug.
Cause:
- Crash: `MakeTrampoline` copied the stolen bytes as they were. A `jmp rel32` is relative to its own address, so run
  from our trampoline it went to 0x03550005 + (0x6E093F30 - 0x54E6E5) = 0x71095850 instead of the damage mod's hook
  at 0x6E093F30.
- Health bar: `WriteCall(0x6005DB, HudUpdateHook)` overwrote the damage mod's redirect, and the hook called the game's
  HUD update 0x4EC5A0 directly. Our hooks are installed in every mode and most only pass the call on outside split
  screen, so the other mod's function was skipped in single player too. Once chained, it read the game's local player
  block, which the relocation leaves empty.
- Exe check: `IsSupportedExe` compared two `call` sites byte for byte, so a mod that redirects either call made the
  plugin refuse to install (the test plugin found it). Installs that compared a function start or a call target
  (EaApplyGains, the split Outrun results, the scene picks, the standings) turned their feature off the same way.
Fix:
- Function starts: a leading `jmp rel32` becomes a jump to the same target, so the call goes on into the other mod's
  hook and from its trampoline to the game; a leading `call rel32` is re-aimed. The other mod's trampoline returns to
  site + 5, so `WriteJump` on a chained site writes only its 5-byte jump and leaves the bytes after it.
- Calls: `WriteCall` records each call's previous target, and every hook that runs the original calls it through
  `CallPrev(site, FN_...)` (shared hooks: `_ReturnAddress()`; asm hooks: a pointer set after their `WriteCall`).
- Checks: `IsCallTo` (a call to the game function or to another plugin's) and `IsCodeAt` (the original bytes, or
  another plugin's jump in front of them) in the exe check and the installs.
- Local players: every frame `MirrorLocalPlayers` copies players 1-2 (by index and in order) and the count, capped at
  2, into the game's block at 0x8900AC, for mods that read it.
- Each chain is logged: `hook at X: chained to another plugin's jump to Y`, `call at X: chained to another plugin's
  function Y`. WidescreenFix's camera (0x453B44) shows up this way on every boot with WidescreenFix.
Not covered:
- A leading `jmp rel8` (a hot-patch jump) at a trampoline site is logged, not chained. No known mod does it.
- Hooks that replace a call instead of wrapping it drop the other plugin's function there: the view getters
  (0x48B1E0 call sites), the car select's seat switches, the force feedback output, the HUD bindings clear and the
  Outrun time limit option.
- A mod loaded after this one finds our patches instead of the original code.
- The exe check still compares the other sites' bytes (a mod that patches the car select's start or the race-mode
  write there makes the plugin refuse to install).
