# drag-heat-bar
Status:       fixed (2026-11-06)
Report:       user, 2026-11-06: in split drag races the engine heat (over-rev) bar is offset and resized for every
              player, relative to the 1P drag tach; it showed up while testing 6P drag. Single player is correct. Every
              player's bar already showed their own heat; only its placement was wrong.
Repro:        any split drag race (2, 3 and 6 players seen, City Drag 4201), SinglePlayerGauges on (default), WidescreenFix
              FixHUD = 1. Hold the throttle in 1st until the bar shows (heat >= ~0.5). Every time.
Area:         [hud.md](../hud.md) (single-player gauges per player).

## Cause
Two layers:
1. The bar has its own HUD element (HUD +0x24, ctor 0x4F69E0, update 0x4C7AA0, bound to `HEAT_Bar`), separate from
   DragRaceTachometer. The per-player 1P gauges didn't include it, so the bar on screen was the 2P package's own
   (`HEAT_Bar` at (-279, -47), 64 x 128), placed by the 2P / quad layout next to a 1P tach it wasn't made for.
2. With the element added to the per-player gauges, each copy's 1P bar was drawn but 109 units too far left (in 6P,
   P2's ended up over P1's view as a diagonal stroke). WidescreenFix has an inline hook in the bar setter 0x4C66F0 at
   0x4C6753 (just before x is read) that moves the bar by its 1P side offset when it sits at its table position
   (`Tachometer_Pos_4` (-254, 171), offset -109 in NFSUnderground2.WidescreenFix.dat). The setter writes x back, so the
   shift lands in the object's data (-254 → -363) on the first update at race start. In single player that matches
   the tach, which WidescreenFix moves by the same offset at render time; the copies' tach is placed by GaugeTransform
   from the 4:3 layout (WidescreenFix never sees its table position), so the bar no longer lined up with it.

## Fix
The heat bar element is one of the per-player gauge kinds. `CreateGauges` records each element object's x as built,
and `RestoreLayoutX` puts it back after the elements' update. The game's elements never move x (the setter passes it
through), so only an outside move is undone; without WidescreenFix nothing changes.
2026-11-08 (code review): the restore went back to the built x, so it would also have undone an x change made outside
the update. Now x is saved right before the update (`SaveLayoutX`) and only the update's own move is undone.

Tested 2026-11-06: 2P, 3P and 6P drag on City Drag (staggered release, every engine blown, `drag_blow.py PERCAR=1`):
each player's bar along the left of their own tach from 0 upward like single player, growing with their own heat;
no 2P bar, no stray bar; results reached. 2P circuit start / restart (`start_matrix.sh 2:0:0`) passed.

## Investigation (2026-11-06)
- Single player, live: bar data x -363, tach group 0x5164D4EA at (-109.2, -25.8). The 1P package data has (-254, 171).
- Setting the copies' x to -254 by hand mid-race placed them exactly like single player, and they stayed there.
- Ruled out: the shift being in the game's setter (Ghidra: x passes through; a sentinel x of -200 stayed -200, only the
  table position moves); an unmapped first draw letting WidescreenFix's transform hook (0x51B190) edit the real data
  (maps built inside `CreateGauges`: no change; our transform hook never saw the real data before the change);
  NFSU2HDContentSupport (it patches the same setter at 0x4C6709, but only forces the bar texture 0x37BD6804's height to
  256; with it disabled the shift was the same; with WidescreenFix disabled there was no shift and 0x4C6753 held the
  original `fst [esi+0x48]`).
- Found with a debug log of the bar x around each element update (the change happened inside the heat update) and a
  diff of the live code against speed2.exe along the setter chain (0x4C6709 and 0x4C6753 patched).
- Hardware write watches only ever showed the setter's own SetPosition write (0x517C10 → 0x5178D0, writer 0x5179BC):
  the hook changes the value in a register before that write.
