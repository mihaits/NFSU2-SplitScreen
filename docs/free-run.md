# Free Run (free roam)

Split-screen Free Run drives the whole city on track 4000 (from the airport) with 2-4 players, without outrun
challenges and without the race HUD. Race type 9 (0x83AAB4), installed by `InstallFreeRun`.

- **Mode list.** The quick race mode list (UIQRModeSelect::Setup, 0x4B2E..) adds Outrun in modes 8/0x200/4 and Free
  Run (icon class 0x790FFC) only in the others. The `je` at 0x4B2F99 is NOPed, so Free Run is listed in split screen.
- **Next screen.** After the mode select (0x4B2A80) split screen's Outrun goes to the race options (0x7F4460) and the
  rest to the track list, except Free Run, which goes to its own screen (0x7F446C: straight to the car select, no
  Players row). `NextScreenAfterMode`: split screen + type 9 → the race options screen, track word (0x83AAC4 + 9 ×
  0x14 + 8) = 4000. No track choice (user).
- **Players.** Free Run's starter (0x53F580) sets 1 local player and creates 1 human; `CreateHumanRacers` makes it 2 in
  split screen, then adds players 3/4 as usual ([players](players.md)).
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
