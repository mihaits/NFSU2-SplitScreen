# Linux

The mod runs through Proton. It's tested up to 4 players with GE-Proton through Lutris; 5-6 players aren't tested.

## Setup
1. Install the game with GE-Proton and WidescreenFix, with the override `dinput8=n,b`.
2. Copy `NFSU2SplitScreen.asi` and `NFSU2SplitScreen.ini` into the game's `scripts` folder.
3. Steam Input hides the lightbar, triggers and speaker of PlayStation pads. Turn it off for the game (Properties →
   Controller → Disable Steam Input) if you run it through Steam.

## Controllers
| Pad | Rumble | Lights and triggers | Speaker |
|---|---|---|---|
| Xbox, USB | yes | no impulse triggers | - |
| Xbox, Bluetooth | yes | impulse triggers with the [setup below](#xbox-bluetooth-impulse-triggers) | - |
| DualSense, USB | yes | yes | yes |
| DualSense, Bluetooth | yes | yes | yes, see [speaker stutter](#bluetooth-speaker-stutter) |
| DualShock 4, USB | yes | yes | yes |
| DualShock 4, Bluetooth | yes | yes | yes, see [speaker stutter](#bluetooth-speaker-stutter) |
| Steam Controller | through Steam only | - | - |

- Two Xbox pads of the same model can swap players when they reconnect.
- If the DualSense USB speaker is quiet, raise its volume in the system's sound settings.

## Optional setup
### Bluetooth speaker stutter
Add this under `[LE]` in `/etc/bluetooth/main.conf`:
```
ScanIntervalAutoConnect=1024
ScanWindowAutoConnect=16
```
then run `sudo systemctl restart bluetooth`. A DualSense may still drop out now and then, about as often as on Windows.

### Xbox Bluetooth impulse triggers
Create `/etc/udev/rules.d/71-xbox-ble-hidraw.rules` with:
```
KERNEL=="hidraw*", KERNELS=="*045E:0B20*", MODE="0660", TAG+="uaccess"
```
then run `sudo udevadm control --reload` and reconnect the pad. To undo, delete the file and reconnect. This is tested on
pad model 045E:0B20; other Xbox Bluetooth pads may need their own line. Impulse triggers don't work over USB.
