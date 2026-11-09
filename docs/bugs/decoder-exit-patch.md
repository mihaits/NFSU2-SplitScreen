# decoder-exit-patch
Status:       fixed (2026-11-09)
Report:       the game crashes about a second after launch, before the main menu, on every start (user and automated
              boots alike). No plugin crash report; Windows logs an access violation at speed2.exe+0x1C20DA.
Repro:        every boot on 2026-11-09 from 14:56, the build of the commit before the fix included; the same build
              had booted earlier that day. Depends only on the plugin's load address (below).
Area:         [input.md](../input.md) "Analog bytes".

## Cause
`InstallAnalogBytes` wrote a 5-byte jump at 0x5C20D0 over `mov byte [esi+0x13], 0x80` (4 bytes) and the `pop edi`
at 0x5C20D4. The separate-pedal path then jumps to the plugin and never runs 0x5C20D4, but 0x5C20D4 is also where
the decoder's early exits land (0x5C189E keyboard read error, 0x5C18CA no buffered keys, 0x5C19DE capture running,
0x5C19F3 no joystick in the slot). The keyboard record takes 0x5C18CA on almost every frame. There the CPU ran the
top byte of the jump's offset instead of `pop edi`. That byte is (plugin address − 0x5C20D5) >> 24, so it depends on
where Windows loads the plugin: with the plugin at 0x57xxxxxx it was 0x57, `push edi`, which leaves the stack 8
bytes off for the epilogue's pops: ebp became −1 at `mov esp, ebp`, and `pop ebp` at 0x5C20DA faulted with ESP = −1.
With ESP invalid no exception handler could run, so the plugin wrote no report.

The epilogue is `pop edi; pop esi; pop ebp; pop ebx; mov esp, ebp; pop ebp; ret`, so the stack depth at 0x5C20D5
decides which saved value lands in ebp and then in esp. A stray byte 0x58..0x5F (a one-byte `pop reg`) keeps the depth
right: only edi and one scratch register come back wrong, which the callers survive. That is a plugin address of about
0x585C2100..0x605C20D4, where Windows' ASLR often puts it (0x5979E030 in a later run gives 0x59, `pop ecx`), which is
why the release build passed its tests. Any other byte shifts the pops and crashes: 0x57 `push edi`, or 0x68
`push imm32`, which swallows the next 4 bytes and runs `in eax, 0x5D` at 0x5C20D9 (exception C0000096, issue #6).
The base is chosen when the DLL is first mapped and is kept until the file changes or Windows restarts, so one machine
either crashes on every boot or on none.

## Measured (2026-11-09)
- Dumps (`%LOCALAPPDATA%\CrashDumps`): eip 0x5C20DA, esp = ebp = esi = edi = 0xFFFFFFFF, ebx 0x874C40 (record 0,
  the keyboard). The decoder's saved registers on the stack were intact, so the stack wasn't overwritten.
- Hardware execute breakpoints (a small Win32 debugger attached at launch): at 0x5C20D4 ESP 0x1AFA68 with the right
  saved registers; at 0x5C20D8 ESP 0x1AFA70 instead of 0x1AFA78, registers shifted by one.
- Live code diff of 0x5C20D0: `C6 46 13 80 5F` → `E9 9B BB 93 57`.
- Ruled out on the way: the new stick-binding code (crashed with it disabled and with the previous commit),
  native DualSense, WidescreenFix's `ImproveGamepadSupport`, Extra Options' `HeadlightsMode`. WidescreenFix's two
  mid-hooks in the decoder (0x5C188E keyboard, 0x5C1A07 front-end remap) only read or write the record.

## Fix
The jump moved to 0x5C20CD and covers 7 bytes, `mov [esi+0x10], cl` and `mov byte [esi+0x13], 0x80`, ending
exactly at 0x5C20D4. No branch lands inside 0x5C20CD..0x5C20D3. The hook does the throttle store itself; +0x13 is
written by `AnalogBytes` as before. The shared-pedal patch (0x5C2044, 8 bytes) ends at 0x5C204C, which nothing
jumps to except the hooks.

## Verified
Boot to the main menu; 2P race: `remap_check.py` (shared pedals, steering and separate pedal axes on the sticks)
and `pedal_check.py` (P1..P3 triggers) pass.
