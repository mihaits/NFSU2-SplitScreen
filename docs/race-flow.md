# Race flow: intro, results, standings

- **Race load.** 0x57F2D0 (ecx = 0x8654A8) adds 1 to the step at +0 and runs that step; the callback 0x571150 of each
  step's load calls it again. Step 2 is the custom HUD textures (call 0x577F60). Step 4 sets game state 5, queues the
  race's cars and hands the callback to the car loader (0x63BCD0, see [streaming](streaming.md)). Later steps load the
  HUD packages; the last one sets state 6. A load that never calls back stops the race load with no error: the
  loading screen stays up at full frame rate.
- **Intro (NIS).** The selector 0x436840 builds a context (0x431F10: racers, humans, slot masks, track byte) and
  filters scenes (0x42FE00). With exactly 2 local players only the 2P list passes (0x42FDA0: IntroNis12/13).
  - Scenes exist for 2 and 4 cars. `IntroContext` (call 0x4368F4) reports 4 racers for a race of 3 or 5-6 cars, so
    the 4-car scenes are used. With 5-6 cars it also keeps only slots 1-4 of the context's slot masks (+8 humans,
    +0xC AI): the scenes' masks cover slots 1-4, so players 5/6 failed every scene (no intro, no grid girls).
  - `IntroEmptyHook` (0x4369FE) handles an empty 2P list: the scene loop reruns with `Intro2PFilter` (0x43695F) letting
    everything through, i.e. the single-player scenes.
- **Grid order.** 0x5264A0(order) numbers the racer specs' grid slots (spec +3): order 1 = humans (spec +4 == 1) then
  AI (2), order 2 = AI first (the URL, drift and Street X starters). With 5-6 cars, order 2 put the humans in slots
  4-6, outside every scene's slots 1-4, so there was no intro. `AssignGrid` uses order 1 in split races with more than
  4 cars. Start spots come from 0x5EB5D0(6, out) (track markers).
- **Extra cars during a 4-car scene (5-6 cars).**
  - The scene binder 0x439240 (run every frame while the scene object is active, which lasts into the race, and on
    restarts, which skip the scene) teleports each bound car (0x4309D0 → 0x5FBD60) to its track's spot, a 2x2 formation,
    and records it in the binding table 0x827B98 + track × 0x14 (+0 track, +4 x, +8 y, +0x10 car array index, -1 = none).
    On a track whose grid is 4 abreast (drift 4311) the formation's second row is grid spots 5/6.
    `NisBindCars` → `MoveExtraCarsBehindScene`: while the scene drives its cars, an extra car that overlaps a bound car
    goes one row behind the formation.
  - The scene leaves its cars off the grid (about a metre on Street X, a row on 4311). In every split race, as soon as no
    car is scene-driven (scene over, or a restart's binder), `PutCarsOnGrid` puts every car on its own start spot
    (`StartSpots` keeps 0x5EB5D0's entries; spec +3 = grid slot).
  - The scene's cars drive through the extra cars' spots. `CarNotVisible` (trampoline on the per-view car check
    0x60D4F0: body 0x6317A2, lights 0x60D594, headlight pools 0x6154D3) hides every car the scene doesn't drive
    (mover car +0x28 != 5) until the scene releases its cars.
- **Post-race scene (outro).** The picker is the intro's (0x436840, branch 1 = outros EndingNis01-04; random 0x43C1C0
  at 0x436BC5; with `Debug = 1` picks are logged and `[DEBUG] ForceOutro` forces one, see [tooling](tooling.md)). It binds its car tracks to race cars by car array index (table 0x827B9C, +0xC car index; binder
  0x4309D0 sets mover mode 5 = NISMvr) and drives the cars it doesn't feature to the world origin (body target via
  0x5A44A0 from NISMvr::Update 0x5AB3C0). With 3-6 players these can be human cars, and with AI opponents also AI cars:
  - The fall-through check 0x5ECBA0 (below ground → car +0x96C = 1) reset the car onto the road every ~0.5 s (0x6009A0
    via 0x601324), each reset switching its mover 5 → 0 → 5: a flash and an audio duck. `FallResetHook` skips the
    reset while the mover is 5.
  - The held car fell forever, and its wind sound played to its player's listener; an AI car fell too (a featured
    car's engine cut out, 3P drift with 3 AI). `HoldSceneParkedCars` (from `EndResultsCount`) holds every such car
    under the map: body = [[car+0x34] + 0x20], z +0x28 (and +0x108) = -50, vz +0xA8 = 0.
  - The streamer followed such a car across the map, see [streaming](streaming.md).
- **Views 4/9 without an intro.** Layouts 1/2 enable view 4 (road reflection; its RT 0x86E784 enabled, 320x240) and
  view 9 (0x5C233D). Layout 3 has no case, so a split race would inherit them from the intro. With road reflections
  on (0x870CD4), the main render 0x5CAAD0 returns immediately when view 4 is inactive (0x5CAB35). A race without an
  intro (the street drift tracks 4174-4179 have none) would draw nothing 3D and crash on quit, so
  `ApplySplitLayout` enables views 4 and 9 every frame.
- **Counting extra racers.** Only add up to g_extraPlayers: the street drift tracks add non-human racers (ids 50+) to
  the race count (+0x74), and counting them overran the standings' static array 0x864640.
- **Results.** The results builder 0x54CCA0 (call 0x551447) fills its pointer list for 0x89E7C0 + [C4] + [C8] + [CC]
  racers. `RaceResults` counts the extra racers into 0x89E7C8, and they stay counted until the race is left
  (`EndResultsCount`), because the results screen rows (0x4BF0A0 / 0x4BF140 / 0x4BF290) use the same sum. Before the
  race setup calls that read the counters (0x5260B0 at 0x5324F2, 0x526970 at 0x530A91 / 0x581902) they're uncounted.
  This only happens with extra players: in plain 2P, 0x89E814 also counts AI and would add phantom rows.
  - `UncountResultsRacers` only uncounts while 0x89E7C8 still holds the value set for the results: a new race's setup
    writes it afresh, and uncounting that went to -1 (crash in the drift scoring 0x56DE6E). It also runs in
    `CreatePlayers` (restart).
  - `CapAiCountToCars`: one race ended with an AI spec that never got a car (cause unknown, not reproduced), and the
    results rows read past the list (crash 0x4BF2D2). `RaceResults` caps the AI counter at the AI race cars that exist.
- **"Hurry!" timer.** When the first racer finishes (0x606FD1), the 30 s race end countdown starts for an AI finisher
  or a human with exactly 2 local players ([0x8900BC]). `InstallHurry`: the `jne` at 0x606FE8 → `jl`, so 2 or more.
- **Standings.** 0x56D850 (calls at 0x5F7B3F, 0x5F94C7, 0x6052F8) → `BuildStandings`, which counts the extra racers for
  the call. Drift scoring needs it.
- **Session points** (`parts/standings.cpp`). Each split race that reaches its results (each Outrun too, not Free Run)
  gives the human players 10 / 6 / 4 / 3 / 2 / 1 by place among the humans, nothing without a finish; AI racers don't score
  (`AwardSessionPoints` after the results builder in `RaceResults`; places as the pad lights read them,
  `FinishedPosition`). A session lasts until the main menu or a change in the number of players.
  - Shown on the game's tournament standings screen (UI_PostRace_TournStandings.fng, class
    cPostRaceTournamentStandings, ctor 0x4F4610) between Event Results and Post Race. The post-race screen list
    (0x4F3EA0: Event Results unless Outrun, tournament results if 0x4C0050, standings if 0x4C00C0, career reward and
    sponsor screens, Post Race unless Outrun) only adds it for career tournaments and split Outrun; its call to
    0x4C00C0 (0x4F3F9A) goes through `ShowStandings`.
  - The screen's setup 0x4EA570 (its call 0x4F4657, `StandingsSetup`) normally builds rows from those records
    (0x4C07A0, 0x4D77A0). The plugin writes the texts itself: the title `HEADER_TEXT` ("Split Screen Standings"; the
    package's own is "Tournament Standings"), `RANKING_DATA_n`, `PLAYER_DATA_n`, `PLAYER_POINT_DATA_n`
    through 0x537B80(package, 0x50B990(name), fmt, ...), and `CAR_DATA_n` from the car's language hash
    (0x50C900(0x537A10(package, hash), 0x50B990("CAR_NAME_%s", car +0xC))); then [0x8363DC] + 0x1C = package and
    0x4A4770(screen; 6, rows). Names as the game's row setter 0x4C03A0 picks them: the profile name (0x5F41B0, thiscall
    car +0x18) for the car whose spec (car +0x14) is the local human's (+4 == 1, +0x900 == 0), otherwise spec +0x90C.
    Equal totals share a rank; the last race's points order them.
- **Outrun** (`parts/standings.cpp`, `InstallOutrun`). In split screen a single round for 2-6 players that ends like
  any race; the game's Outrun is a tournament of rounds (below). It runs in the outrun state (0x89E7B0 = 7, local player count capped at 2 as in Free Run, see [free-run](free-run.md)).
  - Each frame the race update (0x607CA9..0x608C01) walks the race manager's racers ([0x890118] +0xBD0, count +0x24),
    which the game keeps in order of position: racer 0 is the leader, so a car that passes the leader takes the lead.
    Racer +0x24 is its straight-line distance to the leader in metres (0 for the leader). At 300 m the racer is out:
    state +0x10 = 7, place +0xC = the last free place, finished count +0x28 + 1. When only the leader is left it
    gets place 1 (state 6) and the round ends. A car that drives away but turns back toward the others stays in.
  - The game's 2-player tournament: +10 / +6 into 0x83ABC8 / 0x83ABC9 (every racer after the first goes into the
    second byte), the Tournament Race Results screen (0x4C0050 true for mode 4 + Outrun) with rows from two lobby-style
    records at 0x85036C (stride 0x224, 0x4C07A0), the last dropped racer's player index in 0x83ABCC (0xFF before
    the first round), and no Post Race: the post-race list (0x4F3EA0) leaves it out (`je` at 0x4F40A9 after
    `cmp [0x83AAB4], 8`), so A on the last screen (0x4BFFC4) loads the next round (0x4BFFCF), or goes back to the
    options screen when the tournament is over. The options' Number of Points to Win (race settings 0x83AAB4 +0x108:
    10 / 20 / 30 / 40 / 50 / 100 from the byte table 0x7992CC, 6 = endless) is checked by 0x5027B0: over when exactly
    one player has the most points and reaches it; it also picks the standings' button text (0x4EA659). That check
    reads the 2-player bytes 0x83ABC8..CB (a 6-player round put 5 × 6 points in one byte).
  - Split Outrun takes the race path: `jmp` at 0x4F3F3C (Event Results) and 0x4C00A5 (no tournament results), and
    the `je` at 0x4F40A9 nopped (Post Race), so a round ends like a race: Event Results lists every player by place
    (the time is when each dropped out), the session standings, then Post Race with Next Race, Restart and Quit.
    `OutrunOver` (jump at 0x5027B0, 7 bytes to a trampoline) says over after every split round. The game's points
    bytes are still written but nothing shows them. The options' row builder for Outrun (0x4CDA20) adds Number of
    Points to Win (vtable 0x7914A0) and Time Limit (0x7914E8); `OutrunPointsRowHook` (0x4CDA52) leaves the first
    out in split screen.
  - Time Limit (settings +0x10C, minutes 0 / 2 / 5 / 8 / 10 from 0x7992D4; race time ticks 4000 per second). At the
    limit the game finishes racers 0 and 1 (0x6082D1..0x608A58): more time in the lead wins. Its time in the lead is
    racer [+0] +0x18 -> +0, which it sets for the leader to the race time minus one chaser's (the last one it
    processed), so it holds only for 2 racers; the other racers kept racing and the last of them became a second
    winner. In split Outrun the option's getter (call 0x6082E7, `TimeLimitOption`) answers "no limit", and
    `OutrunTimeLimitFrame` (each frame from `FreeStateRaceHook`, before the outrun logic) counts each player's time as
    racer 0. From the limit on it puts the player with the most time in slot 0 and gives the one with the least a gap
    far past 300 m, one per frame, so the game's own drop-out finishes them from the last place up; the last one
    left wins. Switching the option on mid-round (by poke) crashed the 2-player HUD in its text copy (0x511648); set
    it before the round loads.
  - Grid. Every round's starter (0x526830; a new round skips creating the racers, 0x52695B) ends in a jump to the grid
    order 0x5264A0(1) (0x526965). In split Outrun that swaps players 1 and 2 so that 0x83ABCC's player starts in front
    (a random pick in the first round) and copies the human slots to 0x89E7D0[player]. `OutrunGrid` replaces the
    jump: after the original it gives the humans their slots by the session's last race, fewest points of it in
    front (its places reversed, the game's "loser starts in front" for any count), or in a random order in a new
    session. Racer specs: [0x89E810], count [0x89E814], stride 0x940; +1 player index, +3 grid slot
    (1 = front), +4 kind (1 human, 3 traffic).
- **Camera.** Per-player settings mirror ([players](players.md)); the drift camera works for all players.
- **HUD binding list (4P quit).** The element at hud+0x70 (0x4F89D0) keeps a data-binding list. P4's last node
  sometimes has next == 0. `ClearHudBindings` (call 0x4F8BAE) stops at broken nodes. The root cause is unknown.
