# Track streaming and memory

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
- **Post-race.** Position blocks are at streamer 0x88EB00 + 0xB0 + k × 0x5C; block +0 is the car array index it
  follows (0x5E4F20 copies the car's +0x60 position and +0x70 velocity). The post-race scene drives cars it doesn't
  feature to the world origin, P1's among them with 3-4 players, so position 0 moved across the map and the finish
  area was dropped; nothing reloads on the post-race screen (black world). After the results (`g_postRace`, set in
  `RaceResults`, cleared in `CreatePlayers`), `FollowCameraForSceneCars` (in `StreamZonesHook`) points every block
  whose car is scene-driven (mover 5) at the view 1 camera. Section byte +9 is not a load state.
