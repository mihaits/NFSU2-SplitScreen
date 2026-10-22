# Players 3/4 and AI opponents

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
joypad bindings (record tables + axis setup, [input](input.md)) and last car are kept in `g_seatStore` and saved to
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
