#pragma once
#include <cstdint>

// Replaces the DirectInput8Create import at `importSlot` so game-controller enumeration reports the plugin's
// virtual gamepads (joystick slot N = virtual device N). Returns false if no XInput runtime is available.
bool InstallXInputDevices(uintptr_t importSlot, bool hideRealControllers);

// Chooses the XInput user index that feeds a virtual device (-1 = idle). Default: device N reads pad N.
void XPadSetResolver(int (*resolver)(int device));

// Once per frame: snapshot all pads (connection, buttons, last pad with any input).
void XPadUpdate();
bool XPadStartPressed(int pad);   // Start went down since the previous XPadUpdate
int  XPadLastActive();
int  XPadMax();

// Test pads (XInput-style state) that appear as pads 4..7 after the real ones (debug tooling).
void XPadSetVirtual(int index, bool connected, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                    int16_t rx, int16_t ry);

// Raw state of a pad (real 0..3 or test 4..7); false if not connected.
bool XPadRawState(int pad, uint16_t* buttons, uint8_t* lt, uint8_t* rt, int16_t* lx);

// Rumble motors of a real pad (0..3): low = left heavy motor, high = right light motor, 0..1. Sent only on change.
void XPadSetRumble(int pad, float low, float high);

// A pad's state as virtual device 0 reports it to the game (DIJOYSTATE2, with the axis ranges the game set on that
// device); for pads that have no joystick slot (players 3/4). False if the pad isn't connected or no device exists.
bool XPadDiState(int pad, void* out, unsigned size);
