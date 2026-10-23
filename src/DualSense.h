#pragma once
#include <cstdint>

// Native Sony DualSense (and DualSense Edge) pads over HID (SetupAPI + hid.dll), USB or Bluetooth, without
// DS4Windows. One I/O thread per open pad keeps the latest input report and sends output reports (rumble, lights,
// triggers) when the wanted output changes. A hotplug thread rescans every 2 s. See docs/input.md.

constexpr int kDsMax = 4;                 // native pad slots

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
};

typedef void (*DsLogFn)(const char* fmt, ...);

// Starts the hotplug thread. rumbleMode: -1 auto (by firmware), 1 old firmware mode, 2 new firmware mode.
// With enabled = false nothing starts (and DsCount() stays 0).
bool DsInstall(bool enabled, int rumbleMode, DsLogFn log);
int  DsCount();                                        // pads connected now
bool DsState(int slot, DsPadState* out, DsExtra* extra);   // false if the slot is empty (extra may be null)
void DsSetOutput(int slot, const DsOutput& o);         // whole wanted output; sent by the I/O thread on change
