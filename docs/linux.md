# Linux compatibility

Tested through Lutris with GE-Proton and DXVK on GNOME Wayland, up to 4 players: it works, with no missing imports and no
crash files. 5-6P were not tested. A launch that exits after 2 s worked on a retry; cause unknown.

**What a player needs.**
- GE-Proton with WidescreenFix and `dinput8=n,b`; the asi and ini in `scripts`.
- Native resolution with fractional scaling: the game otherwise sees a virtual X size above the panel's.
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
| DualSense Bluetooth | yes, keeps its slot | yes | yes | yes, with the scan setting below | native |
| DS4 USB | yes | yes | yes | yes | native |
| DS4 Bluetooth | yes | yes | yes | yes, with the scan setting below | native |
| Steam Controller, Steam closed | yes | no (see below) | - | - | WGI 28DE:1102, Bluetooth 1106 |
| Steam Controller, through Steam | yes | yes | - | - | WGI 28DE:11FF |
| Sony pads, through Steam | yes, not doubled | not checked | **lost** | **lost** | WGI 28DE:11FF |

**Xbox pads through WGI.** Xbox and Steam Controller pads read and rumble through Wine's WGI, with no doubled pad next to
native Sony pads. `NonRoamableId` reads empty, so a pad number is kept by VID:PID: pads of different models get their own
number back in any order, and two of one model can swap. If WGI lists no pad, pads read through XInput (not seen here).

**DualSense USB speaker.** PipeWire's UCM profile exposes the pad as a mono "Internal Mono Speaker" node, yet Wine reports 4
channels (mask 0x33). The stream has 4 ports (AUX0-3) and only AUX0 is linked to the node, so a signal on channel 2 alone,
as on Windows, is silent. Under Wine (`wine_get_version` in ntdll) `UsbSpeakerThread` fills channels 1 and 2 of a DualSense
endpoint. If it's quiet, raise that node's volume in the system's sound settings.

**Xbox Bluetooth rumble.** It came 1-4 s late while the mod sent every change, up to 125 times a second. Capped at 20 sends a
second under Wine (`Apply` in `XboxWgi.cpp`, always the latest value) it's on time, checked by A/B on the same setup. The pad
(045E:0B20, hid-microsoft) is BLE: each rumble change is a GATT Write Request (handle 0x0022) that BlueZ sends one at a time
and the uhid and hid-microsoft layers above don't slow down. `btmon`: uncapped, the link stayed saturated at 40 writes/s
(median 23 ms each); capped, 14-19/s, 14 ms each, with idle gaps. So the excess most likely queued; the tail after a stop and
the exact queue weren't measured. Wine's process holds the pad's event node; no udev rule is needed.

**Bluetooth speaker dropouts (optional setup).** Constant stutter with default settings, caused by BlueZ's LE background scan,
which runs while any bonded LE device (a pad or mouse, say) is disconnected and takes the radio from the pad's BR/EDR link 30
ms out of every 60 ms. Reports leave the mod on time, and Wi-Fi off or a single pad doesn't help. To cut it, under `[LE]` in `/etc/bluetooth/main.conf`:
```
ScanIntervalAutoConnect=1024
ScanWindowAutoConnect=16
```
then `sudo systemctl restart bluetooth` (10 ms every 640 ms). On a DualSense, input gaps over 20 ms fell from about 24 to 0.7 a second (Windows: 0.3); a DS4 sounded
clean. The DualSense's `speaker link:` log lines show these gaps. LE devices still reconnected within a couple of seconds.

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
On USB the triggers stay off and the rule can't help: the pad (045E:02EA) uses the xpad driver, which has no hidraw node, and
evdev has no trigger path. So Bluetooth with the rule is the working path on Linux.

**Steam Controller (2015) rumble without Steam.** Not possible through Wine: no input node has force feedback. On cable
(hid-steam) the gamepad node has no EV_FF and disappears while the game holds hidraw; on Bluetooth (hid-generic) only
keyboard and mouse nodes exist. Keeping its rumble on XInput instead of WGI changed nothing (tested on Bluetooth). Steam
drives the pad itself, so use Steam with this pad. Native HID haptic pulses over hidraw would work but aren't worth doing.

## To fix
- **Remaining DualSense Bluetooth speaker dropouts.** With the scan setting above, a few are left, about like Windows. A DS4 with
  as many link gaps is clean, so it may be specific to the DualSense's audio format or buffer; cause unknown. To pick up with
  the adaptive bitrate work (sound.md).
