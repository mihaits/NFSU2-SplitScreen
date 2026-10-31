# Free Run (free roam)

Split-screen Free Run drives the whole city on track 4000 with 2-6 players, without outrun challenges and without the
race HUD, from the start the Start row picks (a city district or a closed venue). Race type 9 (0x83AAB4), installed by
`InstallFreeRun`.

- **Mode list.** The quick race mode list (UIQRModeSelect::Setup, 0x4B2E..) adds Outrun in modes 8/0x200/4 and Free
  Run (icon class 0x790FFC) only in the others. The `je` at 0x4B2F99 is NOPed, so Free Run is listed in split screen.
- **Next screen.** After the mode select (0x4B2A80) split screen's Outrun goes to the race options (0x7F4460) and the
  rest to the track list, except Free Run, which goes to its own screen (0x7F446C: straight to the car select, no
  Players row). `NextScreenAfterMode`: split screen + type 9 → the race options screen, track word (0x83AAC4 + 9 ×
  0x14 + 8) = the Start row's track (`SetFreeRunTrack`: 4000, or the venue's) and the direction dword (+0xC) = 0
  (forward). No track list (user).
- **Back.** The race options' previous screen (0x4B2AF0) is the mode select (0x7F445C) for split screen's Outrun, else
  the track list. `PrevScreenRaceOptions` returns the mode select for split screen's Free Run too.
- **Options rows.** Free Run's row builder (0x4B4A10) adds Track Direction (vtable 0x791608, settings +4) when the track
  can be reversed (track info +0x86 == 1, 4000 can), then Traffic. `FreeRunDirectionRowHook` (0x4B4A42) skips the
  direction row in split screen and adds the Start row there instead.
- **Start row.** `kFreeRunStarts`: Airport (4000's own spots), City Core, Beacon Hill, Jackson Heights, Coal Harbor,
  Random (one of these per start, also on a restart), then closed venues on their base tracks (the area without a race
  layout, like 4000 for the city): Speedway Industrial, its drift pad (4708's start) and Speedway Airport on 4700,
  Stadium and Vacant Lot (drift) on 4300. The venues' race tracks (4701, 4311, ...) brought their layout's barriers
  and checkpoints (user). Default: ini `[FREERUN] Start`; the choice is kept while the game runs. Same widget as the
  Players row ([frontend](frontend.md)), text in English.
- **Start spots.** The race start placement 0x6065B0 calls 0x5EB5D0(6, out) at 0x6065C6 (`StartSpots`) during the
  loading screen; the markers give 6 entries {x, y, z +0, heading word +0x10} (angle = h × 2π / 65536, forward =
  (cos, sin)). The streamer then loads around the placed cars, so moved spots come up fully loaded (no preload
  override needed). A start with its own spot skips the marker read and gets the game's grid there (`kStartGrid`, from
  4141's markers: 4 abreast 3.5 m apart, 2 more 5.7 m behind); the base tracks 4700/4300 have no start markers, and
  0x5EB5D0's fallback crashed (null at 0x5DA392). Spots are spot 0 of a race there: 4101 City Core, 4121 Beacon Hill,
  4063 Jackson Heights (up in the hills, city view), 4142 Coal Harbor, 4701, 4708, 4711, 4311, 4301
  (`tools/trackmarkers.py` lists every track's markers). With Debug on, every race logs its spots, and
  `[DEBUG] FreeRunStart = x y z heading` overrides the chosen start's spot (to try a new one).
- **Venue barriers.** Each layout's barriers are a scenery group `BARRIERS_<track>` (list at 0x88F760: node +0x28 name
  hash, +0x2C on, +0x30 count, +0x34 object ids), switched on by the race load 0x578060 (also `PLAYER_BARRIERS_<track>`,
  and `BARRIERS_CAREER<n>` / `FREE_ROAM*` in the free state). The base tracks switch none, so every barrier stays hidden
  (object entry 0x5D6430(id) +4 bit 0x10; instance flags +0x1A, applied by 0x5DB0E0). Their collision is not part of
  the group: every layout's barriers have vertical wall polygons in the region's collision mesh, identical in a race and
  in free roam and without a layout tag (a race just never drives near the other layouts' walls). Hidden barriers were
  invisible walls (user). Collapsing the walls opened areas with no ground collision (cars fell and were reset, user).
  `UpdateVenueBarriers` (per frame, every 0.5 s from the race load on 4700 / 4300) collects the loaded walls that
  collide here: flags 0x16, the infinitely tall invisible walls (the finite 0x12 walls belong to the layouts and don't,
  user). Per venue (user): the stadium works along each wall line: first its hidden barriers that sit on a wall (it
  crosses the box + 0.5 m and passes within 0.6 m of the box centre; not the layouts' 90 m road marking meshes), the
  most shared first, unless barriers already shown on that line (centre within 1.5 m, the game's own visible ones
  included) cover more than 30 % of its extent along the wall (layouts have their own copies of a spot, some parallel
  just off the shared line); then, for gaps where a layout only has angled pieces, the closest remaining piece (centre
  within 2 m, at least 70 % of it along the wall's span, not sticking out past its end) that is at least half
  over the gap. The vacant lot and Speedway Industrial show nothing; the airport speedway (4700, east of x 1000) shows
  no barriers, because its walls stand off them, and `DrawVenueWalls` draws the walls instead: 2 m red and white
  pieces, 1.2 m high from the ground at the wall's ends (`FindWallGround`: the start grid's ground query, 0x5E0B40 /
  0x5E68D0 / 0x5E7E70, cached, within 350 m, at most 600 a pass), fading out from 220 to 300 m. It runs in the split
  pass after the headlight pools (0x5CB2E3), fixed function with the view's view-projection (platInfo +0x80).
- **Collision mesh.** Packs per streamed section at 0x88F770 (0x208 entries; +8 records, +0xC count; cells link cars
  +0x68 and built items +0x60). Record 0x20 bytes: +1 word material, +3 flags (bit 2: wall; 0x16 = the infinitely tall
  invisible walls, z ±2048), x / y int16 / 8 at +8 / +0x10 (4 vertices; a wall is two vertical pairs). Near a car the
  game builds items from the records (0x5E10F0 via 0x5E0C90: +9 vertex count, +0xC record, +0x10 off when != 0, 2D box
  +0x14, vertices +0x30) into the car's cache ([body +0x60] +0x3C, count +0x2E0); 0x5AE930 turns an item the car meets
  into a TrackPolygonCollisionBody (pool [0x86AD68], body list 0x86B4C4, type +0x11 = 3, name +0x30, item +0x60).
- **Free state on other tracks.** Free Run's starter (0x53F580) checks the track (0x53F5A0): 4000 gets race state 0
  (then the free state 7) and 3 in 0x89E7C4, any other track state 1, a normal race (split times, clean section and
  lead lap bonuses on a race track, user). `FreeRunTrackCheckHook` treats every track as 4000 in split screen.
- **Players.** Free Run's starter (0x53F580) sets 1 local player and creates 1 human; `CreateHumanRacers` makes it 2 in
  split screen, then adds players 3..6 as usual ([players](players.md)).
- **Free state.** When driving starts, Free Run switches the race to its free state (0x89E7B0 = 7, the outrun race type; 0x607190 is inside the outrun
  start 0x607180) and sets
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
Game modes and extras to make Free Run more fun (discussed 2026-10-26; the start location is planned
separately):
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
