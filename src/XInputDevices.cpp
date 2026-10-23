// XInput pads as DirectInput game controllers.
//
// The PC game only knows DirectInput, opens at most two game controllers in enumeration order, and gets no data
// from some devices (e.g. a raw DualSense, which only works through DS4Windows' virtual Xbox pad). We wrap the
// game's IDirectInput8A: game-controller enumeration always reports kVirtualDevices virtual "Xbox 360 gamepads"
// (the real DirectInput controllers are hidden), and CreateDevice returns a small IDirectInputDevice8A that
// answers GetDeviceState from XInput in the layout Windows uses for Xbox pads under DirectInput, so the game's own
// controller mapping and bindings keep working. Which physical pad feeds which virtual device (= joystick slot =
// player) is decided by the plugin through a resolver callback; a missing pad reads as idle, so the game never
// sees a device disappear.

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "XInputDevices.h"
#include "DualSense.h"

namespace {

// ---------------------------------------------------------------- XInput (loaded dynamically)
struct XPadState {
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
};
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XPadState*);
XInputGetStateFn g_xinputGetState;
struct XPadVibration { WORD left, right; };
typedef DWORD(WINAPI* XInputSetStateFn)(DWORD, XPadVibration*);
XInputSetStateFn g_xinputSetState;
WORD g_rumbleSent[4][2];

enum : WORD {
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LTHUMB = 0x0040, XB_RTHUMB = 0x0080,
    XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

constexpr int kRealPads = 4;            // XInput user indices 0..3
constexpr int kDebugPads = 4;           // test pads 4..7 driven by XPadSetVirtual
constexpr int kDsFirst = kRealPads + kDebugPads;   // native DualSense pads 8..11
constexpr int kMaxPads = kDsFirst + kDsMax;
constexpr int kVirtualDevices = 2;     // the game has two joystick slots
int (*g_resolver)(int device);         // virtual device -> XInput user index (-1 = idle)

// per-frame pad snapshot (XPadUpdate)
bool g_connected[kMaxPads];
WORD g_buttons[kMaxPads], g_prevButtons[kMaxPads];
int g_lastActive = 0;

bool g_virtualConnected[kDebugPads];
XPadState g_virtual[kDebugPads];

// native DualSense: merged output per pad, touchpad click edge
DsOutput g_dsOut[kDsMax];
bool g_touch[kMaxPads], g_prevTouch[kMaxPads];
bool g_dsConnected[kDsMax];

DWORD GetPad(int pad, XPadState* s) {
    if (pad >= kDsFirst && pad < kMaxPads) {
        static_assert(sizeof(DsPadState) == sizeof(XPadState), "DsPadState mirrors XPadState");
        return DsState(pad - kDsFirst, reinterpret_cast<DsPadState*>(s), nullptr) ? ERROR_SUCCESS
                                                                                 : ERROR_DEVICE_NOT_CONNECTED;
    }
    if (pad >= kRealPads && pad < kDsFirst) {
        if (!g_virtualConnected[pad - kRealPads]) return ERROR_DEVICE_NOT_CONNECTED;
        *s = g_virtual[pad - kRealPads];
        return ERROR_SUCCESS;
    }
    if (pad < 0 || pad >= kRealPads || !g_xinputGetState) return ERROR_DEVICE_NOT_CONNECTED;
    return g_xinputGetState(pad, s);
}

bool PadActive(const XPadState& s) {
    const int dz = 12000;
    return s.buttons || s.leftTrigger > 30 || s.rightTrigger > 30 || abs(s.thumbLX) > dz || abs(s.thumbLY) > dz ||
           abs(s.thumbRX) > dz || abs(s.thumbRY) > dz;
}

// Instance GUID {58494E50-0000-0000-xxxx-000000000000} ("XINP") carries the XInput user index in Data4[0].
GUID PadInstanceGuid(int i) {
    GUID g = {0x58494E50, 0, 0, {static_cast<BYTE>(i), 0, 0, 0, 0, 0, 0, 0}};
    return g;
}

int PadIndexFromGuid(const GUID& g) {
    GUID base = PadInstanceGuid(0);
    if (g.Data1 != base.Data1 || g.Data2 || g.Data3 || g.Data4[0] >= kVirtualDevices) return -1;
    for (int b = 1; b < 8; ++b)
        if (g.Data4[b]) return -1;
    return g.Data4[0];
}

void FillDeviceInstance(DIDEVICEINSTANCEA* d, int pad) {
    DWORD size = d->dwSize ? d->dwSize : sizeof(DIDEVICEINSTANCEA);
    memset(d, 0, size);
    d->dwSize = size;
    d->guidInstance = PadInstanceGuid(pad);
    // product GUID of a wired Xbox 360 pad as DirectInput reports it ("PIDVID" + VID 045E / PID 028E)
    d->guidProduct = {0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 'P', 'I', 'D', 'V', 'I', 'D'}};
    d->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    wsprintfA(d->tszInstanceName, "Split Screen Pad %d", pad + 1);
    lstrcpyA(d->tszProductName, "Controller (XBOX 360 For Windows)");
}

// ---------------------------------------------------------------- virtual device
LONG Scale(int v, int inMax, LONG outMin, LONG outMax) {
    // [-inMax, inMax] -> [outMin, outMax]
    double t = (static_cast<double>(v) + inMax) / (2.0 * inMax);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return static_cast<LONG>(outMin + t * (outMax - outMin));
}

// DirectInput object GUIDs (dinput.h declares them, dxguid.lib defines them; spelled out to avoid the lib)
const GUID kGuidXAxis  = {0xA36D02E0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidYAxis  = {0xA36D02E1, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidZAxis  = {0xA36D02E2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidRxAxis = {0xA36D02F4, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidRyAxis = {0xA36D02F5, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidButton = {0xA36D02F0, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
const GUID kGuidPov    = {0xA36D02F2, 0xC9F3, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// The objects of an Xbox pad as Windows exposes it to DirectInput (c_dfDIJoystick2 offsets).
struct PadObject {
    DWORD ofs, type;
    const GUID* guid;
    const char* name;
};
constexpr int kAxes = 5, kButtons = 10;
const PadObject kObjects[] = {
    {0x00, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0), &kGuidXAxis, "Left Stick X"},
    {0x04, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(1), &kGuidYAxis, "Left Stick Y"},
    {0x08, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(2), &kGuidZAxis, "Triggers"},
    {0x0C, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(3), &kGuidRxAxis, "Right Stick X"},
    {0x10, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(4), &kGuidRyAxis, "Right Stick Y"},
    {0x20, DIDFT_POV | DIDFT_MAKEINSTANCE(0), &kGuidPov, "D-Pad"},
    {0x30, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0), &kGuidButton, "A"},
    {0x31, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(1), &kGuidButton, "B"},
    {0x32, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(2), &kGuidButton, "X"},
    {0x33, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(3), &kGuidButton, "Y"},
    {0x34, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(4), &kGuidButton, "LB"},
    {0x35, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(5), &kGuidButton, "RB"},
    {0x36, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(6), &kGuidButton, "Back"},
    {0x37, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(7), &kGuidButton, "Start"},
    {0x38, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(8), &kGuidButton, "Left Stick"},
    {0x39, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(9), &kGuidButton, "Right Stick"},
};

void FillObject(DIDEVICEOBJECTINSTANCEA* o, const PadObject& src) {
    DWORD size = o->dwSize ? o->dwSize : sizeof(DIDEVICEOBJECTINSTANCEA);
    memset(o, 0, size);
    o->dwSize = size;
    o->guidType = *src.guid;
    o->dwOfs = src.ofs;
    o->dwType = src.type;
    lstrcpynA(o->tszName, src.name, MAX_PATH);
}

const PadObject* FindObject(DWORD obj, DWORD how) {
    for (const auto& o : kObjects) {
        if (how == DIPH_BYOFFSET && o.ofs == obj) return &o;
        if (how == DIPH_BYID && (o.type & 0x00FFFFFF) == (obj & 0x00FFFFFF)) return &o;
    }
    return nullptr;
}

class XInputDevice;
XInputDevice* g_devices[kVirtualDevices];   // the game's devices (XPadDiState uses device 0's axis ranges)

class XInputDevice final : public IDirectInputDevice8A {
public:
    explicit XInputDevice(int device) : device_(device) {
        for (auto& r : range_) { r[0] = 0; r[1] = 65535; }
        if (device >= 0 && device < kVirtualDevices) g_devices[device] = this;
    }
    ~XInputDevice() {
        if (device_ >= 0 && device_ < kVirtualDevices && g_devices[device_] == this) g_devices[device_] = nullptr;
    }

    // a pad's state in this device's layout and axis ranges
    void StateOf(const XPadState& s, DIJOYSTATE2* js) const {
        memset(js, 0, sizeof(*js));
        js->lX = Scale(s.thumbLX, 32768, range_[0][0], range_[0][1]);
        js->lY = Scale(-s.thumbLY - 1, 32768, range_[1][0], range_[1][1]);
        // Windows' DirectInput view of an Xbox pad: both triggers share Z (LT pushes one way, RT the other)
        js->lZ = Scale((s.leftTrigger - s.rightTrigger) * 128, 32768, range_[2][0], range_[2][1]);
        js->lRx = Scale(s.thumbRX, 32768, range_[3][0], range_[3][1]);
        js->lRy = Scale(-s.thumbRY - 1, 32768, range_[4][0], range_[4][1]);
        js->rgdwPOV[0] = PovFromDpad(s.buttons);
        js->rgdwPOV[1] = js->rgdwPOV[2] = js->rgdwPOV[3] = 0xFFFFFFFF;
        static const WORD order[kButtons] = {XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_BACK, XB_START, XB_LTHUMB, XB_RTHUMB};
        for (int b = 0; b < kButtons; ++b)
            js->rgbButtons[b] = (s.buttons & order[b]) ? 0x80 : 0;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID* out) override {
        *out = this;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }

    // IDirectInputDevice8A
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override {
        if (!caps) return E_POINTER;
        DWORD size = caps->dwSize ? caps->dwSize : sizeof(DIDEVCAPS);
        memset(caps, 0, size);
        caps->dwSize = size;
        caps->dwFlags = DIDC_ATTACHED;
        caps->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
        caps->dwAxes = kAxes;
        caps->dwButtons = kButtons;
        caps->dwPOVs = 1;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override {
        // The game sets the range of every absolute axis and counts force-feedback actuators (none here);
        // the Controls screen lists the buttons.
        if (!cb) return DIERR_INVALIDPARAM;
        DWORD kinds = flags & 0xFF;
        for (const auto& src : kObjects) {
            if (kinds != DIDFT_ALL && !(src.type & kinds)) continue;
            DIDEVICEOBJECTINSTANCEA o = {};
            o.dwSize = sizeof(o);
            FillObject(&o, src);
            if (cb(&o, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER hdr) override {
        if (!hdr) return E_POINTER;
        if (&prop == &DIPROP_RANGE) {
            int axis = AxisFromHeader(hdr);
            auto* r = reinterpret_cast<DIPROPRANGE*>(hdr);
            r->lMin = range_[axis < 0 ? 0 : axis][0];
            r->lMax = range_[axis < 0 ? 0 : axis][1];
            return DI_OK;
        }
        if (&prop == &DIPROP_BUFFERSIZE) {
            reinterpret_cast<DIPROPDWORD*>(hdr)->dwData = bufferSize_;
            return DI_OK;
        }
        if (&prop == &DIPROP_DEADZONE || &prop == &DIPROP_SATURATION) {
            reinterpret_cast<DIPROPDWORD*>(hdr)->dwData = &prop == &DIPROP_DEADZONE ? 0 : 10000;
            return DI_OK;
        }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER hdr) override {
        if (&prop == &DIPROP_RANGE && hdr) {
            auto* r = reinterpret_cast<const DIPROPRANGE*>(hdr);
            int axis = AxisFromHeader(hdr);               // -1: whole device
            for (int a = 0; a < kAxes; ++a)
                if (axis < 0 || axis == a) { range_[a][0] = r->lMin; range_[a][1] = r->lMax; }
        }
        if (&prop == &DIPROP_BUFFERSIZE && hdr) bufferSize_ = reinterpret_cast<const DIPROPDWORD*>(hdr)->dwData;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override {
        if (!data || size < sizeof(DIJOYSTATE)) return DIERR_INVALIDPARAM;
        DIJOYSTATE2 js;
        ReadState(&js);
        memset(data, 0, size);
        memcpy(data, &js, size < sizeof(js) ? size : sizeof(js));
        return DI_OK;
    }

    // Buffered input (used by the Controls screen to detect "press a button to bind"): events are the changes
    // between consecutive reads of the pad.
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD objSize, LPDIDEVICEOBJECTDATA out, LPDWORD count, DWORD flags) override {
        if (!count) return DIERR_INVALIDPARAM;
        DIJOYSTATE2 js;
        ReadState(&js);
        QueueChanges(js);
        DWORD n = queued_ < *count ? queued_ : *count;
        if (out) {
            for (DWORD i = 0; i < n; ++i)
                memcpy(reinterpret_cast<BYTE*>(out) + i * objSize, &queue_[i],
                       objSize < sizeof(DIDEVICEOBJECTDATA) ? objSize : sizeof(DIDEVICEOBJECTDATA));
        }
        if (!(flags & DIGDD_PEEK)) {
            memmove(queue_, queue_ + n, (queued_ - n) * sizeof(queue_[0]));
            queued_ -= n;
        }
        *count = n;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override {
        if (!o) return E_POINTER;
        const PadObject* src = FindObject(obj, how);
        if (!src) return DIERR_OBJECTNOTFOUND;
        FillObject(o, *src);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA info) override {
        if (!info) return E_POINTER;
        FillDeviceInstance(info, device_);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID, LPCDIEFFECT, LPDIRECTINPUTEFFECT*, LPUNKNOWN) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA, REFGUID) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Poll() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }

private:
    static int AxisFromHeader(LPCDIPROPHEADER hdr) {
        if (hdr->dwHow == DIPH_BYOFFSET && hdr->dwObj < kAxes * 4) return static_cast<int>(hdr->dwObj / 4);
        if (hdr->dwHow == DIPH_BYID) {
            const PadObject* o = FindObject(hdr->dwObj, DIPH_BYID);
            if (o && o->ofs < kAxes * 4) return static_cast<int>(o->ofs / 4);
        }
        return -1;
    }

    void ReadState(DIJOYSTATE2* js) {
        XPadState s = {};
        int pad = g_resolver ? g_resolver(device_) : device_;
        if (GetPad(pad, &s) != ERROR_SUCCESS)
            memset(&s, 0, sizeof(s));                     // no pad: idle, never DIERR_INPUTLOST
        StateOf(s, js);
    }

    void Push(DWORD ofs, DWORD value) {
        if (queued_ >= kQueue || queued_ >= bufferSize_) return;   // overflow: drop like DirectInput
        DIDEVICEOBJECTDATA& e = queue_[queued_++];
        memset(&e, 0, sizeof(e));
        e.dwOfs = ofs;
        e.dwData = value;
        e.dwTimeStamp = GetTickCount();
        e.dwSequence = ++sequence_;
    }

    void QueueChanges(const DIJOYSTATE2& js) {
        if (!haveLast_) { last_ = js; haveLast_ = true; return; }
        LONG* was = &last_.lX;
        const LONG* now = &js.lX;
        for (int a = 0; a < kAxes; ++a) {
            LONG span = range_[a][1] - range_[a][0];
            if (labs(now[a] - was[a]) <= span / 64) continue;   // ignore stick noise
            Push(a * 4, static_cast<DWORD>(now[a]));
            was[a] = now[a];
        }
        if (js.rgdwPOV[0] != last_.rgdwPOV[0]) Push(0x20, js.rgdwPOV[0]);
        last_.rgdwPOV[0] = js.rgdwPOV[0];
        for (int b = 0; b < kButtons; ++b)
            if (js.rgbButtons[b] != last_.rgbButtons[b]) Push(0x30 + b, js.rgbButtons[b]);
        memcpy(last_.rgbButtons, js.rgbButtons, kButtons);
    }

    static DWORD PovFromDpad(WORD b) {
        bool up = b & XB_DPAD_UP, down = b & XB_DPAD_DOWN, left = b & XB_DPAD_LEFT, right = b & XB_DPAD_RIGHT;
        if (up && right) return 4500;
        if (right && down) return 13500;
        if (down && left) return 22500;
        if (left && up) return 31500;
        if (up) return 0;
        if (right) return 9000;
        if (down) return 18000;
        if (left) return 27000;
        return 0xFFFFFFFF;
    }

    static constexpr DWORD kQueue = 64;
    LONG refs_ = 1;
    int device_;
    LONG range_[kAxes][2];
    DWORD bufferSize_ = kQueue;          // DIPROP_BUFFERSIZE (games may not set it)
    DIDEVICEOBJECTDATA queue_[kQueue];
    DWORD queued_ = 0, sequence_ = 0;
    DIJOYSTATE2 last_ = {};
    bool haveLast_ = false;
};

// ---------------------------------------------------------------- IDirectInput8A wrapping
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceFn)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
typedef HRESULT(STDMETHODCALLTYPE* EnumDevicesFn)(IDirectInput8A*, DWORD, LPDIENUMDEVICESCALLBACKA, LPVOID, DWORD);
CreateDeviceFn g_origCreateDevice;
EnumDevicesFn g_origEnumDevices;

HRESULT STDMETHODCALLTYPE EnumDevicesHook(IDirectInput8A* self, DWORD type, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref,
                                          DWORD flags) {
    if (type != DI8DEVCLASS_GAMECTRL || !cb) return g_origEnumDevices(self, type, cb, ref, flags);
    for (int i = 0; i < kVirtualDevices; ++i) {
        DIDEVICEINSTANCEA d = {};
        d.dwSize = sizeof(d);
        FillDeviceInstance(&d, i);
        if (cb(&d, ref) == DIENUM_STOP) return DI_OK;
    }
    return DI_OK;                        // the real game controllers stay hidden (they'd take the two slots)
}

// The system mouse: its movement is recorded as the game reads it (GetDeviceState, DIMOUSESTATE/2), for the plugin's
// per-player free camera (XMouseTakeDelta). The game still gets the same data.
const GUID kGuidSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
typedef HRESULT(STDMETHODCALLTYPE* GetDeviceStateFn)(IDirectInputDevice8A*, DWORD, LPVOID);
GetDeviceStateFn g_origMouseGetState;
IDirectInputDevice8A* g_mouse;
LONG g_mouseDx, g_mouseDy;
HRESULT STDMETHODCALLTYPE MouseGetStateHook(IDirectInputDevice8A* dev, DWORD size, LPVOID data) {
    HRESULT hr = g_origMouseGetState(dev, size, data);
    if (SUCCEEDED(hr) && dev == g_mouse && data && size >= sizeof(DIMOUSESTATE)) {
        g_mouseDx += static_cast<const DIMOUSESTATE*>(data)->lX;
        g_mouseDy += static_cast<const DIMOUSESTATE*>(data)->lY;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateDeviceHook(IDirectInput8A* self, REFGUID guid, LPDIRECTINPUTDEVICE8A* out,
                                           LPUNKNOWN outer) {
    int pad = PadIndexFromGuid(guid);
    if (pad < 0 && IsEqualGUID(guid, kGuidSysMouse)) {
        HRESULT hr = g_origCreateDevice(self, guid, out, outer);
        if (SUCCEEDED(hr) && out && *out) {
            g_mouse = *out;
            void** vt = *reinterpret_cast<void***>(*out);
            if (vt[9] != reinterpret_cast<void*>(&MouseGetStateHook)) {   // vtable shared by the device class
                DWORD old;
                VirtualProtect(vt + 9, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
                g_origMouseGetState = reinterpret_cast<GetDeviceStateFn>(vt[9]);
                vt[9] = reinterpret_cast<void*>(&MouseGetStateHook);
                VirtualProtect(vt + 9, sizeof(void*), old, &old);
            }
        }
        return hr;
    }
    if (pad < 0) return g_origCreateDevice(self, guid, out, outer);
    if (!out) return E_POINTER;
    *out = new XInputDevice(pad);
    return DI_OK;
}

void HookDirectInput(IDirectInput8A* di) {
    void** vt = *reinterpret_cast<void***>(di);
    if (vt[3] == reinterpret_cast<void*>(&CreateDeviceHook)) return;
    DWORD old;
    VirtualProtect(vt + 3, 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    g_origCreateDevice = reinterpret_cast<CreateDeviceFn>(vt[3]);
    g_origEnumDevices = reinterpret_cast<EnumDevicesFn>(vt[4]);
    vt[3] = reinterpret_cast<void*>(&CreateDeviceHook);
    vt[4] = reinterpret_cast<void*>(&EnumDevicesHook);
    VirtualProtect(vt + 3, 2 * sizeof(void*), old, &old);
}

typedef HRESULT(WINAPI* DirectInput8CreateFn)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
DirectInput8CreateFn g_origDirectInput8Create;

HRESULT WINAPI DirectInput8CreateHook(HINSTANCE inst, DWORD version, REFIID iid, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = g_origDirectInput8Create(inst, version, iid, out, outer);
    if (SUCCEEDED(hr) && out && *out) HookDirectInput(static_cast<IDirectInput8A*>(*out));
    return hr;
}

}  // namespace

void XPadUpdate() {
    for (int i = 0; i < kMaxPads; ++i) {
        XPadState s = {};
        g_prevButtons[i] = g_buttons[i];
        g_connected[i] = GetPad(i, &s) == ERROR_SUCCESS;
        g_buttons[i] = g_connected[i] ? s.buttons : 0;
        g_prevTouch[i] = g_touch[i];
        g_touch[i] = false;
        if (i >= kDsFirst) {
            DsPadState ds;
            DsExtra ex = {};
            if (DsState(i - kDsFirst, &ds, &ex)) g_touch[i] = ex.touchClick;
            g_dsConnected[i - kDsFirst] = g_connected[i];
        }
        if (g_connected[i] && PadActive(s)) g_lastActive = i;
    }
}

bool XPadStartPressed(int pad) {
    return pad >= 0 && pad < kMaxPads && (g_buttons[pad] & XB_START) && !(g_prevButtons[pad] & XB_START);
}

int XPadLastActive() { return g_lastActive; }

bool XPadRawState(int pad, uint16_t* buttons, uint8_t* lt, uint8_t* rt, int16_t* lx) {
    XPadState st = {};
    if (GetPad(pad, &st) != ERROR_SUCCESS) return false;
    *buttons = st.buttons; *lt = st.leftTrigger; *rt = st.rightTrigger; *lx = st.thumbLX;
    return true;
}

void XPadSetVirtual(int index, bool connected, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                    int16_t rx, int16_t ry) {
    if (index < 0 || index >= kDebugPads) return;
    g_virtualConnected[index] = connected;
    XPadState& v = g_virtual[index];
    ++v.packet;
    v.buttons = buttons; v.leftTrigger = lt; v.rightTrigger = rt;
    v.thumbLX = lx; v.thumbLY = ly; v.thumbRX = rx; v.thumbRY = ry;
}

int XPadMax() { return kMaxPads; }

void XPadSetRumble(int pad, float low, float high) {
    auto toByte = [](float v) { return static_cast<uint8_t>((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f + 0.5f); };
    if (pad >= kDsFirst && pad < kMaxPads) {     // native DualSense: left = large motor, right = small motor
        DsOutput& o = g_dsOut[pad - kDsFirst];
        uint8_t lo = toByte(low), hi = toByte(high);
        if (o.largeMotor == lo && o.smallMotor == hi) return;
        o.largeMotor = lo; o.smallMotor = hi;
        DsSetOutput(pad - kDsFirst, o);
        return;
    }
    if (pad < 0 || pad >= kRealPads || !g_xinputSetState) return;     // test pads have no motors
    auto speed = [](float v) { return static_cast<WORD>((v < 0 ? 0 : v > 1 ? 1 : v) * 65535.0f); };
    XPadVibration v = {speed(low), speed(high)};
    if (v.left == g_rumbleSent[pad][0] && v.right == g_rumbleSent[pad][1]) return;   // only changes
    if (g_xinputSetState(pad, &v) == ERROR_SUCCESS) { g_rumbleSent[pad][0] = v.left; g_rumbleSent[pad][1] = v.right; }
}

bool XPadIsDualSense(int pad) { return pad >= kDsFirst && pad < kMaxPads && g_dsConnected[pad - kDsFirst]; }

void XPadSetLights(int pad, const uint8_t rgb[3], uint8_t playerLeds) {
    if (pad < kDsFirst || pad >= kMaxPads) return;
    DsOutput& o = g_dsOut[pad - kDsFirst];
    if (!memcmp(o.rgb, rgb, 3) && o.playerLeds == playerLeds) return;
    memcpy(o.rgb, rgb, 3);
    o.playerLeds = playerLeds;
    DsSetOutput(pad - kDsFirst, o);
}

void XPadSetTriggers(int pad, const uint8_t l[11], const uint8_t r[11]) {
    if (pad < kDsFirst || pad >= kMaxPads) return;
    DsOutput& o = g_dsOut[pad - kDsFirst];
    if (!memcmp(o.trigL, l, 11) && !memcmp(o.trigR, r, 11)) return;
    memcpy(o.trigL, l, 11);
    memcpy(o.trigR, r, 11);
    DsSetOutput(pad - kDsFirst, o);
}

bool XPadTouchClickPressed(int pad) {
    return pad >= kDsFirst && pad < kMaxPads && g_touch[pad] && !g_prevTouch[pad];
}

void XPadSetResolver(int (*resolver)(int device)) { g_resolver = resolver; }

bool XPadRightStick(int pad, int16_t* rx, int16_t* ry) {
    XPadState st = {};
    if (GetPad(pad, &st) != ERROR_SUCCESS) return false;
    *rx = st.thumbRX; *ry = st.thumbRY;
    return true;
}

void XMouseTakeDelta(int* dx, int* dy) {
    *dx = g_mouseDx; *dy = g_mouseDy;
    g_mouseDx = g_mouseDy = 0;
}

bool XPadDiState(int pad, void* out, unsigned size) {
    XPadState s = {};
    if (!g_devices[0] || !out || size < sizeof(DIJOYSTATE2) || GetPad(pad, &s) != ERROR_SUCCESS) return false;
    memset(out, 0, size);
    g_devices[0]->StateOf(s, static_cast<DIJOYSTATE2*>(out));
    return true;
}

bool InstallXInputDevices(uintptr_t importSlot, bool nativeDualSense, int dsRumbleMode,
                          void (*log)(const char* fmt, ...)) {
    static const char* const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (const char* dll : dlls) {
        if (HMODULE m = LoadLibraryA(dll)) {
            g_xinputGetState = reinterpret_cast<XInputGetStateFn>(GetProcAddress(m, "XInputGetState"));
            g_xinputSetState = reinterpret_cast<XInputSetStateFn>(GetProcAddress(m, "XInputSetState"));
            if (g_xinputGetState) break;
        }
    }
    if (!g_xinputGetState) return false;
    for (int i = 0; i < kDsMax; ++i) {     // until the lights policy exists: dim blue + player 1 LED mark a native pad
        g_dsOut[i].rgb[2] = 64;
        g_dsOut[i].playerLeds = 0x04;
    }
    if (nativeDualSense) {
        DsInstall(true, dsRumbleMode, log);
        for (int i = 0; i < kDsMax; ++i) DsSetOutput(i, g_dsOut[i]);
    }

    auto* slot = reinterpret_cast<void**>(importSlot);
    g_origDirectInput8Create = reinterpret_cast<DirectInput8CreateFn>(*slot);
    void* hook = reinterpret_cast<void*>(&DirectInput8CreateHook);
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}
