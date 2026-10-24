# Race flow: intro, results, standings

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
  - The scene binder 0x439240 (run at the scene's start, and again on restarts, which skip the scene) teleports each
    bound car (0x4309D0 → 0x5FBD60) to its track's spot, a 2x2 formation, and records it in the binding table
    0x827B98 + track × 0x14 (+0 track, +4 x, +8 y, +0x10 car array index, -1 = none). On a track whose grid is 4
    abreast (drift 4311) the formation's second row is grid spots 5/6. `NisBindCars` → `MoveExtraCarsBehindScene`:
    an extra car that overlaps a bound car goes one row behind the formation.
  - The scene's cars drive through the extra cars' spots. `CarNotVisible` (trampoline on the per-view car check
    0x60D4F0: body 0x6317A2, lights 0x60D594, headlight pools 0x6154D3) hides every car the scene doesn't drive
    (mover car +0x28 != 5) until the scene releases its cars.
- **Post-race scene (outro).** The picker is the intro's (0x436840, branch 1 = outros EndingNis01-04; random 0x43C1C0
  at 0x436BC5; with `Debug = 1` picks are logged and `[DEBUG] ForceOutro` forces one, see [tooling](tooling.md)). It binds its car tracks to race cars by car array index (table 0x827B9C, +0xC car index; binder
  0x4309D0 sets mover mode 5 = NISMvr) and drives the cars it doesn't feature to the world origin (body target via
  0x5A44A0 from NISMvr::Update 0x5AB3C0). With 3-4 players these can be human cars:
  - The fall-through check 0x5ECBA0 (below ground → car +0x96C = 1) reset the car onto the road every ~0.5 s (0x6009A0
    via 0x601324), each reset switching its mover 5 → 0 → 5: a flash and an audio duck. `FallResetHook` skips the
    reset while the mover is 5.
  - The held car fell forever, and its wind sound played to its player's listener. `HoldSceneParkedCars` (from
    `EndResultsCount`) holds it under the map: body = [[car+0x34] + 0x20], z +0x28 (and +0x108) = -50, vz +0xA8 = 0.
  - The streamer followed such a car across the map, see [streaming](streaming.md).
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
  - `UncountResultsRacers` only uncounts while 0x89E7C8 still holds the value set for the results: a new race's setup
    writes it afresh, and uncounting that went to -1 (crash in the drift scoring 0x56DE6E). It also runs in
    `CreatePlayers` (restart).
  - `CapAiCountToCars`: one race ended with an AI spec that never got a car (cause unknown, not reproduced), and the
    results rows read past the list (crash 0x4BF2D2). `RaceResults` caps the AI counter at the AI race cars that exist.
- **"Hurry!" timer.** When the first racer finishes (0x606FD1), the 30 s race end countdown starts for an AI finisher
  or a human with exactly 2 local players ([0x8900BC]). `InstallHurry`: the `jne` at 0x606FE8 → `jl`, so 2 or more.
- **Standings.** 0x56D850 (calls at 0x5F7B3F, 0x5F94C7, 0x6052F8) → `BuildStandings`, which counts the extra racers for
  the call. Drift scoring needs it.
- **Camera.** Per-player settings mirror ([players](players.md)); the drift camera works for all players.
- **HUD binding list (4P quit).** The element at hud+0x70 (0x4F89D0) keeps a data-binding list. P4's last node
  sometimes has next == 0. `ClearHudBindings` (call 0x4F8BAE) stops at broken nodes. The root cause is unknown.
