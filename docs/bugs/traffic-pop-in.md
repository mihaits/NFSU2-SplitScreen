# traffic-pop-in
Status:       open, set aside (2026-11-11): not reproduced with automated runs
Report:       user, 2026-11-10: playing with friends, traffic cars spawned and disappeared way too close, at least in
              P1's view, maybe others. Traffic, in plain view straight ahead on a straight road (not around a bend).
              Maybe on a highway track.
Repro:        none yet. Every automated run (AI driving every car, 90 s, 1P, 3P city, 3P and 6P freeway circuit 4085)
              shows traffic parked and spawned at about 250 on straight roads, the same as single player.
Area:         [players.md](../players.md) "Traffic", [rendering.md](../rendering.md) car drawing.

## Known
- How the game removes and adds traffic, and where it hides a car, is in [players.md](../players.md) "Traffic". A
  parked car is not drawn (screenshot: P1's view with a parked car 127-149 ahead on an open freeway, nothing on the
  road), so a car vanishes when it is parked.
- Drawing: the car loop 0x6316D0 skips a car without +0x550, one 0x60D4F0(car, view) rejects, one without the view's
  bit in car +0x5B4 (set to -1, every view, at 0x6042B6), and past 99 cars. The renderer 0x620AE0 then skips a car
  under 4 pixels: 0x48ACB0 gives view +0xC · 3 / (distance - 3), and view +0xC = 256 / tan(FOV/2) from the view's
  camera (0x5C80AD, every frame for every active view). P1 / P2 draw cars to about 270 at the default chase FOV.
- The far plane of P1's view is the same in 3P and 1P (about 10,000-15,000).
- With the AI driving, traffic positions along the route are right (339 car-player pairs within 80 in a straight line,
  none more than 150 apart along the route; +0x8FC changes about every 2 s).

## Ruled out
- Route positions of traffic being wrong: see above.
- Mover +0x77C as a "remove now" flag: the traffic tick sets it whenever it parks a car (0x42FAB0, hardware write watch
  on four cars) and a respawn clears it (0x42A0D1). The reset flag car +0x96C had no writes on four cars in 60 s.
- A view mask hiding P1's cars because of other views: car +0x5B4 is -1.
- Split views drawing farther than single player: same far plane and the same +0xC for P1.
- P3's view +0xC of 256 (cars drawn to 195): P3 was in a bumper camera with a 90° FOV; a wider FOV draws cars smaller,
  so the earlier cull is right.
- Not the user's case, but measured: where the race route bends or passes another part of itself, 300 along the route
  can be 150 straight ahead, so the base game also parks and spawns traffic at 108-244 in a straight line, in single
  player too. A fix (also keep cars and refuse spawn points and cross-traffic emitter nodes within 250 in a straight
  line: 0x409800, 0x410DE0, the calls at 0x4181CD and 0x42E98A) removed those in every run, with the same traffic
  density. Not approved and not in the plugin; the patch is `_splitscreen_mod/private/traffic-straight-line.patch`.

## Leads
None of these can be triggered by the AI drivers or by a known input, so they need a real session:
1. A traffic car someone hit: 0x417BB0 calls a car with mover +0x798 out of range when no local player faster than
   20 is within 50 of it on the route (0x4094A0), so it is parked while players stand next to it.
2. A car reset: the reset paths (0x601300 for racers, 0x5FBED0) were not checked for traffic side effects.
3. Flushes: 0x4099B0 parks every traffic car at once. Called from the traffic tick when traffic +8 is set (0x42F92C)
   and from game event objects with flag +0x62 (0x4302C0, 0x43B930, 0x43BAA0 and two more around 0x43B9EA /
   0x43BB5D). What fires those events is unknown.

## Tools
`tools/traffic_watch.py 0.2 0 --radius 150` during a real race logs, per player and with a timestamp: `hidden` (parked
within range), `pop-in` / `pop-out`, `undrawn` (+0x550 off without a park) and `FLUSH` (3 or more cars parked in one
sample). A pop the user notes with the clock time can then be matched to a line.
