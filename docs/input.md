# Input and rumble

**Game side.** DirectInput only, and at most 2 joysticks are opened (slots 0x86FFB4[2]). The poll function
0x5CFD60(port, &type, &prev): port 0 polls every device (records at 0x874C40, stride 0x370, max 6; record +0x230 =
joystick slot, keyboard = record 0, type 0x13) and merges them into 0x86EAF0; port 1 returned the same buffer. Players
read port = 0x83A9D8[player]. Input port tables: 0x8655A0, 2 × 0x1C8 (binding node list +0, port id +0x1B4, device type
+0x1B8); poll loop 0x5809C0 → `0x57B0B0` → events at 0x865984.

**XInputDevices.cpp.** It hooks the DirectInput8Create IAT slot 0x783024 and wraps IDirectInput8A.
EnumDevices/CreateDevice expose virtual "Split Screen Pad N" gamepads with full DirectInput emulation (objects with
Xbox names, GetObjectInfo, properties, buffered GetDeviceData — Options > Controls needs all of these). Real DirectInput
controllers are hidden. Pads 0..3 are the real Xbox pads (read through WGI, see below), 4..9 debug pads driven through the mailbox, 10..17 native DualSense pads (below).

**Native DualSense (`DualSense.cpp`).**
- HID through SetupAPI + hid.dll (`hid.lib setupapi.lib`), USB and Bluetooth, DualSense 054C:0CE6 and Edge 054C:0DF2.
  Native slots 0..7 (`kDsMax` 8) are pads 10..17 of the pad layer (`kDsFirst`), so joining, bindings, P3..P6, pause ownership and
  rumble work as for XInput pads. `XInputDevices.cpp` keeps one merged `DsOutput` per native pad (rumble, lights,
  triggers) and hands it to `DsSetOutput`; `XPadIsDualSense`, `XPadSetLights`, `XPadSetTriggers` and
  `XPadTouchClickPressed` are the exports for the lights and triggers (the lights/triggers policy is not there yet:
  a native pad shows dim blue and player LED 1 meanwhile).
- A hotplug thread rescans every 2 s (gamepad interface only, usage page 1 / usage 5); one I/O thread per pad does
  overlapped reads into the slot's latest state and overlapped writes (100 ms timeout). A read error frees the slot
  (the pad then reads idle) and the next rescan reopens it. A pad gets back the slot it had, so it keeps its player: each slot keeps the
  pad's Bluetooth address (feature 0x09 bytes 1..6, last byte first; else the HID serial string) after it drops, and
  a pad takes the free slot with its address, else a never used one, else the one whose pad dropped longest ago. The
  same pad on its other connection (a cable plugged into a Bluetooth pad) isn't opened while the first one is still
  open. Logged as "DualSense N: pad ..xxxx, back in its slot / new slot / takes a slot another pad had" (last 4
  digits only). `[INPUT] NativeDualSense = 0` turns the backend off.
- **Input stalls:** a connected pad streams full input reports every few ms. Through a gap the game keeps the last
  input; after 1 s (`kStallMs`) `DsState` returns neutral input (no buttons, sticks centred, triggers released) until
  reports come back. Gaps from 250 ms (`kStallLogMs`) are logged: "DualSense N: no input for ... ms" / "input again
  after ... ms". Seen 2026-10-30
  (5 pads on one Bluetooth adapter): one pad dropping froze every pad's last input for ~19 s, until it reconnected,
  and cars spun on held steering and throttle. Windows logged no error for the other pads, so the plugin had no way
  to see it before.
- **DS4Windows:** native pads are closed, and none opened, only while `DS4Windows.exe` runs *and* exposes a virtual
  pad: a present device whose parent has hardware id `Nefarius\ViGEmBus\Gen1` (SetupAPI `DIGCF_PRESENT`,
  `CM_Get_Parent`; checked live: started = `USB\VID_045E&PID_028E` under the bus, stopped = no child). Open but
  stopped, native stays on. The rule is re-checked on every 2 s rescan and each change is logged ("DS4Windows not
  running / running but stopped / is exposing a virtual pad"). Its virtual pad is a normal XInput pad.
- **Init per pad:** read feature 0x20 (firmware; logged as "DualSense N: USB|Bluetooth, hardware, firmware, update,
  rumble vN"; on Bluetooth this switches the pad to the full 0x31 report), feature 0x05, then a light-out report that
  releases the startup blue.
- **Bluetooth quirks (seen on a real pad):** the device path reads `VID&0002054C_PID&0CE6` (not `VID_054C&PID_0CE6`),
  and `HidP_GetCaps` reports an output size of 547 (the largest report in the descriptor); the 0x31 report is 78 bytes
  and a 78-byte `WriteFile` is accepted, so the buffer is capped at 78. Transport = `InputReportByteLength` (64 = USB).
- **Input:** USB report 0x01 (64 bytes) or BT 0x31 (78 bytes, offsets +1; the short BT report 0x01 is ignored).
  Cross/Circle/Square/Triangle = A/B/X/Y, L1/R1 = LB/RB, Create = Back, Options = Start, L3/R3, hat = D-pad, analog
  L2/R2 = triggers, sticks `(v - 128) * 258` with Y negated. PS, Mute and touchpad click go to `DsExtra` only.
- **Output:** USB report 0x02 (48 bytes) or BT 0x31 (78 bytes, CRC32 seed 0xA2 at 74..77). Sent on change, every 4 s
  while rumbling and every 500 ms on BT. On BT a change waits until 25 ms after the last report (at most 40 reports/s
  per pad: the rpm lightbar changes every frame, and several pads share one adapter; 125 ms while the speaker
  streams, below); rumble starting or stopping and new trigger effects go out at once (50 ms while streaming). After a failed write (100 ms timeout) the next try waits 250 ms, so a pad on a
  weak link keeps reading input; the failure and the recovery are logged once each. Rumble v2 (flag2 0x04) when `update_version >= 0x0215` or on an Edge, else v1
  (flag0 0x01); `DualSenseRumbleMode` overrides. The small motor is halved (as SDL does) to match an Xbox pad.
  Large motor = the game's left/low value, small = right/high.
- **Mute button and LED.** `XPadMutePressed(pad)` is the press edge of `DsExtra.mute` (like the touchpad click);
  `XPadSetMuteLed(pad, mode)` sets `DsOutput.muteLed` (common block byte 8, valid flag1 bit 0x01: 0 off, 1 on, 2
  pulsing). In split races the mute button turns the player's pad speaker off / on (pad_lights.cpp → sound.cpp
  `PadSpeakerToggle`); the LED is lit while it's off.
- **Speaker (Bluetooth; `DsSpeakerWrite`, `SpeakerThread`).** Windows has no audio device for a Bluetooth DualSense;
  the speaker takes Opus in output report 0x39 (547 bytes, layout from awalol/DS5Dongle and KytyPS5): [1] seq << 4,
  [2] 0x91 config (len 6: 0x7E = no microphone stream back, 4 × 48, [9] counter += 2), [10] 0xD2 haptics (len 64,
  silent), [140] 0xD3 speaker (len 200) with two 480-frame Opus packets (48 kHz stereo, CBR 160 kbit/s, complexity 5:
  200 bytes each), CRC32 seed 0xA2 over 0..542 at 543. One report per 21.333 ms; the pad plays them at 45 kHz, so the
  queue is resampled to 45 kHz (linear) and encoded as if 48 kHz. A 0x31 report first sets audio_control = speaker
  path (0x30), speaker volume 0x64 and preamp 2 (flags 0x80 | 0x20, 0x80). libopus 1.5.2 is built from
  `third_party/opus` by `build.bat`. `tools/ds_bt_speaker.py` sends the same reports from Python (ffmpeg encodes).
  - A thread per pad starts on the first samples and ends 1 s after the last (leftover samples are dropped); the I/O
    thread stops it before closing the handle. A soft limiter (ceiling 0.9) replaces clipping.
  - Queue: starts at 2048 input samples; the game delivers in bursts, so the lowest point left after a report is held
    near 768 samples (~17 ms) per half second by trimming the read rate (±1 %, proportional + integral); an underrun
    lowers the rate at once and refills only to one report plus that reserve. The 5 s stats line (Debug) shows
    reports, underruns, peak, the lowest reserve, the rate trim and the worst send lateness. A DualSense's `speaker link:`
    line adds the writes slower than one period, the longest write, and the input reports, gaps over 20 ms and longest gap.
  - While the speaker streams, other output reports wait at least 125 ms (rumble start/stop and trigger changes 50
    ms): at 40 lightbar reports/s on the same link the speaker dropped out now and then. A state sub-packet (0x90,
    the 0x31 settings block) in place of the haptics one, to carry the lightbar inside the audio reports, silenced the
    speaker.
- **Speaker (USB; `UsbSpeakerThread`).** The pad is also a Windows audio device: a render endpoint of 4 channels at 48
  kHz (shared-mode mix format 32-bit float; speaker tap L/R, haptics L/R). It plays on the jack, plugged or not, until
  output path 3 is set: the same route report as Bluetooth, as report 0x02 (`SpeakerRoute`), mutes the jack and plays
  channel 2 on the speaker; channels 1, 3 and 4 stay zero. Normal output reports don't undo the route. Path 0 at the
  end of a stream puts it back on the jack. If the game dies mid-stream the route stays until the pad reconnects.
  `tools/ds_usb_speaker.cpp` checks this one test per run (right channel, left channel, rumble, jack path).
  - Endpoint: the active render device whose PKEY_Device_ContainerId is the HID device's `DEVPKEY_Device_ContainerId`.
    The pads have no USB serial; Windows generated a different time-based id for a DualSense and a DS4 on USB, so
    identical pads shouldn't collide. If they ever do: match the endpoint's `MI_00` to the HID `MI_03` by their parent.
    After a plug it shows up about 10 s after the HID interface, so a USB slot starts with `spkNoDevice` set and the
    hotplug thread (`CheckUsbEndpoints`, every 2 s) clears it once the endpoint is there. Until then `DsSpeakerWrite`
    returns false and the car stays on the PC. sound.cpp takes the car back as soon as `XPadSpeakerReady` says so.
  - Under Wine a DualSense also gets channel 1, and any smaller endpoint all its channels (docs/linux.md). The mix format
    is logged.
  - Queue, rate trim and limiter are shared with Bluetooth (`SpkTake`, at 48 kHz here). Event driven, 30 ms buffer. No
    limit on other output reports while streaming. A stream that fails logs why; "no active audio endpoint" and
    "endpoint gone" wait for the endpoint again, anything else stays off until the pad reconnects.
  - Windows may make the pad the default output when it connects. The PC mix then plays on the pad too (logged), and
    on the jack, so silent, after the stream ends. Left to the user: the only way to change it is the undocumented
    `IPolicyConfig`, and the default output is the user's setting.
  - The pad's Bluetooth address (report 0x09) is the same on both connections, so a pad moved from USB to Bluetooth
    keeps its slot and player.
- **Player LEDs (`parts/pad_lights.cpp`, `UpdatePadLights` from `LayoutApplyHook`, `[LIGHTS] PlayerLeds`):** from the
  car-select join until the race is left, a pad a player joined with shows that player's number (1 `--x--`, 2 `-x-x-`,
  3 `x-x-x`, 4 `xx-xx`); the session counts as on in the split car select and in game states 4..6 of a split game (state 3 is the frontend,
  also after a race).
  While a Press START popup is open (`2P_PressStart.fng`) no pad controls, so every pad that has not joined flashes.
  Otherwise the controlling pad (last pad used; on the car select the pad whose turn it is) shows 1, and every other
  DualSense flashes all 5 at 1 Hz. 
- **Lightbar (`[LIGHTS] Lightbar`, `LightbarBrightness`):** while racing (state 6, race stage `*[0x890118]` = 4, not
  paused) the pad that drives local player k shows that car's rpm: green at idle, yellow, red at the shift-light rpm,
  red flashing at 4 Hz from there, solid blue while nitrous burns. The addresses (tach rpm, shift rpm, nitrous) are in
  the comment block of `parts/pad_lights.cpp`; `tools/telemetry.py` prints the same numbers. Countdown (race stage 3, 3.0 s): red, red,
  amber with the 3-2-1, each flashing (0.6 s on, 0.4 s off), then green for 1 s from GO (stage 4), timed from the frame stage 3 starts, so a restart from the
  pause menu works the same. Once a player has finished (player +0x50 != 0; place = racer +0xA, 1-based, the order on track; in drift the rank by
  score: the player's node in the standings ring 0x864630, `DriftPlace`),
  their pad shows the finish place until the game state leaves 6: 1st gold (255, 180, 0), 2nd silver (200, 200, 220), 3rd
  bronze (205, 100, 30), off outside the podium. The log shows each player's finish as seen here ("finish: player N
  place P") and each pad's finish colour turning on and off ("lightbar: pad N ..." with the state and race stage). While the pause menu is open (`PauseMenuOpen`)
  every driving pad shows its player's colour at 25% (1P: P1's). In split racing a click on the
  touchpad (`XPadTouchClickPressed`) toggles that player's solid minimap colour (fully saturated, as the bar washes pale
  colours out: P1 (0, 255, 0), P2 (255, 0, 0), P3 (0, 64, 255), P4 (255, 150, 0)), kept per seat until the game closes. In other
  situations the
  bar keeps a dim blue until the pause, countdown, finish and player-colour steps.
- **Adaptive triggers (`[LIGHTS] AdaptiveTriggers`, `TriggerStrength`):** while a player races with the pad (not paused,
  not finished) L2 has light resistance and buzzes (35 Hz) while braking with a wheel sliding (brake slip >= 0.06);
  R2 has light resistance, buzzes at 25 Hz in wheelspin (drive slip >= 0.06), at 60 Hz at the rev limiter (shift-light
  rpm) and pushes harder for the first 300 ms of a nitrous burn. Slip is the same wheel-vs-ground measure as the Xbox
  impulse triggers (below); the grip used (`wheel+0x170`) used before reads 1.0 at full throttle without wheelspin,
  so it only marks the car airborne (0 on all four). The hold times are described in `parts/pad_lights.cpp`.
  Everything else sends Off. Only the official trigger modes are used (0x05 off, 0x21 feedback, 0x26 vibration);
  0xFC..0xFE break the trigger.
  Each player turns them on or off with the "Adaptive Triggers" row of the pause options (`PauseFeedbackAdd`: added
  after the Feedback row, `call 0x54F170` at 0x4C4985, when the pausing player's pad is a DualSense). The choice is
  saved as `[PlayerN] AdaptiveTriggers` in `NFSU2SplitScreen_Players.ini`; without one, the ini's `AdaptiveTriggers`
  applies.
- Tested with 4 native DualSense pads over Bluetooth at once, next to 2 Xbox pads (5-6 player races, 2026-10-24).

**Native DualShock 4 (`DualSense.cpp`).** The DualSense backend also opens DS4 pads (054C:05C4, 054C:09CC; `Slot.ds4`),
in the same slots, threads and hotplug, under `[INPUT] NativeDualSense` and the same DS4Windows rule. `XPadIsDualSense` is
false for them and `XPadIsDs4` true, so they get no player LEDs, trigger row or mute button.
- **Init:** feature 0x02 (calibration, read only); on Bluetooth it switches the pad to the full 0x11 input report, as
  any 0x11 output report does. Logged as "DS4 N: USB|Bluetooth, report sizes in .. out ..". Nothing else is needed.
- **Address:** feature 0x12 (same layout as the DualSense's 0x09), else the HID serial string. A cable plugged into a
  Bluetooth DS4 mid-race changes nothing: the pad keeps sending on Bluetooth and the slot stays there (2026-10-30).
- **Input:** USB 0x01 or BT 0x11 (offsets +2). Same bits as the DualSense at other offsets: sticks 1..4, hat + face 5,
  L1/R1/Share/Options/L3/R3 6, PS and touchpad click 7, L2 8, R2 9, battery 30. The input buffer is `kInMax` (600 bytes).
- **Output:** USB 0x05 (32 bytes, flags 0x07 at 1, from 4 on) or BT 0x11 (78 bytes, 0xC4 at 1, flags 0x03 at 3, from 6 on,
  same CRC as the DualSense): small motor, large motor, R, G, B. The motors aren't scaled.
- **Lightbar (`Ds4Lightbar` in `parts/pad_lights.cpp`, `[LIGHTS] Lightbar`, `LightbarBrightness`):** the bar faces away from the
  driver and there are no player LEDs, so it shows who the pad is, never the rpm: the player's colour from the join on
  (`kPlayerColour`; 1P and the controlling pad: P1's), at 25% in the pause. The start animation is the DualSense's
  (`StartCue`). Finished: the podium colours, dim white (60, 60, 60) from 4th down, since off would look like a dead pad.
  Any other pad flashes that dim white at 1 Hz, as the DualSense flashes its player LEDs.
- **Speaker (Bluetooth; `Ds4SpeakerThread`, `SbcEncoder.h`):** SBC in output report 0x17 (462 bytes, layout from PadForge's
  docs, checked by ear): [1] 0x40, [2] 0xA0, [3..4] frame counter += 4, [5] 0x02 speaker (0x24 jack), 4 SBC frames from
  6, CRC32 seed 0xA2 at 458. A frame is 128 samples at 32 kHz, so one report per 16 ms (218 kbit/s with headers: as heavy as a DualSense stream).
  Report 0x11 with flag 0x80 at 3 and the volume (0x4F) at 24 turns it on. `SbcEncoder.h` is our own, from the A2DP
  spec: 32 kHz, 16 blocks, 8 subbands, stereo, SNR allocation, bitpool 48, 108-byte frames, mono in both channels; 51.5 dB
  SNR through ffmpeg's decoder (`tools/sbc_test.cpp`, `tools/sbc_check.py`). The queue is `SpkTake` at 32 kHz. The pad's
  own mute button is missing, so the touchpad click toggles the speaker in split races (`PadSpeakerToggle`); the car moving
  between the pad and the PC is the only feedback.
- **Speaker (USB; `UsbSpeakerThread`, `UsbRoute`).** Second model (09CC) only; the first model has no USB audio, so it finds no endpoint and its car stays on the PC. The stream is the DualSense's USB one at 32 kHz on the right channel, with E0 3 and the speaker volume at the start and E0 0 at the end. The pad is a 2-channel 32 kHz "Headset Earphone" endpoint that normally plays on the jack. **Feature report E0 "Set Audio Output", `E0 00 n`, routes it** (2026-10-31, tone heard on the speaker on a cable, no Bluetooth): 0 stereo headset, 1 left to headset, 2 left to headset and right to speaker, 3 right to speaker, 4 off. The value table is in the Controller Collective wiki, under the wireless dongle, but the pad's own descriptor declares E0 too, and it refuses a read (`tools/ds4_usb_speaker.cpp output n`). So the car goes on the right channel.
  - What led there: 0x05 has no route byte (`sweep`), and report volumes 19, 20 and 22 stop at 0x4F. The factory sine, feature A0 `01 01` / `01 00`, plays on the speaker on a cable and 0x05's speaker volume changes it (`sine`, `sinevol`). A0 `05` (audio chip write) is rejected here: A4 reads FF FF (`chipstatus`). Never send A0 `04 02`, `04 03` or `0A`: they reach flash or strand the pad.
  - After a replug the speaker comes back a few seconds later, once the endpoint is there (user, 2026-10-31).
  - A pad connected over Bluetooth stays there with a cable in and keeps its speaker: that is how to charge and use the speaker on a first-model pad.

**Xbox pads through Windows.Gaming.Input (`XboxWgi.cpp`).**
- Pads 0..3 read through WGI: a `Gamepad` gives input, id and vibration from one object, and XInput can't drive the
  trigger motors of Xbox One / Series pads or say which pad a user index is. WinRT is loaded from combase.dll at run time
  (no import). One MTA thread owns the list and sends vibration; the game thread calls `GetCurrentReading` itself
  (`WgiReadPad`, under a lock against the list changing), so a reading adds no delay.
- `GetPad` converts a `GamepadReading` to an XInput-shaped state: triggers round to 0..255, so a full pull is exactly 255
  (`StateOf` needs that for full brake, "Pedals on the shared Z axis" below), sticks to -32768..32767.
- **Focus.** Readings are live only while the game window is focused. Unfocused they read neutral (timestamp 0 until the
  first focus), where XInput kept reading, so **test real pads with the window focused**. The plugin's test pads 4..9
  don't depend on focus.
- **Fallback.** XInput stays the input path, with the pad number as the user index, until WGI has listed a pad. So older
  Windows, or a Wine whose list stays empty, still reads pads, without impulse triggers. From the first listed pad on,
  WGI is the only path.
- The list is reread when `GamepadAdded` / `GamepadRemoved` fires, in about 50 ms, and every second as a backstop: an event
  can come before the list shows the change. The handler only wakes the thread. Some desktop apps report an empty list
  without one (not confirmed here).
- **Pad numbers.** A pad takes the number holding its `IRawGameController2.NonRoamableId`, else a number no pad has, else
  the one that dropped longest ago (`Bind`, `FreeSlot`). So a returning pad keeps its player at once, in any order, with
  no press. The id held across reconnects within a run and was different in a later run, so numbers aren't remembered
  across runs. Under Wine the id reads empty, so the key is VID:PID: pads of different models keep their numbers, two of
  one model can swap. Numbers follow the list order at start. A fifth pad has no number until one goes free.
- A pad's whole vibration goes through WGI, Xbox 360 class pads too (`XPadSetRumble` and `XPadSetImpulseTriggers` merge
  into one `WgiSetVibration`; checked on a virtual 360 pad). `WgiHasTriggers` is false for the 360 class PIDs
  045E:028E/028F/0291/02A1/0719, which include DS4Windows' virtual pad. Resent every second while non-zero (Windows drops
  vibration when the window loses focus). Under Wine at most 20 sends a second: faster made Xbox Bluetooth rumble 1-4 s
  late (docs/linux.md).
- A native DualSense doesn't show in the `Gamepad` list, so it isn't doubled. A virtual Xbox pad from a mapper is a normal
  `Gamepad` with its own number.
- Effects (`UpdateImpulseTriggers` in `parts/pad_lights.cpp`), Forza Horizon style: the motors buzz in proportion to
  tyre slip, wheel surface speed (angular speed `wheel+0x28` × the wheel's radius `*(*(wheel+0x14)+0x10)`, per wheel
  from the car's tyre data) against ground speed (length of the body velocity `[mover+0x20]+0xA0`), s = (surface −
  ground) / max(ground, 3 m/s). R = ramp(fastest axle's s, 0.04..0.5) × throttle (live: launch wheelspin 1..3, RWD
  power oversteer 0.07..0.1); L = ramp(slowest wheel's −s, 0.04..0.25) × brake (hard braking 0.08..0.2 on the fronts;
  the game has no ABS, the wheels do slip). Mapped to 0.08..0.5 (R) / 0.6 (L) × `TriggerStrength`, smoothed 40 ms up /
  80 ms down; silent in the air, below 1 m/s, paused or finished. No engine cues (limiter, nitrous). Debug logs
  "impulse: pad N L x R y" once a second while on. Same per-player switch as the DualSense (the pause row reads
  "Impulse Triggers").
- Tested with an Xbox One S pad (045E:02E0) over Bluetooth.

**Pad resolver (`PadForDevice`).**
- Outside split: device 0 = the last active pad.
- Split menus: only the current FE player's pad (its joined pad on the car select, else the last active pad).
- Racing: each player's assigned pad. A pad that turns off keeps its player until it comes back (DualSense: same
  slot, see above). **Takeover** (`UpdatePadTakeover`): START on a connected pad no player has gives it the first
  player whose pad is off ("player N's pad P is off: pad Q takes over"); `RacePad` reads it idle until its buttons are
  released, so that START doesn't pause.
- A missing pad is reported idle, never as DIERR_INPUTLOST (the game would drop the slot).
- **Pedals on the shared Z axis (`StateOf`).** 0x5C2029 turns Z into one byte (+0x13: idle 0x7F, 0..0xFE; merged at
  0x86EAFB, port 0), with single-precision x87 (control word 0x007F), so ±10000 × 0.0127 is exactly ±127. Records at
  0x7FE190.. (+8 event, +0xC decoder, +0x10 polarity, +0x12 bit, +0x14 mode, +0x15 curve) feed the events. The polarity is
  1 for buttons (0x571CC0 XORs and keeps bit 0) and also for the driving axes, 0x90 steer, 0x92/0x95 pedal axes, 0x93/0x96
  shared axis. The decoder 0x5795C0 then: XORs the whole byte with it, runs it through a curve (0x576790; pedals (0,0) (123,128) (133,128) (255,255), the flat part is the dead zone), and makes a value 0..254:
  throttle (0x7F - x) × 2, brake (x - 0x80) × 2. Only 254 becomes 255. Brake +0x2DC = value / 255 (0x407F2A); throttle
  takes the value × 1.07 in gear, capped at 255 (0x416E48), and goes through a curve in neutral (0x416F51). The unused
  separate pedal channels rest at 0, which the XOR turns into 1, so idle pedals read 1/255 (+0x2D1, +0x2D9). Digital
  keys send a fixed 255: brake 1.0, measured.
  Because of the XOR, the top is byte 0xFE for the brake and byte 1 for the throttle; bytes 0xFF and 0 aren't. Byte 0
  gives 252, and 0xFD gives 246 (brake 0.965). A real Xbox pad through DirectInput at this range reads Z -9961..9961, so
  it also stops at 0xFD and 1: the original game gives such a pad at most 0.965 brake. `StateOf` scales LT to the axis end
  to reach 1.0, as a digital brake does, and leaves RT at the real pad's scale. This model matches every measured value.
  The XOR also swaps neighbouring steps, so the top of the pedal isn't quite monotonic (0xFC 248, 0xFD 246); harmless.
- While the Controls screen shows the P2 page, slot 2 is fed by the last active pad (`UpdateEditingSlot2`).

**Per-port merge.** Ports 0/1 are merged per device slot, which separates P1 and P2.

**Keyboard player.** The keyboard (record 0, slot 0, type 0x13) is left out of the slot merge and goes to one port
(`KeyboardPort`): racing, its player's port 0..5 (ports 2..5 in `PollExtraPort`); during a pause the menu's port, only
when its player paused (`g_pausePort`; P3..P6: port & 1); on the car select only on its player's turn
(`PadForDevice` gives that turn no pad); elsewhere in the split menus port 0. After a change it waits until every key
is up. Its player's pause menus show Feedback greyed and off: `FfCapable` (jump at 0x5B8240) returns false, and
`SyncPlayerSettings` clears the game entry's +4 while they are open and puts it back after (the setting is kept). The
mouse free camera follows the keyboard player (with no keyboard player, the mouse turns no camera).

**Players 3..6.**
- The port tables are relocated to `g_portTables[INPUT_PORTS]` (6) (`RelocatePortTables`: 15 refs, loop bounds 0x57B3AC/0x580AD4,
  static ctor/dtor counts 0x77D9CB/0x782296). This runs before the game's static init.
- The binding node pool is raised from 0xA0 (2 ports) to 0x50 per port (0x579559).
- `InputInit` (call at 0x57ED80) gives ports 2..5 their id, and ports 2/3 action 1. Action 1's handler 0x571A20 keeps
  per-port state in the game's 4-slot arrays: bytes `[port + 0x864788]` / `[port + 0x8647A0]` (0x86478C is another
  variable) and the frontend flags 0x50C5D0(port): `[0x7F91D4 + row * 0x1C + port * 4]`, 21 rows of 4 port columns
  and 3 other dwords. Action 1 on ports 4/5 zeroed those other dwords and no pad worked in the menus.
- `PollPortHook` serves ports 2..5 from P3..P6's pads (`PollExtraPort`) with their own bindings (below).
  Player k has port k.
- 0x50D2A0 maps FE masks 1/2/4/8 to ports 0..3; the frontend only uses masks 1/2 (see the pause below).

**Pause by P3/P4.** The pause ctor 0x4EB410 sets the package input mask to 1 << the pausing port (at 0x4EB469), but
frontend input only has masks 1/2. `PauseInputMask` maps it to slot (port & 1), and `PadForDevice` feeds that slot from
the owner's pad until the pause menus close. That pad stays idle until all its buttons are released; otherwise the
still-held Start closed the menu at once.

**Frontend actions in slot 1.** Action tables 1..3 of a pad record are the frontend's (Controls and the profile only
use table 0). The widescreen fix (`ImproveGamepadSupport`) assigns the frontend buttons, START among them, in the
slot-0 pad record only, every frame, so START did nothing in a menu read through slot 1: a pause by P2/P4/P6 only closed
with Back (the game's own P2 pause too). `MirrorFrontendTables` copies slot 0's tables 1..3 into the slot-1 record
before each poll (no-op without the widescreen fix: both records then have the game's defaults). Players 3..6 already
use a copy of the slot-0 record.

**Old widescreen fix builds (`RepairOldWidescreenFixPoll`).** The 0x5CFA10 loop polls slot esi = 0..1 through 0x5C8700
(GetDeviceState at 0x5C8828). Old WidescreenFix builds (the one bundled with underground2.net 1.0.3, 366 KB) add a stick
deadzone there with `call; nop` over `mov esi, eax; test esi, esi; jge` at 0x5C882B. The hook resumes at 0x5C8869 by
writing it over the saved esi on the stack instead of its return address, so the loop stops after slot 0. Slot 1 is
never polled, and its zeroed DIJOYSTATE2 reads POV 0, D-pad up. The game alone shows this only with two real pads;
with the plugin, the virtual pad in slot 1 sends an endless Up to every menu. Before each port-0 poll, the plugin checks
the site. If it holds `E8 rel32 90` and the call goes into a module whose name contains "WidescreenFix", the plugin puts
back the original `8B F0 85 F6 7D 38` once and logs "input: removed an old WidescreenFix joystick hook". The old fix's
deadzone (slot 0 only) is lost with it. Current builds hook the same site with a safetyhook `jmp` (E9) and are left
alone. Test copies that run both builds share `Settings.ini` (`WriteSettingsToFile`). There, the current build saves
`g_RacingResolution` as packed width/height, which the old build reads as an index: device creation fails at
startup, and the game crashes at 0x5D2780 (null device 0x870974). Set it to 0 to go back.

**Device records and bindings.** A record turns its device's state into action bits (+8, 8 bytes, active low) and
analog values (+0x10..+0x13) every frame: 0x5CF220 → 0x5C1840. For joysticks it reads the slot's DIJOYSTATE2 at
0x86FFC0 + slot*0x110 (buttons) and at [0x870760] + slot*0x110 (axes, POV). Bindings: 4 action tables of 32 inputs at
+0x18/+0x98/+0x118/+0x198 (16 buttons, then axis directions/POV), axis setup +0x344..+0x368, POV count +0x36C. The
profile saves table 0 and the axis setup per device GUID (0x5B9420 called at 0x52A42A; load 0x5B9570 at 0x534660).

**Controls for players 3..6.** They have no joystick slot, so their bindings live in the seat store ([players](players.md)).
- The Controls screen (ctor 0x4ECD10) builds its pages with 0x4ACE00 (call at 0x4ECFD5 → `BuildControlsPages`): table
  0x835930, stride 12 {type, ?, record}, 6 max, count at screen+0x158, joypad pages at +0x154. The table is relocated
  to `g_controlsPages[8]` (`RelocateControlsPages`: 77 refs, 0x4ACE69 `mov ebp, 0x835930` and 76 `[reg*4 + 0x835938]`;
  the next object starts at 0x835978). Four pages are appended for P3..P6, all on the slot-0 pad record.
- While one of them is shown (`ControlsPageSeat`: page index and UI_Options_PC_Controller.fng active), that record
  holds the player's bindings and P1's are kept aside (`SetBindSwap`). The game's editing, capture and "Reset Keys To
  Default" then work unchanged. The swap is applied when the joypad page is built (`SetJoypadPageTitle`, called at
  0x4DCA18 inside the builder 0x4DC9F0) and checked every frame. It is undone when the page is left and before the
  profile saves or loads bindings (`ProfileSaveBindings` / `ProfileLoadBindings`).
- In the race, `PollExtraPort` runs 0x5C1840 on a private copy of the slot-0 pad record that holds the player's
  bindings. For the call, the player's pad state (`XPadDiState`: DIJOYSTATE2 with device 0's axis ranges) is put into
  slot 0's two state buffers and restored right after. The result is merged like the game merges a port's devices.

**Force feedback (rumble).** The rumble engine is shared with the PS2 version (SLUS_210.65), so it was restored from
there rather than tuned by feel:
- Events 0x5727B0(type, port, amount) pick an effect from the table 0x7FB7D8 (PS2 0x523070, same parameter blocks) and
  queue its curve on the port's channel (0x5726D0 → 0x572390).
- Channels 0x8653B0 + port*0x60: 2 sub-channels of 0x30, each summing up to 3 curves into an int 0..255 at +0x28
  (0x572470).
- The per-frame update 0x572610 (called at 0x5814AD) hands the values to the pad. On PS2 that is 0x333530(port, v, v)
  with v = sub-channel 0's value (DualShock: small motor on when v > 0x80, large motor = v). On PC it is an empty stub
  (0x4022C0 at 0x57266E).
- `FfPadOutput` (replaces the stub call) records v per port. `FfUpdate` (call at 0x5814AD) runs the update and sends
  large motor = v/255 and small motor = v > 128 to the player's own XInput pad (`XPadSetRumble`), scaled by
  RumbleStrength.
- Rumble is off in the pause menus, outside racing, when the player's Feedback option is off (player settings +4) or
  with Rumble=0. The Feedback option is greyed unless the last DirectInput device enumerated reported
  DIDC_FORCEFEEDBACK (0x5B8240 returns 0x87078C, set at 0x5C1221; only the two option screens read it); it is forced
  selectable while rumble is on.
- The channel state exists for 2 ports (channels 0xC0 bytes, strengths 0x864728, 0x60 bytes indexed type*2 + port).
  Each further pair of ports (2/3 for P3/P4, 4/5 for P5/P6) gets a shadow copy that `RumbleExtraPort` swaps in for
  its events and for one more run of the update; `g_ffPortBase` (2 or 4) maps the game's port 0/1 back to the real one.
- Collisions (0x572E50 → 0x865360) only drive the wheels' constant force, as on PS2 (`ImpactRumbleHook` ignores
  players >= 2). `JoySlotPresent` (0x5BFD70: slot >= 2 → false) guards the driving force feedback 0x406460, which read
  past the 2-slot array.

**Steering wheel check.** The steering filter 0x41DFC0 (slot = player index) picks the wheel routine 0x4061C0 (no
speed-sensitive steering) when 0x5C1000 says the slot has a wheel (0x86FFB4[slot] != 0 && byte 0x86E5BC[slot] != 0,
2 slots), else the pad routine 0x416BB0. Index 4 read slot 0's joystick buttons (0x86FFC4) and the low byte of
0x86E5C0, both non-zero, so player 5's pad was treated as a wheel. `JoySlotWheel` (jump at 0x5C1000) returns false
for slots >= 2. Whether index 4 reads true depends on what slot 0 holds; forced true, P5's full-lock output at speed is about double ([details](bugs/p5-steering-sharp.md)).

**More than 4 Xbox pads (not done).** The WGI path has no 4 pad limit of its own (no documented cap, 8 Xbox pads
reported); the Xbox block of the pad layer, pads 0..3 (`kRealPads`), does. Other games get past it with SDL (merges XInput,
Raw Input, WGI and HIDAPI; matches each Raw Input device to its XInput twin by input activity), GameInput (needs its
redistributable, x86 support unclear), Raw Input / HID (PlayStation pads have no limit, this mod's native DualSense path;
Xbox pads over Bluetooth HID report both triggers on one axis and can't rumble) or Steam Input (only for games started
through Steam).
