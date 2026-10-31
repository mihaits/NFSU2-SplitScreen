# Linux compatibility

Tested 2026-10-31 with build 610e9ca2 (`main` before the headlight fix), up to 4 players: it works, with no missing imports
and no crash files. 5-6P were not tested. Two of about 20 launches exited after 2 s (wait status 40); a retry worked, cause
unknown.

**Setup.** Ubuntu 26.04.1, kernel 7.0.0-38, GNOME Wayland, RTX 4070 Mobile (595.99.02) + Intel Arc, Ultra 7 155H, 2560x1600
at 240 Hz, 133 % scaling. Lutris 0.5.22, umu 1.4.4, GE-Proton11-11, DXVK 3.1.1, win64 prefix, current WidescreenFix.

**What a player needs.**
- GE-Proton with WidescreenFix and `dinput8=n,b`; the asi and ini in `scripts`.
- AC power: on battery the GPU drops to 10 W and the game crawls.
- Native resolution with fractional scaling: the game otherwise sees a virtual X size above the panel's (3840x2400 here).
  Use `PROTON_ENABLE_WAYLAND=1` and `WindowedMode = 5`, with the resolution set in the registry before launch
  (`g_RacingResolution`, high word 0x8000 | width, low word height). Under X11, the game's resolution menu shrinks mode 5
  to 2/3 size. Windows testing only used mode 4.
- Steam closed, for native PlayStation features. Native access relies on the uaccess rule in `60-steam-input.rules` for
  hidraw; a system without it was not tested. `PROTON_SONY_AUTO_XINPUT=1` (default) still ran Sony pads natively.

| Pad | Input | Rumble | Lights, triggers | Speaker | Path |
|---|---|---|---|---|---|
| Xbox USB | yes | yes | no impulse triggers | - | WGI |
| Xbox Bluetooth | yes | yes | impulse triggers with the udev rule below | - | WGI |
| DualSense USB | yes | yes | yes | yes | native |
| DualSense Bluetooth | yes, keeps its slot | yes | yes | dropouts | native |
| DS4 USB | yes | yes | yes | yes | native |
| DS4 Bluetooth | yes | yes | yes | dropouts | native |
| Steam Controller, Steam closed | yes | no (see below) | - | - | WGI 28DE:1102, Bluetooth 1106 |
| Steam Controller, through Steam | yes | yes | - | - | WGI 28DE:11FF |
| Sony pads, through Steam | yes, not doubled | not checked | **lost** | **lost** | WGI 28DE:11FF |

**DualSense USB speaker.** PipeWire's UCM profile exposes the pad as a mono "Internal Mono Speaker" node, yet Wine reports 4
channels (mask 0x33). The stream has 4 ports (AUX0-3) and only AUX0 is linked to the node, so a signal on channel 2 alone,
as on Windows, is silent. Under Wine (`wine_get_version` in ntdll) `UsbSpeakerThread` fills channels 1 and 2 of a DualSense
endpoint. Quiet at first: the node's volume was 0.40; at 1.00 the level is right, and that survives a reconnect.

**Xbox Bluetooth rumble.** It came 1-4 s late while the mod sent every change, up to 125 times a second. Capped at 20 sends a
second under Wine (`Apply` in `XboxWgi.cpp`, always the latest value) it's on time, checked by A/B on the same setup. The pad
(045E:0B20, hid-microsoft) is BLE: each rumble change is a GATT Write Request (handle 0x0022) that BlueZ sends one at a time
and the uhid and hid-microsoft layers above don't slow down. `btmon`: uncapped, the link stayed saturated at 40 writes/s
(median 23 ms each); capped, 14-19/s, 14 ms each, with idle gaps. So the excess most likely queued; the tail after a stop and
the exact queue weren't measured. Wine's process holds the pad's event node; no udev rule is needed.

**Xbox Bluetooth impulse triggers (optional setup).** Without hidraw access Wine reaches the pad through evdev, which has no
trigger motors. Steam's `60-steam-input.rules` covers 045E:0B22 but not this pad's 045E:0B20, so its hidraw node stays
root-only. A mod can't grant itself that access, so this takes one root step. Create
`/etc/udev/rules.d/71-xbox-ble-hidraw.rules` with:
```
KERNEL=="hidraw*", KERNELS=="*045E:0B20*", MODE="0660", TAG+="uaccess"
```
then run `sudo udevadm control --reload` and reconnect the pad. To undo, delete the file and reconnect. Tested on 0B20 only:
Wine's process moves to the pad's hidraw node, the pad still appears once, the triggers buzz and rumble stays on time. Other
Xbox Bluetooth ids may need their own line. Adding 0B20 to Steam's rules (steam-devices) would make this unnecessary.

**Steam Controller (2015) rumble without Steam.** Not possible through Wine: no input node has force feedback. On cable
(hid-steam) the gamepad node has no EV_FF and disappears while the game holds hidraw; on Bluetooth (hid-generic) only
keyboard and mouse nodes exist. Keeping its rumble on XInput instead of WGI changed nothing (tested on Bluetooth). Steam
drives the pad itself. A fix would be native HID haptic pulses over hidraw, writable on both links: a new feature, not started.

**Frame rate** (AC, native, all effects, vsync, 240 cap): 1P 182 median, 155 low; 2P about 145-170; 3P about 110-155; 4P 89
median, 78 low, with the main thread at 98 % of a core and the GPU at 67 %: CPU limited. No physics check at high frame rates.

## To fix
- **Xbox USB impulse triggers.** Not tested with hidraw access; the rule below only matches the Bluetooth pad.
- **Bluetooth speaker dropouts**, worse than Windows, on DualSense and DS4: on the laptop a constant stutter. The reports leave
  on time: a 60 s race held 235 reports per 5 s, 0 underruns, 0 short packets, max late 2.9 ms, no write errors. So the loss
  is below the mod and no mod-side fix is known. It stutters with the DualSense alone on the adapter too. `btmon` (20 s, classic
  HID through bluetoothd's uhid): reports reach the controller on time (gap 21.3 ms, max 29.6), but a quarter take longer than
  the 21.3 ms report period to complete (median 12.6 ms, p99 35.3, max 52.3), and the pad's input pauses over 20 ms about 24
  times a second. So the link itself stalls, probably airtime or retransmits (not visible in HCI); the Intel AX211 shares its
  radio with Wi-Fi. Next checks, not done: a race with Wi-Fi off, the same capture on Windows. Lower bitrate is the only
  mod-side lever (the to-do in sound.md); unknown if it helps.
