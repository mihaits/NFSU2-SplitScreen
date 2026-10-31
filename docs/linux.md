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
| Xbox Bluetooth | yes | 1-4 s delay | no impulse triggers | - | WGI |
| DualSense USB | yes | yes | yes | yes | native |
| DualSense Bluetooth | yes, keeps its slot | yes | yes | dropouts | native |
| DS4 USB | yes | yes | yes | yes | native |
| DS4 Bluetooth | yes | yes | yes | dropouts | native |
| Steam Controller, Steam closed | yes | **no** | - | - | WGI 28DE:1102 |
| Steam Controller, through Steam | yes | yes | - | - | WGI 28DE:11FF |
| Sony pads, through Steam | yes, not doubled | not checked | **lost** | **lost** | WGI 28DE:11FF |

**DualSense USB speaker.** PipeWire's UCM profile exposes the pad as a mono "Internal Mono Speaker" node, yet Wine reports 4
channels (mask 0x33). The stream has 4 ports (AUX0-3) and only AUX0 is linked to the node, so a signal on channel 2 alone,
as on Windows, is silent. Under Wine (`wine_get_version` in ntdll) `UsbSpeakerThread` fills channels 1 and 2 of a DualSense
endpoint. Quiet at first: the node's volume was 0.40; at 1.00 the level is right, and that survives a reconnect.

**Frame rate** (AC, native, all effects, vsync, 240 cap): 1P 182 median, 155 low; 2P about 145-170; 3P about 110-155; 4P 89
median, 78 low, with the main thread at 98 % of a core and the GPU at 67 %: CPU limited. No physics check at high frame rates.

## To fix
- **Steam Controller rumble without Steam.** `XboxWgi.cpp` treats any pad off its Xbox 360 list as having trigger motors, so
  its rumble goes through WGI. Try limiting that path to vendor 045E. Unproven.
- **Xbox impulse triggers.** The mod logs and sends them ("impulse: pad 1 L 0.00 R 0.50"), nothing vibrates. Likely Wine's
  WGI; not checked.
- **Xbox Bluetooth rumble delay.** Xbox only. Its hidraw node (045E:0B20) is root-only while Sony and Valve pads have
  uaccess; the link is unproven and no udev rule was tried.
- **Bluetooth speaker dropouts**, worse than Windows, on DualSense and DS4. 235 reports per 5 s, 0 underruns, so the loss is
  probably after the queue, in Wine or the Bluetooth stack. Not measured.
