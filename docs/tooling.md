# Tooling and workflow

The `tools/` scripts expect this repo checked out **inside the game folder** (`<game>/<repo>/tools`). They read
`<game>/speed2.exe` and `<game>/scripts/NFSU2SplitScreen.log`. Python 3 (32/64-bit), `capstone` for disassembly.

- **Static analysis** (from the exe file, no game needed):
  - `sdis.py addr [n]` disassembles.
  - `refs.py lo hi` finds immediate references to an address range (strings).
  - `callers.py addr` lists call/jmp sites.
  - `opscan.py substr [lo hi]` finds instructions by operand text.
- **Live process**:
  - `trainer.py` reads/writes memory and takes window screenshots.
  - `packages.py` lists the FE package cache.
  - `fe_objects.py <pkg>` dumps a package's object tree (flags, colour, texture key, position).
  - `strscan.py` / `ptrscan.py` search memory.
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
  | 4 | virtual pad state (pads 4-7 = `fe.py pad n`) |
  | 5 | hardware write watch (logs the writer's code address and a stack scan) |
  | 6 | log SetViewport calls for N frames |
  | 7 | dump an IDirect3DSurface9* global to surfdump.raw |
- **Automation**:
  - `restart.sh` installs build/NFSU2SplitScreen.asi and launches the game.
  - `boot.py` gets to the main menu.
  - `go_race_nointro.sh <mode>` starts a split race without the intro, joining players on virtual pads. Optional
    `POKES="addr=value"`.
  - `go_1p.sh <mode>` starts a single-player race (a reference for comparisons).
  - `race_again.sh` starts the race again; `quit_race.py` quits to the menu.
  - `rec.py record|play` records and replays real pad input keyed to the active FE screens
    (`recordings/start_to_split2p.json`).
- The linker map `build/NFSU2SplitScreen.map` maps crash offsets in the ASI to functions (RVA = section offset +
  0x1000).
- When testing visuals, compare against single player (`go_1p.sh`) and dump the FE objects of both.
