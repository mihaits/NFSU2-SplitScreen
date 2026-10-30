# HUD

**Map mode.** Gameplay > Map mode (0x83AA1B: Off/Static/Rotate, handler 0x49AFE0) only offers Off/Static in split mode
(max = (mode != 4) + 1). With per-player minimaps Rotate works, so `setne cl` at 0x49AFF9 is patched to `mov cl, 1`.

**FE objects.** The relevant FE object fields are:

| offset | field |
|---|---|
| obj+4 | next object |
| obj+0x10 | name hash |
| obj+0x18 | type: 1 image, 2 text, 5 group (children at +0x64, count +0x60) |
| obj+0x1C | flags: bit 0 = **hidden**, 0x400000 = transform changed |
| obj+0x24 | texture key |
| obj+0x2C | data |

Data (0x54 bytes):

| field | meaning |
|---|---|
| [0] | colour |
| [4..6] | pivot |
| [7]/[8] | x/y in a 640x480 space centred at 0 |
| [0xE]/[0xF] | scale |

Other functions and rules:
- Package objects are at package+0x4C.
- 0x50CA00 hides an object and 0x50CA50 shows it. **Both recurse into a group's children.**
- The FE object transform is 0x51B0B0 (thiscall: data, out matrix, out colour, level, scale), hooked by
  `FeObjXformHook`. Group transforms are computed in a pre-pass (0x51B7F0 → 0x51B0B0 at 0x51B818).
- **Transforms are cached until the changed flag is set**, so anything we move is marked changed at every remap.
- The object render 0x5370A0 (stdcall (obj, arg), ECX = render context) is called from a render queue at 0x537262. It's
  wrapped by `RenderFeObject` (naked, preserves ECX) → `RenderFeObjectWith`, which skips objects or sets per-player
  placement. Children are queued and rendered individually.

**2P HUD widening.** WidescreenFix only knows the 1P HUD positions. In split racing, level-0 objects with |x| ≥ 100 and
|y| ≤ 240 get a shifted copy of their data, x ± (240·aspect − 320) (`HudObjectFilter`). This is off while UI_Pause,
UI_PauseOptions, UI_PauseOptionsMain or UI_PC_Customize_Options is active (`g_hudWiden`).

**HUD packages.**
- The player ctor 0x5F1670 picks 2PHudTop/2PHudBot (+_DRAG/_DRIFT/_OUTRUN) by index when NUM_PLAYERS == 2. The HUD
  ctor 0x4F8BF0(fng, index) (call 0x5F1A0C, `PlayerHudCtor`) builds the same element classes for every package. The
  elements bind to the package's objects by name and read player data by index.
- **Package copies** (`CloneFePackage`): players 3..6 (and in quadrants / the grid P2/P4/P6, which use a copy of the
  *Top* variant) get private copies registered under a name of the same length ("2PHudTop" → "2PHudTo3").
- Minimap: `MinimapUpdate` only centres on players with a car entity (centring on a player without one crashed at
  0x4AC2B3).
- How a copy is made: the cache entry's chunk is either JDLZ-compressed (0x30210; block at hdr+0xC, uncompressed size at
  +8) or stored (0x30203). The copy is always a **stored** chunk: compressed ones are decompressed with
  0x574670(jdlz, out). The name is patched only in the FNG header: the name at +0x28 (the cache finds packages by it)
  and the path. Then an entry is built with the game's ctor 0x51B8E0 (entry size 0x54) and linked into the cache.
- Do **not** patch inside the compressed stream: back-references copy the patched byte into later strings. For
  example, HUD_Drag's "Drag_*.tga" textures became "Dra1_*.tga" and went invisible.
- `SendFeMessage` (trampoline on 0x55DDA0) forwards messages sent to a source package to its copies.
- Package lookup is by name with 0x51BB70 (cache; name) → entry, active package at +0x18.

**Quad HUD (3-4 players, and 2P when `QuadHud2P`).** A half of a screen narrower than 4:3 can't hold the 640-wide 2P
layout, and left / right views (`VerticalSplit`) aren't halves of it, so 2P there uses this HUD too (P2 gets a copy of
the Top variant). In a left / right view the layout goes to the top of the view instead of its centre. `UpdateQuadHud` maps every object (and its data pointer) of each player's package to that
player, rebuilt every 30 frames. `QuadTransform` scales the top-half 2P layout uniformly about its centre (0,-120) into the
player's cell (`LayoutFeRect`), centred on it, by min(cell width / 640, cell height / 240) (= halfW/640 in quadrants). Objects pushed more than 160 from the centre (icon pools
parked off-screen) are moved 2000 further out. Pivots are scaled too.

**Minimaps.**
- The minimap is objects of P1's 2PHudTop around (-230, 5): Track2000_map, Minimap_Mask, Minimap_North_indicator
  (pivot (0,72,0)), TrackMapTargetRing (a group of 4), and markers.
- One manager drives it, 0x8364D4 (ctor 0x4EC950(fng)). Its update 0x4CA840 (called at 0x4DA650) centres on car entity
  (type 4, id 0). A type-4 entity's id indexes the local players.
- Markers: 0x4DB900(fng) scans a package for 17 name patterns × 8 (table 0x7F6828 {fmt, type}) into the descriptor pool
  0x836560. The pool is raised from 0x66 to 0x7F.
- `ExtendMinimapManager` adds P3..P6's car markers (PlayerCarIndicator_1/_2 of P3's copy for P3/P4 and of P5's copy
  for P5/P6, recoloured: P3 blue, P4 yellow, P5 purple, P6 cyan). The other packages' own minimap objects are hidden
  (`MapMinimapObjects`). 6P fits the 0x7F descriptor pool.
- `MinimapUpdate` (call 0x4DA650) runs the update once per player, centred on that player (the id imm8 at 0x4CA901).
  After each run it snapshots the minimap objects' data, **keyed by data pointer**. Lists are rebuilt as markers come
  and go.
- `RenderFeObjectWith` draws each minimap object once per player from that player's snapshot (`MinimapPlace`:
  bottom-left of the view's cell, ×0.55 × `HudCellScale`; 2P ×0.8). The car markers are drawn together, with the own marker last. Markers that aren't
  on that player's map are skipped. The ring group's transform is forced to the origin, and its children are placed
  per player.
- **2P**: the HUD packages keep their own layout (no quad HUD mapping). `UpdateMinimaps2P` maps only P1's minimap
  objects (on a new package and every 30 frames) and sets `g_mm2p`. It goes by the race's players (no extra players),
  not `g_screenViews`: that is still 2 in a 3+ player race's first frame, and the minimap update then looked for P3's
  car entity before it existed (crash 0x4AC2B3). Then `MinimapUpdate` runs per player, the render
  hook picks the minimap objects out without a render slot, and `HudObjectFilter` sends only them through
  `QuadTransform` → `MinimapPlace` (bottom-left of each half). The game's own map in the middle of the left edge is gone.
- `DestroyMinimapManager` (call 0x4FBFF2) unlinks P3/P4's entities first. They belong to P3's package, which is freed
  earlier, and without this quitting crashed. The pause menu's choice 4 with race state 0x89E7B0 != 0 (0x606B32 ->
  0x5EA980 -> 0x4F92A0, e.g. in Free Run) rebuilds the manager; its destroy call 0x4F92B1 goes through
  `DestroyMinimapManager` too, so `g_mmExtendedMgr` is reset and the new manager is extended even at the freed
  address (the game's allocator usually returns a new one: tested by calling 0x4F92A0 in a 3P Free Run).
- **When P1 finishes** (or pauses) the game hides P1's HUD, and with it the minimap objects every player's map is drawn
  from. `UpdateMinimapOwner` (per frame, after the quad HUD / 2P minimap mapping) keeps the objects that were visible just before
  visible for the players still racing; a finished player's own map is skipped (`g_mmFinished[k]`, player
  +0x50; racer progress +0x14 counts laps). Only a hide with P1 finished and no menu over the HUD counts; during a pause the kept objects are hidden
  too, and so they are once every local player has finished (the results screen). The object list is rebuilt from P1's **live** package every frame: a restart rebuilds the HUD's objects,
  sometimes at the same addresses, and a cached list crashed. `g_hudPackages` is cleared outside split racing and in
  `CreatePlayers`.

**Free Run HUD.** Free Run has no race position or race time. `HudUpdateHook` hides, per update in split Free Run, the
position element's objects (HUD +0x20, ctor 0x4F4AC0: RacePositionNumber +0x28, RacePositionNth +0x2C, NumberRacers
+0x30, PositionBacking +0x3C) and, by name in the HUD's package, CurrentLapTime, CurrentLapTimeText and
LapCounterBacking_2/3. The outrun element (HUD +0x6C, ctor 0x4F87E0) binds objects only the outrun packages have; with
the split race HUDs they are null, so `OutrunElementUpdate` (call 0x4EC69A) skips it then. See [free-run](free-run.md).

**Single-player gauges per player (`CreateGauges`).** Built right after each player's HUD.
- Elements, all thiscall (fng, index); each is updated with (player):

| element | ctor | size | update |
|---|---|---|---|
| Tachometer (or, in drag, DragRaceTachometer) | 0x4F5100 (drag 0x4F6500) | 0xC8 (0xA8) | 0x4C68D0 (0x4C7090) |
| TurboMeter | 0x4F5A70 | 0x38 | 0x4C6B80 |
| NitrousGauge | 0x4F6A90 | 0x38 | 0x4C7B20 |

- They're bound to a copy of `HUD_SingleRace` / `HUD_Drag` / `HUD_Drift` ("HUD_SingleRac1.fng", "HUD_Dra2.fng"...),
  pushed with 0x555F10(fng, 0, 0, 0). The copy is removed with 0x5379A0 when the player's HUD is destroyed
  (`HudDtorHook`, call 0x4FD963). Until the frontend drops the package, all its objects stay hidden.
- Ctor, state and update run with NUM_PLAYERS temporarily 1, which gives the classes their 1P look and behaviour.
- `HudUpdateHook` (call 0x6005DB) wraps the HUD update 0x4EC5A0:
  - The 2P package's own gauge elements (hud +0x18 / +0x1C / +0x28 / +0x34) are swapped out during the update. Both
    sets updating the same player fought over shared state.
  - Our elements get SetState 0x4C6690(hud+8 & 0xFDFBFFFF, hud+0xC) whenever the HUD state changes, then their update.
- Element object list: head at element+0, node+8 = object.
- Shown: our elements' objects and their subtrees, plus groups that hold them. Only the group's own hidden bit is
  cleared: the recursive show re-showed the elements' hidden objects (blinking shift light, stuck N2O overlay).
- Hidden: everything else in the copies, and the 2P package's gauge objects. They're skipped at render time, as is
  every gauge object during the intro and the pause menus.
- `GaugeTransform`:
  - Normal races: the 1P layout's bottom-right corner goes on the view's (cell's) bottom-right corner, scaled by
    GaugeScalePercent.
  - Drag: the whole layout is fitted to the view (min(view width / 640, view height / 480), centred), and side elements stay at the view sides. The top-right
    speed moves below the 2P position box.
  - Both are also scaled by `HudCellScale(k)` (as are the minimap and its corner offsets): the cell's 640x240 layout
    fit relative to quadrants, capped at view height / 240. 1 up to 4 players, about 0.67 in the narrower 5/6P cells.
- Messages to a player's 2P HUD package are forwarded to their copy (drag shift messages).
- Custom gauge textures (`HudTexLoadHook`):
  - The race load step 0x577F60 (call 0x57F3D8) loads `GLOBAL\HUD_CustomTextures_<HUDINDEX>.bin` for P1's car only and
    makes its 8 textures resident. The names come from 0x5ED9B0(spec, car parts, units, out, index), and the index is
    attribute "HUDINDEX" of car part 0xA3, read with 0x61B500(key, 0).
  - Other players' packs are loaded the same way, after the game's load: 0x4944D0(path, 0, 0, 0) and wait
    0x490170(0), once per index. Each player then gets their own list (0x5ED9B0 picks the textures by car too: two
    stock cars share index 0 but not their tach textures) made resident with 0x494D60(list, 8, cb, ctx, 0). Done
    before the game's load, a player sharing P1's index but not P1's car asked for its textures before P1's pack was
    loaded: its tach had no numbers or dots (user, P3).
  - The race load only waits for P1's request (its callback 0x571150 starts the next load step); the others' callback
    just logs "custom HUD textures resident for player N". Measured (3P drift, P3 with its own pack): resident 47 ms
    after the request, 0.3 s before CreatePlayers. The later load steps wait for more reads queued after them.
  - Unload (call 0x5816D4): every player's list is released first (0x490150(list, 8); a pack can be P1's), then the
    game's unload 0x580FC0, then the plugin's pack files (0x494550(path, 0, 0)).
  - In drag, the load runs with NUM_PLAYERS = 1, which selects the drag texture set.
- The 1P drag package starts with its gauge groups hidden. The game shows 1P packages with 0x52CFC0, but only outside
  drag.

**Live gap.** The gap text element (0x4DA414) computes slot0 vs slot1 and shows 0 for everyone else.
- Drift branch → `DriftGapHook` (0x4DA445).
- Others → `RaceGapHook` (0x4DA4DC → 0x4DA526, with 0x5F82D0([0x890118]; &out, other, self)).
- Both take the minimum over the other local players: leading = + gap to the next car, otherwise − gap to the leader.
