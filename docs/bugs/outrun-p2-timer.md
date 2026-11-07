# outrun-p2-timer
Status:       fixed (2026-11-07, 6092936)
Report:       user, 2026-11-07: in a 3-player Outrun, P2 has no Time element; P1 and P3 do.
Repro:        split Outrun with 3-6 players, any track. Every time. Only P2 (P4 and P6 have theirs).
Area:         [hud.md](../hud.md) "P2's left groups".

## Cause
With 3-6 players every player's HUD is a copy of the top 2P package; P2's is `2PHudTop_OUTRU2`. Its
`Timer_Group_01` sat at y 235 instead of the top package's -165: 235 is the bottom package's own spot (low in P2's
2P view, on the minimap). The quadrant layout (`QuadTransform`) parks an object more than 160 from the cell's centre
off-screen, so the timer vanished.

Something in the game writes 235 to player 2's group every frame: reset to -165 by poke, it was back within 10 ms. It
follows the game's player 2, not the layout: in a 4-player Outrun P4's top copy kept -165.

## Ruled out
- An FE script: every player's Timer group has the same single script.
- A message forwarded from the bottom package: logging `SendFeMessage` showed no messages to the Outrun copies.
- Plugin code: no plugin path stores 235; the per-view layout only writes transform copies.
The writing instruction itself was not found (it needs a write watch on the object's data +0x20).

## Fix
`HudUpdateHook` (gauges.cpp) already overrides the bottom package's timer every frame for 2P (y 75). The same rule now
puts the timer back at -165 in the top copies (`2PHudTop_OUTRU*`), a no-op for every player but P2. Checked by
screenshot in 3P: P2's timer shows next to P1's and P3's. Drift's left groups (best, multiplier, total), which the same
rule moves in 2P's bottom package, are not affected: in a 3P drift race P2 shows all of them like P1 and P3 (user
screenshot, 2026-11-07).
