# Frontend: menu, players row, joining, car select

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
On a later seat's popup only the previous seat's pad or keyboard drives the frontend (`CarselDriverSeat`, used by
`PadForDevice` and `KeyboardPort`), so only the player who joined last can press Back to undo their pick. P1's popup
starts a new round, so any pad or the keyboard drives it (the previous round's pads no longer count).

**Car select sounds: one per action.** Every FE sound goes through 0x458520 (thiscall sound manager [0x82B884]; id),
hooked by `FeSound`. Two layers play them: the generic package layer (0x51DD40: runs the package's handler, then plays
the table 0x837FE0 sound of the input it handled, call at 0x51DD81) plays Back (5) / Start (6) for whatever the
controlling device presses, and the car select plays its own for each action it takes (0x50D180 from 0x4FC728 /
0x4FC753 / 0x4FC7AF and `CarselFilter`). Together Back played twice on popups and FE 0's pick but once on FE 1's pick,
a join played 6 only for the device that drove the frontend (not other pads or the keyboard), and an ignored Start or
a Start still held after joining played it as well. In the split car select the generic 5/6 is dropped, and
`PressStartFilter` plays 6 on each join. With `Debug = 1` every car select sound (source, "dropped") and every
Start/Back/A/B, Enter/Esc/Backspace press is logged with the seat state (`LogCarselInput`).

**Seats (3-4 players).** The car select serves FE players 0/1 (0x4FA2A0(fe)). The player 1 pick switches to FE 1
(calls at 0x4FC3A8 / 0x4FC9B6, `CarselToP2`), and the player 2 pick starts the race (0x4FC3BE / 0x4FC9C2 → 0x53FC20,
`CarselStartHook`). With 3-4 players, the P2 pick goes to seat 2 on FE 0, and the P3 pick goes to seat 3 on FE 1 (or
starts the race). Cars (0x83AAB8) and ports (0x83A9D8) are saved per seat, and P1/P2's are restored before the race.
Back on a seat's pick screen undoes only that seat's join: the game's own Back reopens its popup (0x4FC739 on FE 1,
0x4FC795 on FE 0; the switch 0x4FA2A0 also opens it at 0x4FA423 unless it is still active). Back on a seat's popup
returns to the previous seat's pick: P2/P4 at 0x4FC723 (`CarselBackToP1`), P3 through `CarselFilter`. The car select
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
