# p5-steering-sharp
Status:       fixed by the guard `JoySlotWheel` (2d7120c), confirmed by A/B test 2026-10-30
Report:       user, 2026-10-30. In a 5P race, player 5's steering is too sensitive at speed. P1..P4 feel normal. It
              stays with P5 when another pad is used for P5. All 5 players were on native DualSense pads (P5 on pad 12).
Repro:        5P quick race circuit, real pads, steer at high speed with P5 and compare with P4. Every time. 6P not
              checked (P6 may have the same problem).
Area:         [input](../input.md) (steering filter, "Steering wheel check"), [players](../players.md)
Known:
- Steering path: the input object (+0x2A8 car, +0x2AC player index) gets events 0x8F/0x90 in handler 0x407750,
  which writes the raw bytes +0x2C4/+0x2C5 and steer +0x2C8 = (byte - 128) * k. Then 0x41E355 calls
  0x41DFC0(steer, edi, index). That picks 0x406A40 if [0x81CB8C] != 0, else the wheel routine 0x4061C0 if
  0x5C1000(index), else the pad routine 0x416BB0. The speed-sensitive curve tables (0x7E9ED8 etc.) are global.
- 0x5C1000 (wheel on slot) reads 2-slot arrays 0x86FFB4[slot] and byte 0x86E5BC[slot]. For index 4 it read 0x86FFC4
  (slot 0 DIJOYSTATE buttons, 0xEB) and 0x86E5C0 (0x04): true, so P5 got the wheel routine. Index 3 read a 0 byte
  (false). Index 5 read 0x8A (could flip with joystick state).
- The plugin now jumps 0x5C1000 to `JoySlotWheel` (camera.cpp: slot >= 2 → false). The live jump and the stub were
  checked in memory (0x5C1000 = e9 ... to the stub).
- P3..P6 have the same bindings and axis setup in `NFSU2SplitScreen_Players.ini`.
Measured 2026-10-30 (`tools/steer_log.py`, Outer Ring 4001, real pads, input object = car +0x30):
- Full lock, full throttle, stick held: 60-79 m/s P4 633-854, P5 569-764 (no trend); 80-89 m/s P4 645-686, P5 777-785,
  about 15-20% more, but P5's 85 m/s bin is mostly one stretch. Speed reads the same for both (car +0x42C = body speed).
- Course change at full lock above 60 m/s: P4 20-26 deg/s, P5 21-22 deg/s. Steering output falls with speed for P5, so
  it no longer takes the wheel routine (lead 2). The ramp +0x240 (filter +0x20) is the same, about 1.8 per s.
- The filter's only uses of the index are 0x5C1000 and force feedback 0x406460; 0x416BB0, 0x405DC0 and 0x4060E0 have
  no per-index data (lead 3 for these). Output also depends on throttle (0x405DC0 reads input +0x20C..+0x214: 650-690
  at full throttle, 1000-1250 lifted, same for both): compare with matched throttle.
- Same with career Corollas (zavoOP, `PROFILE=1`) on both: at 70-84 m/s, full lock and full throttle, output 635-762
  vs 677-750, and the steering output's rate on reversals p99 23882 vs 24464 per s. No P5 difference in this data.
- 8192 (45 deg, the clamp) at low speed or sliding is not P5's alone: P4 shows it too.
A/B test 2026-10-30 (Corollas, no-guard build, Outer Ring): whether the bug shows depends on memory. 0x86FFC4 was 0 with
P1-P3 on virtual pads, so P5 stayed on the pad routine and matched P4. With the check forced true for index 4 only (table
pointer at 0x5C1007 moved to a page where only entry 4 is non-zero), P5 had the reported feel (user: "that's it"): full
lock output 1360-1500 at 60-90 m/s against 680-780 on the pad routine. Writing 0x86FFC4 from outside doesn't hold, the
game rewrites it during a race. Ruled out:
- Stability Control. Its only reader is 0x5A33E0 in 0x5A3380, called by the physics update at 0x5AB007. Above
  30 mph it multiplies by [0x7A1990] = 1.0, so it has no effect at speed. (The pause-menu toggle for P3..P6 was also
  wrong. That is fixed in 0c0afa8, but it did not change the steering.)
- The pad. Another pad on P5 has the same problem.
Leads (best first):
1. Measure it live. In a 5P race at the same speed, log the input object's +0x2C4/+0x2C5/+0x2C8 for P4 and P5 and
   the steer value 0x41DFC0 returns. This shows whether the difference is in the input (raw value, scale k) or after
   the filter (car side).
2. Confirm that P5 now gets 0x416BB0: breakpoint or counter on 0x4061C0 vs 0x416BB0 with index 4. Check for other
   callers of 0x5C1000 or an inlined copy of the wheel test (`callers.py 0x5C1000`, `opscan.py` for 0x86FFB4 /
   0x86E5BC).
3. Look for other per-index arrays of 2 (or 4) in 0x416BB0, 0x41DFC0, 0x407750 and 0x41E355 that index 4 reads past
   (the same kind of bug as 0x5C1000 and `JoySlotPresent`).
4. Car side: per-car steering state that depends on the player or port (input object +0x2AC, the player's settings
   entry at player +0x20/+0x21, the Transmission/Stability values from the 0x83AA64 table, which has only 2 entries).
5. The plugin's own input path for pad 12 / port 4 (XInputDevices.cpp, analog scale or dead zone per seat).
Tools:        `python tools/sdis.py <addr>`, `tools/callers.py`, `tools/opscan.py`, `tools/trainer.py` (`Game`:
              u8/u32/read, live memory), `tools/telemetry.py` (shows FIN: a finished player's input object is
              freed and the AI drives). Read the input objects while all players are still racing.
