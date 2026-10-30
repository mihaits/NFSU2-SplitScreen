#pragma once
#include <cstdint>

// Replaces the DirectInput8Create import at `importSlot` so game-controller enumeration reports the plugin's
// virtual gamepads (joystick slot N = virtual device N) and hides the real game controllers. Returns false if no
// XInput runtime is available.
// nativeDualSense: also starts the native DualSense backend (pads 8..11, DualSense.h); dsRumbleMode -1 auto, 1, 2.
bool InstallXInputDevices(uintptr_t importSlot, bool nativeDualSense, int dsRumbleMode,
                          void (*log)(const char* fmt, ...));

// Chooses the XInput user index that feeds a virtual device (-1 = idle). Default: device N reads pad N.
void XPadSetResolver(int (*resolver)(int device));

// Once per frame: snapshot all pads (connection, buttons, last pad with any input).
void XPadUpdate();
bool XPadStartPressed(int pad);   // Start went down since the previous XPadUpdate
bool XPadYPressed(int pad);       // Y (triangle) went down since the previous XPadUpdate
int  XPadLastActive();
int  XPadMax();

// Pads: 0..3 real XInput, 4..7 test pads (below), 8..11 native DualSense (DualSense.h).
// Test pads (XInput-style state) that appear as pads 4..7 after the real ones (debug tooling).
void XPadSetVirtual(int index, bool connected, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                    int16_t rx, int16_t ry);

// Raw state of a pad (real 0..3 or test 4..7); false if not connected.
bool XPadRawState(int pad, uint16_t* buttons, uint8_t* lt, uint8_t* rt, int16_t* lx);

// Rumble motors of a real pad (0..3) or native DualSense (8..11): low = left heavy motor, high = right light motor, 0..1. Sent only on change.
void XPadSetRumble(int pad, float low, float high);

// Xbox One / Series pads (real pads 0..3, once XboxWgi.cpp has paired them): trigger motors, 0..1. Rumble and
// triggers then go out together through Windows.Gaming.Input.
bool XPadHasImpulseTriggers(int pad);
void XPadSetImpulseTriggers(int pad, float left, float right);

// Native DualSense pads (8..11) only; no-ops for other pads. The pad layer keeps one merged output per pad (rumble,
// lights, triggers) and sends it on change.
bool XPadIsDualSense(int pad);                                                   // connected native DualSense
bool XPadIsDs4(int pad);                                                         // connected native DualShock 4
void XPadSetLights(int pad, const uint8_t rgb[3], uint8_t playerLeds);           // lightbar + player LEDs (bits 0..4)
void XPadSetTriggers(int pad, const uint8_t l[11], const uint8_t r[11]);         // trigger effects (mode + 10 params)
bool XPadTouchClickPressed(int pad);   // touchpad click went down since the previous XPadUpdate
bool XPadMutePressed(int pad);         // mute button went down since the previous XPadUpdate
void XPadSetMuteLed(int pad, uint8_t mode);   // mute button LED: 0 off, 1 on, 2 pulsing
bool XPadSpeakerWrite(int pad, const float* mono, int frames, int rate);   // DualSense speaker (DualSense.h)
bool XPadIsBluetoothSpeakerPad(int pad);   // DualSense or DS4 on Bluetooth
bool XPadHasSpeaker(int pad);              // a DualSense, or a DS4 on Bluetooth
bool XPadSpeakerReady(int pad);

// A pad's state as virtual device 0 reports it to the game (DIJOYSTATE2, with the axis ranges the game set on that
// device); for pads that have no joystick slot (players 3/4). False if the pad isn't connected or no device exists.
bool XPadDiState(int pad, void* out, unsigned size);

// Right stick of a pad (XInput values); false if not connected.
bool XPadRightStick(int pad, int16_t* rx, int16_t* ry);

// Mouse movement the game has read since the last call (DirectInput units), then reset.
void XMouseTakeDelta(int* dx, int* dy);
