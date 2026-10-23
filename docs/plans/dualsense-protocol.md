# DualSense HID protocol reference (for plans/dualsense.md)

Sources, read on 2026-10-23, nothing downloaded:
- **[L]** Linux `drivers/hid/hid-playstation.c` (Sony-authored)
- **[S]** SDL `src/joystick/hidapi/SDL_hidapi_ps5.c`
- **[N]** Nielk1's DualSense trigger-effect gist (MIT)
- **[D]** dualsensectl `main.c`
- **[W]** the DS4Windows fork `schmaldeo/DS4Windows`, `DualSenseDevice.cs`

**UNVERIFIED** marks anything none of these sources showed; check it on the real pad and log it.

## Device and transport
- VID 0x054C. PID 0x0CE6 is the DualSense, 0x0DF2 the DualSense Edge; both use the same protocol.
- **Opening:** enumerate with SetupAPI (`GUID_DEVINTERFACE_HID` from `HidD_GetHidGuid`), then `HidD_GetAttributes` for
  VID/PID. Open with `GENERIC_READ|GENERIC_WRITE` and `FILE_SHARE_READ|FILE_SHARE_WRITE`, with `FILE_FLAG_OVERLAPPED`.
  If the open fails, log the device as "busy" and skip it.
- **USB or BT:** call `HidP_GetCaps` and read `InputReportByteLength`. 64 means USB; anything else is BT (78) [W].
  Size the output buffer from `OutputReportByteLength`: 48 on USB, 78 on BT.
- **Writing:** send outputs with overlapped `WriteFile` with a timeout, not `HidD_SetOutputReport` [W]. BT writes can
  block for milliseconds, so do them on the pad's I/O thread. W calls `HidD_SetNumInputBuffers(h, 3)`.

## Init sequence [L, W]
1. Read feature 0x20 (firmware info, 64 bytes) with `HidD_GetFeature`, `buf[0] = 0x20`. Then read 0x05 (calibration,
   41 bytes). On BT, these reads switch the pad to the full 0x31 input report; so does sending any 0x31 output. Retry
   up to 5 times.
2. Log the firmware info, as little-endian values:

   | offset | field |
   |---|---|
   | 24 | hardware_version (u32) |
   | 28 | firmware_version (u32) |
   | 44 | update_version (u16) |

   This answers the "unknown revision" question.
3. **Rumble mode:** v2 when `update_version >= 0x0215` (Linux) or when the device is an Edge; otherwise v1. SDL uses
   0x0224 instead, so the sources disagree for 0x0215..0x0223. Add an ini override `DualSenseRumbleV1 = 0/1/auto`
   (default auto) in case the pad is in that range and rumble feels wrong.
4. Send one output with `valid_flag2 = 0x02` (lightbar setup) and `lightbar_setup = 0x02` (light out). This releases
   the startup blue light on both transports. Then send colour and player LEDs.

## Input report
- USB: id 0x01, 64 bytes, payload at `buf[1]`.
- BT: id 0x31, 78 bytes, payload at `buf[2]`, so **BT offset = USB offset + 1**. The CRC32 at 74..77 uses seed 0xA1;
  it can be ignored for input. Ignore BT reports with id 0x01: that is the short report, which comes before the full
  mode is on.

Offsets below are USB offsets, counted from the report-id byte:

| off | field |
|---|---|
| 1, 2 | LX, LY (0..255, centre 0x80, down = 255) |
| 3, 4 | RX, RY |
| 5, 6 | L2, R2 analog 0..255 |
| 8 | low nibble hat (0 N, 1 NE, 2 E, 3 SE, 4 S, 5 SW, 6 W, 7 NW, >= 8 none); 0x10 Square, 0x20 Cross, 0x40 Circle, 0x80 Triangle |
| 9 | 0x01 L1, 0x02 R1, 0x04 L2 digital, 0x08 R2 digital, 0x10 Create, 0x20 Options, 0x40 L3, 0x80 R3 |
| 10 | 0x01 PS, 0x02 touchpad click, 0x04 mute (Edge: 0x10 Fn1, 0x20 Fn2, 0x40/0x80 back paddles) |
| 33..36, 37..40 | touch points: b0 bit7 = 1 means **no finger**; x = b1 \| (b2 & 0x0F) << 8, y = (b2 >> 4) \| b3 << 4 (1920 x 1080) |
| 53 | low nibble battery 0..10; high nibble 0 discharging, 1 charging, 2 full, 0xA/0xB/0xF error |
| 54 | bit3 = USB powered |

## Output report
- **USB:** `[0] = 0x02`, common block at `[1]`, 48 bytes total.
- **BT:** `[0] = 0x31`, `[1] = seq << 4` (4-bit counter; a fixed 0 also works [S]), `[2] = 0x10`, common block at
  `[3]`, zeros up to 73. CRC32 at `[74..77]`, little-endian, over the seed byte 0xA2 followed by bytes 0..73.

Common block, 47 bytes, offsets from the block start:

| c-off | field |
|---|---|
| 0 | valid_flag0: 0x01 compatible vibration (v1), 0x02 haptics select (always with rumble), 0x04 right-trigger effect, 0x08 left-trigger effect |
| 1 | valid_flag1: 0x01 mute LED, 0x04 lightbar colour, 0x08 release LEDs, 0x10 player LEDs |
| 2 | right motor (small, high frequency) 0..255 |
| 3 | left motor (large) 0..255 |
| 8 | mute LED: 0 off, 1 on, 2 pulse |
| 10..20 | right trigger (R2) effect: mode + 10 params |
| 21..31 | left trigger (L2) effect: mode + 10 params |
| 38 | valid_flag2: 0x01 LED brightness, 0x02 lightbar setup, 0x04 compatible vibration v2 |
| 41 | lightbar setup: 0x01 light on, 0x02 light out |
| 42 | player LED brightness: 0 high, 1 mid, 2 low (needs flag2 0x01) |
| 43 | player LEDs: bits 0..4 (left to right), **0x20 = change instantly (no fade)** |
| 44, 45, 46 | lightbar R, G, B (needs flag1 0x04) |

- **Rumble:** set `flag0 |= 0x02`, plus `flag2 |= 0x04` (v2) or `flag0 |= 0x01` (v1). Then write c-off 2 (small) and
  3 (large). SDL halves the small motor to match Xbox strength.
- **Player patterns:** P1 0x04 (--x--), P2 0x0A (-x-x-), P3 0x15 (x-x-x), P4 0x1B (xx-xx), all 0x1F. OR in 0x20 so
  the plugin's software flashing isn't smoothed by the fade.

## Trigger effects [N, D]
- An effect stays on until another one is sent; mode 0x05 = off.
- Use only the official modes: 0x05 Off, 0x21 Feedback, 0x25 Weapon, 0x26 Vibration.
- **Never send 0xFC..0xFE.** These calibration modes break the trigger until the pad's reset button is pressed.
- Set `flag0 |= 0x04` for the right trigger and `0x08` for the left in every report that changes that trigger.

```c
static void trig_off(uint8_t* b) { memset(b, 0, 11); b[0] = 0x05; }
// zones 0..9 along the travel; strength/amplitude 1..8 stored as v-1 in 3 bits per zone
static void pack_zones(uint8_t* b, int start, int v) {
    uint16_t act = 0; uint32_t f = 0;
    for (int i = start; i < 10; i++) { act |= 1u << i; f |= (uint32_t)((v - 1) & 7) << (3 * i); }
    b[1] = act & 0xFF; b[2] = act >> 8; b[3] = f & 0xFF; b[4] = f >> 8; b[5] = f >> 16; b[6] = f >> 24;
}
static void trig_feedback(uint8_t* b, int startZone, int strength) {          // resistance from startZone on
    memset(b, 0, 11); if (strength <= 0) { trig_off(b); return; }
    b[0] = 0x21; pack_zones(b, startZone, strength);
}
static void trig_vibration(uint8_t* b, int startZone, int amp, int freqHz) {  // vibrates past startZone
    memset(b, 0, 11); if (amp <= 0 || freqHz <= 0) { trig_off(b); return; }
    b[0] = 0x26; pack_zones(b, startZone, amp); b[9] = (uint8_t)freqHz;
}
```

## CRC32 (reflected, 0xEDB88320)
```c
static uint32_t crc32_upd(uint32_t c, const uint8_t* p, size_t n) {
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & -(int32_t)(c & 1)); }
    return c;
}
static void ds_bt_sign(uint8_t r[78]) {               // output report: seed 0xA2 over r[0..73]
    uint8_t s = 0xA2; uint32_t c = ~crc32_upd(crc32_upd(0xFFFFFFFFu, &s, 1), r, 74);
    r[74] = (uint8_t)c; r[75] = (uint8_t)(c >> 8); r[76] = (uint8_t)(c >> 16); r[77] = (uint8_t)(c >> 24);
}
```

## Rates and coexistence
- **Input rate:** about 250 Hz over USB, about 1000 Hz over BT [S]. Keep only the latest report.
- **Output:** send only on change, at most one report per game frame. While rumble is non-zero, also re-send every
  4 s [W]. On BT, send a keep-alive every 500 ms if nothing else was sent [S].
- **Other programs:** DS4Windows and Steam also write lightbar and rumble, and the last writer wins. That is why
  native mode turns off while `DS4Windows.exe` runs.
