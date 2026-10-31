// Xbox pads' trigger motors through Windows.Gaming.Input (XboxWgi.h, docs/input.md).
//
// One thread (MTA) owns every WGI object: it lists the Gamepads once a second, pairs each with its XInput user index
// and sends the wanted vibration of the paired pads. WinRT is loaded from combase.dll at run time, so the plugin
// still loads where it is missing.

#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.gaming.input.h>
#include <cmath>
#include <cstring>

#include "XboxWgi.h"

using namespace ABI::Windows::Gaming::Input;

namespace {

typedef HRESULT(WINAPI* RoInitializeFn)(RO_INIT_TYPE);
typedef HRESULT(WINAPI* RoGetActivationFactoryFn)(HSTRING, REFIID, void**);
typedef HRESULT(WINAPI* WindowsCreateStringFn)(PCNZWCH, UINT32, HSTRING*);
typedef HRESULT(WINAPI* WindowsDeleteStringFn)(HSTRING);
RoInitializeFn g_roInitialize;
RoGetActivationFactoryFn g_roGetActivationFactory;
WindowsCreateStringFn g_createString;
WindowsDeleteStringFn g_deleteString;

struct XState {                       // XINPUT_STATE
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

constexpr int kUsers = 4, kMaxWgi = 16;
WgiXInputGetStateFn g_getState;
WgiLogFn g_log;
CRITICAL_SECTION g_lock;
float g_want[kUsers][4];              // large, small, left trigger, right trigger (under g_lock)
volatile LONG g_hasTriggers[kUsers];

// thread only
struct Pad {
    IGamepad* gp;
    int user;                         // paired XInput user, -1 = not yet
    bool triggers;                    // has trigger motors (not an Xbox 360 class pad)
    UINT16 vid, pid;
    float sent[4];
    DWORD sentAt;
    bool sentValid;
};
Pad g_pads[kMaxWgi];
int g_padCount;

template <class T> T* Factory(const wchar_t* cls) {
    HSTRING name = nullptr;
    T* f = nullptr;
    if (FAILED(g_createString(cls, static_cast<UINT32>(wcslen(cls)), &name))) return nullptr;
    if (FAILED(g_roGetActivationFactory(name, __uuidof(T), reinterpret_cast<void**>(&f)))) f = nullptr;
    g_deleteString(name);
    return f;
}

void Unpair(Pad& p, const char* why) {
    if (p.user < 0) return;
    g_log("xbox: pad %d unpaired from WGI gamepad %04X:%04X (%s)", p.user + 1, p.vid, p.pid, why);
    InterlockedExchange(&g_hasTriggers[p.user], 0);
    if (p.gp && p.triggers) {
        GamepadVibration v = {};
        p.gp->put_Vibration(v);
    }
    p.user = -1;
    p.sentValid = false;
}

void Pair(Pad& p, int user, const char* how) {
    p.user = user;
    p.sentValid = false;
    InterlockedExchange(&g_hasTriggers[user], p.triggers ? 1 : 0);
    g_log("xbox: pad %d = WGI gamepad %04X:%04X (%s), %s", user + 1, p.vid, p.pid, how,
          p.triggers ? "impulse triggers; its rumble goes through WGI" : "no trigger motors; rumble stays on XInput");
}

void Refresh(IGamepadStatics* statics, IRawGameControllerStatics* raw) {
    __FIVectorView_1_Windows__CGaming__CInput__CGamepad* list = nullptr;
    if (FAILED(statics->get_Gamepads(&list)) || !list) return;
    unsigned n = 0;
    list->get_Size(&n);
    Pad next[kMaxWgi];
    int count = 0;
    for (unsigned i = 0; i < n && count < kMaxWgi; ++i) {
        IGamepad* gp = nullptr;
        if (FAILED(list->GetAt(i, &gp)) || !gp) continue;
        Pad p = {};
        bool known = false;
        for (int j = 0; j < g_padCount; ++j)
            if (g_pads[j].gp == gp) {      // same pad as before: keep its pairing, drop the old reference
                p = g_pads[j];
                g_pads[j].gp->Release();
                g_pads[j].gp = nullptr;
                known = true;
                break;
            }
        p.gp = gp;
        if (!known) {
            p.user = -1;
            IGameController* gc = nullptr;
            if (raw && SUCCEEDED(gp->QueryInterface(__uuidof(IGameController), reinterpret_cast<void**>(&gc)))) {
                IRawGameController* rc = nullptr;
                if (SUCCEEDED(raw->FromGameController(gc, &rc)) && rc) {
                    rc->get_HardwareVendorId(&p.vid);
                    rc->get_HardwareProductId(&p.pid);
                    rc->Release();
                }
                gc->Release();
            }
            // Xbox 360 class pads (wired / wireless receiver; DS4Windows' virtual pad is a wired 360 pad) have none
            p.triggers = !(p.vid == 0x045E && (p.pid == 0x028E || p.pid == 0x028F || p.pid == 0x0291 ||
                                               p.pid == 0x02A1 || p.pid == 0x0719));
            g_log("xbox: WGI gamepad %04X:%04X connected%s", p.vid, p.pid, p.triggers ? "" : " (Xbox 360 class)");
        }
        next[count++] = p;
    }
    list->Release();
    for (int j = 0; j < g_padCount; ++j)
        if (g_pads[j].gp) {               // gone from the list
            g_log("xbox: WGI gamepad %04X:%04X disconnected", g_pads[j].vid, g_pads[j].pid);
            IGamepad* gp = g_pads[j].gp;
            g_pads[j].gp = nullptr;
            Unpair(g_pads[j], "WGI gamepad gone");
            gp->Release();
        }
    memcpy(g_pads, next, sizeof(Pad) * count);
    g_padCount = count;
}

bool Active(const XState& x) {
    const int dz = 12000;
    return x.buttons || x.lt > 30 || x.rt > 30 || abs(x.lx) > dz || abs(x.ly) > dz || abs(x.rx) > dz || abs(x.ry) > dz;
}

bool Same(const XState& x, const GamepadReading& r) {
    // XInput button bit, WGI GamepadButtons bit
    static const WORD kMap[][2] = {{0x0010, 0x0001}, {0x0020, 0x0002}, {0x1000, 0x0004}, {0x2000, 0x0008},
                                   {0x4000, 0x0010}, {0x8000, 0x0020}, {0x0001, 0x0040}, {0x0002, 0x0080},
                                   {0x0004, 0x0100}, {0x0008, 0x0200}, {0x0100, 0x0400}, {0x0200, 0x0800},
                                   {0x0040, 0x1000}, {0x0080, 0x2000}};
    unsigned wb = static_cast<unsigned>(r.Buttons);
    for (auto& m : kMap)
        if (!(x.buttons & m[0]) != !(wb & m[1])) return false;
    auto close = [](double a, double b) { return fabs(a - b) <= 0.04; };
    return close(x.lt / 255.0, r.LeftTrigger) && close(x.rt / 255.0, r.RightTrigger) &&
           close(x.lx / 32767.0, r.LeftThumbstickX) && close(x.ly / 32767.0, r.LeftThumbstickY) &&
           close(x.rx / 32767.0, r.RightThumbstickX) && close(x.ry / 32767.0, r.RightThumbstickY);
}

// XInput doesn't say which WGI Gamepad a user index is. A free user and a free WGI pad are paired when their input
// matches while it is not idle and neither matches any other free one (two players pressing A at once wait), or by
// elimination: one of each left and as many WGI pads as connected XInput users.
void PairPads() {
    XState xs[kUsers] = {};
    bool conn[kUsers];
    int connected = 0;
    for (int u = 0; u < kUsers; ++u) {
        conn[u] = g_getState(u, &xs[u]) == 0;
        connected += conn[u];
    }
    bool used[kUsers] = {};
    for (int i = 0; i < g_padCount; ++i) {
        Pad& p = g_pads[i];
        if (p.user >= 0 && !conn[p.user]) Unpair(p, "XInput pad gone");
        if (p.user >= 0) used[p.user] = true;
    }
    int freeUsers = 0, freePads = 0, lastUser = -1, lastPad = -1;
    for (int u = 0; u < kUsers; ++u)
        if (conn[u] && !used[u]) { ++freeUsers; lastUser = u; }
    for (int i = 0; i < g_padCount; ++i)
        if (g_pads[i].user < 0) { ++freePads; lastPad = i; }
    if (!freeUsers || !freePads) return;
    if (freeUsers == 1 && freePads == 1 && connected == g_padCount) {
        Pair(g_pads[lastPad], lastUser, "the only pad left");
        return;
    }
    GamepadReading rd[kMaxWgi] = {};
    for (int i = 0; i < g_padCount; ++i)
        if (g_pads[i].user < 0) g_pads[i].gp->GetCurrentReading(&rd[i]);
    for (int u = 0; u < kUsers; ++u) {
        if (!conn[u] || used[u] || !Active(xs[u])) continue;
        int match = -1, matches = 0;
        for (int i = 0; i < g_padCount; ++i)
            if (g_pads[i].user < 0 && Same(xs[u], rd[i])) { match = i; ++matches; }
        if (matches != 1) continue;
        bool other = false;
        for (int v = 0; v < kUsers; ++v)
            if (v != u && conn[v] && !used[v] && Same(xs[v], rd[match])) other = true;
        if (other) continue;
        Pair(g_pads[match], u, "matched by input");
        used[u] = true;
    }
}

void Apply(DWORD now) {
    for (int i = 0; i < g_padCount; ++i) {
        Pad& p = g_pads[i];
        if (p.user < 0 || !p.triggers) continue;
        float w[4];
        EnterCriticalSection(&g_lock);
        memcpy(w, g_want[p.user], sizeof(w));
        LeaveCriticalSection(&g_lock);
        bool on = w[0] > 0 || w[1] > 0 || w[2] > 0 || w[3] > 0;
        // resent every second while on: Windows drops a pad's vibration when the window loses focus
        if (p.sentValid && !memcmp(w, p.sent, sizeof(w)) && !(on && now - p.sentAt >= 1000)) continue;
        // under Wine at most 20 sends/s, the latest value: more may queue up on a Bluetooth link
        static const bool wine = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != nullptr;
        if (wine && p.sentValid && now - p.sentAt < 50) continue;
        GamepadVibration v = {w[0], w[1], w[2], w[3]};
        if (SUCCEEDED(p.gp->put_Vibration(v))) {
            memcpy(p.sent, w, sizeof(w));
            p.sentAt = now;
            p.sentValid = true;
        }
    }
}

// Some desktop apps see an empty Gamepads list until something listens to GamepadAdded; the list itself is polled.
typedef ABI::Windows::Foundation::IEventHandler<Gamepad*> GamepadHandler;
struct AddedHandler : GamepadHandler {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(GamepadHandler) || iid == __uuidof(IAgileObject)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }    // static object
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IGamepad*) override { return S_OK; }
};
AddedHandler g_added;

DWORD WINAPI WgiThread(void*) {
    g_roInitialize(RO_INIT_MULTITHREADED);
    auto* statics = Factory<IGamepadStatics>(L"Windows.Gaming.Input.Gamepad");
    auto* raw = Factory<IRawGameControllerStatics>(L"Windows.Gaming.Input.RawGameController");
    if (!statics) {
        g_log("xbox: Windows.Gaming.Input not available; no impulse triggers");
        return 0;
    }
    EventRegistrationToken token;
    statics->add_GamepadAdded(&g_added, &token);
    g_log("xbox: Windows.Gaming.Input started");
    DWORD lastRefresh = GetTickCount() - 1000;
    for (;;) {
        DWORD now = GetTickCount();
        if (now - lastRefresh >= 1000) {   // the list fills in shortly after the first call, and on hotplug
            Refresh(statics, raw);
            lastRefresh = now;
        }
        PairPads();
        Apply(now);
        Sleep(8);
    }
}

}  // namespace

bool WgiInstall(WgiXInputGetStateFn getState, WgiLogFn log) {
    HMODULE combase = LoadLibraryA("combase.dll");
    if (!combase || !getState) return false;
    g_roInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
    g_roGetActivationFactory =
        reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
    g_createString = reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
    g_deleteString = reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
    if (!g_roInitialize || !g_roGetActivationFactory || !g_createString || !g_deleteString) return false;
    g_getState = getState;
    g_log = log;
    InitializeCriticalSection(&g_lock);
    HANDLE t = CreateThread(nullptr, 0, WgiThread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

bool WgiHasTriggers(int user) { return user >= 0 && user < kUsers && g_hasTriggers[user]; }

void WgiSetVibration(int user, float largeMotor, float smallMotor, float leftTrigger, float rightTrigger) {
    if (user < 0 || user >= kUsers || !g_getState) return;
    auto clamp = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    EnterCriticalSection(&g_lock);
    g_want[user][0] = clamp(largeMotor);
    g_want[user][1] = clamp(smallMotor);
    g_want[user][2] = clamp(leftTrigger);
    g_want[user][3] = clamp(rightTrigger);
    LeaveCriticalSection(&g_lock);
}
