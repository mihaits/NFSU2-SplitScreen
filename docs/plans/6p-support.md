# Plan: 5-6 player split screen

## Goal
Split-screen races and Free Run with 2-6 local players. 5-6 players are two more "extra players" on top of the game's
2-player mode, the same way players 3/4 are added now ([architecture](../architecture.md)): NUM_PLAYERS stays 2,
`g_extraPlayers` goes up to 4.

Done when:
- The Players row offers 2..6 (2..4 in drag, 2 in outrun), and players 5/6 can join, pick a car, race, pause, finish,
  see the results and quit, in every race type and in Free Run, without crashes.
- Each of the 6 players has their own view, camera, HUD (gauges, minimap with every car marker), input, bindings,
  settings, rumble, sound listener and streaming.
- 2-4 player behaviour is unchanged (regression test after every step).

## Layout
| players | layout |
|---|---|
| 5 | 3 cells on top, 2 on the bottom centred (bars of w/6 on both sides, like the 3P bottom) |
| 6 | 3 × 2 grid |

Each cell is w/3 × h/2 (aspect 1.19 on a 16:9 screen). Decided as defaults; change before step 2 if wanted.

## Hard limits found
These are game limits that the 4-player code fits inside, but 6 players do not. The step 0 findings (2026-10-23, live
4P circuit race on debug pads + static analysis) are in each item.

1. **World view table: 12 entries. SOLVED (spike).** 0x5B9C20 fills a table on the main render's stack (0x5CAAD0:
   `sub esp, 0x734`, table at esp+0x6C, entries 0x90 bytes, count +0x6C0, cull cursor/limit +0x6C8/+0x6CC right after
   entry 11; 0x5E9A30 culls every entry into the shared item buffer 0x876DE0..0x8832C0). A 4P race uses all 12: views
   1, 2, 5, 6, env faces 0x10..0x15, 4, 9. Views 7/8 would need 14.
   - Fix: **one shared entry for every extra player view.** Register only view 5's entry. At the top of each extra
     player view, point the entry (+0x40) at that view, re-cull it with the existing `RecullHeadlightView` code (it
     finds an entry by its view and culls into the free space after the cursor), then move the world cursor (+0x6C8)
     past it so the headlight re-cull doesn't overwrite it. Put the cursor back at the next player view.
   - Spike result in a 4P race: 11 entries, P4's view complete while driving (screenshots compared with the
     separate-entry build), no frame rate change. The spike diff is `6p-spike-shared-world-entry.diff` next to this
     plan. It has a probe (`[DEBUG] WorldProbe`) that logs every entry's culled bytes.
   - Cull buffer: 4P uses 7.7-9.9 KB of 50.4 KB. A player view culls about 1.5-3.3 KB, so 6P stays well under the limit.
   - Rejected: dropping the game's env-face entries. Player 1's cube is the game's own face pass, and
     `RenderEnvCube` re-culls those face entries for the other players. Moving the table to a static buffer would
     mean patching every esp-relative use in 0x5CAAD0.
2. **Controls pages: 6 max. Needs a relocation.** Table 0x835930, stride 12. 0x4ACE00 adds one page per device
   record that has a slot (records 0x874C40..0x8760E0, 6 of them). The game's pages are keyboard, joypad P1 and joypad
   P2, so with P3/P4 there are 5. P3-P6 would make 7.
   - All 77 references are disp32: 1 to the base (0x4ACE69 `mov ebp, 0x835930`) and 76 to +8 (`[reg*4 + 0x835938]`).
     The next object starts at 0x835978 (referenced), so the table can't grow in place.
   - Relocate it to a plugin array of 8 or more entries, rewriting the disp32s the way `RelocateLocalPlayers` does.
     Then `CONTROLS_PAGE_MAX` = the new size.
3. **Minimap marker descriptor pool. Rewrite the allocator if needed.** 0x4AC6D0 (46 bytes) makes two pools with
   0x440DF0: `push 0x66; push 8` (patched to 0x7F at 0x4AC6D8) and `push 0x66; push 0x20` (0x4AC6E8, not patched).
   The count is a sign-extended `push imm8`, so 0x7F is the most it can hold. If P5/P6's markers need more, replace
   0x4AC6D0 with a C function that calls 0x440DF0 with any counts and stores the results in 0x8364B4 / 0x8364B8.
   Measure in step 7 whether that is needed.
4. **Racers: 6 cars at most.** Start spots 0x5EB5D0(6, out). The skin name is `DUMMY_SKIN%d` (0x7A8664, formatted
   by 0x61C280), and single player already uses 1..6 (player + 5 AI on DUMMY_SKIN<2 + i>), so P5/P6 can use 5/6.
   6 humans leave no AI and 5 humans leave 1, so the Opponents row max becomes 6 − players. Drag stays at 4 cars
   (Players row max 4 there).
5. **Pads.** XInput has 4 user slots (0..3). Pads 4..7 are debug pads and 8..11 are native DualSense pads
   (`kDsMax = 4` in `DualSense.h`).
   - DualSense pads under DS4Windows/ViGEm become XInput pads and use up the same 4 slots.
   - The user has about 5 DualSense pads, so 6 players means native mode (DS4Windows off) with **`kDsMax` raised to
     6+**. That also means changing `kMaxPads`, the pad layer's arrays and `busy[kDsMax * 2]`, and moving native pads
     off 8..11 if the pad index range matters anywhere.
   - Only one native pad has ever been tested (input.md), so test 2+ native pads early (step 6).
   - Real Xbox pads #5/#6: see "More than 4 pads" below.
6. **Memory. OK.** Measured at a 4P race: private 386 MB, virtual 846 MB (peak 865 MB) of the 2 GB the
   non-large-address-aware exe gets.
   - By the pool formula, 6P adds about 70 MB (track pool 60 MB × 7/2, car pool 11 MB × 3), plus 2 views' targets,
     env cubes and reflection textures. Expected virtual size is about 1.0-1.1 GB.
   - Measure again in step 8 (largest track, Free Run). If it gets tight, setting the large-address-aware flag is the
     user's call (it changes the exe).
7. **Frame cost. Unknown at 6P.** 4P measured 75 fps (game frame counter 0x7FB708, driving, no rain). That is
   probably the display's refresh rate cap, so the headroom is unknown. 6P adds 2 world renders and 2 road
   reflections, and each env cube is refreshed every 5 frames. Measure in step 3. If it is too slow, rotate the road
   reflections like the env maps.

## More than 4 pads: what other games do
XInput stops at 4. Other games and libraries get past that like this:
- **SDL2/SDL3** (most indie games): merges XInput, Raw Input, Windows.Gaming.Input (WGI) and HIDAPI. It matches each
  Raw Input device to its XInput twin by input activity. The Raw Input backend was added specifically for more than
  4 Xbox pads.
- **Windows.Gaming.Input**: no documented cap (8 Xbox pads reported working). It is WinRT, but callable from Win32
  desktop code; `Gamepad` reads only while the window is in the foreground.
- **GameInput** (Microsoft, Windows 10 19H1+): needs its redistributable installed, and x86 support is unclear.
- **Raw Input / HID**: PlayStation pads have no limit (this mod's native DualSense path). Xbox pads over Bluetooth
  HID report both triggers on one axis and can't rumble through HID.
- **Steam Input**: virtualizes any number of pads, but only for games started through Steam.
- Games with 5-8 local players (TowerFall, Overcooked, Gang Beasts, ...) rely on one of the stacks above. Players often
  add Steam Input on top.

For this mod: PlayStation pads already go past 4 through native HID (raise `kDsMax`). If real Xbox pads #5/#6 are
wanted, add a **WGI backend** for Xbox pads beyond the XInput 4 (built into Windows 10/11, no redistributable). Check
that it works from the x86 DLL and while the game window has focus. Not needed for the user's DualSense setup, so it
is left out of this plan.

## Things already shaped for 6
- `x & 1` parity (P1/P2's FE slot, ctor index, pause slot, settings entry, car/port save) holds for seats 4/5.
- `kPlayerViewId` comment: views 5..8 are unused by the game; `DrawSky` already maps 5..8 to 1.
- The view loop counter imm8 (0x5CB722), the human component capacity (0x4856C8) and the input port table count use
  `MAX_LOCAL_PLAYERS`.
- Catch-up, AI rubber-band target, nearest player/racer, live gap and the "Hurry!" timer loop over every local player.
- Results/standings counting adds `g_extraPlayers` racers.
- The intro already handles 5-6 cars (4-car scene, extra cars one row behind).

## Steps (one change at a time; test and commit each)

### 0. Spikes: done (see "Hard limits found")
Still open: 6P frame rate (step 3), marker pool use (step 7), 2+ native DualSense pads (step 6).

### 1. `MAX_LOCAL_PLAYERS` = 6, no new players yet
Raise the constant and fix everything that assumes 4, with the Players row still capped at 4:
- `core.cpp`: `kPlayerViewId = {1, 2, 5, 6, 7, 8}`; `g_portCur/Prev[4]` → `[MAX_LOCAL_PLAYERS]`.
- Initialisers written for 4 entries: `g_assignedPad`, `g_rumblePad`, `g_hudTexIndex` (loops or `std::fill` at init).
- `seats.cpp`: `g_seatStore[2]` → `[MAX_LOCAL_PLAYERS - 2]`; `[Player5]` / `[Player6]` in the players ini
  (`NFSU2SplitScreen.cpp:126` loop `< 4`).
- `input_ports.cpp`: binding node pool 0x140 → 0x1E0 (0x50 per port); `g_ffMotor[4]` and the `& 3` port ring.
  Rumble shadow channels for ports 2..5 (`RumbleExtraPort`, `port > 3` check).
- `sound.cpp`: `RelocateListenerArrays` to 6 entries; `ListenerUpdate` runs a third pass with P5/P6 posing as 0/1;
  `NearestListener` over 6; parking for listeners 1..5.
- `pad_lights.cpp`: `kPlayerColour` / `kPlayerLedPattern` for P5/P6 (proposed: P5 purple (160, 0, 255), LEDs
  `xxxxx` 0x1F; P6 cyan (0, 220, 220), LEDs `-xxx-` 0x0E).
- `hud_split.cpp`: `kMarkerColour` / `g_mmExtraIcon` for P5/P6 (same colours as the lightbar);
  `g_mmGroupData[4]` stays (ring group of 4).
- `gauges.cpp:410` marker count, `racers.cpp:102` `extra < 2` clamps, `NFSU2SplitScreen.cpp:88` debug clamp → 4.
- Grep check after: `grep -nE "\[4\]|< ?4\b|> ?3\b|< ?2 \?|>= ?2 \?" src` reviewed line by line.
Test: 2, 3, 4P race + Free Run + pause by P3/P4 + Controls P3/P4 pages + quit (regression).

### 2. Layout from a region table (done: `LayoutCell` / `LayoutFeRect` in core.cpp)
Today the HUD, minimap and gauge transforms derive the cell from bits (`k & 1`, `k & 2`, `halfW`, "P3 centred in 3P").
Make `ApplySplitLayout` the only place that knows the layout: per view k a rect `g_regionX/Y/W/H[k]`, and
`QuadTransform`, `MinimapPlace`, `GaugeTransform` (hud_split.cpp:360, :416), `ClearUnusedAreas` (render_weather.cpp:422
bars) and `AdjustProjection` read the rect. Uniform HUD scale = min(cellW / 640·aspect, cellH / 240) of the 2P top
layout. Add the 5P/6P layouts to the table but keep them unreachable.
Test: 2/3/4P races look identical to before (screenshots before/after), `[DEBUG] QuadTest`.

### 3. Views 7/8 (done: shared world entry, `[DEBUG] CloneViews`; 6 views 75 fps, 753 MB virtual)
- World view table: the shared extra-view entry from the spike diff (always on, no ini switch), generalised:
  `RecullHeadlightView` → `RecullWorldEntry(View&)`; keep the probe as a `Debug = 1` log line.
- 2-entry tables by view id: tint (`g_viewTint[2]` / `g_viewTintInit[2]` → 4 for views 5..8), rain/splash slots
  (`RainRegionSlot` / `SplashRegionSlot`: k − 2 → (k − 2) & 1 or private slots), glare views (`GlareView`), emitter
  range cameras (`EmitterRangeHook`: views 7/8), view update loop n = 1..6 (`PatchViewUpdateLoop`, `last` imm).
- `g_screenViews` 5/6 and the 5P/6P regions; `ExtraCamera(k)` for k = 4, 5.
- Reflections, env cubes, blur, overbright, streak regions already loop over `g_screenViews`; check each for 4-based
  `& 1` / `>> 1` placement.
Tested with `[DEBUG] ExtraPlayers = 2` + `CloneViews = 1/2` (views 7/8 on P1/P2's cameras). HUD note for step 7:
in the 5-view layout a white line crossed P3's minimap (narrower cell). Was: test with `[DEBUG] ExtraPlayers = 3/4`: all 6 views draw the world, sky, rain, headlights,
reflections, glares. Measure FPS and memory here.

### 4. Racers and players 5/6
- `CreateHumanRacers`: the second pass covers players 3..6; `RacerSpecIndexShift` shifts by 2/4 (spec index =
  player index), car and name per seat (`g_extraNames` "Player 5/6"), skins DUMMY_SKIN5/6 (`ExtraRacerSkin`);
  `AiSetSkin` starts the AI after every human.
- `CreatePlayers` loop k = 2..5 (already bounded by `MAX_LOCAL_PLAYERS`): view 7/8, camera, ctor (port, k & 1).
- AI count: `CreateHumanRacers` caps opponents at 6 − players; Opponents row max follows the Players row.
- Per-player settings mirror (`g_settings`) and `HudStateOf` for k = 4, 5 (run as P1/P2 by parity).
- Teardown (`inOrder − 4` patch at 0x60AA76) and the results/standings counts with 4 extra racers.
Test with the debug clone path: 5P and 6P circuit, sprint, drift, URL, Street X; 5P with 1 AI; restart and quit.

### 5. Frontend: Players row 2..6, joining and car select
- `g_splitPlayers` 2..6 (drag 2..4, outrun 2); `SplitPlayers()`.
- Press START for seats 4/5 ("Player Five/Six Press START"; English only, like seats 2/3), `g_keyboardSeat` any seat.
- Car select seat order: P1 (FE 0) → P2 (FE 1) → P3 (FE 0) → P4 (FE 1) → P5 (FE 0) → P6 (FE 1) → race; Back walks
  back (`CarselFilter`, `SwitchSeat`, `CarselStartHook`); Customize return restores the seat.
- Seat store last car for P5/P6.
Test with real pads for 5-6 seats as far as the pads go, debug pads for the rest.

### 6. Input for players 5/6
- Input ports 4/5: step 1 kept `INPUT_PORTS` = 4 (input_ports.cpp). With 6 ports the splash ignored every pad: the
  action 1 binding that `InputInit` adds per port (handler 0x571A20) writes `[port + 0x864788]` and calls
  0x5726A0(port) / 0x50C5D0(port, ..), per-port arrays sized for fewer ports. Size them (or remap ports 4/5 for that
  handler) before raising `INPUT_PORTS`. Port tables at 6 and the node pool at 0x1E0 alone were fine.
- `PollExtraPort` for ports 4/5, `PadForDevice`, `KeyboardPort` (ports 4/5), pause by P5/P6 (`PauseInputMask`,
  `g_pauseOwner`), `SyncPlayerSettings` swap for seats 4/5.
- Controls pages: relocate the page table (0x835930, 77 disp32 refs) to 8+ entries, then pages for P5/P6
  (`BuildControlsPages`, `ControlsPageSeat`, `SetBindSwap`, seat store bindings).
- Native DualSense: `kDsMax` 6+ (5 DualSense pads + keyboard or an Xbox pad); test 2+ native pads at once.
- Free camera (`g_freeCam`, `XPadRightStick`) for P5/P6.
Test: gameplay with real pads (user), rebinding P5/P6 controls, pause/resume from each seat, rumble per pad.

### 7. HUD for players 5/6
- Package copies for P5/P6 (`CloneFePackage`: "2PHudTo5"/"2PHudTo6", Top variant for every cell); the clone choice
  (`hud_players.cpp:103` `who == 1 || who == 3`) becomes "every seat except P1".
- `UpdateQuadHud` / `UpdateMinimapOwner` for 6 packages; `g_mmFinished`, snapshots.
- Minimap car markers for P5/P6 (`ExtendMinimapManager`: PlayerCarIndicator_1/_2 of P5's copy), colours. Log the
  marker pool use; if 0x7F is short, replace the pool setup 0x4AC6D0 (see hard limit 3).
- Gauges (`CreateGauges`) and custom gauge textures for P5/P6; `HudDtorHook`.
- `ClearHudBindings` (4P quit fix) with 6 HUDs.
Test: every player's HUD in its own cell, all 6 markers on every minimap, finish/pause hides, quit.

### 8. Streaming and sound for 6
- `StreamZonesHook` / `StreamSectionsHook` / `StreamPriorities`: P5 under position 0, P6 under position 1 (as P3/P4).
- Pool sizes for 6 (`CreateTrackStreamingPool` / `CreateSecondStreamingPool`); measure peak memory, cap the formula if
  the address space gets tight.
- `FollowCameraForSceneCars` post-race with 6 humans.
Test: 6P on the largest track with players spread out (no missing sections), Free Run across the city, post-race
scene, results ambient sound.

### 9. Pad lights and docs
- LEDs, lightbar colour mode and podium lights for P5/P6.
- README: features (2-6 players), to-do, version. Docs: players, rendering (layout table, world view table),
  hud, input, frontend, streaming, sound; addresses for any new globals. Delete this plan.

## Files to touch
`src/parts/`: core, seats, input, input_ports, sound, render_core, render_passes, render_weather, render_envmap,
local_players, racers, race_rules, player_settings, hud_split, hud_players, gauges, minimap_players, camera,
pad_lights, streaming, frontend; `src/NFSU2SplitScreen.cpp` (patch values); docs listed in step 9.

## Open questions for the user
- 5P layout (3 top + 2 bottom proposed).
- P5/P6 colours and LED patterns (proposed above).
- Pads: about 5 DualSense pads (answered 2026-10-23), so native mode with `kDsMax` raised. Real Xbox pads #5/#6
  (WGI backend) only on request.
