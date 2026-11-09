# Crash with mods that hook the same function first (CrashingForeignCars, issue #5)
Status:       fixed 2026-11-09 (`MakeTrampoline` chains a foreign `jmp`/`call rel32`; `WriteJump` keeps its bytes)
Report:       [issue #5](https://github.com/mihaits/NFSU2-SplitScreen/issues/5), 0.13.0 with CrashingForeignCars 1.0.0
              (FRS Mods' damage mod) and nine other ASI mods. Crashed in the frontend, 6 s after load.
Repro:        `sh tools/chain_test.sh`: a test plugin that loads first hooks every trampoline site. 0.14.0 crashes
              at load (eip in unmapped memory); the fixed build boots, and `RACE=1` starts a 2P split race.
Area:         architecture.md (patch helpers), tooling.md (`chain_test.sh`)
Known:
- Crash report: access violation executing 0x71095850 on the main thread, mode 2 state 3; the value at esp was
  asi+B760, the return address of `call [g_origHelpBarSet]` in `HelpBarSet` (0x54E6E0, the help bar's set buttons).
  The minidump held `g_origHelpBarSet` = 0x03550000 (our trampoline); 0x71095850 isn't in any module.
- CrashingForeignCars hooks 12 game functions at their entry with a 5-byte `jmp rel32` (its installer copies whole
  instructions to its own trampoline and NOPs the rest): 0x4B0140, 0x5289F0, 0x547570, 0x54E6E0 (its "Repair" button
  in the button bar), 0x558790, 0x55B5C0, 0x55C230, 0x5FFE70, 0x60D7F0, 0x620AE0, 0x620C2B, 0x637D70, and rewrites
  the call at 0x55DF22. Only 0x54E6E0 is one of our sites. The ASI loader loads plugins alphabetically, so its hook is
  already there when ours installs.
Cause:
- `MakeTrampoline` copied the stolen bytes as they were. A `jmp rel32` is relative to its own address, so run from our
  trampoline it went to 0x03550005 + (0x6E093F30 - 0x54E6E5) = 0x71095850 instead of the damage mod's hook at
  0x6E093F30. Any mod loaded first that hooks one of our trampoline sites this way crashes the same way.
Fix:
- `MakeTrampoline`: a leading `jmp rel32` becomes a jump to the same target, so the call goes on into the other mod's
  hook and from its trampoline to the game. A leading `call rel32` is copied with its displacement corrected. Each
  chain is logged (`hook at X: chained to another plugin's jump to Y`).
- The other mod's trampoline returns to site + 5, so on a chained site `WriteJump` writes only its 5-byte jump and
  leaves the bytes after it as they are. Before this, where we steal more than 5 bytes, the NOP padding would have
  skipped the original instructions there.
- A leading `jmp rel8` (a hot-patch jump) can't be chained this way. It is logged, not handled. No known mod does it.
- Sites whose install first compares the original bytes (EaApplyGains 0x724F5D, OutrunOver 0x5027B0 with the rest of
  the split Outrun results) leave a foreign hook alone and turn their feature off. They don't crash.
- Not covered: a mod that patches one of the sites `IsSupportedExe` compares makes the plugin refuse to install (a
  message box), and a mod loaded after this one sees our jumps instead of the original bytes.
