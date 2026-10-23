# Tooling and workflow

The `tools/` scripts expect this repo checked out **somewhere inside the game folder**; `paths.py` finds the game
folder (the nearest parent holding speed2.exe). They read `<game>/speed2.exe` and `<game>/scripts/NFSU2SplitScreen.log`. Python 3 (32/64-bit), `capstone` for disassembly.

- **Static analysis** (from the exe file, no game needed):
  - `sdis.py addr [n]` disassembles.
  - `refs.py lo hi` finds immediate references to an address range (strings).
  - `callers.py addr` lists call/jmp sites.
  - `opscan.py substr [lo hi]` finds instructions by operand text.
- **Live process**:
  - `trainer.py` reads/writes memory and takes window screenshots.
  - `telemetry.py [interval] [seconds]` streams every local player's car values live (race stage, engine and
    shift-light rpm, gear, nitrous, grip used per wheel, wheels in the air, position, finished); read-only, needs the
    plugin loaded. The values the DualSense lights and triggers use.
  - `drcheck.py [--clear]` lists game threads that still have hardware breakpoints armed (e.g. after a `hwwatch.py`
    run); `--clear` zeroes DR0-3/6/7 on them.
  - `packages.py` lists the FE package cache.
  - `fe_objects.py <pkg>` dumps a package's object tree (flags, colour, texture key, position).
  - `strscan.py` / `ptrscan.py` search memory.
  - `hwwatch.py addr[,addr..] [--size] [--rw] [--seconds] [--max] [--all]`: hardware data breakpoints (DR0-3 on every
    game thread, attached as a debugger). Prints each new code site that writes (or reads) the address live, with
    registers and the code addresses on the stack. Found the post-race fall-through reset (body position writer).
    Stop it with Ctrl+C, never kill it: armed breakpoints without the debugger crash the game. Unlike the mailbox
    write watch (cmd 5) it needs no plugin and also catches other threads (sound).
  - `minidump.py <dmp>` shows the exception, registers and code addresses on the stack (dumps land in
    `%LOCALAPPDATA%\CrashDumps`).
- **Device loss / flicker log** (ini `Debug = 1`, `HookDeviceDebug`, installed from the first frame): logs Present
  failures, TestCooperativeLevel changes (with the free address space), every Reset with its call stack, and the
  activation/focus/size/display messages the game window receives ("d3d:" / "wnd:" lines). The game resets on
  TestCooperativeLevel == D3DERR_DEVICENOTRESET (0x5D2AFF / 0x5D2BE2 → 0x5D1C40, which calls Reset twice) and after a
  video option change (0x5D288B). Each reset recreates speed2.exe's D3DProxyWindow, which can be watched from outside
  the game without the plugin. A flicker loop (device lost every ~0.5 s with the game in front) was seen once and not
  reproduced; read this log while it happens.
- **Debug mailbox** (ini `Debug = 1`; the address is logged as "mailbox at ...", struct `Mailbox` in the source).
  Commands run on the game thread (`fe.py command`):

  | cmd | action |
  |---|---|
  | 1 | send FE message |
  | 2 | switch screen |
  | 3 | write dword |
  | 4 | virtual pad state (pads 4-9 = `fe.py pad n`, n 0..5) |
  | 5 | hardware write watch (logs the writer's code address and a stack scan) |
  | 6 | log SetViewport calls for N frames |
  | 7 | dump an IDirect3DSurface9* global to surfdump.raw |
- **Scene picks** (ini `Debug = 1`, `InstallScenePicks` in `parts/intro.cpp`): every intro/outro pick is logged
  ("scene pick: kind K, N candidates -> i"); `[DEBUG] ForceIntro` / `ForceOutro` = list index (-1 = random) force it.
- **Automation**:
  - `restart.sh` kills the game (waits until it is gone), installs build/NFSU2SplitScreen.asi and launches the game.
  - `boot.py` gets to the main menu (re-focusing the game and retrying while it loads).
  - `go_race_nointro.sh <mode>` starts a split race without the intro, joining players on virtual pads. Optional
    `POKES="addr=value"`; `INTRO=1` keeps the intro.
  - `go_1p.sh <mode>` starts a single-player race (a reference for comparisons).
  - `race_again.sh` starts the race again; `quit_race.py` quits to the menu.
  - `rec.py record|play` records and replays real pad input keyed to the active FE screens
    (`recordings/start_to_split2p.json`).
- The linker map `build/NFSU2SplitScreen.map` maps crash offsets in the ASI to functions (RVA = section offset +
  0x1000).
- When testing visuals, compare against single player (`go_1p.sh`) and dump the FE objects of both.
