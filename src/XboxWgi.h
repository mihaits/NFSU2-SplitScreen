#pragma once
#include <cstdint>

// Xbox pads through Windows.Gaming.Input: input, id and vibration (both motors + both triggers) from one object. A
// thread keeps each pad on its pad number 0..7 by its NonRoamableId, so a pad keeps its player when it returns. Where
// WGI lists no pad (older Windows, Wine without it) the pad layer falls back to XInput. See docs/input.md.

typedef void (*WgiLogFn)(const char* fmt, ...);

struct WgiPadState {                  // mirrors XINPUT_STATE
    unsigned long packet;
    uint16_t buttons;
    uint8_t leftTrigger, rightTrigger;
    int16_t thumbLX, thumbLY, thumbRX, thumbRY;
};

// Starts the WGI thread; false if WinRT isn't there.
bool WgiInstall(WgiLogFn log);
bool WgiLive();                   // WGI has listed a pad: it is the input path from now on
bool WgiReadPad(int pad, WgiPadState* state);   // pad number 0..7, false = no pad there
bool WgiHasTriggers(int pad);     // pad number 0..7 has trigger motors
// Whole wanted vibration of a pad, 0..1 each; sent by the thread on change.
void WgiSetVibration(int pad, float largeMotor, float smallMotor, float leftTrigger, float rightTrigger);
