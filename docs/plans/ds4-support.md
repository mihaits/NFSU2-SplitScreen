# Native DualShock 4 support

## Goal
DS4 pads work without DS4Windows, next to DualSense and Xbox pads, over USB and Bluetooth: input, rumble, a
player-coloured lightbar, and the car on the pad's speaker if the speaker can be made to work.

Hardware for testing: **one DS4**, on Bluetooth or USB. Identity of two DS4s and more than one DS4 stream can't be tested.

## Decisions (user, 2026-10-30)
- **Lightbar:** player colour, no rpm, no nitrous or shift cues. The start animation stays, as on the DualSense:
  red, red, amber with the 3-2-1, then green for 1 s from GO. It flashes when the pad isn't controlling, like the
  DualSense player LEDs.
- **Podium:** gold, silver, bronze as on the DualSense. 4th-6th show a dim colour, never off, so nobody thinks the pad
  turned off. DualSense keeps "off" outside the podium.
- **Touchpad click = mute button.** The DS4 has no mute button, and with no rpm the click has no lightbar job left.
- **Stages, one commit each, tested and approved in order:** input + rumble, lightbar, speaker (Bluetooth, then USB).

Decisions made while planning:
- Pause: the player's colour at 25%, same step as the DualSense, so a pause is still visible.
- Pads with no player: 1 Hz flash in dim white (they have no player colour). Colour (60, 60, 60) × brightness.
- 4th-6th place: the same dim white, solid.
- `[LIGHTS] Lightbar = 0` turns the bar off on a DS4 too; the flash and the podium follow it. `PlayerLeds` doesn't apply.
- `[INPUT] NativeDualSense` keeps its name and now covers DS4 (the ini comment says so). The DS4Windows rule is unchanged.
- PIDs 05C4 and 09CC. The Sony wireless dongle (0BA0) is left out: nobody can test it.
- No adaptive triggers row on a DS4.

## Protocol
From SDL (`SDL_hidapi_ps4.c`) and hid-playstation. **Check each on the real pad; only the DualSense side is proven here.**

| | USB | Bluetooth |
|---|---|---|
| Input | report 0x01, 64 bytes | 0x11, 78 bytes, offsets +2 from USB. 0x01 (short) until feature 0x02 has been read |
| Output | 0x05, 32 bytes: [1] flags 0x07, motors at 4 | 0x11, 78 bytes: [1] 0xC0 + interval, [3] flags 0x03, motors at 6, CRC at 74 |
| Address | feature 0x12, bytes 1..6 | HID serial string |

- Input bytes (USB offsets): 1..4 sticks, 5 hat in the low nibble + Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80,
  6 L1 0x01, R1 0x02, Share 0x10, Options 0x20, L3 0x40, R3 0x80, 7 PS 0x01, touchpad click 0x02, 8 L2, 9 R2,
  30 battery low nibble.
- Output, from the motor offset: small motor, large motor, R, G, B, flash on, flash off. The flash is done in software
  (as the DualSense's flashing LEDs), so the hardware bytes stay 0.
- CRC: `BtSign` as it is (seed 0xA2 over 0..73, little-endian at 74).

**Risks to check first:**
- `OpenPad` rejects `inLen > 256`, and `IoThread` reads into `in[256]`. A Bluetooth DualSense reports 78 for input, but
  a DS4's descriptor may report the largest report (547). The buffer has to fit what `HidP_GetCaps` says, or `ReadFile`
  fails. The output is already capped at 78 for the same reason.
- `s->usb = InputReportByteLength == 64` may need another check, as above.
- Clone pads may fail feature reads: log it and go on. A pad that never sends 0x11 stays silent on Bluetooth.

## Stage 1: input and rumble
`src/DualSense.cpp`, `DualSense.h`:
- `Slot.ds4`, set in `OpenPad`. `Rescan` matches `pid_05c4`/`pid&05c4`, `pid_09cc`/`pid&09cc`.
- `ParseInput`: a DS4 path (`ParseInputDs4`) filling the same `DsPadState`/`DsExtra`; `mute` stays false.
- `BuildReport`: a DS4 path; `IoThread` init: feature 0x02 (37 bytes) with retries in place of 0x20/0x05, no
  `setup` report, `rumbleV2` unused. Log "DS4 N: USB|Bluetooth". `outLen` check per model (32 on USB).
- `ReadMac`: feature 0x12 for a DS4 on USB (feature 0x09 isn't the pairing report there).
- `DsIsDs4(slot)`; the speaker functions return false for a DS4 slot (`spkNoDevice` true).

`src/XInputDevices.cpp`/`.h`:
- `XPadIsDualSense` becomes DualSense only, so these stay right without changes: the pause row
  (`pad_lights.cpp:197`), `sound.cpp:453,474`, and `XPadSpeakerReady`.
- New `XPadIsDs4(pad)`.
- `pad_lights.cpp:459` skips the DS4 at this stage. Its `DsOutput.rgb` starts as dim blue, so the bar isn't dark.

**Test (user, real pad, USB then Bluetooth):** menus, join as P2 next to a DualSense or Xbox pad, all buttons in Options >
Controls, sticks and triggers in a race, rumble, unplug and reconnect mid-race (same player), USB to Bluetooth keeps
the slot.

Docs: `input.md` (a DS4 block after Native DualSense), README requirements, features and to-do, ini comment.

## Stage 2: lightbar
`src/parts/pad_lights.cpp`, in `UpdatePadLights`: a DS4 branch next to `XPadIsDualSense` (a small function, say
`Ds4Lightbar(pad, seat, ctl, ...)`, not more cases inside the DualSense block):

| Situation | Bar |
|---|---|
| Seat in the session (from the join on) | the seat's `kPlayerColour` |
| Controlling pad, no seat | P1's colour |
| Any other pad, and unjoined pads at Press START | dim white flash, 1 Hz |
| Countdown and GO, on a pad that drives a player | the same red, red, amber flashes and 1 s of green as the DualSense |
| Pause | seat's colour at 25% |
| Finished, 1st-3rd | `kPlace` colours until state 6 ends |
| Finished, 4th-6th | dim white, solid |
| `Lightbar = 0` | off |

Reuse `sessionOn`, `seatLimit`, `ctl`, `flashOn`, `paused`, `FinishedPosition`, `g_lightbarBrightness`, and the
countdown state (`countdown`, `stageStart`, `greenUntil`); move the colour step into a helper both models call. No rpm
reads for a DS4. The touchpad toggle (line 476) is DualSense only, so the click does nothing yet on a DS4.

**Test (user):** P2 on the DS4: colour after the join, flash before it, start animation, pause dim, finish on and off the podium,
frontend after the race. One check per run, asked right after.

Docs: `input.md`, the README table, the ini comments for `Lightbar`.

## Stage 3: speaker
Unknown ground: the protocol below has one source and no capture. **Prove it outside the game first**, one test per
run, by ear, the way `ds_bt_speaker.py` and `ds_usb_speaker.cpp` did it. If one transport fails twice, write
`docs/bugs/ds4-speaker.md`, ship the other, and put the failed one in the README to-do. Stages 1 and 2 don't depend on this.

**3a. Bluetooth.** `tools/ds4_bt_speaker.py`, as a copy of the DualSense tool:
- Lead: [PadForge wiki](https://github.com/hifihedgehog/PadForge/wiki/Controller-Audio) and
  [its internals](https://github.com/hifihedgehog/PadForge/wiki/Controller-Audio-Internals): SBC, 32 kHz, 256-sample blocks,
  report 0x17 (4 frames) or 0x14 (2 frames), after a 0x11 report that enables audio and sets the volumes
  (speaker volume at 24 of 0x11, per Chromium).
- Find out: frame layout, SBC parameters, CRC, the report rate, and whether normal 0x11 reports disturb the stream.
- Then in `DualSense.cpp`: `Ds4SpeakerThread` on `SpkTake` (queue, trim and limiter shared; `outRate` 32000). The SBC
  encoder: small and our own, or a library whose licence fits (BSD like Opus; libsbc is LGPL, so not statically).

**3b. USB.** `tools/ds4_usb_speaker.cpp`, as a copy of `ds_usb_speaker.cpp`:
- Find out: does the pad show an audio endpoint, its channels and rate, and which bytes of report 0x05 reach the
  built-in speaker (volumes at around 19..22, unconfirmed).
- `FindPadEndpoint` and `UsbSpeakerThread` should fit as they are, with a route step per model.

**Game side, both:**
- `XPadHasSpeaker(pad)` for `sound.cpp:474` and `XPadSpeakerReady`.
- `PadSpeakerDefaults` counts Bluetooth DS4 streams too. `XPadIsBluetoothDualSense` becomes a "Bluetooth speaker pad"
  check. Measure a DS4 stream's bandwidth against the 2-stream limit (`kSpeakerDualSenseMax`).
- Touchpad click on a DS4 = `PadSpeakerToggle(seat)`, in the mute block at `pad_lights.cpp:517`. There is no LED to show
  the state: the car moving between the pad and the PC is the feedback.
- If only one transport works, `DsSpeakerReady` is false on the other, and the click does nothing there.

**Test (user):** by ear, DS4 + DualSense in one 2P race, each pad's own car, the click, reconnect.

Docs: `input.md`, `sound.md`, `tooling.md`, README, ini (`PadSpeakers`, the mute wording).

## Done
- Stages 1 and 2 committed and approved on USB and Bluetooth. Stage 3: working, or a handoff and a README to-do item.
- `README.md`: the to-do line "DualShock 4 without DS4Windows" removed, a pad list that names the DS4.
- This file deleted.
