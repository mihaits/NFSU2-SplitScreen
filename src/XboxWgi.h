#pragma once
#include <cstdint>

// Xbox One / Series pads through Windows.Gaming.Input, for the trigger motors (impulse triggers) XInput can't reach.
// XInput stays the input path; WGI doesn't say which XInput user index a Gamepad is, so a thread pairs them by
// comparing their input (or by elimination when one of each is left) and, once paired, sends that pad's whole
// vibration (both motors + both triggers) through WGI. Pads without trigger motors (Xbox 360 class, DS4Windows'
// virtual pad) stay on XInput. See docs/input.md.

typedef void (*WgiLogFn)(const char* fmt, ...);
typedef unsigned long(__stdcall* WgiXInputGetStateFn)(unsigned long user, void* state);   // XInputGetState

// Starts the pairing thread; false if WGI isn't available (older Windows, Wine without it).
bool WgiInstall(WgiXInputGetStateFn getState, WgiLogFn log);
bool WgiHasTriggers(int user);    // XInput user 0..3 is paired with a WGI pad that has trigger motors
// Whole wanted vibration of a paired pad, 0..1 each; sent by the thread on change.
void WgiSetVibration(int user, float largeMotor, float smallMotor, float leftTrigger, float rightTrigger);
