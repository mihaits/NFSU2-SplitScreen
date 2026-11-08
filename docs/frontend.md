# Frontend: menu, players row, joining, car select

**Menu icon.** The entry's icon is MAIN_ICON_SPLIT_SCREEN (128x64 DXT3). HD interface mods redraw the other main menu
icons at 512x256 but not this one. `SwapSplitMenuIcon` runs each time the entry is added: when the loaded Quick Race
icon is wider than 128, it replaces this texture's D3D texture with the embedded 512x256 DXT5 one (`src/assets/`, an
upscale of the original). It reads TextureInfo through 0x4901D0(hash, 0, 0): +0 platform block (+0x18 the D3D
texture), +0x24 hash, +0x44 width. The game reloads the menu textures after a race, so the swap runs again then.

**Main menu entry.** The main menu builder 0x4AEE30 adds options. At 0x4AEED8 (the `call 0x570A60` right after Quick
Race is added), `MainMenuHook` allocates an option (0x48 bytes, game malloc 0x575620) and initializes it with
`0x51F670(opt, 0x883E25D6, 0x6A2B93EB, 0)`, which gives the console icon and the localized "2 Player Split-Screen" text,
both still in the data. The text is shortened to "Split-Screen": `RenameSplitMenuText` points that
entry of the loaded language table (entries {hash, text*} sorted by hash at [0x8383D8], count [0x8383D0]; lookup
0x4FF9D0) at a shortened text when it holds a known original (English, German, French, Italian, Spanish, Dutch, Danish,
Swedish; others keep theirs). Its vtable is `{0x4AEB70 dtor, SplitMenuReact}` and it's added with `menu->vtbl[0x18](opt)`.
`SplitMenuReact` sets a flag and jumps to the Quick Race react 0x4FC050 (thiscall, ret 0x14). `QuickRaceModeHook`
(0x4FC061) then sets mode 4 instead of 2.

**Garage backup.** When the quick race flow backs out to the main menu in mode 4, it restores player 1's garage
(0x4B2A00 → 0x515D70, from backup +0x1677C / car +0x20354). PC lost the code that takes that backup, so it would restore
zeros (no car in the main menu, and Customize crashes). The plugin takes the backup when mode 4 is entered (`BackupGarage`).

**FE player after a race.** FE player 0x85AD28 is the player whose car select ran last: 1 after a 2P race, 0 after 3P
(player 3 picks on FE player 0). It stays that through the race, Event Results, Post Race (and its Quit to Main Menu?
dialog), the track select and the mode select. These screens listen to the current FE player's input device, and
`PadForDevice` feeds that device from the last active pad (see [input](input.md), pad resolver), so any player's pad
drives them and Back walks them out to the main menu. Post Race's Restart reloads the race with every player on their own
pad, and the next car select starts on FE player 0 with player 1's pick, so the FE player needs no reset after the car
select. Leaving split resets FE player 0x85AD28 and the FE ports; a stale FE player 1 crashes the next 1P car select.

**Players row (2-6).** It's added to the split race options screen. Option rows are 0x54-byte widgets: base ctor
0x51E230, vtable slot 1 `Act(fng, msg)` (left 0x9120409E / right 0xB5971BF1), slot 3 `Draw`. Rows are added with
`0x54F170(screen; row, 1)`, and each race type has its own row builder (switch at 0x4E2C9A).
`0x539650(0)` is called at 10 sites (0x4E2CAC..0x4E2D5F), and `QrFinishRows` appends our row there. The row's vtable is
copied from the laps row (0x7910B8). The value is `g_splitPlayers`; the ini `ExtraPlayers` gives its default.
Every mode, Outrun and drag included, takes 6 racers: every track's grid has 8 spots per direction (drag: 4 abreast,
2 rows), and the race start placement reads 6 of them.

**Cars row.** Added after the Players row in every split mode (same row type; `[DEBUG] CarsMode` gives its default):
Pick Each (the car select as it is), Pick One (player 1 picks for everyone), Random. With the last two a seat whose car is given has no
pick screen: its join (`JoinedSeat`) marks it, and on the next car select tick `AutoPickTick` writes the car to
0x83AAB8[fe] and goes on as a pick would. Where a pick asks for the gearbox (0x4FC3D4: not drag, the FE slot's ask
setting 0x83AA6A + fe * 0x28 on) it opens the same dialog (0x497D50(carsel)), whose answer (0x1A2826E1 automatic /
0x5F5E3886 manual, then 0xBA5B5FCE) runs the after-pick calls; otherwise it runs the after-pick step itself
(`CarselToP2` on FE 0, `CarselStart` on FE 1), and players 1/2 keep their FE slot's gearbox, players 3-6 their own
player settings. Either way the next seat's Press START opens or the race starts. The pick also stores the pressing
device's port (0x50D2A0(port)); a given car keeps its join's port (calling 0x50D2A0 without the port stored garbage
there and crashed the race in 0x579434, which indexes a per-port table). Pick One gives player 1's pick of the round to the others. Random deals at player 1's join
(`PickRandomCars`): one car of the pool at random, then the cars within 4% of its rating (the tolerance doubles until
there are enough), a different one per player while there are enough. Back on the next seat's popup returns to a seat
that had no pick screen; `BackToSeat` marks it and the game's own Back on its pick screen (sent as an FE message)
reopens its popup, so Back still undoes only the last join.
- The pool is what the car select offers in its categories 1 Stock, 2 My Cars, 4 Career, 8 bonus (the RB cycle at
  0x4EED10; 0x10 entries are never listed), by the game's own tests (list builder 0x4E22E0, category count 0x534850).
  Each entry needs +0x10 set, a valid car type (vtable slot 1), a usable car type record
  ([0x8A1CCC] + type * 0x890: +0x840 slot passes 0x5022C0 and the record 0x610180), and to be unlocked:
  0x534180(0x497DA0(), id, 0) for most cars, +0x7F5 for a career car. The regional CORSA / PEUGOT106 entries have no
  rating, so they never come up.
- Rating: the game's performance rating of the car as built, upgrades included (`CarRating`, about 2 ms a car): the
  entry's own build into a scratch RideInfo (ctor 0x5AC6E0 on ride +0x10; vtable slot 3, thiscall (entry; mode 0,
  ride, 0): 0x503A80 stock, 0x516AC0 My Cars / career, 0x5039D0 bonus), then 0x5B6990(ride +0x10; 0, -1.0f) and the
  release 0x4022C0(ride +0x384). Entry +0xC is no use for this: a My Cars / career entry's setup (0x516B90) stores
  the rating of a fresh stock build of its car type (0x610270(type, 1, 0, 0)) and nothing updates it; a stock entry's
  slot 3 stores its own, which matches.
- Garage car list (0x844968 = P1's garage 0x83AD90 + 0x9BD8, 0x5B entry pointers, built by 0x5036F0): 0x30 stock
  entries (0x1C bytes, vtable 0x79AADC, +0x18 car type), 0x14 My Cars (0x434, vtable 0x79B12C), then career cars,
  0x10 entries and bonus cars (vtable 0x79AD18). Every entry: +8 id (the car id the picks store), +0xC performance rating
  (float, 0x5B6990(ride + 0x10; 0, -1.0) as cached by the entry's ride builder, vtable slot 3: 0x503A80 stock, 0x516CA0
  My Cars), +0x14 category.

**Joining (Press START).** The 2P_PressStart popup handler 0x498070 only reacted to the console START message
0xB5AF2461. `PressStartHook`/`PressStartFilter` instead waits for XInput Start on an unassigned pad or keyboard Enter
(any seat, one keyboard player per round: `g_keyboardSeat`), records `g_assignedPad[seat]`, and injects START with mask 1 << player. The popup text object 0x2EBB974D is
set to "Player Three/Four/Five/Six Press START" for seats 2..5 (English only). A pad that has joined can't join again.
On a later seat's popup only the previous seat's pad or keyboard drives the frontend (`CarselDriverSeat`, used by
`PadForDevice` and `KeyboardPort`), so only the player who joined last can press Back to undo their pick. P1's popup
starts a new round, so any pad or the keyboard drives it (the previous round's pads no longer count).

**Last race again.** Each split race started from the car select is remembered (`RememberRound`: player count, every
seat's pad or keyboard, car 0x83AAB8, port 0x83A9D8, gearbox +0x15). Player 1's Press START popup then also shows
"Press Y for Rematch" ("Y / SPACE" when the keyboard played), while the player count matches and every
car is still in the garage and every pad still connected (`LastRoundReady`). Y on any of those pads (or Space) joins
player 1 (`PressStartFilter`), restores every seat's assignment, and on the next car select tick `QuickStart` puts the
cars, ports and gearboxes back and starts the race (0x53FC20) without the picks. A pad takeover in a race updates the
remembered pad.

**Next Race.** `parts/next_race.cpp`. Post Race's builder 0x4BEE10 (fastcall menu) adds its icon options with
`0x51F670(option, icon, text, 0)` and `menu->vtbl[0x18](option)`: career's Next Race (icon 0x12BB5EA2, text "Next Race"
0x23F359B8, react 0x4BEC90), Restart (0x4BECC0), Race Stats, Replay (not in split) and Quit to Main Menu (0x4BED40).
In split (mode 4) `PostRaceBuildHook` (at 0x4BEE26) adds Next Race first with the game's own icon and
localized text. Its react sends UI_PostRace.fng 0x30ED2368, the message Quit's confirm dialog sends on OK. The screen's
handler 0x4F39A0 answers with 0x24DD40CF, then 0xD78D7BF1 → 0x4A3E80, which leaves the race for the screen 0x502FF0
names (stored at 0x8384C8): the quick race mode select in split (garage scene behind it, the race's mode selected).
`NextRaceTick` accepts the mode select once it has been up for 3 frames, the same as A, which opens the track select
of that mode; the accept's menu sound (id 4) is dropped (`g_feSoundMuted`), since Next Race's own accept already played it. While `g_nextRace` is set, the track select's next screen (0x498240, `QrTrackNextHook`) is the car
select instead of the options, and player 1's Press START popup takes the last round by itself, as Rematch does
(Random deals new cars first); if `LastRoundReady` fails, the round is joined as usual. Back from the track select
reaches the mode select, and another mode also starts without its options screen. Another mode takes the last race's options it also has (`CarryRaceOptions`, run on
player 1's popup also when the round is joined as usual), since its options screen is skipped. Quick race settings per type at 0x83AAC4 + type ×
0x14: +0 local players, +1 opponents, +2 AI difficulty, +4 traffic, +5 catch-up, +8 track (word), +0xA laps (word),
+0xC direction (dword); the split options rows offer (measured by changing each row): direction in circuit, sprint,
drag, Street X (type 10), URL (4) and Lap KO (7; from its row builder 0x4CDB40), laps in circuit, drift, Street X and
URL, traffic in circuit, sprint and drag, catch-up in all but drift, difficulty in every type with AI. Direction is
copied only when the new track can be raced reversed (0x5D3E40(track) +0x86 == 1, the Direction row's own test). That mode list leaves out Free
Run (not a race): `ModeListFreeRunHook` (0x4B2F8A, the entry's first test) skips past it (to 0x4B2FE0) while
`g_nextRace` is set. Outrun has no track select (its track is random), so Next Race into an Outrun (after one, or
picked on the mode select) starts it straight away: `RandomTick` accepts its options screen, which comes up in place
of a track select, like the skipped options of every other mode. To change its Time Limit, Quit on Post Race to
reach the mode select without Next Race. The main menu ends Next Race, so the next visit to split lists every mode. Quit to Main Menu stays as it is.
- The quick race screens step through a chain (0x4E2900: mode select, options, track select, each with a next and a
  previous screen function; [0x836340], current node +0xC) that the mode select builds (0x4EF1B0) when it is accepted.
  A track select opened straight from the race (by pointing 0x8384C8 at it) has no chain, and A on it crashes in
  0x4B29A0 (null chain), and the garage scene (pushed by the quick race screen manager 0x4F9930,
  `0x555F50(0, "GarageMain.fng", 0, 0, 0)`) is missing. Going through the mode select avoids both.
- A Quit from Post Race whose A is still held when the mode select loads goes on to the track select: that's the held
  A on the mode select, not the quit's destination.

**Random.** `RandomTick` (`parts/next_race.cpp`): Y on any pad, Space or the help bar's Random button rolls on the
split track and mode selects. A roll steps the list to the drawn entry with the screen's own left / right (one per
frame), then accepts it. The steps are silent (the mode select plays its tick, sound 3, for each step message, a buzz
at that speed; the track select plays none); the accept sounds.
- Track select: one of the listed tracks; locked tracks and the track just raced (same type) are left out. After Next
  Race the race loads, otherwise the options come next. The screen is [0x836344] - 0x118 (its ctor 0x4E2F30 stores
  that; vtable 0x7974F0). Its icon list (built by 0x4CDED0) is at +0x4C: node +0 next, +8 locked, +0x14 track id,
  +0x2C track record or 0 (record +0x8A track id); +0x54 is the selected node. A list entry for Random would need a
  track record: the icon, map, distance and best time are all drawn from it.
- Mode select: one of the seven race modes (entries 1-7: circuit 0, sprint 1, drag 2, drift 3, Street X 10, URL 4,
  Outrun 8), then the track select that opens rolls its track by itself. Outrun opens its options screen instead:
  after a roll on the mode select the player accepts it, after Next Race it is accepted by itself.
  `[DEBUG] RollMode = n` makes every mode roll pick entry n (tests). The highlighted entry is in the frontend's per-screen table at
  0x7F7DC8 (stride 0x1C up to 0x7F9178: +0 screen name, +0x10 entry, 1-based, 0 = the first; 0x50B8A0 reads it when a
  screen is built, the screen updates it as the cursor moves). The entries' react calls 0x4B2C00(type), which sets the
  race type and its quick race settings, and the screen then goes on by that type (0x4FA470); an accept message
  skips the react (it went on with the previous type), so the roll calls 0x4B2C00 before accepting.
- A mode roll starts a random series (`g_randomSeries`): Next Race rolls mode (not the one just raced) and track
  again with no input (`NextRaceTick` starts the mode roll in place of its accept). The series ends when the mode
  select comes up any other way (Quit, Back, also Back from a track select that Next Race opened: only Next Race's
  own arrival, `g_nextRaceToTrack`, keeps it) and at the main menu.
- Help bar: `HelpBarSet` adds the Customize button (flag 0x1000; label from 0x52D330) on the split track and mode
  selects and
  sets its text (`RandomButtonText`). The bar's buttons are a list at bar +0x4C (button = node - 4: +0x10 text
  object, +0x1C its flag). The game's PC buttons are plain clickable words; WidescreenFix (`[MISC]
  ImproveGamepadSupport`) swaps a fixed list of English labels for "(A) Continue"-style ones while the last input was
  a pad, so with it loaded the button reads "(Y) Random" ("(Triangle) Random" with PlayStation text). Otherwise it
  is `$BUTTON_Y$ Random`: the PC font skips `$...$` console button tokens (plain "Random"), and the coming FusionFix
  draws them as button images ([plan](plans/fusionfix-compat.md)). A click sends the screen 0xC519BFC0 (the shared
  Customize action), which `SendFeMessage` turns into a roll. The text is set with 0x535FE0, which grows the string;
  the FE printf 0x537BE0 cuts at the object's current length (+0x68, 17 here).

**Car select sounds: one per action.** Every FE sound goes through 0x458520 (thiscall sound manager [0x82B884]; id),
hooked by `FeSound`. Two layers play them: the generic package layer (0x51DD40: runs the package's handler, then plays
the table 0x837FE0 sound of the input it handled, call at 0x51DD81) plays Back (5) / Start (6) for whatever the
controlling device presses, and the car select plays its own for each action it takes (0x50D180 from 0x4FC728 /
0x4FC753 / 0x4FC7AF and `CarselFilter`). Together Back played twice on popups and FE 0's pick but once on FE 1's pick,
a join played 6 only for the device that drove the frontend (not other pads or the keyboard), and an ignored Start or
a Start still held after joining played it as well. In the split car select the generic 5/6 is dropped, and
`PressStartFilter` plays 6 on each join. With `Debug = 1` every car select sound (source, "dropped") and every
Start/Back/A/B, Enter/Esc/Backspace press is logged with the seat state (`LogCarselInput`).

**List screen sounds: one per mouse click.** The list screens (the race options among them) share the handler 0x556020
(thiscall screen; msg, ...), which plays the sound of the input a message stands for at its end (table 0x837FE0, call
at 0x55647B; the message is the saved edi on top of the caller's stack). The accept input (8, sound 4) stands for both
the accept 0x0C407210 and the package click 0x406415E3. The FE mouse code (0x54E0E9-0x54E23E) sends a left click as the
clicked object's accept (with the object as the message's p3) or as the package's 0x406415E3, a right click the same
way with 0x14DD0705. A left click on a row delivers only 0x406415E3; a left click on the help bar's Continue delivers
0x406415E3 and, one frame later, the accept, so the game played the accept sound twice. `FeSound` (`ListClickEcho`)
drops the accept's sound when this call site played the same sound for a click within the last 200 ms. The mode and
track selects have their own handlers and play one sound per click. A right click (Back) plays one. With `Debug = 1`
every sound from this site is logged ("list sound N for message M").

**Seats (3-6 players).** The car select serves FE players 0/1 (0x4FA2A0(fe)). The player 1 pick switches to FE 1
(calls at 0x4FC3A8 / 0x4FC9B6, `CarselToP2`), and the player 2 pick starts the race (0x4FC3BE / 0x4FC9C2 → 0x53FC20,
`CarselStartHook`). With 3-6 players the seats go in order on FE seat & 1: P2's pick goes to seat 2 on FE 0, P3's to
seat 3 on FE 1, P4's to seat 4, ... and the last seat's pick starts the race. Cars (0x83AAB8) and ports (0x83A9D8) are saved per seat, and P1/P2's are restored before the race.
Back on a seat's pick screen undoes only that seat's join: the game's own Back reopens its popup (0x4FC739 on FE 1,
0x4FC795 on FE 0; the switch 0x4FA2A0 also opens it at 0x4FA423 unless it is still active). Back on a seat's popup
returns to the previous seat's pick: FE 1 seats (P2/P4/P6) at 0x4FC723 (`CarselBackToP1`), P3/P5 through `CarselFilter`. The car select
message handler 0x4FC1F0 (`CarSelectHook`) retags FE messages with the current seat's port: PC tags every message with
mask 1, and the handler drops messages whose port doesn't match `FE_PORTS[feplayer]`. While the FE player has no port it
retags to 0 (= any); otherwise Back softlocks. The name box ("Player_Name") is set each frame to the seat's name.

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

**Simultaneous car select (not built; static study 2026-11-06).** Picking at the same time in each player's view
can't reuse the car select: one screen with one list (+0x50/+0x54, 0x930-byte entries by 0x511DB0, cursor +0x58), a
global FE player (0x85AD28) and category (0x7F444C), packages found by name (a second push would not be a second
instance), and one FE ride for the 3D car (0x8371B0, load flag 0x83623C). The second ride (0x836750) belongs to
GarageMain_2P.fng, console code that PC never pushes and whose package isn't in FRONTEND/FrontB.lzc. The FE car draw
0x630DE0(view, mode) draws every car of its list into the view it gets, so drawing has no one-car limit. A picker the
plugin draws itself (per-view lists from the garage list above, names `CAR_NAME_%s` via 0x4FFA80, the manufacturer /
logo textures of 0x511E60, the picks handed over like `QuickStart`) is the workable design; Customize would stay one
player at a time (the shop is FE player 0 on one shared garage). Open for a live test: which view draws the FE
garage scene, the FE memory for a second car, whether the logo textures resolve in the car select.

**FE API (useful)**
- 0x55DDA0 `(msg, fng)` sends a message to a package (hooked: `SendFeMessage`).
- 0x52CF60 `(fng)`: is the package active?
- 0x537980 `(from, to, flags)` switches screens. `from` must be active, or the game crashes.
- 0x555F10 `(fng, 0, 0, 0)` pushes/activates a package; 0x5379A0 `(fng)` removes it.
- 0x537BE0 `(obj, fmt, ...)` is printf-style SetText; 0x50C900 `(obj, hash)` sets localized text; 0x4FFA80 `(hash)` returns a localized string.
- 0x505450 is the FE name hash (case-insensitive ×33, seed 0xFFFFFFFF). 0x43DB50 is the same hash, case-sensitive
  (used for texture and attribute keys).
