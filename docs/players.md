# Players 3/4 and AI opponents

**Local player arrays.** These are 0x8900AC byIndex[2], 0x8900B4 inOrder[2] and 0x8900BC count, with about 300 code
references. Many of them are address-bounded loops (`p < 0x8900B4`). They are relocated as one block,
`g_localPlayers {byIndex[4], inOrder[4], count}`, so that "end of byIndex == inOrder" and "end of inOrder == count" stay
true (`RelocateLocalPlayers` rewrites every disp32). One indexed reference is "inOrder - 4": the race teardown loop at
0x60AA76 (`while (count) delete inOrder[count-1]`). It is patched to &inOrder[0]-4; otherwise quitting with 3 players
hung.

**Human racers.** 0x525ED0(numPlayers) creates human racers (racer spec ctor 0x6373E0(spec; playerIndex, human, car,
profile, ...), AddRacer 0x601600; AddRacer stops at 2 humans). Its 8 call sites → `CreateHumanRacers`: the original,
then one more pass per pair of extra players (P3/P4, P5/P6) with `RacerSpecIndexShift` (call 0x52602A), which shifts
playerIndex by `g_extraRacerShift` (2 or 4) and uses that seat's car and name (`g_extraNames[index - 2]`, skin
DUMMY_SKIN<index + 1>). The human component manager capacity (type 1) imm at 0x4856C8 is raised from 2 to
`MAX_LOCAL_PLAYERS`. 6 humans fill the 6 start spots (Opponents row max = max racers - players).

**Racer of player.** 0x5ED960(index) reads the race's 3 human slots, so `RacerOfPlayer` sends index >= 2 directly to
0x5ED920 (find racer by player index).

**Players.** The call at 0x60A798 → `CreatePlayers`. After the original, for each extra player k = 2..5:
1. racer 0x5ED920(0x89E7A0, k); car = race car list [0x890080] entry with short +0xA == k;
2. `InitPlayerView(5..8)` and its own camera (malloc 0x320 + 0x44F5A0). This must happen before the player: camera
   movers (0x44AB90) read view->camera.
3. The player is allocated (0x9C0 bytes) and built with ctor 0x5F1670(port k, k & 1). It's built as P1/P2 so that a
   loaded 2P HUD package is used.
4. byIndex is fixed up; +0x10 = k, +0x18 = view 5..8, +0x1C = 0 (no mirror); `0x5F3E10(player, car)` attaches it.

**Per-player settings.** Table 0x83AA64 + i*0x28 has 2 entries (camera +0x10, Feedback +4, HUD options, ...), so P3
read race data as its settings (e.g. camera 0 instead of P1's drift camera). The 27 in-race readers (kSettingsReaders)
now use a 4-entry mirror `g_settings`. P1/P2 are synced both ways each frame: menus write the game's table, while the
in-race camera change 0x5F2FA0 → 0x502540 writes the mirror. P3/P4's entries are their own (seat store; the first race
without one copies P1's / P2's). HUD state 0x600090 (call 0x600595, `HudStateOf`) has 2-player rules on the index, so P3/P4 run it as P1/P2 with
their own options put into that mirror entry for the call.

**Gearbox.** The car select's gearbox dialog writes the FE player's entry +0x15 (0x4FC267 on 0x1A2826E1 → 0,
0x4FC5B3 on 0x5F5E3886 → 1). P3/P4 pick on FE player 0/1, so their choice overwrote P1's / P2's. `SaveSeat` keeps
each seat's choice and `RestoreSeat` puts P1/P2's back. The player ctor copies entry +0x15 / +0x16 (0x5F17E9 /
0x5F17F2) to player +0x20 / +0x21, which the attach 0x5F3E10 applies to the car (+0x20 through the car's vtable +0x34,
forced to 1 in drag; +0x21 to car +0x464). P3/P4 were built as P1/P2, so `CreatePlayers` writes their own
(`g_settings[idx]`, which takes the seat's pick) before the attach.

**Pause menu settings.** Most pause player options (handler pairs 0x4C31D0..0x4C4700: HUD options +0..+5, +0x10,
+0x14, Transmission +0x15, Stability Control +0x16 (0 = on)) pick the entry by comparing the pausing port
[0x8900D8]+0x20 with P1/P2's ports, so any other port gets entry 0; the camera options use 0x4A7AE0 (the pausing
player's index among P1/P2, 0 for anyone else). While P3..P6 owns the pause (`g_pauseOwner`), `SyncPlayerSettings`
swaps their entry into the game's table at entry 0, and `PausingPlayer` (jump at 0x4A7AE0) returns 0. The menus then
show and change the pausing player's own settings; the table goes back to P1/P2 when the pause menus close.
Transmission and Stability Control also apply the change to the car at once (message 0x578EF0 type 1 / 2 →
0x4A7B30: player +0x20 / +0x21, car gearbox / car +0x464, on byIndex[index]); the index is the same port comparison,
so `PauseGearboxMsgHook` / `PauseStabilityMsgHook` (0x4C3D53 / 0x4C32B3) put the pausing P3..P6's own index there.
The frontend Options screen's copies (0x4B75E0..) always edit entry 0 (0x836364, only ever set to 0).

**Seat store (players 3..6's own data).** The game keeps per-player data for 2 players only. P3/P4's settings entry,
joypad bindings (record tables + axis setup, [input](input.md)) and last car are kept in `g_seatStore` and saved to
`NFSU2SplitScreen_Players.ini` ([Player3] .. [Player6], hex) whenever they change.
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
logic with their own state (offsets, idle timer, car speed): their own right stick (`XPadRightStick`) and, for the
keyboard player only, the mouse (nobody's when everyone uses a pad). Single player races take the same path with the
last active pad and the mouse: WidescreenFix's stick is the last polled slot, slot 1, which has no pad there. The mouse movement is recorded as the game reads the system mouse (`XMouseTakeDelta`, a GetDeviceState
hook in XInputDevices). Settings are read from WidescreenFix's ini. WidescreenFix's own function still runs once per
frame on a throwaway copy, because it also hides the mouse cursor while racing.

**Nearest player / racer.**
- 0x459AC0 (player) and 0x459A50 (player's car) compared players 1/2 only. They are replaced by `NearestPlayer` /
  `NearestPlayerCar`, which consider every local player.
- 0x5F1160, "distance to the nearest racer", measured racers 0/1 only. Per-car effects (0x60A1AB) skip cars more than
  150 m away, so P3's car had no particles. Now `NearestRacerDistance` (trampoline) uses every racer when extra
  players exist.

**Traffic.** The World update 0x609CE0 copies the first +0x14 race cars (the racers, up to 6) into World slots:
position +0x80 + 0x10·i, road position +0xE0 + 4·i (car +0x8FC). Traffic runs from there every ~1 s (0x42F910): each
tick it takes the next traffic car, and if 0x417BB0 says it is out of range it respawns it next to a random local
player (0x42F800: a cross-traffic emitter 0x42E790, or ahead on that player's road 0x42E9F0 / 0x42E620).
- "Out of range" and "in view" use the World distance helpers 0x5F0FB0 (xy), 0x5F1160 (xyz) and 0x5F12A0 (road), which
  read slots 0/1 only outside online races. Keep a car: within 250 of a slot (0x409800). Reject a spawn point: within
  250 (0x409840 / 0x409890 from 0x418110). Parked cars (0x40BEE0) and TrafficController moves (0x42A5E0) also use
  them. So traffic around P3..P6 was recycled over and over and respawned in their view: in a 6P sprint with P6
  driving away from the rest, 168 recycles and 59 cars appearing within 250 of P6 in 60 s. `WorldDistXy` /
  `WorldDistRoadHook` (trampolines) also measure to players 3..6's cars (0x5F1160 is `NearestRacerDistance`). Same
  test after: 14 recycles, 5 appearances at 206-245 (the edge of the 250 road distance).
- 0x42F910 fills a 3-float stack array (ebp-0x18) with each player's traffic spacing (0x418240) by player index, read
  by 0x42F800 (0x42F88C) for the player it spawns around. P3 shared slot 2 with the car 0x5F0E30(-1), P4 overwrote a
  saved timer, P5 the cars-per-tick limit. `TrafficSpacingFillHook` / `TrafficSpacingReadHook` keep players 3..6 in
  `g_trafficSpacing`.
- The same tick moves at most ebp-8 traffic cars (16 every 0.3 s in drag, else 1 every 1 s), shared by every player, so
  with players apart the cars one had passed came back slowly. The limit is now at least one per local player (in the
  fill hook). Same 6P test: traffic near P6 2.8 -> 4.3 on average, near the parked group 0.5 -> 1.8, 6 appearances
  near P6 (196-231). Traffic level 3, starts staggered 8 s apart (P6 first), 90 s: 2.7-3.3 cars near every player,
  10 appearances, all at 196-250.
- The count of traffic cars is the track's table entry for the traffic level (0x4FE9F0: track info +0xEC + direction +
  level * 2, at most 16), the same as single player, shared by all players. Not raised: the race car array holds 22
  cars (World +0x1C; `IndexCars` 0x5F0D20 has no bound, and its global arrays 0x89CCF8/0x89CD50/0x89CDA8 are 22 long),
  6 racers + 16 traffic fill it exactly, and every car costs car pool memory and per-car resources.
- Traffic car models still stream for P1's area only (0x412950: the zone at inOrder[0]'s car).
