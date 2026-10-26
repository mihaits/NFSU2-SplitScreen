# Input and rumble

**Game side.** DirectInput only, and at most 2 joysticks are opened (slots 0x86FFB4[2]). The poll function
0x5CFD60(port, &type, &prev): port 0 polls every device (records at 0x874C40, stride 0x370, max 6; record +0x230 =
joystick slot, keyboard = record 0, type 0x13) and merges them into 0x86EAF0; port 1 returned the same buffer. Players
read port = 0x83A9D8[player]. Input port tables: 0x8655A0, 2 × 0x1C8 (binding node list +0, port id +0x1B4, device type
+0x1B8); poll loop 0x5809C0 → `0x57B0B0` → events at 0x865984.

**XInputDevices.cpp.** It hooks the DirectInput8Create IAT slot 0x783024 and wraps IDirectInput8A.
EnumDevices/CreateDevice expose virtual "Split Screen Pad N" gamepads with full DirectInput emulation (objects with
Xbox names, GetObjectInfo, properties, buffered GetDeviceData — Options > Controls needs all of these). Real DirectInput
controllers are hidden. Pads 0..3 are the real XInput pads, 4..9 debug pads driven through the mailbox, 10..17 native DualSense pads (below).

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
    near 384 samples per half second by trimming the read rate (±1 %, proportional + integral); an underrun lowers
    the rate at once and refills only to one report plus that reserve. The 5 s stats line (Debug) shows reports,
    underruns, peak, the lowest reserve, the rate trim and the worst send lateness.
  - While the speaker streams, other output reports wait at least 125 ms (rumble start/stop and trigger changes 50
    ms): at 40 lightbar reports/s on the same link the speaker dropped out now and then. A state sub-packet (0x90,
    the 0x31 settings block) in place of the haptics one, to carry the lightbar inside the audio reports, silenced the
    speaker.
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
  not finished) L2 has light resistance and buzzes (35 Hz) while braking with the front tyres at their grip limit (above a minimum wheel speed; the rear tyres read
  1.0 whenever the car is slow or stopped, so they aren't used); R2 has light
  resistance, buzzes at 25 Hz in wheelspin (driven wheels at the limit), at 60 Hz at the rev limiter (shift-light rpm) and
  pushes harder for the first 300 ms of a nitrous burn. The grip signal (`wheel+0x170`, 1.0 = at the limit, 0 on all four
  wheels = airborne) and the hold times are described in `parts/pad_lights.cpp`. Everything else sends Off. Only the
  official trigger modes are used (0x05 off, 0x21 feedback, 0x26 vibration); 0xFC..0xFE break the trigger.
  Each player turns them on or off with the "Adaptive Triggers" row of the pause options (`PauseFeedbackAdd`: added
  after the Feedback row, `call 0x54F170` at 0x4C4985, when the pausing player's pad is a DualSense). The choice is
  saved as `[PlayerN] AdaptiveTriggers` in `NFSU2SplitScreen_Players.ini`; without one, the ini's `AdaptiveTriggers`
  applies.
- Tested with 4 native DualSense pads over Bluetooth at once, next to 2 Xbox pads (5-6 player races, 2026-10-24).

**Pad resolver (`PadForDevice`).**
- Outside split: device 0 = the last active XInput pad.
- Split menus: only the current FE player's pad (its joined pad on the car select, else the last active pad).
- Racing: each player's assigned pad. A pad that turns off keeps its player until it comes back (DualSense: same
  slot, see above). **Takeover** (`UpdatePadTakeover`): START on a connected pad no player has gives it the first
  player whose pad is off ("player N's pad P is off: pad Q takes over"); `RacePad` reads it idle until its buttons are
  released, so that START doesn't pause.
- A missing pad is reported idle, never as DIERR_INPUTLOST (the game would drop the slot).
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

**More than 4 Xbox pads (not done).** XInput stops at 4. Other games get past that with SDL (merges XInput, Raw Input,
Windows.Gaming.Input and HIDAPI; matches each Raw Input device to its XInput twin by input activity),
Windows.Gaming.Input (no documented cap, 8 Xbox pads reported; WinRT but callable from Win32; `Gamepad` reads only
while the window is in the foreground), GameInput (needs its redistributable, x86 support unclear), Raw Input / HID
(PlayStation pads have no limit, this mod's native DualSense path; Xbox pads over Bluetooth HID report both triggers on
one axis and can't rumble) or Steam Input (only for games started through Steam). For this mod: a WGI backend for Xbox
pads past the XInput 4 (built into Windows 10/11); check that it works from the x86 DLL and with the game focused.
