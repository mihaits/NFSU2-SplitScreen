# Rendering: views, layout, per-view passes

## Rendering core

**Views.** `eView[22]` at 0x832DE0, stride 0x70, `GetView(id)` = 0x48B1E0.
- Fields: +8 active; +0x40 camera; +0x58 render target struct; +0x60 cull/effect object (0x3B0 bytes, ctor 0x5E0260);
  +0x64 render/weather object (0x51C0 bytes, ctor 0x61E150(view, 0); then +0x509C = 1.0 and 0x613050(5, 1.0f)).
- Ids: 0 FE, 1/2 players, 3 mirror, 4 road reflection (320x240), 9 headlight projector, 0x10..0x15 car env map faces.
- Player views: `kPlayerViewId = {1, 2, 5, 6, 7, 8}` (`MAX_LOCAL_PLAYERS` 6; views 7/8 are not drawn yet).
  `InitPlayerView` sets up views 5/6 like 0x5C2F90 does for 1..4. It
  memsets them to 0 first: game malloc isn't zeroed, and the ctors assume zeroed memory.

**Render target structs** (0x1C bytes): [0] type, [1] owner view, [2] surface, [3] depth, [4] enabled, [5] width,
[6] height. The backbuffer RT 0x86E768 is shared by view 1 and the HUD.

**Layout.** 0x5C2120 computes layout 3 for split, but the PC per-frame apply 0x5C21E0 has no case for it.
`ApplySplitLayout` (each frame while SplitRacing):
- `g_screenViews` = 2 / 3 / 4.
- `LayoutCell(n, k, w, h)` (core.cpp) is the only place that knows the layout: view k's pixel rect with n views.
  2P = top/bottom halves; 4P = quadrants; 3P = P1/P2 on top, P3 centred at the bottom (x = w/4); 5P = 3 cells
  (w/3 x h/2) on top, 2 centred below (x + w/6); 6P = 3 x 2 grid (5P/6P not reachable yet). `LayoutFeRect` gives the
  same rect in FE units for the HUD.
- Each player view gets a private copy of the backbuffer RT (`g_rt[k]`, the cell's size) and a region
  (`g_regionX/Y`, the cell's corner).
- `g_rt` is refreshed every frame and before each player view (`RefreshViewTarget`), because a device reset
  (alt-tab) releases the surfaces.

**Player view loop.** In the main render 0x5CAAD0, the loop counter (from 1) ends at `cmp eax,1` (imm at 0x5CB722 →
`MAX_LOCAL_PLAYERS`). `ViewLoopTopHook` (0x5CB000) maps the counter to kPlayerViewId, skips inactive views and sets `g_currentView`.
`ViewLoopExitHook` (0x5CB72D) runs after the loop (restores, clears unused areas).

**Viewports.** Every game SetViewport uses X=Y=0 and the RT size. The device vtable hooks are:
- `SetViewportHook` (slot 47): offsets by the current view's region when the target is the backbuffer, and clamps
  full-screen viewports inside a player view to its region.
- `SetRenderTargetHook`: SetRenderTarget silently resets the viewport and the scissor rect. The hook restores the
  view's region, and re-applies the overbright scissor.

**World view registration.** 0x5B9C20 registers the per-frame world views (entries 0x90 bytes, max 12; count +0x6C0):
views 1..3, env 0x10..0x15, 4 and 9, via 0x4888A0(view, entry, flags). The table lives on the main render's stack
(0x5CAAD0: `sub esp, 0x734`, table at esp+0x6C; cull cursor/limit +0x6C8/+0x6CC right after entry 11; 0x5E9A30 culls
every entry into the shared item buffer 0x876DE0..0x8832C0, 50.4 KB). `WorldViewsHook` (0x5B9CAA) adds one entry for
the extra player views, view 5's (the mirror is off in split, so it fits), and captures the table (`g_worldViews`).
- Views 6..8 share it (`SharedWorldEntryFor`, from `RefreshViewTarget`): before each of them the entry is pointed at
  that view, re-culled with `RecullWorldEntry` into the free buffer space, and the cursor is moved past it so the
  headlight re-cull doesn't overwrite it. The cursor goes back at the next player view. 11 entries at 4P-6P; the
  buffer holds 6.5-8.4 KB (a player view culls 1.5-2.2 KB). With Debug = 1 the table is logged every 600 frames
  ("world views: ...").
- Debug views: `[DEBUG] CloneViews = 1/2` adds 1-2 views after the players' (5/6 view layouts) that show P1's /
  P2's camera (`ViewPlayer(k)` maps them to P1/P2 for the per-view player lookups: headlights, rain car, nitrous
  effect, road reflection car, env map car). 4P + 2 clone views: 75 fps (display cap), 753 MB virtual.

**Sky.** 0x60F880 (called at 0x5CB227) only draws for ids 1..3 and the env faces. `DrawSky` passes views 5/6 off as 1.

**Projection.** 0x5C7EC0 (thiscall platInfo; view, ...) builds P (platInfo +0x40 P00, +0x54 P11) from the RT's
width/height, then view-proj and the frustum. `ProjectionHook` at 0x5C80B0 (after P, before view-proj) corrects the
aspect and FOV for the view's region (`AdjustProjection`; camera FOV ushort at camera+0xC4). View 4 (road reflection)
uses the reflecting player's shape. WidescreenFix hooks 0x5C7F50 for view 1 and patches constants at 0x5C7FEA..0x5C8021.

**3P (5P) side bars.** `ClearUnusedAreas` (after the view loop) ColorFills the two bars left and right of the short
bottom row (from `LayoutCell`) in the backbuffer. With
overbright on, it also fills them in the streak buffer.

## Per-view render passes

The game ran many passes for "view 1" or "player 1" once per frame. The general fix: run them per player view, or
redirect their view or player choice to the view currently being drawn (`g_currentView` / the view's player).

**Headlights.**
- World headlights are one projector (view 9, camera 0x870A20). It's aimed once per frame by 0x5C2AE0(view 9,
  human 0) and read by the world shaders through 0x5BDAA0(…, GetView(9)).
- `HeadlightsForPlayer(k)` (called from `RefreshViewTarget`) re-aims it at player k's car for each view.
- `RecullWorldEntry` redoes view 9's entry of the world view table per player: frustum 0x5C26E0, cull 0x5E9A30 /
  0x5E99C0 into the space after the cursor (world +0x6C8, limit +0x6CC) without advancing it. This is needed because
  the projector's geometry was culled from P1's position. It sets the entry's fields as pass 1 does (0x5E9A70..):
  +0x60 camera position (camera +0x40), +0x70 direction (camera +0x50), +0x80 view +0xC, +0x84 scenery section.
  The direction used to go to +0x6C, one float off: views 6..8 (P4-P6) then culled with a direction mixed from
  view 5's camera and drew scenery only close to the camera.
- **Projectors for every player** (`WorldDrawWithBeams`, call 0x5D6AD0 at 0x5CB23B). The headlight pass
  (0x5D6D20(world; view 9, 0x488B90) at 0x5CB218) only flags the meshes in view 9's culled list (+8 → +0x1B |= 0x80).
  The world draw turns the flag into render flag 0x200 (0x5D6B85), which picks the material's headlight shader
  variant (0x5C5953: worldheadlight.efx etc.), and clears the flag. The world draw only queues meshes; 0x5CF420 draws
  the queue and sets the projector params per draw (0x5BDAA0, colour from 0x60F510 at 0x5BDC20). So one projector
  per draw. After the view's world draw the plugin draws the queue, then for each other player with lights on
  (within 250 m) aims view 9 at that car and draws its flagged meshes twice: with colour 0 subtracted (blend op
  REVSUBTRACT), then with the beam added (ADD). The difference is mesh × beam; base lighting and fog cancel. The
  world draw's include mask (imm at 0x5D6B0F, normally 0x2000 for flags & 0x10) is 0x200 meanwhile, so only flagged
  meshes are queued. A SetRenderState hook forces no depth writes, LESSEQUAL with a small bias and ONE/ONE blending.
  A DrawIndexedPrimitive hook skips materials that asked for alpha blending: semi-transparent pixels don't cancel
  (the subtracted copy clamps at black). The same runs in the full screen race intro (view 1); the device hooks
  are installed on first use because the first intro comes before any split frame.
- Light pools on the road (0x615490(view), "2PLAYERHEADLIGHT1" decal) are for traffic only: player cars have their
  lights flag (+0x550) cleared during the call (`PoolsForTraffic`). The split path calls it after 0x566400
  (`SplitPassWithHeadlightPools`, 0x5CB2E3); the 1P path (intro) at 0x5CAC07.

**Frame scratch.** Car matrices and world draw data come from a per-frame scratch allocator: two malloc'd
0x64000-byte (400 KB) buffers (0x48CD60), swapped every frame by 0x488800 (cursor 0x82DA5C, limit 0x82DA60). A full
buffer returns null (overflow flag 0x82DA6C, bytes 0x82DA70, worst 0x82DA74) and that car or mesh is silently skipped
(0x620D0C.. in the car renderer 0x620AE0). 6 views needed ~470-550 KB: views 7/8, drawn last, lost every car and
most of the world. `EnlargeFrameScratch` raises the four size immediates to 1 MB; the debug "world views" line
prints the last frame's use and the worst overflow (6P drift: 549 KB, no overflow).

**Car skins.** A car's paint and vinyls are composited into the texture DUMMY_SKIN<n> named by its ride info
(+0x584, set by 0x61C280(ride; n), composited by 0x61D790). The garage entries choose n from the player index
(0x503A80: index + 1; 0x503880: 1 or 2), so players 3/4, built with index 0/1, used P1's / P2's skin texture and
rendered with mismatched textures (e.g. a door card texture on the hood). `RacerSpecIndexShift` calls `ExtraRacerSkin` before the racer spec copies the ride info:
DUMMY_SKIN3/4, the single-player opponents' slots (split races have no AI). Race type 9 sets skin 1 for every human
(0x526007) and is left alone.

**Start zoom.** After the race intro the chase camera eases out from close to the car: CubicCameraMover's offset
spring (+0x38: +0 distance, +8 its target, +0x60 height, +0x68 its target, the rest rates). Only player 1's camera
is created then (view 1 showed the intro); the others exist from the race load and finished the ease-out unseen.
`UpdateStartZoom`: on the first split frame, once player 1's spring starts, the other players' springs are set to
the same fractions of their targets. Movers are found through a hook on the vtable's Update (0x787F2C → 0x453570).

**Car detail.** With the split layout (0x5B98A0 == 3) the car renderers 0x620AE0 and 0x6162E0 multiplied the car's
screen size by 0.7 (0x7A6700) and forced its LOD range to 1..1, the low model, ignoring Car Geometry Detail (0x870CF8
-> 0x8A1C10/14). The car parts renderer 0x615630 scaled the size too. The `jne` past each block (0x620C8C, 0x616316,
0x6156CC) is patched to `jmp`. Not touched: 0x61F332 scales a car value at +0x5094 by 1/8 in split (purpose unknown).

**Car light glares.** 0x5D6570 made glares only for views 1/2 (GetView(2) check at 0x5D683C, mask from GetView(1) at
0x5D686F). `GlareView` counts views 5/6 as view 1, reading the rendering view from the model renderer's context
([ebp+0xC]+0x40). The slot counters (0x82DAE8[2], capacity 0x7F3D44) are reset at each player view. The screen tint
update 0x5D77F0 indexes 2-entry tables (0x88EA08 / 0x88F210) by view id - 1, so `TintIndexHook` (0x5D7831) and
`TintUpdateWrapper` give views 5..8 private entries (views 7/8 read slots 0/1 like 5/6).

**Per-view world update.** 0x5DBE80 (from 0x5CBADC / 0x5D2C21) looped over views 1..2, covering visibility, emitter
lists and the per-view nitrous effect flag. `PatchViewUpdateLoop` (installed after the player relocation) makes it
loop n = 1..`MAX_LOCAL_PLAYERS` using kPlayerViewId[n-1] / inOrder[n-1].

**Particles.** The emitter update 0x491E50 spawns only within range of the particle system's 3 camera snapshots
(0x48BD90 copies views 1, 2, 0). `EmitterRangeHook` (0x491E8F) also tests the cameras of views 5/6. Per-car effects
also needed the nearest-racer fix ([players](players.md)).

**Road reflections.**
- View 4 renders P1's camera into a 320x240 texture (0x870894; surface 0x870890 = view 4's RT; blur 0x5C7E40 through
  0x87089C/0x870898). Road materials bind [0x870894] when they draw (0x5BC7D1) and build REFLECTEDPROJ from the
  drawing view's own matrix.
- Players 2..4 get their own texture, rendered right after P1's (`ReflectionsDoneHook` at 0x5CACCC, before the env
  map) with their camera, view update and recull, plus the block's glow pass. [0x870894] is swapped at the top of each
  player view (`UseReflectionOf`) and restored after the loop.
- This has to happen **before** the view loop: rendering the reflections inside the loop left render state that broke
  the following view.
- 0x631250 (the car's own reflection) picked local player 0 for view 4, else 1; `ReflectCarPickHook` (0x631289) picks
  the reflecting player.

**Car reflections (env map).**
- The cube RT is 0x870858 (size 0x7FEE6C, faces 0x87085C[6]), rendered by views 0x10..0x15 from cameras 0x871BC0.
  Those cameras are placed by 0x61AB50 at human 0's car, and oriented by 0x5C3450 using **GetView(1)'s camera**: the
  cube is in camera space, and cars sample it with a camera-space reflection vector.
- Players 2..4 each have their own cube, and one of them is rendered per frame in turn (`EnvMapsForPlayers` at
  0x5CAE12). During that, view 1's camera is temporarily that player's, and the faces go through view update, recull,
  RT begin, sky without Z and the world (if 0x870CBC).
- Before each player view, its faces are StretchRect'ed into the game's cube (`UseEnvMapOf`).
- These are D3DPOOL_DEFAULT textures and cubes; they are released in a hook on 0x5BA610 (device lost,
  `ReleaseTargetsHook`) and recreated on demand.
- Cost per frame: (players - 1) road reflections + 6 env faces.

**Weather.**
- Every view has its own rain object (view +0x64), but the frame only used view 1's, full screen.
- Falling rain and the drop update 0x61F120 (call 0x5CB46B) → `RainForViews`: once per view, viewport = its region
  (the previous viewport is restored afterwards; later passes depend on it).
- Lens droplets 0x5C7AB0 (call 0x5CB9A6) → `RainDropsForViews`; its four 0x48B1E0(1) calls → `RainDropsView`; the
  drop getter 0x6142C0 (call 0x5C7C52) → `RainDropGet` remaps x/y into the region.
- The weather caches (0x8A3180 / 0x8A1E90 / 0x8A31F0 / 0x8A3250) are [2] arrays indexed by view id - 1, so views 5/6
  (and 7/8) use slots 0/1 (`RainRegionSlot` 0x6136C3, `SplashRegionSlot` 0x61437E). Without that, the race start crashed.
- The rain emitter frame (0x61E700) picked human slot 0 for view 1, else slot 1. `RainCarHook` (0x61E724) maps the view
  to its player's racer.

**Speed blur.** 0x5CC460 (per player view, call at 0x5CB31F) used view 1's camera (calls at 0x5CC474/0x5CC490), one
smoothed strength (0x870900) and a full-backbuffer StretchRect (0x5CC760).
- `BlurView`: the current view's camera.
- `MotionBlurForView`: a per-view strength.
- `BlurCopy`: copies only the region, then puts the full-screen copy back, because 0x86EB0C[2] is also the frame-end
  post chain's screen copy.
- The blur composite (0x5C6BE0) rebinds the backbuffer, which `SetRenderTargetHook` handles.
- The blur starts at 25 m/s camera speed (thresholds 0x7FF7B0/B4).

**Light streaks and overbright.**
- Overbright needs streaks (0x870CE0) and glow (0x870CE4); its flag is 0x8707CE. It accumulates in the streak buffer
  0x86FF70 / tex 0x86FF74, which is **screen sized** (the 960x540 buffers are the blur targets).
- The streak pass 0x5CBB00 (in the view loop) fades the buffer (0x5C6BE0, blend factor) and adds the view's glows. It
  hard-coded view 1 (`push 1; call 0x48B1E0` at 0x5CBC00 → `StreakView`).
- `StreaksAllowed` / `StreaksBranchHook` (0x5CB577): split views run the pass, and every viewport on the streak
  surface inside a view is that view's region (`StreakRegionViewport`; the blits are clip-space quads).
- With FSAA (0x870D08 > 0), the pass renders through the full-screen copy 0x86F2E4 / 0x86F2E0. That surface gets the
  same regions (`IsStreakSurface`), except for its copy of the whole previous image (0x5C7120 at 0x5CBBB0,
  `StreakAACopy`).
- Without overbright, the pass ends with its own composite of the buffer onto the view (0x5C6BE0 at 0x5CBDA6), a
  full-clip-space quad with UVs covering the whole buffer, which would squeeze every player's part into the view.
  `StreakComposite` marks the player view while it runs, and `BlitQuadReadyHook` (call 0x5BCBC0 at 0x5C6EC6, after
  the blit has built its quad on its stack: 4 vertices of 0x30 bytes at esp+0x30, position + 4 UV pairs) remaps the
  UVs to the view's part of the buffer (`RemapStreakQuad`).

**Overbright intensity per view.**
- 0x5BC180 (once per frame, call at 0x5D2B5A) reads view 1's effect object (+0x60: +0x3C nitrous on, +0x1E0
  intensity). It sets the accumulation amount 0x7FF114 (blend factor) and the bright-pass threshold 0x7FF10C.
- `OverbrightAmounts` computes both per view.
- `OverbrightAccumulate` (0x5CCBA5) runs the accumulation blit once per view with its own blend factor and a scissor =
  its region.
- `BrightPassPerView` (0x5CCA25 / 0x5CCA40 / 0x5CCA61) does the same for the bright-pass blits.
- The nitrous channel handler 0x5D7770 picked the car by view id (1 → inOrder[0], else inOrder[1]).
  `NosFxPlayerHook` (0x5D778D, 13 bytes, installed after the relocation, which rewrites a disp32 inside it) maps the
  view to its own player.
