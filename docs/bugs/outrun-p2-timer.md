# outrun-p2-timer
Status:       fixed (2026-11-07, 6092936; root cause and final fix 2026-11-08)
Report:       user, 2026-11-07: in a 3-player Outrun, P2 has no Time element; P1 and P3 do.
Repro:        split Outrun with 3-6 players, any track. Every time. Only P2 (P4 and P6 have theirs).
Area:         [hud.md](../hud.md) "P2's left groups".

## Cause
With 3-6 players every player's HUD is a copy of the top 2P package; P2's is `2PHudTop_OUTRU2`. Its
`Timer_Group_01` sat at y 235 instead of the top package's -165: 235 is the bottom package's own spot (low in P2's
2P view, on the minimap). The quadrant layout (`QuadTransform`) parks an object more than 160 from the cell's centre
off-screen, so the timer vanished.

The game moves it: the lap time element's update (0x4AA1B0, called per HUD from 0x4EC618) has an outrun-state path
(ebx = 1 at 0x4AA3AA) that, when the HUD's player index (+0x10) is 1, puts Timer_Group_01 at (-163, 235)
(0x4AA6FA..0x4AA729, 0x542630(package, "Timer_Group_01", pos) -> 0x5178D0) every frame. That is player 2's spot in
the bottom 2P package, and it follows the game's player 2, not the layout (P4 / P6 kept theirs). Found with the
plugin's write watch (mailbox command 5, a hardware breakpoint on the group's data +0x20): two writers per frame,
the game at 0x5179C2 (235, called from 0x5426AF) and, at the time, the plugin's own override (-165).

## Ruled out first
- An FE script: every player's Timer group has the same single script.
- A message forwarded from the bottom package: logging `SendFeMessage` showed no messages to the Outrun copies.
- Plugin code: no plugin path stores 235; the per-view layout only writes transform copies.

## Fix
`OutrunTimerP2Hook` (0x4AA6FE) skips the game's move in split screen, where the plugin places that group itself
(75 in the bottom package for 2P, the top package's own -165 in the top copies). The first fix (2026-11-07, 6092936)
put it back to -165 in the top copies every frame after the game's write; the hook replaces that (2026-11-08).
Checked with the write watch: no writes to P2's group in 3P (-165) or 2P (75) any more. Drift's left groups are not
affected: in a 3P drift race P2 shows all of them like P1 and P3 (user screenshot, 2026-11-07).
