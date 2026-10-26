# Free Run (free roam)

Split-screen Free Run drives the whole city on track 4000 (from the airport) with 2-6 players, without outrun
challenges and without the race HUD. Race type 9 (0x83AAB4), installed by `InstallFreeRun`.

- **Mode list.** The quick race mode list (UIQRModeSelect::Setup, 0x4B2E..) adds Outrun in modes 8/0x200/4 and Free
  Run (icon class 0x790FFC) only in the others. The `je` at 0x4B2F99 is NOPed, so Free Run is listed in split screen.
- **Next screen.** After the mode select (0x4B2A80) split screen's Outrun goes to the race options (0x7F4460) and the
  rest to the track list, except Free Run, which goes to its own screen (0x7F446C: straight to the car select, no
  Players row). `NextScreenAfterMode`: split screen + type 9 → the race options screen, track word (0x83AAC4 + 9 ×
  0x14 + 8) = 4000 and the direction dword (+0xC) = 0 (forward). No track choice (user).
- **Back.** The race options' previous screen (0x4B2AF0) is the mode select (0x7F445C) for split screen's Outrun, else
  the track list. `PrevScreenRaceOptions` returns the mode select for split screen's Free Run too.
- **Options rows.** Free Run's row builder (0x4B4A10) adds Track Direction (vtable 0x791608, settings +4) when the track
  can be reversed (track info +0x86 == 1, 4000 can), then Traffic. `FreeRunDirectionRowHook` (0x4B4A42) skips the
  direction row in split screen.
- **Players.** Free Run's starter (0x53F580) sets 1 local player and creates 1 human; `CreateHumanRacers` makes it 2 in
  split screen, then adds players 3..6 as usual ([players](players.md)).
- **Free state.** When driving starts, Free Run switches the race to its free state (0x89E7B0 = 7, 0x607190) and sets
  the local player count from the human cars (0x6071D5). With 3-4 that stopped the game's 2-player paths (dark
  screen). `FreeStatePlayersHook` (0x6071D2) caps it at 2.
- **Skins.** Race type 9 sets skin 1 for every human (0x526007). `FreeRunSkinHook` (0x525FFE) skips that in split
  screen, so every player keeps their own paint.
- **No outrun challenges.** In the free state the race update runs the challenge logic 0x607CA9..0x608C01 (picks an
  opponent, race +0xBD0, ends the race when the player pulls away). `FreeStateRaceHook` (0x607C9C) skips it in split
  screen. The player update aims a 3D direction arrow at that opponent (0x5F3AE0, MARKER_DIRECTION_AID);
  `DirectionArrowHook` (trampoline) returns at once (`ret 4`) in split screen.
- **HUD.** The position, lap time and outrun elements are hidden or skipped; see "Free Run HUD" in [hud](hud.md).

## Ideas (not built yet)
Game modes and extras to make Free Run more fun (discussed 2026-10-26; the start location is planned in
[plans/free-run-spawn](plans/free-run-spawn.md)):
1. **Waypoint race** (user's idea, first pick): any player drops a pin on the map, or the game picks a random one;
   everyone gets an arrow to it, first there scores a point, short scoreboard. The direction arrow (0x5F3AE0,
   MARKER_DIRECTION_AID, skipped today) might be reusable.
2. **Tag / "it"**: bump someone to pass it on; least time as "it" wins. Needs only car-car contact detection.
3. **King of the hill**: a zone that scores for whoever is alone in it, moving now and then.
4. **Outrun duels on demand**: two players flash their lights to start the game's own outrun challenge between them.
5. **Speed traps / longest drift**: a running best top speed through a spot, or drift score, per player.
6. **Convoy / cop chase**: one runs, the others box them in within a time limit.
7. **Quality of life**: regroup (teleport everyone to the leader), a "where is everyone" map ping, time of day and
   weather.
