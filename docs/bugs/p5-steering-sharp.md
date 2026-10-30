# p5-steering-sharp
Status:       open (a guard for one out-of-range read is in, it did not fix the symptom)
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
  checked in memory (0x5C1000 = e9 ... to the stub). **P5 is still too sharp with it** (user test, 2026-10-30).
- P3..P6 have the same bindings and axis setup in `NFSU2SplitScreen_Players.ini`.
Ruled out:
- Stability Control. Its only reader is 0x5A33E0 in 0x5A3380, called by the physics update at 0x5AB007. Above
  30 mph it multiplies by [0x7A1990] = 1.0, so it has no effect at speed. (The pause-menu toggle for P3..P6 was also
  wrong. That is fixed in 0c0afa8, but it did not change the steering.)
- The pad. Another pad on P5 has the same problem.
- The wheel check alone (see above). Either it was not the cause, or something else also sends P5 down a different
  path.
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
