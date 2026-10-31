#pragma once
#include <cstdint>

// Native Sony DualSense, DualSense Edge and DualShock 4 pads over HID (SetupAPI + hid.dll), USB or Bluetooth, without
// DS4Windows. One I/O thread per open pad keeps the latest input report and sends output reports (rumble, lights,
// triggers) when the wanted output changes. A hotplug thread rescans every 2 s. See docs/input.md.

constexpr int kDsMax = 8;                 // native pad slots

// Same field layout as the XInput state XInputDevices.cpp uses.
struct DsPadState {
    uint32_t packet;                      // +1 per input report
    uint16_t buttons;                     // XInput button bits (XINPUT_GAMEPAD_*)
    uint8_t leftTrigger, rightTrigger;
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
};

struct DsExtra {
    bool touchClick, mute, ps;
    uint8_t battery;                      // 0..10 (low nibble of the report; 10 = full)
    bool usb;                             // false = Bluetooth
};

struct DsOutput {
    uint8_t rgb[3];                       // lightbar
    uint8_t playerLeds;                   // bits 0..4, left to right (the "no fade" bit is added inside)
    uint8_t smallMotor, largeMotor;       // 0..255
    uint8_t trigR[11], trigL[11];         // trigger effect: mode + 10 parameters; mode 0 = leave as it is
    uint8_t muteLed;                      // mute button LED: 0 off, 1 on, 2 pulsing
};

typedef void (*DsLogFn)(const char* fmt, ...);

// Starts the hotplug thread. rumbleMode: -1 auto (by firmware), 1 old firmware mode, 2 new firmware mode.
// With enabled = false nothing starts (and DsCount() stays 0).
bool DsInstall(bool enabled, int rumbleMode, DsLogFn log);
int  DsCount();                                        // pads connected now
bool DsState(int slot, DsPadState* out, DsExtra* extra);   // false if the slot is empty (extra may be null)
void DsSetOutput(int slot, const DsOutput& o);         // whole wanted output; sent by the I/O thread on change

// Speaker of a pad: mono samples in -1..1 at `rate` Hz are queued and streamed to it (DualSense on Bluetooth: Opus in
// output report 0x39; DS4 on Bluetooth: SBC in report 0x17; on USB: the pad's Windows audio endpoint, see docs/input.md),
// starting the stream if needed; it stops by
// itself after 1 s without samples. False (nothing queued) if the slot has no connected pad, or a USB pad's stream
// failed (a thread reports that a moment after the first samples).
bool DsSpeakerWrite(int slot, const float* mono, int frames, int rate);
bool DsIsDs4(int slot);                                // a DualShock 4 (no player LEDs, triggers, mute)
bool DsIsBluetooth(int slot);                          // connected over Bluetooth
bool DsSpeakerReady(int slot);                         // DsSpeakerWrite would take samples
