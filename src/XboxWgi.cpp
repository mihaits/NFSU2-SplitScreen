// Xbox pads through Windows.Gaming.Input (XboxWgi.h, docs/input.md).
//
// One thread (MTA) owns the WGI list: it keeps each Gamepad on its pad number by its id and sends the wanted vibration.
// The game thread reads a pad directly, so a reading is as fresh as WGI's. WinRT is loaded from combase.dll at run
// time, so the plugin still loads where it is missing.

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
typedef PCWSTR(WINAPI* WindowsGetStringRawBufferFn)(HSTRING, UINT32*);
RoInitializeFn g_roInitialize;
RoGetActivationFactoryFn g_roGetActivationFactory;
WindowsCreateStringFn g_createString;
WindowsDeleteStringFn g_deleteString;
WindowsGetStringRawBufferFn g_stringBuffer;

constexpr int kPads = 8, kMaxWgi = 16;
WgiLogFn g_log;
CRITICAL_SECTION g_lock;
HANDLE g_listChanged;
IGamepad* g_slotPad[kPads];           // pad number -> its pad (under g_lock; the Pad in g_pads holds the reference)
float g_want[kPads][4];               // per pad number: large, small, left trigger, right trigger (under g_lock)
volatile LONG g_hasTriggers[kPads];   // per pad number
volatile LONG g_live;                 // WGI has listed a pad; until then a pad number is its XInput user

// thread only
struct Pad {
    IGamepad* gp;
    int slot;                         // its pad number, -1 = none free
    bool triggers;                    // has trigger motors (not an Xbox 360 class pad)
    UINT16 vid, pid;
    UINT32 id;                        // hash of IRawGameController2.NonRoamableId, else VID:PID, 0 = none
    float sent[4];
    DWORD sentAt;
    bool sentValid;
};
Pad g_pads[kMaxWgi];
int g_padCount;
struct Slot {
    UINT32 id;                        // the pad that has this number, 0 = none
    DWORD droppedAt;
};
Slot g_slots[kPads];

template <class T> T* Factory(const wchar_t* cls) {
    HSTRING name = nullptr;
    T* f = nullptr;
    if (FAILED(g_createString(cls, static_cast<UINT32>(wcslen(cls)), &name))) return nullptr;
    if (FAILED(g_roGetActivationFactory(name, __uuidof(T), reinterpret_cast<void**>(&f)))) f = nullptr;
    g_deleteString(name);
    return f;
}

UINT32 PadId(IRawGameController* rc) {
    IRawGameController2* rc2 = nullptr;
    if (FAILED(rc->QueryInterface(__uuidof(IRawGameController2), reinterpret_cast<void**>(&rc2))) || !rc2) return 0;
    HSTRING id = nullptr;
    UINT32 hash = 0;
    if (SUCCEEDED(rc2->get_NonRoamableId(&id)) && id) {
        UINT32 len = 0;
        const wchar_t* s = g_stringBuffer(id, &len);
        hash = 2166136261u;           // FNV-1a
        for (UINT32 i = 0; i < len; ++i) hash = (hash ^ s[i]) * 16777619u;
        g_deleteString(id);
    }
    rc2->Release();
    return hash;
}

// A number no pad has, else the one whose pad dropped longest ago, -1 = all taken.
int FreeSlot() {
    int oldest = -1;
    for (int s = 0; s < kPads; ++s) {
        if (g_slotPad[s]) continue;
        if (!g_slots[s].id) return s;
        if (oldest < 0 || g_slots[s].droppedAt - g_slots[oldest].droppedAt >= 0x80000000u) oldest = s;
    }
    if (oldest >= 0) g_slots[oldest].id = 0;
    return oldest;
}

// A pad gets back the number it had, so it keeps its player when pads return in another order.
void Bind(Pad& p) {
    int slot = -1;
    for (int s = 0; s < kPads; ++s)
        if (p.id && g_slots[s].id == p.id && !g_slotPad[s]) slot = s;
    if (slot < 0) slot = FreeSlot();
    if (slot < 0) return;
    g_slots[slot].id = p.id;
    p.slot = slot;
    p.sentValid = false;
    InterlockedExchange(&g_hasTriggers[slot], p.triggers ? 1 : 0);
    EnterCriticalSection(&g_lock);
    g_slotPad[slot] = p.gp;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_live, 1);
    g_log("xbox: pad %d = WGI gamepad %04X:%04X%s", slot + 1, p.vid, p.pid, p.triggers ? ", impulse triggers" : "");
}

void Unbind(Pad& p) {
    if (p.slot < 0) return;
    g_log("xbox: pad %d lost WGI gamepad %04X:%04X", p.slot + 1, p.vid, p.pid);
    InterlockedExchange(&g_hasTriggers[p.slot], 0);
    EnterCriticalSection(&g_lock);
    g_slotPad[p.slot] = nullptr;
    LeaveCriticalSection(&g_lock);
    g_slots[p.slot].droppedAt = GetTickCount();
    p.slot = -1;
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
            if (g_pads[j].gp == gp) {      // same pad as before: keep its number, drop the old reference
                p = g_pads[j];
                g_pads[j].gp->Release();
                g_pads[j].gp = nullptr;
                known = true;
                break;
            }
        p.gp = gp;
        if (!known) {
            p.slot = -1;
            IGameController* gc = nullptr;
            if (raw && SUCCEEDED(gp->QueryInterface(__uuidof(IGameController), reinterpret_cast<void**>(&gc)))) {
                IRawGameController* rc = nullptr;
                if (SUCCEEDED(raw->FromGameController(gc, &rc)) && rc) {
                    rc->get_HardwareVendorId(&p.vid);
                    rc->get_HardwareProductId(&p.pid);
                    p.id = PadId(rc);
                    if (!p.id) p.id = p.vid << 16 | p.pid;   // none under Wine: tells models apart, not two of a model
                    rc->Release();
                }
                gc->Release();
            }
            // Xbox 360 class pads (wired / wireless receiver; DS4Windows' virtual pad is a wired 360 pad) have none
            p.triggers = !(p.vid == 0x045E && (p.pid == 0x028E || p.pid == 0x028F || p.pid == 0x0291 ||
                                               p.pid == 0x02A1 || p.pid == 0x0719));
            g_log("xbox: WGI gamepad %04X:%04X id %08X connected%s", p.vid, p.pid, p.id,
                  p.triggers ? "" : " (Xbox 360 class)");
        }
        next[count++] = p;
    }
    list->Release();
    for (int j = 0; j < g_padCount; ++j)
        if (g_pads[j].gp) {               // gone from the list
            g_log("xbox: WGI gamepad %04X:%04X disconnected", g_pads[j].vid, g_pads[j].pid);
            Unbind(g_pads[j]);
            g_pads[j].gp->Release();
            g_pads[j].gp = nullptr;
        }
    memcpy(g_pads, next, sizeof(Pad) * count);
    g_padCount = count;
    for (int i = 0; i < g_padCount; ++i)
        if (g_pads[i].slot < 0) Bind(g_pads[i]);
}

void Apply(DWORD now) {
    for (int i = 0; i < g_padCount; ++i) {
        Pad& p = g_pads[i];
        if (p.slot < 0) continue;
        float w[4];
        EnterCriticalSection(&g_lock);
        memcpy(w, g_want[p.slot], sizeof(w));
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

// Some desktop apps see an empty Gamepads list until something listens to GamepadAdded. The events only wake the thread.
typedef ABI::Windows::Foundation::IEventHandler<Gamepad*> GamepadHandler;
struct ListHandler : GamepadHandler {
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
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable*, IGamepad*) override {
        SetEvent(g_listChanged);
        return S_OK;
    }
};
ListHandler g_listHandler;

DWORD WINAPI WgiThread(void*) {
    g_roInitialize(RO_INIT_MULTITHREADED);
    auto* statics = Factory<IGamepadStatics>(L"Windows.Gaming.Input.Gamepad");
    auto* raw = Factory<IRawGameControllerStatics>(L"Windows.Gaming.Input.RawGameController");
    if (!statics) {
        g_log("xbox: Windows.Gaming.Input not available; pads read through XInput, no impulse triggers");
        return 0;
    }
    EventRegistrationToken added, removed;
    statics->add_GamepadAdded(&g_listHandler, &added);
    statics->add_GamepadRemoved(&g_listHandler, &removed);
    g_log("xbox: Windows.Gaming.Input started");
    DWORD lastRefresh = GetTickCount() - 1000;
    for (;;) {
        bool changed = WaitForSingleObject(g_listChanged, 8) == WAIT_OBJECT_0;
        DWORD now = GetTickCount();
        // the list fills in shortly after the first call; an event can come before the list shows the change
        if (changed || now - lastRefresh >= 1000) {
            Refresh(statics, raw);
            lastRefresh = now;
        }
        Apply(now);
    }
}

}  // namespace

bool WgiInstall(WgiLogFn log) {
    HMODULE combase = LoadLibraryA("combase.dll");
    if (!combase) return false;
    g_roInitialize = reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize"));
    g_roGetActivationFactory =
        reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
    g_createString = reinterpret_cast<WindowsCreateStringFn>(GetProcAddress(combase, "WindowsCreateString"));
    g_deleteString = reinterpret_cast<WindowsDeleteStringFn>(GetProcAddress(combase, "WindowsDeleteString"));
    g_stringBuffer = reinterpret_cast<WindowsGetStringRawBufferFn>(GetProcAddress(combase, "WindowsGetStringRawBuffer"));
    if (!g_roInitialize || !g_roGetActivationFactory || !g_createString || !g_deleteString || !g_stringBuffer)
        return false;
    g_log = log;
    InitializeCriticalSection(&g_lock);
    g_listChanged = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, WgiThread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

bool WgiLive() { return g_live != 0; }

bool WgiReadPad(int pad, WgiPadState* s) {
    if (pad < 0 || pad >= kPads || !g_live) return false;
    GamepadReading r;
    bool ok = false;
    EnterCriticalSection(&g_lock);
    if (g_slotPad[pad]) ok = SUCCEEDED(g_slotPad[pad]->GetCurrentReading(&r));
    LeaveCriticalSection(&g_lock);
    if (!ok) return false;
    // XInput button bit, WGI GamepadButtons bit
    static const WORD kMap[][2] = {{0x0010, 0x0001}, {0x0020, 0x0002}, {0x1000, 0x0004}, {0x2000, 0x0008},
                                   {0x4000, 0x0010}, {0x8000, 0x0020}, {0x0001, 0x0040}, {0x0002, 0x0080},
                                   {0x0004, 0x0100}, {0x0008, 0x0200}, {0x0100, 0x0400}, {0x0200, 0x0800},
                                   {0x0040, 0x1000}, {0x0080, 0x2000}};
    unsigned wb = static_cast<unsigned>(r.Buttons);
    s->packet = static_cast<unsigned long>(r.Timestamp);
    s->buttons = 0;
    for (auto& m : kMap)
        if (wb & m[1]) s->buttons |= m[0];
    auto trigger = [](double v) { return static_cast<uint8_t>(lround(v * 255)); };   // full must be 255 (StateOf)
    auto stick = [](double v) { return static_cast<int16_t>(lround(v < 0 ? v * 32768 : v * 32767)); };
    s->leftTrigger = trigger(r.LeftTrigger);
    s->rightTrigger = trigger(r.RightTrigger);
    s->thumbLX = stick(r.LeftThumbstickX);
    s->thumbLY = stick(r.LeftThumbstickY);
    s->thumbRX = stick(r.RightThumbstickX);
    s->thumbRY = stick(r.RightThumbstickY);
    return true;
}

bool WgiHasTriggers(int pad) { return pad >= 0 && pad < kPads && g_hasTriggers[pad]; }

void WgiSetVibration(int pad, float largeMotor, float smallMotor, float leftTrigger, float rightTrigger) {
    if (pad < 0 || pad >= kPads || !g_live) return;
    auto clamp = [](float v) { return v < 0 ? 0.0f : v > 1 ? 1.0f : v; };
    EnterCriticalSection(&g_lock);
    g_want[pad][0] = clamp(largeMotor);
    g_want[pad][1] = clamp(smallMotor);
    g_want[pad][2] = clamp(leftTrigger);
    g_want[pad][3] = clamp(rightTrigger);
    LeaveCriticalSection(&g_lock);
}
