# Input and rumble

**Game side.** DirectInput only, and at most 2 joysticks are opened (slots 0x86FFB4[2]). The poll function
0x5CFD60(port, &type, &prev): port 0 polls every device (records at 0x874C40, stride 0x370, max 6; record +0x230 =
joystick slot, keyboard = record 0, type 0x13) and merges them into 0x86EAF0; port 1 returned the same buffer. Players
read port = 0x83A9D8[player]. Input port tables: 0x8655A0, 2 × 0x1C8 (binding node list +0, port id +0x1B4, device type
+0x1B8); poll loop 0x5809C0 → `0x57B0B0` → events at 0x865984.

**XInputDevices.cpp.** It hooks the DirectInput8Create IAT slot 0x783024 and wraps IDirectInput8A.
EnumDevices/CreateDevice expose virtual "Split Screen Pad N" gamepads with full DirectInput emulation (objects with
Xbox names, GetObjectInfo, properties, buffered GetDeviceData — Options > Controls needs all of these). Real DirectInput
controllers are hidden (ini). Virtual pads 4..7 (after the 4 real XInput pads) are debug pads driven through the mailbox.

**Pad resolver (`PadForDevice`).**
- Outside split: device 0 = the last active XInput pad.
- Split menus: only the current FE player's pad (its joined pad on the car select, else the last active pad).
- Racing: each player's assigned pad.
- A missing pad is reported idle, never as DIERR_INPUTLOST (the game would drop the slot).
- While the Controls screen shows the P2 page, slot 2 is fed by the last active pad (`UpdateEditingSlot2`).

**Per-port merge.** Ports 0/1 are merged per device slot, which separates P1 and P2.

**Keyboard player.** The keyboard (record 0, slot 0, type 0x13) is left out of the slot merge and goes to one port
(`KeyboardPort`): racing, its player's port 0..3 (ports 2/3 in `PollExtraPort`); during a pause the menu's port, only
when its player paused (`g_pausePort`; P3/P4: port & 1); on the car select only on its player's turn
(`PadForDevice` gives that turn no pad); elsewhere in the split menus port 0. After a change it waits until every key
is up. Its player's pause menus show Feedback greyed and off: `FfCapable` (jump at 0x5B8240) returns false, and
`SyncPlayerSettings` clears the game entry's +4 while they are open and puts it back after (the setting is kept). The
mouse free camera follows the keyboard player.

**Players 3/4.**
- The port tables are relocated to `g_portTables[4]` (`RelocatePortTables`: 15 refs, loop bounds 0x57B3AC/0x580AD4,
  static ctor/dtor counts 0x77D9CB/0x782296). This runs before the game's static init.
- The binding node pool is raised from 0xA0 to 0x140 (0x579559).
- `InputInit` (call at 0x57ED80) gives ports 2/3 their id and action 1.
- `PollPortHook` serves ports 2/3 from P3/P4's XInput pads (`PollExtraPort`) with their own bindings (below).
  Players 3/4 have ports 2/3.
- Player port = index 0..3 (0x50D2A0 maps FE masks 1/2/4/8 to 0..3).

**Pause by P3/P4.** The pause ctor 0x4EB410 sets the package input mask to 1 << the pausing port (at 0x4EB469), but
frontend input only has masks 1/2. `PauseInputMask` maps it to slot (port & 1), and `PadForDevice` feeds that slot from
the owner's pad until the pause menus close. That pad stays idle until all its buttons are released; otherwise the
still-held Start closed the menu at once.

**Device records and bindings.** A record turns its device's state into action bits (+8, 8 bytes, active low) and
analog values (+0x10..+0x13) every frame: 0x5CF220 → 0x5C1840. For joysticks it reads the slot's DIJOYSTATE2 at
0x86FFC0 + slot*0x110 (buttons) and at [0x870760] + slot*0x110 (axes, POV). Bindings: 4 action tables of 32 inputs at
+0x18/+0x98/+0x118/+0x198 (16 buttons, then axis directions/POV), axis setup +0x344..+0x368, POV count +0x36C. The
profile saves table 0 and the axis setup per device GUID (0x5B9420 called at 0x52A42A; load 0x5B9570 at 0x534660).

**Controls for players 3/4.** They have no joystick slot, so their bindings live in the seat store ([players](players.md)).
- The Controls screen (ctor 0x4ECD10) builds its pages with 0x4ACE00 (call at 0x4ECFD5 → `BuildControlsPages`): table
  0x835930, stride 12 {type, ?, record}, 6 max, count at screen+0x158, joypad pages at +0x154. Two pages are appended
  for P3/P4, both on the slot-0 pad record.
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
  P3/P4 (ports 2/3) get a shadow copy that `RumbleExtraPort` swaps in for their events and for a second run of the
  update.
- Collisions (0x572E50 → 0x865360) only drive the wheels' constant force, as on PS2 (`ImpactRumbleHook` ignores
  players >= 2). `JoySlotPresent` (0x5BFD70: slot >= 2 → false) guards the driving force feedback 0x406460, which read
  past the 2-slot array.
