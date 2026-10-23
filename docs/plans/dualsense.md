# Plan: native DualSense (DS5) support

An implementation spec for an executing session. Start from the root `CLAUDE.md`, then read this file,
[dualsense-protocol.md](dualsense-protocol.md) (byte-level HID reference) and [input.md](../input.md). All game
addresses below were **verified live** on 2026-10-23 (1P circuit, 1P drag, 3P split) unless marked *verify*.

Workflow rule (user): one step at a time. Implement → `dev.sh deploy` → the user tests with the real pad → user
approves → `dev.sh commit` (README + `docs/input.md` + `docs/addresses.md` in the same commit) → next step. If a bug
survives two attempts, write `docs/bugs/<name>.md` (format in `docs/bug-report.md`) and stop.

## Goal
DualSense pads work without DS4Windows: input, rumble, player LEDs that show who is in control, a lightbar showing
RPM (nitrous blue, player colour on request), race lights (pause, countdown, finish), and adaptive triggers driven by
the tyres' grip. Input goes through the existing pad layer (`src/XInputDevices.cpp`), so joining, bindings, P3/P4,
pause ownership and rumble work without changes.

Test hardware: one DualSense, revision unknown. Step 1 logs its firmware. The user normally runs an Xbox pad plus the
DualSense through DS4Windows (HidHide off). Native mode needs DS4Windows closed.

## Decisions (user, 2026-10-23)
- **DS4Windows:** native support is on by default (`NativeDualSense = 1`). While `DS4Windows.exe` runs and exposes its
  virtual Xbox pad (a present device under a `Nefarius\ViGEmBus` bus; open but stopped doesn't count), the plugin
  doesn't open DualSense pads and logs why, once per change.
- **Player LEDs:**
  - From the car-select join (Start) until the race is exited, the pad shows its player number.
  - At all other times, the pad that controls the game shows 1.
  - Every other DualSense flashes **all 5** LEDs.
- **Lightbar while racing:** RPM from green to red, flashing red at the shift point, blue while nitrous burns.
- **Touchpad click** switches that player's lightbar between RPM and the player's solid minimap colour. This works in
  split screen only. **Mute** and touch movement are unused, and so is the PS button.
- **Extras, each its own step:** pause dim, countdown lights, finish colour, adaptive triggers. The user said: ship
  the triggers only if slip data is achievable. It is (see "Grip"), so they're in.

## Game data (live-verified)
Each local player `p` = `GetLocalPlayer(k)` (k 0..3). Its car is `car = *(p + 4)`.

| what | how |
|---|---|
| player's port | `*(int*)(p + 8) & 3` (as `FfUpdate` uses) |
| **engine RPM (what the tach shows)** | if `spec = *(car+0x14)`, `*(int*)(spec+4) == 1`, `*(u8*)(spec+0x900) == 0`, `r = *(car+0x968)` non-null, and not drag/drift: `*(float*)(r + 0x58)`. Otherwise `*(float*)(car + 0x440)`. Seen: launch 5600 engine vs 970 at car+0x440. car+0x440 follows the wheels and is what the drag tach path starts from. |
| idle / max RPM | `eng = *(car + 0x20)`: idle `*(float*)(eng+0x1F0)` (850), max `*(float*)(eng+0x1F4)` (9500) |
| gear | `*(int*)(car + 0x434)`: 0 reverse, 1 neutral, 2 = 1st … The automatic box shifts 2 → 3 → 4 by itself. |
| **shift-light RPM** | Only when `*(int*)(car+0x28) == 1`, which holds for player cars. `drv = *(*(car+0x34) + 0x4C)`; `up = *(float*)(drv + 0x84 + 4*gear)`; `f = *(float*)(0x7FBF60 + 4*(gear + 9 * *(int*)(drv+0xF0))) * *(float*)(drv+0xE4)`; `shift = up - *(float*)0x7FBF5C * f`. The tach's shift light (0x4C6ACF..0x4C6B45) is on when rpm ≥ shift. Seen: 1st-gear `up` 9400, k 0.95, f 360 → 9058. Each car differs (P3's car: 5688). |
| **nitrous burning** | `burn = *(u32*)(car + 0x40 + 0x420)`; on while `burn != 0 && burn != 0x7FFFFFFF` (burn start time). Nitrous object at car+0x40: +0x41C remaining, +0x418 capacity. Default pad binding: LB. |
| drag / drift race | `IsDragRace()` / `IsDriftRace()` (gauges.cpp) |
| **race stage** | `*(int*)*(int*)0x890118`: 0 none, 1 flyby, 2 prerace, 3 countdown, 4 racing. The game mirrors it in 0x82B8D4 (writer 0x483DBD). The countdown always lasted 3.0 s, so 3/2/1 change every second from the frame stage 3 starts, and GO is stage 4. |
| game clock | `*(float*)0x7FB718`, seconds. Use it, or the plugin's own frame time, to time the countdown digits. |
| finished / position | `racer = *(car + 0x1C)` (`RacerOfLocalPlayer`); finished when `*(float*)(racer+0x14) >= 1` (`PlayerFinished(k)`); position = `*(u8*)(racer + 0xA)`, 1-based |
| game state | `*(int*)0x8654A4`: 3..5 loading, 6 in a race (results included). Anything else = frontend. |
| pause | `PauseMenuOpen()` (input_ports.cpp); pausing port `g_pausePort` |
| **grip used per wheel** | `mover = *(car + 0x34)`; wheels `w[i] = *(mover + 0x38 + 4*i)`, i = 0 FL, 1 FR, 2 RL, 3 RR (vtable 0x7A20E8). See "Grip" below. *verify:* left/right order; it doesn't matter for the triggers. |
| minimap colours (RGB) | P1 (48, 207, 48) green, P2 (207, 48, 48) red: the game's markers, read live from `g_mmMarker[0/1]` data (4 dwords B,G,R,A at `*(obj+0x2C)`). P3 (40, 90, 230), P4 (230, 210, 40) come from `kMarkerColour` (hud_split.cpp, stored B,G,R,A). |

### Grip (the slip signal for the triggers)
The tyre model at 0x59D690..0x59D764 computes, per wheel:
- the grip limit from load and friction (`[esp+0x18]`),
- the requested force (`[esp+0x1C]`).

If the request exceeds the limit, it scales the forces at +0x124 (longitudinal) and +0x128 (lateral) down to the
limit. It then stores **`*(float*)(w + 0x170)` = request / limit, clamped to 1**. So 1.0 means the tyre is at its limit
(sliding or spinning), and lower values are how much grip is in use.

The code also forces 1 when `*(u8*)(w+0x160)` or `*(int*)(w+0x10C)` is set. Those are not airborne flags: they stayed
0 through a jump, and their meaning is unknown.

**Jump test** (2026-10-23, car lifted 2 m at 21 m/s under full throttle):
- In flight: grip **0.00** on all four wheels for the whole 0.45 s.
- Landing: it ramped back up normally (rear 0.13 → 0.93), with no false 1.0.
- A separate one-frame 1.0 on two wheels showed up later (a kerb or bump).

So airborne never reads as "at the limit". Airborne = all four wheels < 0.005. Filter single-frame spikes with a short
hold time (below).

Live trace (RWD car):

| situation | value |
|---|---|
| coasting | 0.03 |
| steady full throttle | rear 0.5 |
| launch | rear 1.0 for about 1.4 s (wheelspin; rear wheel speed > front at +0x28) |
| hard braking from 150 km/h | front 0.3, rear 1.0 |
| full-lock turn at speed | front 1.0 (understeer) |

Other fields:
- **+0x178** is brake input per wheel (0.95 front, 0.98 rear), not lock-up.
- **+0x28** is wheel angular speed. Wheels never lock under braking, so there is no ABS-style lock signal. The
  brake-side effect uses "grip at the limit while braking" instead.

`tools/telemetry.py` (new, read-only) streams all of this per player live. Use it while testing:

```bash
python -I _splitscreen_mod/github/tools/telemetry.py 0.25
```

## Code: where things go

### 1. `src/DualSense.cpp` + `src/DualSense.h` (new, HID backend; own namespace, like XInputDevices.cpp)
- **Build:** add `src\DualSense.cpp` and `hid.lib setupapi.lib` to the `cl` line in `build.bat`. Include
  `<hidsdi.h>` and `<setupapi.h>`.
- **API** (C++ functions, header style like `XInputDevices.h`):
  ```c
  bool DsInstall(bool enabled, int rumbleMode);   // starts the hotplug thread; rumbleMode -1 auto, 1 v1, 2 v2
  int  DsCount();                                 // native slots 0..3 (kDsMax = 4)
  bool DsState(int i, XPadLikeState* out, DsExtra* extra);   // false if slot empty
  void DsSetOutput(int i, const DsOutput& o);     // whole desired output state; the I/O thread sends on change
  ```
  - `DsExtra`: `touchClick`, `mute`, `ps` (bools), `battery`, `usb`.
  - `DsOutput`: `uint8 rgb[3]`, `uint8 playerLeds` (the 0x20 bit is added inside), `uint8 smallMotor`,
    `uint8 largeMotor`, `uint8 trigR[11]`, `uint8 trigL[11]`.
- **Threads:** one hotplug thread rescans every 2 s and one I/O thread per open pad.
  - Rescan: enumerate VID 054C, PID 0CE6/0DF2. Check `DS4Windows.exe` with `CreateToolhelp32Snapshot`; if it runs,
    close all native pads and open none.
  - I/O thread: overlapped `ReadFile` loop into the slot's latest state, under a `CRITICAL_SECTION`. When the desired
    output differs from the last sent, or at the keep-alive times in the protocol doc, build the report (USB 0x02 or
    BT 0x31 + CRC) and `WriteFile` it, overlapped with a 100 ms timeout.
  - On a read error: close the slot, log it, leave it free for the rescan.
- **Init per pad**, in this order (protocol doc "Init sequence"):
  1. Read feature 0x20 and log `hardware/firmware/update version` and USB/BT.
  2. Read feature 0x05.
  3. Send the light-out report.
  4. Pick rumble v1/v2.
- **Mapping to the XInput-style state** (the `XPadState` layout in XInputDevices.cpp):

  | DualSense | XInput |
  |---|---|
  | Cross | A |
  | Circle | B |
  | Square | X |
  | Triangle | Y |
  | L1 / R1 | LB / RB |
  | Create | Back |
  | Options | Start |
  | L3 / R3 | LTHUMB / RTHUMB |
  | hat | D-pad bits (diagonals set two bits) |
  | L2 / R2 analog | leftTrigger / rightTrigger |
  | sticks | `(v - 128) * 258`, clamped to ±32767; **Y negated** |

  PS, Mute and the touchpad go only into `DsExtra`. Bump `packet` on every new report.

### 2. `src/XInputDevices.cpp` / `.h`
- **Pad indices:**
  - Native DualSense slots become pads **8..11**: `kDsFirst = 8`, `kMaxPads = 12`.
  - Pads 4..7 stay the debug pads, so `fe.py pad`, the mailbox (cmd 4) and `trainer.py` keep working.
  - Every `kMaxPads`-sized array grows automatically; `g_virtual*` stays `kMaxPads - kRealPads`. Re-size the
    `XPadSetVirtual` bound check to 4: today it uses `kMaxPads - kRealPads`, which becomes 8.
- **`GetPad`:** pads 8..11 → `DsState(pad - 8, ...)`.
- **`XPadSetRumble`:** pads 8..11 → store the two motors in that slot's `DsOutput` (scale 0..1 → 0..255) and call
  `DsSetOutput`.
- **New exports:**
  ```c
  bool XPadIsDualSense(int pad);
  void XPadSetLights(int pad, const uint8_t rgb[3], uint8_t playerLeds);            // keeps the motors/triggers
  void XPadSetTriggers(int pad, const uint8_t l[11], const uint8_t r[11]);
  bool XPadTouchClickPressed(int pad);   // edge since the last XPadUpdate, like XPadStartPressed
  ```
  Keep one merged `DsOutput` per native pad here, so rumble, lights and triggers never overwrite each other.
- **`InstallXInputDevices`:** also calls `DsInstall` (pass the ini values through new parameters).
- **DS4Windows note:** while it runs, its virtual Xbox pad is a normal XInput pad (0..3), as today.

### 3. `src/parts/pad_lights.cpp` (new part)
- **Include** after `parts/camera.cpp` in `NFSU2SplitScreen.cpp`, because it uses `FfUpdate`'s helpers,
  `GetLocalPlayer`, `PauseMenuOpen` and `g_assignedPad`.
- **Call** `UpdatePadLights()` from `LayoutApplyHook` (render_passes.cpp, right after `UpdateSeats()`). That is the
  per-frame entry, which also runs in menus.
- **Comment block** at the top, in the style of the other parts: the decisions above, the addresses, and why.

**Per frame, for each pad with `XPadIsDualSense(pad)`:**

1. **Player slot of the pad (`seat`):**
   - A pad has a seat k when `g_assignedPad[k] == pad` and the split session is on: `g_inCarSelect` is true, or
     game state is 3..6 with `SplitMode()`.
   - When the game goes back to the frontend after a race (state leaves 3..6 while not on the car select), seat
     numbers stop showing. `g_assignedPad` itself is left alone.
   - A keyboard player has no pad, so nothing to show.
2. **Controlling pad (`ctl`):** factor the frontend part of `PadForDevice` (seats.cpp) into
   `static int ControllingPad()`:
   - not split → `XPadLastActive()`
   - car select → the pad of the seat whose turn it is (or `XPadLastActive()`)
   - other split menus → `XPadLastActive()`
   - split racing → no single controller; only seats matter
3. **Player LEDs:**
   - seat k → pattern of k+1 (0x04, 0x0A, 0x15, 0x1B)
   - otherwise, `pad == ctl` → 0x04
   - otherwise → 0x1F when `(frame time % 1 s) < 0.5 s`, else 0 (1 Hz flash of all five)
4. **Lightbar** (brightness from the ini as a 0..100 % scale on RGB):
   - **Racing** (state 6, stage 4, not paused) and the pad drives local player k (split: seat k; 1P: `pad == ctl`,
     k = 0):
     - colour mode (toggled, split only) → the player colour
     - nitrous burning → blue (0, 0, 255)
     - rpm ≥ shift → red flashing at 4 Hz (255, 0, 0) / off
     - otherwise `t = clamp((rpm − idle) / (shift − idle), 0, 1)`, hue = 120° × (1 − t), full saturation and value
       (HSV → RGB)
   - **Pause** (step 5) → that player's colour at 25 %.
   - **Countdown** (step 6) → stage 3: red at second 0 and 1, amber at second 2; green for 1 s after stage 4 starts;
     then RPM.
   - **Finished** (step 7) → position 1 gold (255, 180, 0), 2 silver (200, 200, 220), 3 bronze (205, 100, 30),
     other white at 30 %, until state leaves 6.
   - **Not racing**, seat or `ctl` → the player colour (P1 for ctl) at 30 %; any other pad → off.
5. **Touchpad toggle** (step 4): in split racing, `XPadTouchClickPressed(pad)` flips `g_lightColourMode[k]`. Keep it
   per seat until the game closes, and log it.
6. **Player colours:** normalise the minimap RGB so the largest channel is 255 → P1 (59, 255, 59), P2 (255, 59, 59),
   P3 (44, 100, 255), P4 (255, 232, 44). LEDs wash pale colours out, so tune with the user if they look off.
7. **Send:** `XPadSetLights(pad, rgb, leds)`. The pad layer only sends on change.

**Triggers** (step 8), same file, `UpdatePadTriggers(pad, k)`, only racing + not paused + `AdaptiveTriggers = 1`;
everything else gets Off on both triggers:
- **L2 (brake):**
  - base feedback resistance: start zone 2, strength 3
  - while braking (L2 > 40) and `max(grip over grounded wheels) >= 0.98`: vibration from zone 2, amplitude 5,
    35 Hz (the GT7 "at the limit" buzz)
- **R2 (throttle):**
  - base feedback resistance: start zone 3, strength 2
  - while throttle (R2 > 40) and the driven wheels' grip ≥ 0.98: vibration, amplitude 4, 25 Hz (wheelspin). Driven
    wheels = the axle with the larger +0x124 under throttle, or all four if both are within 10 %.
  - rpm ≥ shift: vibration, amplitude 3, 60 Hz (limiter buzz)
  - first 300 ms of a nitrous burn: feedback strength 8 from zone 0 (kick)
- **At the limit** = grip ≥ 0.98 held for ≥ 50 ms (about 3 frames). This ignores the one-frame kerb spikes seen in
  the jump test. Release after 100 ms below 0.98, so the buzz doesn't chatter.
- **Airborne** (all four wheels < 0.005) → base resistance only, no vibration.
- **Scaling:** `TriggerStrength` ini (0..100) scales amplitudes and strengths, minimum 1 when non-zero.
- **Send rate:** change the effect bytes only when the chosen effect changes, not every frame.

### 4. Ini (`src/NFSU2SplitScreen.ini` and the dev ini in `scripts/`), read in `LoadConfig`
```ini
[INPUT]
; 1 = PlayStation 5 DualSense pads work without DS4Windows (off automatically while DS4Windows runs).
NativeDualSense = 1
; DualSense rumble mode: 0 = auto (by firmware), 1 = old firmware mode, 2 = new firmware mode.
DualSenseRumbleMode = 0

[LIGHTS]
; DualSense lightbar: 1 = RPM / nitrous / race lights, 0 = off.
Lightbar = 1
LightbarBrightness = 100
; DualSense player LEDs show the player number.
PlayerLeds = 1
; DualSense adaptive triggers (resistance, grip-limit and limiter vibration).
AdaptiveTriggers = 1
TriggerStrength = 100
```

### 5. Docs, in the same commit as each step
- `docs/input.md`: new section "Native DualSense": backend, pad indices 8..11, DS4Windows rule, lights policy,
  triggers.
- `docs/addresses.md`: car RPM / shift / nitrous / grip fields, race stage 0x890118, game clock 0x7FB718, racer +0xA.
- `docs/tooling.md`: `telemetry.py` and `drcheck.py` (both new, untracked, from the planning session).
- `README`: features, ini, to-do.

## Steps (one commit each, after the user's approval)

1. **Native input + rumble.** Backend, pad indices, mapping, rumble, DS4Windows auto-skip, firmware log, hotplug.
   Commit `tools/telemetry.py` and `tools/drcheck.py` + their tooling.md lines here too.
   - Test (USB first, then BT if the user can pair it), DS4Windows closed:
     - menus
     - a 1P race
     - split join with the DualSense as P1, then as P2
     - P3 with the Xbox pad
     - rumble on impacts
     - Options > Controls lists and binds it
     - unplug and replug mid-race (the pad reads idle, then works again)
     - start DS4Windows with the game running → the log says native off; input through DS4Windows' pad; no double
       input
   - Done: the log shows firmware versions and transport. Nothing changes for the Xbox pad, the keyboard or the
     debug pads.
2. **Player LEDs.**
   - Test:
     - main menu with the DualSense used last → 1
     - touch the Xbox pad → the DualSense flashes all 5
     - split: join as P2 → 2 through the race, results and restart
     - quit to the menus → back to 1 or flashing
     - P3/P4 patterns: join as P3 with the DualSense
3. **Lightbar RPM + nitrous.**
   - Test in 1P and split, compared with the HUD:
     - idle green
     - turns red as RPM rises
     - flashes exactly when the tach's shift light comes on
     - blue while nitrous burns
     - at least two cars with different redlines
     - a drag race (manual gears, drag tach)
     - drift
   - Use `telemetry.py` to compare numbers.
4. **Touchpad toggle to the player colour.**
   - Test: each player's colour matches their minimap marker; a click toggles back; it doesn't work outside split
     racing.
5. **Pause dim.** Test: pause in 1P and split (P3 pauses too); colours come back on resume.
6. **Countdown lights.** Test: red, red, amber with 3-2-1, green on GO, then RPM. Also after a restart from the pause
   menu.
7. **Finish colour.** Test: finish 1st, then lose a race. The colour holds until the race is left.
8. **Adaptive triggers.**
   - Test with the user: launch wheelspin buzz on R2, limiter buzz, nitrous kick, brake-limit buzz on L2 under hard
     braking, quiet when cruising, no buzz in the air or on landing (a jump: Free Run or a track with a ramp;
     `telemetry.py`'s `air` column shows it).
   - Tune strengths. `AdaptiveTriggers = 0` → triggers free.

## Done criteria
- The DualSense works natively with DS4Windows closed. With DS4Windows running, everything behaves as before.
- Player LEDs, lightbar and triggers behave as specified on every screen, in 1P and 2-4P. The Xbox pad, the keyboard
  and the debug tooling are unchanged.
- Every step committed after the user's approval, with its docs.
- `docs/plans/dualsense.md` and `dualsense-protocol.md` deleted in the last step's commit. The protocol facts that
  matter move into `docs/input.md`.

## Open points to settle with the user while executing
- Flash rates and menu brightness: tune by eye.
- Only one DualSense is available. Two native pads are untested; the docs should say so.
- If the firmware log shows an `update_version` of 0x0215..0x0223 and rumble feels wrong, try
  `DualSenseRumbleMode = 1`.
