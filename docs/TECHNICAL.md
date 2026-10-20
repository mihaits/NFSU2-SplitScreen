# NFSU2 Split Screen — technical reference

This document describes how the plugin works and records the game internals it relies on. It reflects the **current,
confirmed** state only. Everything here was verified in game, and every subsystem in `src/NFSU2SplitScreen.cpp` starts
with a comment block that goes into more detail.

- Target: `speed2.exe` v1.2 (the cracked v1.2 exe's `.rdata` matches the SecuROM v1.2 one). Image base 0x400000, no ASLR, x86.
- All addresses below are virtual addresses in that exe. "imm" = immediate operand, "disp" = displacement.
- Calling conventions: `__thiscall` (ecx = this), `__cdecl`, `__stdcall` as noted. "ret N" = callee pops N bytes.

## Contents
1. [Plugin architecture](#1-plugin-architecture)
2. [Core globals](#2-core-globals)
3. [Frontend: menu, players row, joining, car select](#3-frontend)
4. [Input](#4-input)
5. [Players 3/4: relocations, race setup, their own settings, bindings and car](#5-players-34)
6. [Rendering: views, layout, projection](#6-rendering-core)
7. [Per-view render passes](#7-per-view-render-passes)
8. [Track streaming and memory](#8-streaming-and-memory)
9. [HUD](#9-hud)
10. [Sound](#10-sound)
11. [Race flow: intro, results, standings, cameras, names](#11-race-flow)
12. [Thread safety](#12-thread-safety)
13. [Open issues and leads](#13-open-issues-and-leads)
14. [Gotchas](#14-gotchas)
15. [Tooling and workflow](#15-tooling-and-workflow)

---

## 1. Plugin architecture
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
- `XInputDevices.cpp` provides virtual DirectInput gamepads backed by XInput (section 4).

## 2. Core globals
| address | meaning |
|---|---|
| 0x850078 | game mode: 1 career, 2 quick race, **4 split screen** |
| 0x89E7C0 | NUM_PLAYERS: local players of the active race (stays 2 with 3-4 players) |
| 0x89E7A0 | race params object (+0x20 = NUM_PLAYERS, +0x68 int[3] human racer slots (0x89E808), +0x74 racer count) |
| 0x89E7E8 | race ports per player [2] (copied from 0x83A9D8, the ports chosen at the joins) |
| 0x8707E0 | applied view layout (1 single, 2 + mirror, 3 = two player cameras) |
| 0x8654A4 | game state (3-5 loading, 6 racing) |
| 0x83AAB4 | quick race type index; 0x83AAC4 + type*0x14 = per-type settings (byte +0 = local player count) |
| 0x83A9E0 + i*0xAB4C | per-player block (name +0, quick race garage +0x3B0); only 2 blocks exist |
| 0x83AA64 + i*0x28 | per-player settings (camera +0x10, HUD options); 2 entries, race data follows at 0x83AAB4 |
| 0x83AAB8[2] | selected car per player |
| 0x7F4454[2] | FE port per FE player (mask 1/2/4/8); 0x85AD28 = current FE player |
| 0x8900AC[2] / 0x8900B4[2] / 0x8900BC | local players byIndex / inOrder / count (relocated, section 5) |
| 0x832DE0 | views `eView[22]`, stride 0x70 |
| 0x870974 | IDirect3DDevice9*; 0x870980/84 screen width/height |
| 0x8637B0 | FE package cache (list head +4, node = entry+4) |
| 0x870CB8.. | graphics options in registry order (0x870CB8 car env map, 0x870CD4 road reflection, 0x870CDC blur, 0x870CE0 light streaks, 0x870CE4 glow, 0x870CF0 DoF, 0x870CFC overbright, 0x870D08 FSAA level, 0x870D10 rain) |

The game reads graphics options from the per-user VirtualStore registry copy
(`HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\EA Games\Need for Speed Underground 2`). If you
launch it from an elevated shell, HKLM is read instead and the settings differ.

## 3. Frontend
**Main menu entry.** The main menu builder 0x4AEE30 adds options. At 0x4AEED8 (the `call 0x570A60` right after Quick
Race is added), `MainMenuHook` allocates an option (0x48 bytes, game malloc 0x575620) and initializes it with
`0x51F670(opt, 0x883E25D6, 0x6A2B93EB, 0)`, which gives the console icon and the localized "2 Player Split-Screen" text,
both still in the data. With 2-4 players the text is shortened to "Split-Screen": `RenameSplitMenuText` points that
entry of the loaded language table (entries {hash, text*} sorted by hash at [0x8383D8], count [0x8383D0]; lookup
0x4FF9D0) at a shortened text when it holds a known original (English, German, French, Italian, Spanish, Dutch, Danish,
Swedish; others keep theirs). Its vtable is `{0x4AEB70 dtor, SplitMenuReact}` and it's added with `menu->vtbl[0x18](opt)`.
`SplitMenuReact` sets a flag and jumps to the Quick Race react 0x4FC050 (thiscall, ret 0x14). `QuickRaceModeHook`
(0x4FC061) then sets mode 4 instead of 2.

**Garage backup.** When the quick race flow backs out to the main menu in mode 4, it restores player 1's garage
(0x4B2A00 → 0x515D70, from backup +0x1677C / car +0x20354). PC lost the code that takes that backup, so it restored
zeros: no car in the main menu, and Customize crashed. The backup is now taken when mode 4 is entered (`BackupGarage`).
Leaving split resets FE player 0x85AD28 and the FE ports; a stale FE player 1 crashes the next 1P car select.

**Players row (2-4).** It's added to the split race options screen. Option rows are 0x54-byte widgets: base ctor
0x51E230, vtable slot 1 `Act(fng, msg)` (left 0x9120409E / right 0xB5971BF1), slot 3 `Draw`. Rows are added with
`0x54F170(screen; row, 1)`, and each race type has its own row builder (switch at 0x4E2C9A).
`0x539650(0)` is called at 10 sites (0x4E2CAC..0x4E2D5F), and `QrFinishRows` appends our row there. The row's vtable is
copied from the laps row (0x7910B8). The value is `g_splitPlayers`; the ini `ExtraPlayers` gives its default.
Outrun (race type 0x83AAB4 == 8) is a 2-player mode: the row isn't added and `SplitPlayers()` returns 2.

**Joining (Press START).** The 2P_PressStart popup handler 0x498070 only reacted to the console START message
0xB5AF2461. `PressStartHook`/`PressStartFilter` instead waits for XInput Start on an unassigned pad or keyboard Enter
(any seat, one keyboard player per round: `g_keyboardSeat`), records `g_assignedPad[seat]`, and injects START with mask 1 << player. The popup text object 0x2EBB974D is
set to "Player Three/Four Press START" for seats 2/3. A pad that has joined can't join again.

**Seats (3-4 players).** The car select serves FE players 0/1 (0x4FA2A0(fe)). The player 1 pick switches to FE 1
(calls at 0x4FC3A8 / 0x4FC9B6, `CarselToP2`), and the player 2 pick starts the race (0x4FC3BE / 0x4FC9C2 → 0x53FC20,
`CarselStartHook`). With 3-4 players, the P2 pick goes to seat 2 on FE 0, and the P3 pick goes to seat 3 on FE 1 (or
starts the race). Cars (0x83AAB8) and ports (0x83A9D8) are saved per seat, and P1/P2's are restored before the race.
Back returns to the previous seat: P2 Back at 0x4FC723 (`CarselBackToP1`), P3 through `CarselFilter`. The car select
message handler 0x4FC1F0 (`CarSelectHook`) retags FE messages with the current seat's port: PC tags every message with
mask 1, and the handler drops messages whose port doesn't match `FE_PORTS[feplayer]`. While the FE player has no port it
retags to 0 (= any), which fixed a softlock on Back. The name box ("Player_Name") is set each frame to the seat's name.

**Customize from any seat.** In mode 4, X on the car select went to the memory card (0x4FCB7A). It's patched to take
the normal shop path, and the shop opens as FE player 0. On return, the car select restarts at P1's Press START; P1
auto-rejoins, and on a later frame `SwitchSeat` moves back to the saved seat. Switching on the very first frame crashed.

**Shared garage.** Everyone uses player 1's "My Cars": the 20 `imul reg, index, 0xAB4C` + block+0x3B0 garage lookups
get multiplier 0 (`ShareGarage`, kGarageIndexSites), and race setup's own lookup at 0x525F92 is redirected to
0x83AD90. Race cars are built from the garage at race start, so customizations always apply.

**Help bar.** When a split race is left, the help bar (0x8384D8; visible flag +0x6C) is rebuilt hidden (ctor
0x551F80). Its button setter 0x54E6E0 returns early while it's hidden, so the split mode select had no buttons.
`HelpBarSet` (trampoline) shows the bar first in the split menus.

**Controls screen titles.** The joypad page title call at 0x4DCA18 → `SetJoypadPageTitle` → "Joypad Configuration - P1/P2".
The slot comes from the page's input device record +0x230. Pages table: 0x835930, stride 12; current page 0x8364EC.

**FE API (useful)**
- 0x55DDA0 `(msg, fng)` sends a message to a package (hooked: `SendFeMessage`).
- 0x52CF60 `(fng)`: is the package active?
- 0x537980 `(from, to, flags)` switches screens. `from` must be active, or the game crashes.
- 0x555F10 `(fng, 0, 0, 0)` pushes/activates a package; 0x5379A0 `(fng)` removes it.
- 0x537BE0 `(obj, fmt, ...)` is printf-style SetText; 0x50C900 `(obj, hash)` sets localized text; 0x4FFA80 `(hash)` returns a localized string.
- 0x505450 is the FE name hash (case-insensitive ×33, seed 0xFFFFFFFF). 0x43DB50 is the same hash, case-sensitive
  (used for texture and attribute keys).

## 4. Input
**Game side.** DirectInput only, and at most 2 joysticks are opened (slots 0x86FFB4[2]). The poll function
0x5CFD60(port, &type, &prev): port 0 polls every device (records at 0x874C40, stride 0x370, max 6; record +0x230 =
joystick slot, keyboard = record 0, type 0x13) and merges them into 0x86EAF0; port 1 returned the same buffer. Players
read port = 0x83A9D8[player]. Input port tables: 0x8655A0, 2 × 0x1C8 (binding node list +0, port id +0x1B4, device type
+0x1B8); poll loop 0x5809C0 → `0x57B0B0` → events at 0x865984.

**XInputDevices.cpp.** It hooks the DirectInput8Create IAT slot 0x783024 and wraps IDirectInput8A.
EnumDevices/CreateDevice expose virtual "Split Screen Pad N" gamepads with full DirectInput emulation (objects with
Xbox names, GetObjectInfo, properties, buffered GetDeviceData — Options > Controls needs all of these). Real DirectInput
controllers are hidden (ini). Virtual pads 4..7 (after the 4 real XInput pads) are debug pads driven through the mailbox.

**Pad resolver (`PadForDevice`).**
- Outside split: device 0 = the last active XInput pad.
- Split menus: only the current FE player's pad (its joined pad on the car select, else the last active pad).
- Racing: each player's assigned pad.
- A missing pad is reported idle, never as DIERR_INPUTLOST (the game would drop the slot).
- While the Controls screen shows the P2 page, slot 2 is fed by the last active pad (`UpdateEditingSlot2`).

**Per-port merge.** Ports 0/1 are merged per device slot, which separates P1 and P2.

**Keyboard player.** The keyboard (record 0, slot 0, type 0x13) is left out of the slot merge and goes to one port
(`KeyboardPort`): racing, its player's port 0..3 (ports 2/3 in `PollExtraPort`); during a pause the menu's port, only
when its player paused (`g_pausePort`; P3/P4: port & 1); on the car select only on its player's turn
(`PadForDevice` gives that turn no pad); elsewhere in the split menus port 0. After a change it waits until every key
is up. Its player's pause menus show Feedback greyed and off: `FfCapable` (jump at 0x5B8240) returns false, and
`SyncPlayerSettings` clears the game entry's +4 while they are open and puts it back after (the setting is kept). The
mouse free camera follows the keyboard player.

**Players 3/4.**
- The port tables are relocated to `g_portTables[4]` (`RelocatePortTables`: 15 refs, loop bounds 0x57B3AC/0x580AD4,
  static ctor/dtor counts 0x77D9CB/0x782296). This runs before the game's static init.
- The binding node pool is raised from 0xA0 to 0x140 (0x579559).
- `InputInit` (call at 0x57ED80) gives ports 2/3 their id and action 1.
- `PollPortHook` serves ports 2/3 from P3/P4's XInput pads (`PollExtraPort`) with their own bindings (below).
  Players 3/4 have ports 2/3.
- Player port = index 0..3 (0x50D2A0 maps FE masks 1/2/4/8 to 0..3).

**Pause by P3/P4.** The pause ctor 0x4EB410 sets the package input mask to 1 << the pausing port (at 0x4EB469), but
frontend input only has masks 1/2. `PauseInputMask` maps it to slot (port & 1), and `PadForDevice` feeds that slot from
the owner's pad until the pause menus close. That pad stays idle until all its buttons are released; otherwise the
still-held Start closed the menu at once.

**Device records and bindings.** A record turns its device's state into action bits (+8, 8 bytes, active low) and
analog values (+0x10..+0x13) every frame: 0x5CF220 → 0x5C1840. For joysticks it reads the slot's DIJOYSTATE2 at
0x86FFC0 + slot*0x110 (buttons) and at [0x870760] + slot*0x110 (axes, POV). Bindings: 4 action tables of 32 inputs at
+0x18/+0x98/+0x118/+0x198 (16 buttons, then axis directions/POV), axis setup +0x344..+0x368, POV count +0x36C. The
profile saves table 0 and the axis setup per device GUID (0x5B9420 called at 0x52A42A; load 0x5B9570 at 0x534660).

**Controls for players 3/4.** They have no joystick slot, so their bindings live in the seat store (section 5).
- The Controls screen (ctor 0x4ECD10) builds its pages with 0x4ACE00 (call at 0x4ECFD5 → `BuildControlsPages`): table
  0x835930, stride 12 {type, ?, record}, 6 max, count at screen+0x158, joypad pages at +0x154. Two pages are appended
  for P3/P4, both on the slot-0 pad record.
- While one of them is shown (`ControlsPageSeat`: page index and UI_Options_PC_Controller.fng active), that record
  holds the player's bindings and P1's are kept aside (`SetBindSwap`). The game's editing, capture and "Reset Keys To
  Default" then work unchanged. The swap is applied when the joypad page is built (`SetJoypadPageTitle`, called at
  0x4DCA18 inside the builder 0x4DC9F0) and checked every frame. It is undone when the page is left and before the
  profile saves or loads bindings (`ProfileSaveBindings` / `ProfileLoadBindings`).
- In the race, `PollExtraPort` runs 0x5C1840 on a private copy of the slot-0 pad record that holds the player's
  bindings. For the call, the player's pad state (`XPadDiState`: DIJOYSTATE2 with device 0's axis ranges) is put into
  slot 0's two state buffers and restored right after. The result is merged like the game merges a port's devices.

**Force feedback (rumble).** The rumble engine is shared with the PS2 version (SLUS_210.65), so it was restored from
there rather than tuned by feel:
- Events 0x5727B0(type, port, amount) pick an effect from the table 0x7FB7D8 (PS2 0x523070, same parameter blocks) and
  queue its curve on the port's channel (0x5726D0 → 0x572390).
- Channels 0x8653B0 + port*0x60: 2 sub-channels of 0x30, each summing up to 3 curves into an int 0..255 at +0x28
  (0x572470).
- The per-frame update 0x572610 (called at 0x5814AD) hands the values to the pad. On PS2 that is 0x333530(port, v, v)
  with v = sub-channel 0's value (DualShock: small motor on when v > 0x80, large motor = v). On PC it is an empty stub
  (0x4022C0 at 0x57266E).
- `FfPadOutput` (replaces the stub call) records v per port. `FfUpdate` (call at 0x5814AD) runs the update and sends
  large motor = v/255 and small motor = v > 128 to the player's own XInput pad (`XPadSetRumble`), scaled by
  RumbleStrength.
- Rumble is off in the pause menus, outside racing, when the player's Feedback option is off (player settings +4) or
  with Rumble=0. The Feedback option is greyed unless the last DirectInput device enumerated reported
  DIDC_FORCEFEEDBACK (0x5B8240 returns 0x87078C, set at 0x5C1221; only the two option screens read it); it is forced
  selectable while rumble is on.
- The channel state exists for 2 ports (channels 0xC0 bytes, strengths 0x864728, 0x60 bytes indexed type*2 + port).
  P3/P4 (ports 2/3) get a shadow copy that `RumbleExtraPort` swaps in for their events and for a second run of the
  update.
- Collisions (0x572E50 → 0x865360) only drive the wheels' constant force, as on PS2 (`ImpactRumbleHook` ignores
  players >= 2). `JoySlotPresent` (0x5BFD70: slot >= 2 → false) guards the driving force feedback 0x406460, which read
  past the 2-slot array.

## 5. Players 3/4
**Local player arrays.** These are 0x8900AC byIndex[2], 0x8900B4 inOrder[2] and 0x8900BC count, with about 300 code
references. Many of them are address-bounded loops (`p < 0x8900B4`). They are relocated as one block,
`g_localPlayers {byIndex[4], inOrder[4], count}`, so that "end of byIndex == inOrder" and "end of inOrder == count" stay
true (`RelocateLocalPlayers` rewrites every disp32). One indexed reference is "inOrder - 4": the race teardown loop at
0x60AA76 (`while (count) delete inOrder[count-1]`). It is patched to &inOrder[0]-4; otherwise quitting with 3 players
hung.

**Human racers.** 0x525ED0(numPlayers) creates human racers (racer spec ctor 0x6373E0(spec; playerIndex, human, car,
profile, ...), AddRacer 0x601600; AddRacer stops at 2 humans). Its 8 call sites → `CreateHumanRacers`: the original,
then a second pass for the extra players with `RacerSpecIndexShift` (call 0x52602A), which shifts playerIndex by 2 and
uses P3/P4's car and name. The human component manager capacity (type 1) imm at 0x4856C8 is raised from 2 to 4.

**Racer of player.** 0x5ED960(index) reads the race's 3 human slots, so `RacerOfPlayer` sends index >= 2 directly to
0x5ED920 (find racer by player index).

**Players.** The call at 0x60A798 → `CreatePlayers`. After the original, for each extra player k = 2..3:
1. racer 0x5ED920(0x89E7A0, k); car = race car list [0x890080] entry with short +0xA == k;
2. `InitPlayerView(5/6)` and its own camera (malloc 0x320 + 0x44F5A0). This must happen before the player: camera
   movers (0x44AB90) read view->camera.
3. The player is allocated (0x9C0 bytes) and built with ctor 0x5F1670(port, k & 1). It's built as P1/P2 so that a
   loaded 2P HUD package is used.
4. byIndex is fixed up; +0x10 = k, +0x18 = view 5/6, +0x1C = 0 (no mirror); `0x5F3E10(player, car)` attaches it.

**Per-player settings.** Table 0x83AA64 + i*0x28 has 2 entries (camera +0x10, Feedback +4, HUD options, ...), so P3
read race data as its settings (e.g. camera 0 instead of P1's drift camera). The 27 in-race readers (kSettingsReaders)
now use a 4-entry mirror `g_settings`. P1/P2 are synced both ways each frame: menus write the game's table, while the
in-race camera change 0x5F2FA0 → 0x502540 writes the mirror. P3/P4's entries are their own (seat store; the first race
without one copies P1's / P2's). HUD state 0x600090 (call 0x600595, `HudStateOf`) has 2-player rules on the index, so P3/P4 run it as P1/P2 with
their own options put into that mirror entry for the call.

**Pause menu settings.** The pause menu's player options edit the entry of 0x4A7AE0, the pausing player's index among
P1/P2 (0 for anyone else). While P3/P4 owns the pause (`g_pauseOwner`), `SyncPlayerSettings` swaps their entry into the
game's table at entry owner & 1, and `PausingPlayer` (jump at 0x4A7AE0) returns that entry. The menus then show and
change the pausing player's own settings; the table goes back to P1/P2 when the pause menus close.

**Seat store (players 3/4's own data).** The game keeps per-player data for 2 players only. P3/P4's settings entry,
joypad bindings (record tables + axis setup, section 4) and last car are kept in `g_seatStore` and saved to
`NFSU2SplitScreen_Players.ini` ([Player3] / [Player4], hex) whenever they change.
- Last car: `SwitchSeat` opens a P3/P4 pick on the stored car if it is still in the garage (`GarageHasCar`: P1's
  quick race car list 0x844968, 0x5B entry pointers, car id at +8, as the lookup at 0x525FBB). Each pick is stored
  (`SaveSeat`). P1/P2's last cars are the game's (0x83AAB8 in their profile blocks).

**Names.** Racer names come from the per-player block, so P3/P4 got P1/P2's. They now get P2's name with its digit
swapped ("Player 3/4", `g_extraNames`, spec +0x90C). Results and labels read the player object's name +0x30 (copied in
the player ctor at 0x5F17B3), which is set for P3/P4 in CreatePlayers. The in-race labels at 0x4C7006 / 0x4C93CF are
patched to read player+0x30.

**Catch-up.** The Quick Race option (0x5028F0, copied to race params 0x89E7F8) drives 0x5F6740, run per human driver
(+0x18 player, +0x1C racer): its factor (+0x468) is a curve (table 0x81CB60 + 0xA6C) of (own progress − the other
human's progress) × 0x88F2B0, progress = racer +0x14. It only ran with exactly 2 local players (`cmp [count], 2` at
0x5F676A), so 3-4 player races had no catch-up. The `jne` at 0x5F6771 becomes `jl`, and the "other human" choice
(0x5F6773, 25 bytes) is `CatchupOtherHuman`: the furthest ahead other human (with 2 players, the other one).

**AI opponents.** Quick race settings 0x83AAC4 + type × 0x14: byte +0 local players, +1 opponents. Every race starter
sets the AI count 0x89E7C4 to 0 when there is more than one local player, creates the humans (0x525ED0, hooked as
`CreateHumanRacers`), then the AI with 0x53EE90(0x89E7C4). `CreateHumanRacers` sets 0x89E7C4 to the Opponents row's
value first (split options, `AddOpponentsRow`), followed by the game's own Difficulty row (ctor 0x4B3930, value
0x502730/0x502990 in the quick race settings; the race starters copy it to 0x89E7F4 in every mode).
- Race types (0x83AAB4, starters table 0x53FCF8): 0/1/7 circuit/sprint/lap KO, 2 drag, 3 drift, 4 URL, 5/10 Street X,
  8 outrun. AI where single player offers opponents (the option builders adding the Opponents row 0x4B3750). In drift
  only circuit drift: the drift starter zeroes the AI on sprint drift tracks (0x5D3E40(track) +0x80 == 1, at
  0x53F8B0; NFSU2 Extra Options marks sprint drift AI as broken), and so does `SprintDriftTrack`. At most 6 racers
  (single player URL allows 5 opponents, Extra Options 5 everywhere: it only changes the menu limits), 4 in drag.
- AI cars (0x4FEAF0): a shuffled list of 26 car types, the first whose stock performance (0x5B6990) is within a
  tolerance (3%, doubling) of a reference car: racer 0's (player 1's; 0x5ED900(race; 0) at 0x53EEE9). In split races
  `AiReferenceRacer` gives the fastest human car by that rating. The check only bounds the AI car from above (any
  slower car passes). Difficulty picks each AI driver's skill tier (0x4FED00:
  weighted random over a 10-entry table per difficulty) and the rubber-band strength.
- AI racers are spec index 100 + i (no clash with players 3/4). Their cars (0x4FEAF0(out, 100 + i, 2 + i, ...)) got
  DUMMY_SKIN<2 + i> (call 0x61C280 at 0x4FEB51), player 2's slot in split: `AiSetSkin` starts them after the humans.
- Rubber-banding 0x4173ED.. picks the human each AI measures against (AI +0x748): 1 player that one, 2 the leader or
  the trailer (threshold AI count / 2, +1 per human within it; racer +0xA = position). With 3-4 it stayed 0 and was
  read at 0x4174E2 (crash). `AiTargetHook` (0x4174C5) applies the same rule over every human.

**WidescreenFix free camera.** WidescreenFix's [CAMERA] free look replaces the eCreateLookAtMatrix call in
CubicCameraMover::Update (0x453B44 → 0x5BA9A0(out, from, to, up); esi = mover: +0x1C camera, +0x48 mode, +0x51 look
behind) and applies one global yaw/pitch offset from the right stick (the last polled joystick slot) and the mouse, so
in split screen everyone's camera moved together. Once WidescreenFix has patched the call (checked per frame,
`UpdateFreeCamHook`), the plugin chains in front of it (`LookAtHook` → `SplitLookAt`). Outside split racing everything
goes to WidescreenFix. While split racing, the player whose view camera is the mover's camera gets WidescreenFix's
logic with their own state (offsets, idle timer, car speed): their own right stick (`XPadRightStick`) and, for player
1, the mouse. The mouse movement is recorded as the game reads the system mouse (`XMouseTakeDelta`, a GetDeviceState
hook in XInputDevices). Settings are read from WidescreenFix's ini. WidescreenFix's own function still runs once per
frame on a throwaway copy, because it also hides the mouse cursor while racing.

**Nearest player / racer.**
- 0x459AC0 (player) and 0x459A50 (player's car) compared players 1/2 only. They are replaced by `NearestPlayer` /
  `NearestPlayerCar`, which consider every local player.
- 0x5F1160, "distance to the nearest racer", measured racers 0/1 only. Per-car effects (0x60A1AB) skip cars more than
  150 m away, so P3's car had no particles. Now `NearestRacerDistance` (trampoline) uses every racer when extra
  players exist.

## 6. Rendering core
**Views.** `eView[22]` at 0x832DE0, stride 0x70, `GetView(id)` = 0x48B1E0.
- Fields: +8 active; +0x40 camera; +0x58 render target struct; +0x60 cull/effect object (0x3B0 bytes, ctor 0x5E0260);
  +0x64 render/weather object (0x51C0 bytes, ctor 0x61E150(view, 0); then +0x509C = 1.0 and 0x613050(5, 1.0f)).
- Ids: 0 FE, 1/2 players, 3 mirror, 4 road reflection (320x240), 9 headlight projector, 0x10..0x15 car env map faces.
- Player views: `kPlayerViewId = {1, 2, 5, 6}`. `InitPlayerView` sets up views 5/6 like 0x5C2F90 does for 1..4. It
  memsets them to 0 first: game malloc isn't zeroed, and the ctors assume zeroed memory.

**Render target structs** (0x1C bytes): [0] type, [1] owner view, [2] surface, [3] depth, [4] enabled, [5] width,
[6] height. The backbuffer RT 0x86E768 is shared by view 1 and the HUD.

**Layout.** 0x5C2120 computes layout 3 for split, but the PC per-frame apply 0x5C21E0 has no case for it.
`ApplySplitLayout` (each frame while SplitRacing):
- `g_screenViews` = 2 / 3 / 4.
- Each player view gets a private copy of the backbuffer RT (`g_rt[k]`, half height; also half width in quad) and a
  region (`g_regionX/Y`): 2P = top/bottom halves; 4P = quadrants; 3P = P1/P2 on top, P3 centred at the bottom
  (x = w/4).
- `g_rt` is refreshed every frame and before each player view (`RefreshViewTarget`), because a device reset
  (alt-tab) releases the surfaces.

**Player view loop.** In the main render 0x5CAAD0, the loop counter (from 1) ends at `cmp eax,1` (imm at 0x5CB722 →
4). `ViewLoopTopHook` (0x5CB000) maps the counter to kPlayerViewId, skips inactive views and sets `g_currentView`.
`ViewLoopExitHook` (0x5CB72D) runs after the loop (restores, clears unused areas).

**Viewports.** Every game SetViewport uses X=Y=0 and the RT size. The device vtable hooks are:
- `SetViewportHook` (slot 47): offsets by the current view's region when the target is the backbuffer, and clamps
  full-screen viewports inside a player view to its region.
- `SetRenderTargetHook`: SetRenderTarget silently resets the viewport and the scissor rect. The hook restores the
  view's region, and re-applies the overbright scissor.

**World view registration.** 0x5B9C20 registers the per-frame world views (entries 0x90 bytes, max 12; count +0x6C0):
views 1..3, env 0x10..0x15, 4 and 9, via 0x4888A0(view, entry, flags). `WorldViewsHook` (0x5B9CAA) adds 5/6 (the mirror
is off in split, so they fit) and captures the world view table pointer (`g_worldViews`).

**Sky.** 0x60F880 (called at 0x5CB227) only draws for ids 1..3 and the env faces. `DrawSky` passes views 5/6 off as 1.

**Projection.** 0x5C7EC0 (thiscall platInfo; view, ...) builds P (platInfo +0x40 P00, +0x54 P11) from the RT's
width/height, then view-proj and the frustum. `ProjectionHook` at 0x5C80B0 (after P, before view-proj) corrects the
aspect and FOV for the view's region (`AdjustProjection`; camera FOV ushort at camera+0xC4). View 4 (road reflection)
uses the reflecting player's shape. WidescreenFix hooks 0x5C7F50 for view 1 and patches constants at 0x5C7FEA..0x5C8021.

**3P side bars.** `ClearUnusedAreas` (after the view loop) ColorFills the two bottom bars in the backbuffer. With
overbright on, it also fills them in the streak buffer.

## 7. Per-view render passes
The game ran many passes for "view 1" or "player 1" once per frame. The general fix: run them per player view, or
redirect their view or player choice to the view currently being drawn (`g_currentView` / the view's player).

**Headlights.**
- World headlights are one projector (view 9, camera 0x870A20). It's aimed once per frame by 0x5C2AE0(view 9,
  human 0) and read by the world shaders through 0x5BDAA0(…, GetView(9)).
- `HeadlightsForPlayer(k)` (called from `RefreshViewTarget`) re-aims it at player k's car for each view.
- `RecullHeadlightView` redoes view 9's entry of the world view table per player: frustum 0x5C26E0, cull 0x5E9A30 /
  0x5E99C0 into the space after the cursor (world +0x6C8, limit +0x6CC) without advancing it. This is needed because
  the projector's geometry was culled from P1's position.
- **Projectors for every player** (`WorldDrawWithBeams`, call 0x5D6AD0 at 0x5CB23B). The headlight pass
  (0x5D6D20(world; view 9, 0x488B90) at 0x5CB218) only flags the meshes in view 9's culled list (+8 → +0x1B |= 0x80).
  The world draw turns the flag into render flag 0x200 (0x5D6B85), which picks the material's headlight shader
  variant (0x5C5953: worldheadlight.efx etc.), and clears the flag. The world draw only queues meshes; 0x5CF420 draws
  the queue and sets the projector params per draw (0x5BDAA0, colour from 0x60F510 at 0x5BDC20). So one projector
  per draw. After the view's world draw the plugin draws the queue, then for each other player with lights on
  (within 250 m) aims view 9 at that car and draws its flagged meshes twice: with colour 0 subtracted (blend op
  REVSUBTRACT), then with the beam added (ADD). The difference is mesh × beam; base lighting and fog cancel. The
  world draw's include mask (imm at 0x5D6B0F, normally 0x2000 for flags & 0x10) is 0x200 meanwhile, so only flagged
  meshes are queued. A SetRenderState hook forces no depth writes, LESSEQUAL with a small bias and ONE/ONE blending.
  A DrawIndexedPrimitive hook skips materials that asked for alpha blending: semi-transparent pixels don't cancel
  (the subtracted copy clamps at black). The same runs in the full screen race intro (view 1); the device hooks
  are installed on first use because the first intro comes before any split frame.
- Light pools on the road (0x615490(view), "2PLAYERHEADLIGHT1" decal) are for traffic only: player cars have their
  lights flag (+0x550) cleared during the call (`PoolsForTraffic`). The split path calls it after 0x566400
  (`SplitPassWithHeadlightPools`, 0x5CB2E3); the 1P path (intro) at 0x5CAC07.

**Car skins.** A car's paint and vinyls are composited into the texture DUMMY_SKIN<n> named by its ride info
(+0x584, set by 0x61C280(ride; n), composited by 0x61D790). The garage entries choose n from the player index
(0x503A80: index + 1; 0x503880: 1 or 2), so players 3/4, built with index 0/1, used P1's / P2's skin texture and
rendered with mismatched textures (e.g. a door card texture on the hood). `RacerSpecIndexShift` calls `ExtraRacerSkin` before the racer spec copies the ride info:
DUMMY_SKIN3/4, the single-player opponents' slots (split races have no AI). Race type 9 sets skin 1 for every human
(0x526007) and is left alone.

**Start zoom.** After the race intro the chase camera eases out from close to the car: CubicCameraMover's offset
spring (+0x38: +0 distance, +8 its target, +0x60 height, +0x68 its target, the rest rates). Only player 1's camera
is created then (view 1 showed the intro); the others exist from the race load and finished the ease-out unseen.
`UpdateStartZoom`: on the first split frame, once player 1's spring starts, the other players' springs are set to
the same fractions of their targets. Movers are found through a hook on the vtable's Update (0x787F2C → 0x453570).

**Car detail.** With the split layout (0x5B98A0 == 3) the car renderers 0x620AE0 and 0x6162E0 multiplied the car's
screen size by 0.7 (0x7A6700) and forced its LOD range to 1..1, the low model, ignoring Car Geometry Detail (0x870CF8
-> 0x8A1C10/14). The car parts renderer 0x615630 scaled the size too. The `jne` past each block (0x620C8C, 0x616316,
0x6156CC) is patched to `jmp`. Not touched: 0x61F332 scales a car value at +0x5094 by 1/8 in split (purpose unknown).

**Car light glares.** 0x5D6570 made glares only for views 1/2 (GetView(2) check at 0x5D683C, mask from GetView(1) at
0x5D686F). `GlareView` counts views 5/6 as view 1, reading the rendering view from the model renderer's context
([ebp+0xC]+0x40). The slot counters (0x82DAE8[2], capacity 0x7F3D44) are reset at each player view. The screen tint
update 0x5D77F0 indexes 2-entry tables (0x88EA08 / 0x88F210) by view id - 1, so `TintIndexHook` (0x5D7831) and
`TintUpdateWrapper` give views 5/6 private entries.

**Per-view world update.** 0x5DBE80 (from 0x5CBADC / 0x5D2C21) looped over views 1..2, covering visibility, emitter
lists and the per-view nitrous effect flag. `PatchViewUpdateLoop` (installed after the player relocation) makes it
loop n = 1..4 using kPlayerViewId[n-1] / inOrder[n-1].

**Particles.** The emitter update 0x491E50 spawns only within range of the particle system's 3 camera snapshots
(0x48BD90 copies views 1, 2, 0). `EmitterRangeHook` (0x491E8F) also tests the cameras of views 5/6. Per-car effects
also needed the nearest-racer fix (section 5).

**Road reflections.**
- View 4 renders P1's camera into a 320x240 texture (0x870894; surface 0x870890 = view 4's RT; blur 0x5C7E40 through
  0x87089C/0x870898). Road materials bind [0x870894] when they draw (0x5BC7D1) and build REFLECTEDPROJ from the
  drawing view's own matrix.
- Players 2..4 get their own texture, rendered right after P1's (`ReflectionsDoneHook` at 0x5CACCC, before the env
  map) with their camera, view update and recull, plus the block's glow pass. [0x870894] is swapped at the top of each
  player view (`UseReflectionOf`) and restored after the loop.
- This has to happen **before** the view loop: rendering the reflections inside the loop left render state that broke
  the following view.
- 0x631250 (the car's own reflection) picked local player 0 for view 4, else 1; `ReflectCarPickHook` (0x631289) picks
  the reflecting player.

**Car reflections (env map).**
- The cube RT is 0x870858 (size 0x7FEE6C, faces 0x87085C[6]), rendered by views 0x10..0x15 from cameras 0x871BC0.
  Those cameras are placed by 0x61AB50 at human 0's car, and oriented by 0x5C3450 using **GetView(1)'s camera**: the
  cube is in camera space, and cars sample it with a camera-space reflection vector.
- Players 2..4 each have their own cube, and one of them is rendered per frame in turn (`EnvMapsForPlayers` at
  0x5CAE12). During that, view 1's camera is temporarily that player's, and the faces go through view update, recull,
  RT begin, sky without Z and the world (if 0x870CBC).
- Before each player view, its faces are StretchRect'ed into the game's cube (`UseEnvMapOf`).
- These are D3DPOOL_DEFAULT textures and cubes; they are released in a hook on 0x5BA610 (device lost,
  `ReleaseTargetsHook`) and recreated on demand.
- Cost per frame: (players - 1) road reflections + 6 env faces.

**Weather.**
- Every view has its own rain object (view +0x64), but the frame only used view 1's, full screen.
- Falling rain and the drop update 0x61F120 (call 0x5CB46B) → `RainForViews`: once per view, viewport = its region
  (the previous viewport is restored afterwards; later passes depend on it).
- Lens droplets 0x5C7AB0 (call 0x5CB9A6) → `RainDropsForViews`; its four 0x48B1E0(1) calls → `RainDropsView`; the
  drop getter 0x6142C0 (call 0x5C7C52) → `RainDropGet` remaps x/y into the region.
- The weather caches (0x8A3180 / 0x8A1E90 / 0x8A31F0 / 0x8A3250) are [2] arrays indexed by view id - 1, so views 5/6
  use slots 0/1 (`RainRegionSlot` 0x6136C3, `SplashRegionSlot` 0x61437E). Without that, the race start crashed.
- The rain emitter frame (0x61E700) picked human slot 0 for view 1, else slot 1. `RainCarHook` (0x61E724) maps the view
  to its player's racer.

**Speed blur.** 0x5CC460 (per player view, call at 0x5CB31F) used view 1's camera (calls at 0x5CC474/0x5CC490), one
smoothed strength (0x870900) and a full-backbuffer StretchRect (0x5CC760).
- `BlurView`: the current view's camera.
- `MotionBlurForView`: a per-view strength.
- `BlurCopy`: copies only the region, then puts the full-screen copy back, because 0x86EB0C[2] is also the frame-end
  post chain's screen copy.
- The blur composite (0x5C6BE0) rebinds the backbuffer, which `SetRenderTargetHook` handles.
- The blur starts at 25 m/s camera speed (thresholds 0x7FF7B0/B4).

**Light streaks and overbright.**
- Overbright needs streaks (0x870CE0) and glow (0x870CE4); its flag is 0x8707CE. It accumulates in the streak buffer
  0x86FF70 / tex 0x86FF74, which is **screen sized** (the 960x540 buffers are the blur targets).
- The streak pass 0x5CBB00 (in the view loop) fades the buffer (0x5C6BE0, blend factor) and adds the view's glows. It
  hard-coded view 1 (`push 1; call 0x48B1E0` at 0x5CBC00 → `StreakView`).
- `StreaksAllowed` / `StreaksBranchHook` (0x5CB577): split views run the pass, and every viewport on the streak
  surface inside a view is that view's region (`StreakRegionViewport`; the blits are clip-space quads).
- With FSAA (0x870D08 > 0), the pass renders through the full-screen copy 0x86F2E4 / 0x86F2E0. That surface gets the
  same regions (`IsStreakSurface`), except for its copy of the whole previous image (0x5C7120 at 0x5CBBB0,
  `StreakAACopy`).
- Without overbright, the pass ends with its own composite of the buffer onto the view (0x5C6BE0 at 0x5CBDA6), a
  full-clip-space quad with UVs covering the whole buffer, which would squeeze every player's part into the view.
  `StreakComposite` marks the player view while it runs, and `BlitQuadReadyHook` (call 0x5BCBC0 at 0x5C6EC6, after
  the blit has built its quad on its stack: 4 vertices of 0x30 bytes at esp+0x30, position + 4 UV pairs) remaps the
  UVs to the view's part of the buffer (`RemapStreakQuad`).

**Overbright intensity per view.**
- 0x5BC180 (once per frame, call at 0x5D2B5A) reads view 1's effect object (+0x60: +0x3C nitrous on, +0x1E0
  intensity). It sets the accumulation amount 0x7FF114 (blend factor) and the bright-pass threshold 0x7FF10C.
- `OverbrightAmounts` computes both per view.
- `OverbrightAccumulate` (0x5CCBA5) runs the accumulation blit once per view with its own blend factor and a scissor =
  its region.
- `BrightPassPerView` (0x5CCA25 / 0x5CCA40 / 0x5CCA61) does the same for the bright-pass blits.
- The nitrous channel handler 0x5D7770 picked the car by view id (1 → inOrder[0], else inOrder[1]).
  `NosFxPlayerHook` (0x5D778D, 13 bytes, installed after the relocation, which rewrites a disp32 inside it) maps the
  view to its own player.

## 8. Streaming and memory
- **Sections.** In 2P the game remaps 'X'/'Y' track sections (num/100 + 0x40) to reduced split variants (+50). The PC
  data has no 'Y' variants, so the world disappeared. The `jnz` at 0x5DACDB → `jmp`: every player streams the full 1P
  sections.
- **Positions.** The streamer 0x5E5110 supports 2 positions, taken from views 1/2's cameras (0x44EFE0 → 0x5D6040).
  `StreamZonesHook` (0x5E5300) computes P3/P4's zones from the cameras of views 5/6. `StreamSectionsHook` (0x5E5110)
  marks P3's zone sections under position 0 and P4's under position 1 (0x5DACB0(streamer; list, n, pos)). The proper
  fix, 4 positions, is still to do.
- **Pools.** The race load (0x57A5E3) sizes the "Track Streaming" pool from 0x79DC54 KB {60000 normal, 43000 two
  players, 36000} (0x5E4E70, call 0x57A63A), and a second pool from 0x7FA9D4 KB {11000, 9000, 16000} (0x610750 on
  0x8A3288, call 0x57A62C). The 2-player sizes were made for the reduced split sections. Split races stream full
  sections for 2-4 far-apart areas, so with them the sections around a distant player went missing.
  `CreateTrackStreamingPool` / `CreateSecondStreamingPool` size them from the single-player values: 60 MB ×
  (players + 1) / 2 and 11 MB × players / 2, plus 3000 KB per AI opponent. The second pool is the car loader's
  (0x8A3288: car models and textures). When it runs short, loading evicts the textures of car types no racer holds
  (0x63A1B0 → 0x639F20, after the memory check 0x48D420); AI racers' types are not held, so without the AI share an
  AI car drew untextured ("transparent"). The game's 6-car size is 16000 KB vs 11000 KB for 4 cars (≈2500 KB a car).
  The exe is not large-address-aware; a 4P race uses about 380 MB.
- Load priority (0x5DF580, called at 0x5E7CFC → `StreamPriorities`): per section (list +0x29C, count +0x298) the game
  takes the smallest 0x5DF3A0(section, position block) over the positions whose bit (+0xB) it has (0..0x63; uses the
  block's x/y +4/+8 and velocity +0xC/+0x10; a still position gives 0) and stores priority × 100000 + section+0x20 at
  +0x40, which orders loading and eviction. P3/P4's sections ride on bits 0/1, so they were ranked by the distance to
  P1/P2. `StreamPriorities` also takes P3's / P4's own blocks for bit 0 / 1, so every section is ranked by its nearest
  player (identical to the game's with 2 players).

## 9. HUD
**Map mode.** Gameplay > Map mode (0x83AA1B: Off/Static/Rotate, handler 0x49AFE0) only offers Off/Static in split mode
(max = (mode != 4) + 1). With per-player minimaps Rotate works, so `setne cl` at 0x49AFF9 is patched to `mov cl, 1`.

**FE objects.** The relevant FE object fields are:

| offset | field |
|---|---|
| obj+4 | next object |
| obj+0x10 | name hash |
| obj+0x18 | type: 1 image, 2 text, 5 group (children at +0x64, count +0x60) |
| obj+0x1C | flags: bit 0 = **hidden**, 0x400000 = transform changed |
| obj+0x24 | texture key |
| obj+0x2C | data |

Data (0x54 bytes):

| field | meaning |
|---|---|
| [0] | colour |
| [4..6] | pivot |
| [7]/[8] | x/y in a 640x480 space centred at 0 |
| [0xE]/[0xF] | scale |

Other functions and rules:
- Package objects are at package+0x4C.
- 0x50CA00 hides an object and 0x50CA50 shows it. **Both recurse into a group's children.**
- The FE object transform is 0x51B0B0 (thiscall: data, out matrix, out colour, level, scale), hooked by
  `FeObjXformHook`. Group transforms are computed in a pre-pass (0x51B7F0 → 0x51B0B0 at 0x51B818).
- **Transforms are cached until the changed flag is set**, so anything we move is marked changed at every remap.
- The object render 0x5370A0 (stdcall (obj, arg), ECX = render context) is called from a render queue at 0x537262. It's
  wrapped by `RenderFeObject` (naked, preserves ECX) → `RenderFeObjectWith`, which skips objects or sets per-player
  placement. Children are queued and rendered individually.

**2P HUD widening.** WidescreenFix only knows the 1P HUD positions. In split racing, level-0 objects with |x| ≥ 100 and
|y| ≤ 240 get a shifted copy of their data, x ± (240·aspect − 320) (`HudObjectFilter`). This is off while UI_Pause,
UI_PauseOptions, UI_PauseOptionsMain or UI_PC_Customize_Options is active (`g_hudWiden`).

**HUD packages.**
- The player ctor 0x5F1670 picks 2PHudTop/2PHudBot (+_DRAG/_DRIFT/_OUTRUN) by index when NUM_PLAYERS == 2. The HUD
  ctor 0x4F8BF0(fng, index) (call 0x5F1A0C, `PlayerHudCtor`) builds the same element classes for every package. The
  elements bind to the package's objects by name and read player data by index.
- **Package copies** (`CloneFePackage`): players 3/4 (and the quad layout's P2/P4, which use a copy of the *Top*
  variant) get private copies registered under a name of the same length ("2PHudTop" → "2PHudTo3").
- How a copy is made: the cache entry's chunk is either JDLZ-compressed (0x30210; block at hdr+0xC, uncompressed size at
  +8) or stored (0x30203). The copy is always a **stored** chunk: compressed ones are decompressed with
  0x574670(jdlz, out). The name is patched only in the FNG header: the name at +0x28 (the cache finds packages by it)
  and the path. Then an entry is built with the game's ctor 0x51B8E0 (entry size 0x54) and linked into the cache.
- Do **not** patch inside the compressed stream: back-references copy the patched byte into later strings. For
  example, HUD_Drag's "Drag_*.tga" textures became "Dra1_*.tga" and went invisible.
- `SendFeMessage` (trampoline on 0x55DDA0) forwards messages sent to a source package to its copies.
- Package lookup is by name with 0x51BB70 (cache; name) → entry, active package at +0x18.

**Quad HUD (3-4 players).** `UpdateQuadHud` maps every object (and its data pointer) of each player's package to that
player, rebuilt every 30 frames. `QuadTransform` scales the top-half 2P layout uniformly about its centre (0,-120) by
halfW/640 into the player's quadrant (P3 is centred in 3P). Objects pushed more than 160 from the centre (icon pools
parked off-screen) are moved 2000 further out. Pivots are scaled too.

**Minimaps.**
- The minimap is objects of P1's 2PHudTop around (-230, 5): Track2000_map, Minimap_Mask, Minimap_North_indicator
  (pivot (0,72,0)), TrackMapTargetRing (a group of 4), and markers.
- One manager drives it, 0x8364D4 (ctor 0x4EC950(fng)). Its update 0x4CA840 (called at 0x4DA650) centres on car entity
  (type 4, id 0). A type-4 entity's id indexes the local players.
- Markers: 0x4DB900(fng) scans a package for 17 name patterns × 8 (table 0x7F6828 {fmt, type}) into the descriptor pool
  0x836560. The pool is raised from 0x66 to 0x7F.
- `ExtendMinimapManager` adds P3/P4's car markers (PlayerCarIndicator_1/_2 of P3's copy, recoloured: P3 blue, P4
  yellow).
- `MinimapUpdate` (call 0x4DA650) runs the update once per player, centred on that player (the id imm8 at 0x4CA901).
  After each run it snapshots the minimap objects' data, **keyed by data pointer**. Lists are rebuilt as markers come
  and go.
- `RenderFeObjectWith` draws each minimap object once per player from that player's snapshot (`MinimapPlace`:
  bottom-left of the view, ×0.55). The car markers are drawn together, with the own marker last. Markers that aren't
  on that player's map are skipped. The ring group's transform is forced to the origin, and its children are placed
  per player.
- `DestroyMinimapManager` (call 0x4FBFF2) unlinks P3/P4's entities first. They belong to P3's package, which is freed
  earlier, and without this quitting crashed.

**Single-player gauges per player (`CreateGauges`).** Built right after each player's HUD.
- Elements, all thiscall (fng, index); each is updated with (player):

| element | ctor | size | update |
|---|---|---|---|
| Tachometer (or, in drag, DragRaceTachometer) | 0x4F5100 (drag 0x4F6500) | 0xC8 (0xA8) | 0x4C68D0 (0x4C7090) |
| TurboMeter | 0x4F5A70 | 0x38 | 0x4C6B80 |
| NitrousGauge | 0x4F6A90 | 0x38 | 0x4C7B20 |

- They're bound to a copy of `HUD_SingleRace` / `HUD_Drag` / `HUD_Drift` ("HUD_SingleRac1.fng", "HUD_Dra2.fng"...),
  pushed with 0x555F10(fng, 0, 0, 0). The copy is removed with 0x5379A0 when the player's HUD is destroyed
  (`HudDtorHook`, call 0x4FD963). Until the frontend drops the package, all its objects stay hidden.
- Ctor, state and update run with NUM_PLAYERS temporarily 1, which gives the classes their 1P look and behaviour.
- `HudUpdateHook` (call 0x6005DB) wraps the HUD update 0x4EC5A0:
  - The 2P package's own gauge elements (hud +0x18 / +0x1C / +0x28 / +0x34) are swapped out during the update. Both
    sets updating the same player fought over shared state.
  - Our elements get SetState 0x4C6690(hud+8 & 0xFDFBFFFF, hud+0xC) whenever the HUD state changes, then their update.
- Element object list: head at element+0, node+8 = object.
- Shown: our elements' objects and their subtrees, plus groups that hold them. Only the group's own hidden bit is
  cleared: the recursive show re-showed the elements' hidden objects (blinking shift light, stuck N2O overlay).
- Hidden: everything else in the copies, and the 2P package's gauge objects. They're skipped at render time, as is
  every gauge object during the intro and the pause menus.
- `GaugeTransform`:
  - Normal races: the 1P layout's bottom-right corner goes on the view's bottom-right corner, scaled by
    GaugeScalePercent.
  - Drag: the whole layout is fitted to the view height (×0.5), and side elements stay at the view sides. The top-right
    speed moves below the 2P position box.
- Messages to a player's 2P HUD package are forwarded to their copy (drag shift messages).
- Custom gauge textures (`HudTexLoadHook`):
  - The race load step 0x577F60 (call 0x57F3D8) loads `GLOBAL\HUD_CustomTextures_<HUDINDEX>.bin` for P1's car only and
    makes its 8 textures resident. The names come from 0x5ED9B0(spec, car parts, units, out, index), and the index is
    attribute "HUDINDEX" of car part 0xA3, read with 0x61B500(key, 0).
  - Other players' packs are loaded the same way: 0x4944D0(path, 0, 0, 0) and wait 0x490170(0), once per index. Each
    player then gets their own list (0x5ED9B0 picks the textures by car too: two stock cars share index 0 but not
    their tach textures) made resident with 0x494D60(list, 8, cb, ctx, 0).
  - Unload (call 0x5816D4): every player's list is released first (0x490150(list, 8); a pack can be P1's), then the
    game's unload 0x580FC0, then the plugin's pack files (0x494550(path, 0, 0)).
  - In drag, the load runs with NUM_PLAYERS = 1, which selects the drag texture set.
- The 1P drag package starts with its gauge groups hidden. The game shows 1P packages with 0x52CFC0, but only outside
  drag.

**Live gap.** The gap text element (0x4DA414) computes slot0 vs slot1 and shows 0 for everyone else.
- Drift branch → `DriftGapHook` (0x4DA445).
- Others → `RaceGapHook` (0x4DA4DC → 0x4DA526, with 0x5F82D0([0x890118]; &out, other, self)).
- Both take the minimum over the other local players: leading = + gap to the next car, otherwise − gap to the leader.

## 10. Sound
- **Listeners.** The audio mixes each sound against one of 2 listeners. Their state is 14 two-entry arrays (0x82BE70,
  0x82BE80, 0x82BECC, 0x82BEDC, 0x82BEF0, 0x82BF10, 0x82BF20, 0x82BF34, 0x82C610, 0x82C670, 0x82C6C4, 0x82BB40,
  0x82BB48, 0x82BB50), written by the listener update 0x46D7E0 (call 0x483CB3) from the audio player list
  ([[0x82B884]+0xAC]). Each sound picks one (sound+0xD0) through the nearest-of-2 choice at 0x45E147.
- The arrays are relocated to 4 entries (`RelocateListenerArrays`, 145 refs). `ListenerUpdate` runs the update a second
  time with P3/P4's nodes posing as 0/1, and `ListenerChoiceHook` / `NearestListener` pick from all four.
- **Intro after a track change.** Sounds created during the load/intro bind to the nearest listener and keep it, and
  listeners 1-3 still had the previous track's positions. From the race load until a player's camera moves, that
  player's listener copies listener 0 and is skipped by NearestListener ("parked").
- Engine sounds use their own path (0x462D51); it's left as is, and every player hears their own engine.

## 11. Race flow
- **Intro (NIS).** The selector 0x436840 builds a context (0x431F10: racers, humans, slot masks, track byte) and
  filters scenes (0x42FE00). With exactly 2 local players only the 2P list passes (0x42FDA0: IntroNis12/13).
  - `IntroContext` (call 0x4368F4) reports 4 racers for a 3-player race, so the 4-car scenes are used.
  - `IntroEmptyHook` (0x4369FE) handles an empty 2P list: the scene loop reruns with `Intro2PFilter` (0x43695F) letting
    everything through, i.e. the single-player scenes.
- **Views 4/9 without an intro.** Layouts 1/2 enable view 4 (road reflection; its RT 0x86E784 enabled, 320x240) and
  view 9 (0x5C233D). Layout 3 has no case, so split races used to inherit them from the intro. With road reflections
  on (0x870CD4), the main render 0x5CAAD0 returns immediately when view 4 is inactive (0x5CAB35). A race without an
  intro (the street drift tracks 4174-4179 have none) therefore drew nothing 3D and crashed on quit.
  `ApplySplitLayout` now enables views 4 and 9 every frame.
- **Counting extra racers.** Only add up to g_extraPlayers: the street drift tracks add non-human racers (ids 50+) to
  the race count (+0x74), and counting them overran the standings' static array 0x864640.
- **Results.** The results builder 0x54CCA0 (call 0x551447) fills its pointer list for 0x89E7C0 + [C4] + [C8] + [CC]
  racers. `RaceResults` counts the extra racers into 0x89E7C8, and they stay counted until the race is left
  (`EndResultsCount`), because the results screen rows (0x4BF0A0 / 0x4BF140 / 0x4BF290) use the same sum. Before the
  race setup calls that read the counters (0x5260B0 at 0x5324F2, 0x526970 at 0x530A91 / 0x581902) they're uncounted.
  This only happens with extra players: in plain 2P, 0x89E814 also counts AI and would add phantom rows.
- **Standings.** 0x56D850 (calls at 0x5F7B3F, 0x5F94C7, 0x6052F8) → `BuildStandings`, which counts the extra racers for
  the call. Drift scoring needs it.
- **Camera.** Per-player settings mirror (section 5); the drift camera works for all players.
- **HUD binding list (4P quit).** The element at hud+0x70 (0x4F89D0) keeps a data-binding list. P4's last node
  sometimes has next == 0. `ClearHudBindings` (call 0x4F8BAE) stops at broken nodes. The root cause is unknown.

## 12. Thread safety
- **Game memory pools.** 0x440390 (alloc) and 0x440540 (free) link blocks into the pool's list outside the pool lock,
  and the sound thread uses them too. This caused list corruption and crashes, which got more likely with 3-4 cars
  loading banks. Both are wrapped in one critical section (`PoolAllocLocked` / `PoolFreeLocked`).
- **Audio file system.** Open and close (vtable 0x7C2E2C: +0xC open, +0x10 close; `CreateSoundFs` hooks the creation)
  are called from two non-main threads, and they share an unsynchronized free-handle list. `SoundFsOpen` /
  `SoundFsClose` serialize them.

## 13. Open issues and leads
- **Light streaks without overbright** in split views (section 7).
- **Results ambient sound stutter (3P).** Continuous ~2 Hz gating, music off. Not frame hitches, sound fs handles, the
  nearest-player functions or ListenerUpdate (each was reverted live with no change). Left to check: the relocated
  listener arrays, or the original 2P behaviour.
- **Black results background**, seen once after a 2P race. If it happens again, capture state while it is still
  happening (g_currentView, views 1-4 active/rt, viewport).

## 14. Gotchas
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

## 15. Tooling and workflow
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
