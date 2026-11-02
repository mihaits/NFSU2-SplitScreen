# Players 5/6 snapped back after the start, and scene cars off the grid
Status:       fixed 2026-11-02 (`PutCarsOnGrid`, `MoveExtraCarsBehindScene` only while the scene drives cars)
Report:       6P on virtual pads, Street X: P5 and P6 teleported a few metres shortly after the start.
Repro:        `ExtraPlayers = 4`, `sh tools/go_race_nointro.sh 4`, then `python -I tools/autorace.py` from the go signal.
              Cars that stay parked don't show it, nor do cars released about 50 s after the go signal.
Area:         race-flow.md (extra cars during a 4-car scene)

## Cause
The scene binder 0x439240 runs every frame while the scene object is active, at least 3 s into the race, not only at
the scene's start. `NisBindCars` ran `MoveExtraCarsBehindScene` each time, so a car coming within 3 m of a bound car's
spot was teleported one row back at speed 0. The binding table by then held the grid spots of P3/P4: at x -329.14,
3 m past P3's -332.14, P5 landed on -332.14 + 5.38 = -326.76. `teleport_watch.py` found the jumps and `hwwatch.py` on
P5's body position caught the writer (stack 0x5FBD79, 0x43AC15).

P5/P6 also started 11-14 m behind their slots, because nothing put them back when the scene ended.

## What was tried
- Moving a car back only while a car is scene-driven stopped the snaps.
- Restoring each moved car's own spot at scene end looked right on Street X and put P5/P6 inside P3/P4 on drift 4311,
  where the scene ends with its cars on grid slots 5/6.
- Keeping a moved car back when its spot was taken worked, but left the row gap and P3/P4 on the wrong row.
- Placing every car on its real slot at scene end replaced both. Tied to the hidden-extra-cars flag it missed the
  restart path: the restart's binder placed the cars and the placement didn't run. It's now tied to the binder itself.

## Measurement pitfalls
- car +0x18's player index isn't stable within a frame: label cars by their list index.
- Positions change once per physics tick: comparing 2 ms samples with velocity reports every tick as a jump.
- The placement moves cars in the frame the scene ends: 1.3 m for P4 on Street X, about 7 m for P3/P4 on 4311. Whether
  that's visible wasn't checked.
