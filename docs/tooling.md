# Tooling and workflow

The `tools/` scripts expect this repo checked out **somewhere inside the game folder**; `paths.py` finds the game
folder (the nearest parent holding speed2.exe). They read `<game>/speed2.exe` and `<game>/scripts/NFSU2SplitScreen.log`. Python 3 (32/64-bit), `capstone` for disassembly.

- **Static analysis** (from the exe file, no game needed):
  - `sdis.py addr [n]` disassembles.
  - `refs.py lo hi` finds immediate references to an address range (strings).
  - `callers.py addr` lists call/jmp sites.
  - `opscan.py substr [lo hi]` finds instructions by operand text.
  - `mod_overlap.py owner/repo ... [--names]` compares our patch sites with another mod's public source (read through
    `gh`, in memory): byte overlaps, near misses, patches on addresses we read or call, and with `--names` that mod's
    function names for our addresses. It sees literal patches only, so a clean result is no substitute for a test.
  - `trackmarkers.py [--track id] [--type text] [--json out] [--tracks] [--verify]` reads every track's position
    markers from the game files: start grids (`Forward/BackwardStartingPosition`, the ones 0x5EB5D0 returns),
    start and finish lines, and track 4000's shop and crib spots (x y z heading). Also the track table with each
    TRACKMAP's calibration. File formats in its docstring; `--verify` checks spots measured in game.
  - `trackmap_explorer.py [-o path]` writes `build/trackmap.html`, a self-contained page with the markers on the game's
    TRACKMAP textures (city, speedways, drift, drag, Street X): filter by track and type, hover for values, click a
    marker or any point (z estimated from the nearest markers, check it in game when far from one), turn it with
    right-drag or Q/E, with a 6-car grid preview. It gives the `kFreeRunStarts` line and `[DEBUG] FreeRunStart`.
    Needs numpy and Pillow.
  - `fxdump/`: the game's shaders. `extract.py` writes the compiled effects (PE resources `IDI_*_FX`) to
    `fxdump/build/fx`; `fxdump.exe` (32-bit, `build.bat`, uses SysWOW64 `d3dx9_33.dll`) `dis` disassembles them,
    `hash` lists each shader's token hash (the plugin recognises shaders by it), `asm` / `frag` assemble.
    `gen_shaders.sh` regenerates `src/parts/headlight_shaders.h` from `src/shaders/*.asm`.
- **Live process**:
  - `trainer.py` reads/writes memory and takes window screenshots.
  - `[DEBUG] ForceScreenW/H` (any size, with WidescreenFix `WindowedMode = 1`) forces the device size at 0x5B9A60, for
    shapes the driver doesn't list.
    WidescreenFix isn't told about the forced size, so FE units stay 16:9 and the HUD is squeezed into the
    centre; use it for the 3D view and the HUD's layout within a cell, not the HUD's position on screen.
  - `set_resolution.sh W H` sets the game's resolution for the next boot (closes the game). WidescreenFix keeps it in
    `HKCU\Software\EA Games\...`, `g_RacingResolution = (0x8000 | W) << 16 | H`, and the mode must be one the driver
    lists (a custom NVIDIA 960x1080 is the faithful test of a tall span). It writes through explorer: `reg` from a
    shell inside the Claude app changes a private view of HKCU that the game never reads.
  - `plugin.py` (for tools): the loaded plugin's base, map symbols and `LocalPlayers` (its local player array; the
    size comes from `MAX_LOCAL_PLAYERS` in the checkout's source, 4 on main, 6 on 6p-support). When the installed asi
    isn't build/'s (an A/B test with an older build), the map doesn't fit: `LocalPlayers` then reads the players from
    the race's human cars (`race_players`, needs a race).
  - `traffic_watch.py [sample_s] [seconds] [--radius 250]` streams the traffic around each local player (count within
    the radius, nearest car) and logs traffic cars that teleport (recycled) within the radius of a player ("pop-in");
    totals at the end. Read-only.
  - `teleport_watch.py [seconds] [--min m] [--grid] [--csv path]` samples every race car about 500 times a second and
    prints each move its velocity can't explain, with the car's state on both sides; every sample goes to the csv.
    Read-only. A jump found this way can then be traced to its writer with `hwwatch.py` on the body position.
  - `telemetry.py [interval] [seconds]` streams every local player's car values live (race stage, engine and
    shift-light rpm, gear, nitrous, grip used per wheel, wheels in the air, position, finished, pedals, ground and
    wheel speed, wheel radius, slip ratio); read-only, needs the plugin loaded. The values the DualSense lights and the
    DualSense / Xbox triggers use.
  - `steer_log.py [--players 4,5] [--speed 20] [--top 200] [--need 200] [--csv path]` compares players' steering live
    (stick, filter output, speed, pedals, velocity) and writes every sample to a csv; read-only. Compare with matched
    throttle: the output depends on it.
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
  - `threadstacks.py [bytes]` prints every game thread's EIP and the return addresses on its stack (module+offset),
    each thread suspended while read: for a hung game (sample twice to see what doesn't move).
  - `hotspots.py [seconds] [--stage n]` samples the main thread for a low frame rate: time by module, this plugin's
    functions, `ntdll` callers (faults and waits show up here) and the hottest return addresses in the game. With
    `Debug = 1` the log also has a `frame:` line a second: fps, worst frame, CPU time and the wait in Present.
  - `screen_pacing.exe [seconds]` (`build_screen_pacing.bat`) records what reaches the primary monitor through Desktop
    Duplication: the gap between shown frames and every hitch. For stutter the game's own timing can't see.
  - `minidump.py <dmp>` shows the exception, registers and code addresses on the stack (dumps land in
    `%LOCALAPPDATA%\CrashDumps`, and the plugin's own in `scripts\NFSU2SplitScreen_crash_*.dmp` with
    `NFSU2SplitScreen_crash.txt`: registers, memory at each register, stack, EBP chain, game state).
    `dumpmem.py <dmp> addr..` prints 64 bytes at each address if the dump has them.
  - `ds_usb_speaker.cpp`: `build\ds_usb_speaker.exe n` plays test n (1-4, one per run, build line in the file) on a USB
    DualSense's speaker through its audio endpoint, to check the output path and channels by ear.
  - `ds4_bt_speaker.py [--list] [--pad n] [--report 0x14|0x17] [--path 0x02|0x24]`: the same for a Bluetooth DS4 (SBC from ffmpeg).
  - `ds4_usb_speaker.cpp`: `build\ds4_usb_speaker.exe [speaker] [headphones]` plays a tone on a USB DS4's endpoint with those volumes (build line in the file).
  - `sbc_test.cpp` + `sbc_check.py`: encode test tones with `src/SbcEncoder.h`, decode with ffmpeg, print the SNR.
  - `ds_bt_speaker.py [--list] [--pad n] [--file f | --freq hz --seconds s] [--gain g]` plays a sound on a Bluetooth
    DualSense's speaker (no game needed): Opus frames from ffmpeg/libopus in HID output report 0x39, after a 0x31
    speaker route. The report layout is in the script's docstring. Experiment for per-player audio.
  - `ea_voices.py [--watch s]` lists the EA mixer voices playing (gains) and searches the sound objects the plugin
    logged ("sound ... listener", Debug = 1) for their channels (see docs/sound.md, voice owners).
  - `car_sounds.py [--watch s]` (in a race) lists every car's sound objects (each has +0x44 = the car's audio node)
    by class and player, and the EA voices each holds; `--watch` prints the ones whose voices change.
  - `unclaimed_voices.py [seconds=8]` (in a split race, read-only) prints the playing voices whose stereo balance
    follows one car's engine over time but that the pad speakers don't route, with their patch name and owning
    object (or bank): finds car sounds missing from `PadSpeakerSounds`. `--all` lists every playing voice once, with
    its gains, routed player (or PC) and patch. A patch's voices are the live handles anywhere in its instance, as the
    plugin finds them.
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
    The scripts stop at the first failed stage: a failed restart (the build not installed) ends them, and a stage piped
    through `tail -1` to keep the output short keeps its exit code (`set -o pipefail`).
  - `boot.py` gets to the main menu (re-focusing the game and retrying while it loads).
    `PROFILE=n` picks the profile n rows down the list (also through `go_race_nointro.sh`).
    `restart.sh` and the `go_*.sh` scripts give it 20 s (a boot takes about 8 s). A boot that hangs on the startup loading screen (main thread
    waiting in d3d9) is WidescreenFix in exclusive fullscreen, also without this plugin: use its `WindowedMode = 4`.
  - `go_freerun.sh [start]` restarts the game and starts a split-screen Free Run from the Start row's entry `start`
    (`[FREERUN] Start`, set in the installed ini for that launch and restored), joining every player on virtual pads
    (`freerun_flow.py`: mode select, options, Press START..., loading; a timestamped line per step, a few seconds'
    timeout each), and waits for the logged start spots. `VIRTUAL=n` joins players 1..n and stops at the next Press
    START for real pads. `go_race_nointro.sh` doesn't fit Free Run's screens (no track list).
  - `go_outrun.sh` starts a split Outrun in about 15 s (`freerun_flow.py` with `RIGHTS=6`, for modes whose options
    screen follows the mode select); `PLAYERS=n` joins n players (the ini's `ExtraPlayers` must be n - 2).
    `autorace.py --players 1` then lets P1 pull away and win; the autopilot can turn back toward the others, so give it
    a long `--stall` (Outrun has no route progress). `outrun_watch.py` prints the race's racer list live (order, state,
    place, gap). `outrun_check.sh [n ...]` (default 2 3 6) plays 2 races per player count (the second through
    Next Race) with a screenshot per post-race screen and the logged grid and session points. `outrun_timelimit.py` checks the Time Limit end on a round loaded with it set (see its
    docstring): places by the time in the lead the plugin logs. `outrun_series.sh` checks a random series that rolls
    Outrun (`[DEBUG] RollMode = 7`, set and restored): the race, Post Race, and Next Race rolling Outrun again with
    no input.
  - `go_race_nointro.sh <mode>` starts a split race without the intro, joining players on virtual pads. Optional
    `POKES="addr=value"`; `INTRO=1` keeps the intro; `TRACK=id` picks that track in the list.
  - Fewer real pads than players: set `[DEBUG] ExtraPlayers` for the player count (read at boot), then
    `VIRTUAL=2 go_race_nointro.sh <mode>` joins P1/P2 on virtual pads and stops at P3's Press START; the user joins
    the rest on real pads. P1/P2 idle at the start line, so a real player finishing starts the Hurry timer and the
    results show the idle ones as unfinished. Set `ExtraPlayers` back afterwards.
  - `go_1p.sh <mode>` starts a single-player race (a reference for comparisons).
  - `drag_blow.py <shot dir>` drives a started drag race to the results without steering: every engine is blown
    (full throttle in 1st, players released 1 s apart), heat logged per car; `PERCAR=1` screenshots each player's view
    at heat 1.0. The autopilot doesn't work in drag (0x5F3AB2 rewrites the mode every frame). Pad: A shifts up, X down.
  - `autorace.py` lets the game's AI drive every local player's car (the game's autopilot: player +0x230 = 4, see
    [addresses](addresses.md)) and follows the race to the results: a progress line every 5 s, screenshots, fails on
    a stuck car or timeout. `--players 1,3` only those (drive the rest on real pads); `--stagger s` releases the cars
    s seconds apart to spread them around the track (a parked car can get hit: use a long `--stall`); `autorace.py off`
    gives the cars back. A full automated race: `go_race_nointro.sh <mode>`, then `python -I autorace.py`. Keep test races short:
    the lap count is byte +0xA of the race type's quick race settings (0x83AAC4 + type × 0x14 + 0xA): circuit
    `POKES="83AACE=1"` (1 lap about 80 s), Street X (mode 4 = type 10) `POKES="83AB96=2"` (2 laps about 45 s),
    drift `POKES="83AB0A=1"` (about 20 s). Works in circuit, Street X and drift; not in drag, where 0x5F3AB2 rewrites
    player +0x230 every frame (the tool fails as soon as the game takes a car back). Under autopilot a car can run on the AI mover (car +0x28 = 3),
    where 0x5ECEC0 reports no turbo, so that player's turbo gauge hides; it isn't a HUD fault.
  - `start_matrix.sh [humans:ai:mode[:track] ...]` runs each setup through start, autopilot drive-off, pause menu
    Restart and drive-off again, for changes to shared race-start code; about 1 min a setup, stops at the first failure,
    and restores the ini's counts. `grid_check.py` is its grid check: every racer's slot and position, fails below 3 m.
  - `cars_check.sh [humans:cars[:mode] ...]` starts split races with each Cars row mode (`[DEBUG] CarsMode`; default 2P
    and 4P Pick One, 2P / 4P / 6P Random, 3P Pick Each) and checks the logged race cars ("race: player N car"),
    the given cars and the random ratings, then grid_check and quit; restores the ini.
  - `next_race_check.sh [rounds]` starts a split drag race, drives it to the results (drag_blow.py), then per round
    takes Post Race → Next Race → track select → A and checks that the race loads without the options screen and with
    the last round (log "next race", "last round again"); screenshots in build/nr/.
  - `random_check.sh [rounds]` boots to the split mode select, presses Y (mode and track roll), joins every player
    and races (drag_blow.py for drag, autorace.py otherwise, 1 lap); per round Post Race -> Next Race must roll a new
    mode and track and load the race with no input. Stopping it mid-run can leave its Python children pressing virtual
    pads: end them by process id.
  - `next_race_options_check.sh` runs a split circuit race, then Next Race → Back → Sprint with P2's virtual pad off:
    the round is joined as usual and Sprint must have the circuit's options (set apart before, then compared).
  - `random_back_check.sh` starts a random series of drag races (`[DEBUG] RollMode = 3`, restored), then Next Race with
    the track roll held off (`g_randomTrackPending` kept clear through the link map) and Back on the track select: the
    series must end (log "random: series ended", `g_randomSeries` 0).
  - `standings_check.sh [races]` runs split circuit races (4013, 1 lap, every car on autopilot released 1 s apart) joined
    by Next Race and checks that the standings screen with the session totals comes between Event Results and Post
    Race (log "session: race", screenshots build/nr/standings_<n>.png).
  - Shortest unlocked track per mode on the test save (track records [0x883DA0], 0x128 each: +0x8A id, +0x98 length in
    metres; locked: the track select's list node +8): circuit 4013 (2.2 km; 4011 / 4012 are shorter but locked), sprint
    4123 (3.9 km), drag 4213 (1.61 km), drift 4311 (0.3 km), Street X 4602 (0.49 km), URL 4701 (1.63 km). Test races use
    these (`TRACK=id`) at 1 lap; `go_race_nointro.sh` fails at once on a locked `TRACK`.
  - `race_again.sh` starts the race again from the mode select; `quit_race.py` quits to the menu.
  - Pause menu: from Continue, `right` once is Restart, twice Options, three times Quit; `a`, then `left` and `a` in the
    confirmation. `b` doesn't leave Pause Options, and `left` there moves a volume slider. In a split race the menu sits
    outside a window capture at the default width: `g.screenshot(path, max_w=1920)` shows it.
  - The game's frame loop stops while the window is out of focus. A one-shot hook on the main loop won't fire then.
  - `rec.py record|play` records and replays real pad input keyed to the active FE screens
    (`recordings/start_to_split2p.json`).
- The linker map `build/NFSU2SplitScreen.map` maps crash offsets in the ASI to functions (RVA = section offset +
  0x1000).
- When testing visuals, compare against single player (`go_1p.sh`) and dump the FE objects of both.
