// Native DualSense backend: HID over SetupAPI + hid.dll. Byte layouts, init sequence and CRC are described in
// docs/input.md ("Native DualSense").

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <tlhelp32.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include "DualSense.h"

namespace {

enum : WORD {   // XInput button bits
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LTHUMB = 0x0040, XB_RTHUMB = 0x0080,
    XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

constexpr WORD kVid = 0x054C, kPidDualSense = 0x0CE6, kPidEdge = 0x0DF2;
constexpr DWORD kKeepAliveBtMs = 500;     // BT: re-send when nothing else was sent
constexpr DWORD kKeepAliveRumbleMs = 4000;
constexpr DWORD kMinGapBtMs = 25;         // BT: at most 40 reports/s per pad (the lightbar changes every frame)
constexpr DWORD kRetryMs = 250;           // after a failed output report

struct Slot {
    volatile LONG alive;                  // I/O thread running
    volatile LONG stop;
    HANDLE thread;
    HANDLE dev;
    HANDLE wake;                          // auto-reset: new wanted output
    char path[MAX_PATH];
    char mac[13];                         // the pad's Bluetooth address (hex); kept after it drops: its slot is kept for it
    DWORD lostAt;
    bool usb, edge;
    int rumbleV2;
    USHORT inLen, outLen;
    CRITICAL_SECTION cs;
    bool have;
    DsPadState st;
    DsExtra ex;
    DsOutput want;
};

Slot g_slot[kDsMax];
bool g_enabled;
int g_rumbleMode = -1;
DsLogFn g_log;
int g_ds4wState = -1;                     // 0 not running, 1 running without a virtual pad, 2 virtual pad exposed

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char line[256];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    g_log("%s", line);
}

// ---------------------------------------------------------------- CRC32 (Bluetooth output reports)
uint32_t Crc32Update(uint32_t c, const uint8_t* p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & -static_cast<int32_t>(c & 1));
    }
    return c;
}

void BtSign(uint8_t* r) {                 // r[78]: seed 0xA2 over r[0..73], little-endian at 74..77
    uint8_t seed = 0xA2;
    uint32_t c = ~Crc32Update(Crc32Update(0xFFFFFFFFu, &seed, 1), r, 74);
    r[74] = static_cast<uint8_t>(c); r[75] = static_cast<uint8_t>(c >> 8);
    r[76] = static_cast<uint8_t>(c >> 16); r[77] = static_cast<uint8_t>(c >> 24);
}

// ---------------------------------------------------------------- input
int16_t Stick(int v, bool invert) {
    int s = (v - 128) * 258;
    if (invert) s = -s;
    return static_cast<int16_t>(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
}

void ParseInput(Slot* s, const uint8_t* buf, DWORD n) {
    const uint8_t* b;                     // b[offset] with the USB offsets of the protocol doc
    if (s->usb) {
        if (n < 11 || buf[0] != 0x01) return;
        b = buf;
    } else {
        if (n < 12 || buf[0] != 0x31) return;   // BT: ignore the short report (id 0x01)
        b = buf + 1;
    }
    DsPadState st = {};
    st.thumbLX = Stick(b[1], false); st.thumbLY = Stick(b[2], true);
    st.thumbRX = Stick(b[3], false); st.thumbRY = Stick(b[4], true);
    st.leftTrigger = b[5]; st.rightTrigger = b[6];
    uint8_t face = b[8], mid = b[9];
    WORD btn = 0;
    if (face & 0x10) btn |= XB_X;         // Square
    if (face & 0x20) btn |= XB_A;         // Cross
    if (face & 0x40) btn |= XB_B;         // Circle
    if (face & 0x80) btn |= XB_Y;         // Triangle
    if (mid & 0x01) btn |= XB_LB;
    if (mid & 0x02) btn |= XB_RB;
    if (mid & 0x10) btn |= XB_BACK;       // Create
    if (mid & 0x20) btn |= XB_START;      // Options
    if (mid & 0x40) btn |= XB_LTHUMB;
    if (mid & 0x80) btn |= XB_RTHUMB;
    switch (face & 0x0F) {                // hat: 0 N, 1 NE, ... 7 NW, >= 8 none
        case 0: btn |= XB_DPAD_UP; break;
        case 1: btn |= XB_DPAD_UP | XB_DPAD_RIGHT; break;
        case 2: btn |= XB_DPAD_RIGHT; break;
        case 3: btn |= XB_DPAD_RIGHT | XB_DPAD_DOWN; break;
        case 4: btn |= XB_DPAD_DOWN; break;
        case 5: btn |= XB_DPAD_DOWN | XB_DPAD_LEFT; break;
        case 6: btn |= XB_DPAD_LEFT; break;
        case 7: btn |= XB_DPAD_LEFT | XB_DPAD_UP; break;
    }
    st.buttons = btn;
    DsExtra ex = {};
    ex.ps = b[10] & 0x01; ex.touchClick = b[10] & 0x02; ex.mute = b[10] & 0x04;
    ex.battery = n > 53 ? b[53] & 0x0F : 0;
    ex.usb = s->usb;
    EnterCriticalSection(&s->cs);
    st.packet = s->st.packet + 1;
    s->st = st;
    s->ex = ex;
    s->have = true;
    LeaveCriticalSection(&s->cs);
}

// ---------------------------------------------------------------- output
// flag0: 0x01 compatible vibration, 0x02 haptics select, 0x04 right trigger, 0x08 left trigger
// flag1: 0x04 lightbar colour, 0x10 player LEDs; flag2: 0x01 LED brightness, 0x02 lightbar setup, 0x04 vibration v2
DWORD BuildReport(const Slot* s, const DsOutput& o, bool setup, uint8_t* r, int seq) {
    memset(r, 0, s->outLen);
    uint8_t* c;
    if (s->usb) { r[0] = 0x02; c = r + 1; }
    else { r[0] = 0x31; r[1] = static_cast<uint8_t>((seq & 0x0F) << 4); r[2] = 0x10; c = r + 3; }
    if (setup) {
        c[38] = 0x02;                     // lightbar setup: light out (releases the startup blue)
        c[41] = 0x02;
    } else {
        c[0] = 0x02;
        if (s->rumbleV2) c[38] |= 0x04; else c[0] |= 0x01;
        c[2] = o.smallMotor / 2;          // the small motor is stronger than an Xbox pad's (SDL halves it too)
        c[3] = o.largeMotor;
        if (o.trigR[0]) { c[0] |= 0x04; memcpy(c + 10, o.trigR, 11); }
        if (o.trigL[0]) { c[0] |= 0x08; memcpy(c + 21, o.trigL, 11); }
        c[1] |= 0x04 | 0x10;
        c[38] |= 0x01;
        c[42] = 0;                        // player LED brightness: high
        c[43] = o.playerLeds | 0x20;      // 0x20: change instantly
        c[44] = o.rgb[0]; c[45] = o.rgb[1]; c[46] = o.rgb[2];
    }
    if (!s->usb) BtSign(r);
    return s->outLen;
}

bool WriteReport(Slot* s, HANDLE ev, uint8_t* r, DWORD len) {
    OVERLAPPED ov = {};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD n = 0;
    if (!WriteFile(s->dev, r, len, &n, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(ev, 100) != WAIT_OBJECT_0) {
            CancelIoEx(s->dev, &ov);
            GetOverlappedResult(s->dev, &ov, &n, TRUE);
            return false;
        }
        if (!GetOverlappedResult(s->dev, &ov, &n, FALSE)) return false;
    }
    return true;
}

// ---------------------------------------------------------------- one pad
DWORD WINAPI IoThread(LPVOID param) {
    Slot* s = static_cast<Slot*>(param);
    int idx = static_cast<int>(s - g_slot);
    HANDLE readEv = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HANDLE writeEv = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ro = {};
    ro.hEvent = readEv;
    uint8_t in[256], out[128];

    // init: firmware info (also switches a Bluetooth pad to the full input report), calibration, light out
    uint32_t hw = 0, fw = 0;
    unsigned upd = 0;
    bool gotFw = false;
    for (int t = 0; t < 5 && !gotFw; ++t) {
        uint8_t f[64] = {};
        f[0] = 0x20;
        if (HidD_GetFeature(s->dev, f, sizeof(f))) {
            memcpy(&hw, f + 24, 4); memcpy(&fw, f + 28, 4);
            upd = f[44] | f[45] << 8;
            gotFw = true;
        }
    }
    for (int t = 0; t < 5; ++t) {
        uint8_t f[41] = {};
        f[0] = 0x05;
        if (HidD_GetFeature(s->dev, f, sizeof(f))) break;
    }
    int v2 = g_rumbleMode == 2 || (g_rumbleMode < 0 && (s->edge || !gotFw || upd >= 0x0215));
    s->rumbleV2 = v2;
    if (gotFw)
        Log("DualSense %d: %s%s, hardware %08X, firmware %08X, update %04X, rumble v%d%s", idx,
            s->usb ? "USB" : "Bluetooth", s->edge ? " (Edge)" : "", hw, fw, upd, v2 ? 2 : 1,
            g_rumbleMode < 0 ? " (auto)" : " (ini)");
    else
        Log("DualSense %d: %s%s, firmware info unreadable (error %lu), rumble v%d", idx, s->usb ? "USB" : "Bluetooth",
            s->edge ? " (Edge)" : "", GetLastError(), v2 ? 2 : 1);

    int seq = 0;
    bool wrote = WriteReport(s, writeEv, out, BuildReport(s, s->want, true, out, seq++));
    if (!wrote) Log("DualSense %d: light-out report failed (error %lu)", idx, GetLastError());

    DsOutput sent = {};
    bool haveSent = false, failing = false;
    DWORD lastSend = 0;
    bool pending = false;
    for (;;) {
        if (s->stop) break;
        DsOutput want;
        EnterCriticalSection(&s->cs);
        want = s->want;
        LeaveCriticalSection(&s->cs);
        DWORD now = GetTickCount();
        bool rumbling = want.smallMotor || want.largeMotor;
        bool changed = !haveSent || memcmp(&want, &sent, sizeof(want)) != 0;
        // BT changes wait for the minimum gap, except rumble starting / stopping and new trigger effects
        bool urgent = !haveSent || rumbling != (sent.smallMotor || sent.largeMotor) ||
                      memcmp(want.trigR, sent.trigR, 11) || memcmp(want.trigL, sent.trigL, 11);
        // after a failed write, wait kRetryMs before the next try (each try can block for 100 ms and stall the reads)
        DWORD gap = failing ? kRetryMs : kMinGapBtMs;
        bool gapOk = failing ? now - lastSend >= kRetryMs : s->usb || urgent || now - lastSend >= kMinGapBtMs;
        bool due = (changed && gapOk) ||
                   now - lastSend >= (rumbling ? kKeepAliveRumbleMs : 0xFFFFFFFFu) ||
                   (!s->usb && now - lastSend >= kKeepAliveBtMs);
        DWORD waitMs = changed && !due ? gap - (now - lastSend) : 100;   // wake when the held change is due
        if (due) {
            if (WriteReport(s, writeEv, out, BuildReport(s, want, false, out, seq++))) {
                if (failing) Log("DualSense %d: output reports work again", idx);
                sent = want; haveSent = true; failing = false;
            } else if (!failing) {
                Log("DualSense %d: output report failed (error %lu), retrying every %lu ms", idx, GetLastError(),
                    kRetryMs);
                failing = true;
            }
            lastSend = now;
        }
        if (!pending) {
            ResetEvent(readEv);
            DWORD n = 0;
            if (ReadFile(s->dev, in, s->inLen, &n, &ro)) { ParseInput(s, in, n); continue; }
            if (GetLastError() != ERROR_IO_PENDING) break;
            pending = true;
        }
        HANDLE hs[2] = {readEv, s->wake};
        if (WaitForMultipleObjects(2, hs, FALSE, waitMs) == WAIT_OBJECT_0) {
            DWORD n = 0;
            pending = false;
            if (!GetOverlappedResult(s->dev, &ro, &n, FALSE)) break;
            ParseInput(s, in, n);
        }
    }
    if (pending) {
        CancelIoEx(s->dev, &ro);
        DWORD n;
        GetOverlappedResult(s->dev, &ro, &n, TRUE);
    }
    if (!s->stop) Log("DualSense %d: lost (error %lu)", idx, GetLastError());
    EnterCriticalSection(&s->cs);
    s->have = false;
    memset(&s->st, 0, sizeof(s->st));
    LeaveCriticalSection(&s->cs);
    CloseHandle(s->dev);
    CloseHandle(readEv);
    CloseHandle(writeEv);
    s->lostAt = GetTickCount();
    InterlockedExchange(&s->alive, 0);
    return 0;
}

// ---------------------------------------------------------------- hotplug
bool Ds4WindowsRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 e = {};
    e.dwSize = sizeof(e);
    bool found = false;
    for (BOOL ok = Process32First(snap, &e); ok && !found; ok = Process32Next(snap, &e))
        found = _stricmp(e.szExeFile, "DS4Windows.exe") == 0;
    CloseHandle(snap);
    return found;
}

// DS4Windows only takes the pad over while it exposes a virtual Xbox pad: a present device whose parent is a ViGEm
// bus (hardware id Nefarius\ViGEmBus\Gen1). Open but stopped, no such child exists.
bool ViGEmPadPresent() {
    HDEVINFO set = SetupDiGetClassDevsA(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        DEVINST parent;
        char id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Parent(&parent, dev.DevInst, 0) != CR_SUCCESS) continue;
        if (CM_Get_Device_IDA(parent, id, sizeof(id), 0) != CR_SUCCESS || _strnicmp(id, "ROOT\\SYSTEM\\", 12)) continue;
        char hw[512] = {};
        ULONG len = sizeof(hw) - 2;
        if (CM_Get_DevNode_Registry_PropertyA(parent, CM_DRP_HARDWAREID, nullptr, hw, &len, 0) != CR_SUCCESS) continue;
        for (const char* p = hw; *p && !found; p += strlen(p) + 1)
            found = _strnicmp(p, "Nefarius\\ViGEmBus", 17) == 0;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

void ReapSlots(bool all) {
    for (auto& s : g_slot) {
        if (!s.thread) continue;
        if (all) InterlockedExchange(&s.stop, 1);
        if (s.alive) {
            if (!all) continue;
            WaitForSingleObject(s.thread, 2000);
        }
        CloseHandle(s.thread);
        s.thread = nullptr;
        s.path[0] = 0;
    }
}

// The pad's Bluetooth address as 12 hex digits, the same over USB and Bluetooth: feature report 0x09 (pairing info,
// address at 1..6, last byte first, as hid-playstation and SDL read it), else the HID serial string (Bluetooth).
bool ReadMac(HANDLE h, char out[13]) {
    uint8_t f[64] = {};
    f[0] = 0x09;
    if (HidD_GetFeature(h, f, sizeof(f)) && (f[1] | f[2] | f[3] | f[4] | f[5] | f[6])) {
        sprintf_s(out, 13, "%02x%02x%02x%02x%02x%02x", f[6], f[5], f[4], f[3], f[2], f[1]);
        return true;
    }
    wchar_t w[64] = {};
    if (!HidD_GetSerialNumberString(h, w, sizeof(w))) return false;
    int n = 0;
    for (const wchar_t* p = w; *p && n < 12; ++p)
        if (iswxdigit(*p)) out[n++] = static_cast<char>(towlower(*p));
    out[n] = 0;
    return n == 12;
}

// Players are bound to the slot (pad 10 + slot), so a pad gets back the slot it had: a free slot with its address,
// else a never used one, else the free slot whose pad dropped longest ago.
Slot* SlotFor(const char* mac) {
    Slot* best = nullptr;
    for (auto& c : g_slot) {
        if (c.thread || c.alive) continue;
        if (mac[0] && !strcmp(c.mac, mac)) return &c;
        bool older = static_cast<LONG>(c.lostAt - (best ? best->lostAt : 0)) < 0;
        if (!best || (best->mac[0] && (!c.mac[0] || older))) best = &c;
    }
    return best;
}

void OpenPad(const char* path, bool edge) {
    bool anyFree = false;
    for (auto& c : g_slot) anyFree |= !c.thread && !c.alive;
    if (!anyFree) return;
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    static char busy[kDsMax * 2][MAX_PATH];     // paths whose failure was logged
    if (h == INVALID_HANDLE_VALUE) {
        for (auto& b : busy)
            if (!strcmp(b, path)) return;
        for (auto& b : busy)
            if (!b[0]) { strcpy_s(b, path); break; }
        Log("DualSense busy or not accessible (error %lu): %s", GetLastError(), path);
        return;
    }
    PHIDP_PREPARSED_DATA pp = nullptr;
    HIDP_CAPS caps = {};
    bool ok = HidD_GetPreparsedData(h, &pp);
    if (ok) { ok = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS; HidD_FreePreparsedData(pp); }
    if (!ok || caps.UsagePage != 1 || caps.Usage != 5) {   // only the gamepad interface
        Log("DualSense: skipped interface (usage %04X/%04X, caps %d): %s", caps.UsagePage, caps.Usage, ok, path);
        CloseHandle(h);
        return;
    }
    char mac[13] = {};
    if (!ReadMac(h, mac)) mac[0] = 0;
    // the same pad on its other transport (a cable plugged into a Bluetooth pad) while the old link is still open:
    // wait for that one to drop, then this one takes its slot on a later rescan
    for (auto& c : g_slot)
        if (mac[0] && c.thread && !strcmp(c.mac, mac)) {
            static char waiting[MAX_PATH];
            if (strcmp(waiting, path)) {
                strcpy_s(waiting, path);
                Log("DualSense %d: also seen on another connection, kept on the open one", static_cast<int>(&c - g_slot));
            }
            CloseHandle(h);
            return;
        }
    Slot* s = SlotFor(mac);
    bool same = mac[0] && !strcmp(s->mac, mac);
    HidD_SetNumInputBuffers(h, 3);
    s->dev = h;
    s->usb = caps.InputReportByteLength == 64;
    s->edge = edge;
    s->inLen = caps.InputReportByteLength;
    s->outLen = caps.OutputReportByteLength;
    if (!s->usb && s->outLen > 78) s->outLen = 78;   // BT descriptor lists the largest report (547); 0x31 is 78 bytes
    if (s->inLen > 256 || s->outLen > 128 || s->outLen < (s->usb ? 48 : 78)) {
        Log("DualSense: unexpected report sizes in %u out %u", s->inLen, s->outLen);
        CloseHandle(h);
        return;
    }
    strcpy_s(s->path, path);
    // the log shows only the address's last 4 digits (logs get attached to bug reports)
    Log("DualSense %d: pad ..%s, %s", static_cast<int>(s - g_slot), mac[0] ? mac + 8 : "(no address)",
        same ? "back in its slot" : s->mac[0] ? "takes a slot another pad had" : "new slot");
    strcpy_s(s->mac, mac);
    s->stop = 0;
    EnterCriticalSection(&s->cs);
    s->want.smallMotor = s->want.largeMotor = 0;   // no stale rumble from before an unplug
    memset(&s->st, 0, sizeof(s->st));
    s->have = false;
    LeaveCriticalSection(&s->cs);
    s->alive = 1;
    s->thread = CreateThread(nullptr, 0, IoThread, s, 0, nullptr);
    if (!s->thread) { s->alive = 0; CloseHandle(h); s->path[0] = 0; }
}

void Rescan() {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsA(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd = {};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd)) break;
        alignas(8) char buf[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A) + MAX_PATH];
        auto* d = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buf);
        d->cbSize = sizeof(*d);
        if (!SetupDiGetDeviceInterfaceDetailA(set, &ifd, d, sizeof(buf), nullptr, nullptr)) continue;
        char lower[MAX_PATH];
        strcpy_s(lower, d->DevicePath);
        _strlwr_s(lower);
        // USB: "vid_054c&pid_0ce6"; Bluetooth: "{00001124-...}_vid&0002054c_pid&0ce6"
        bool sony = strstr(lower, "vid_054c") || strstr(lower, "vid&0002054c");
        bool ds = sony && (strstr(lower, "pid_0ce6") || strstr(lower, "pid&0ce6"));
        bool edge = sony && (strstr(lower, "pid_0df2") || strstr(lower, "pid&0df2"));
        if (!ds && !edge) continue;
        bool open = false;
        for (auto& s : g_slot)
            if (s.thread && !_stricmp(s.path, d->DevicePath)) open = true;
        if (!open) OpenPad(d->DevicePath, edge);
    }
    SetupDiDestroyDeviceInfoList(set);
}

DWORD WINAPI HotplugThread(LPVOID) {
    for (;;) {
        int state = !Ds4WindowsRunning() ? 0 : ViGEmPadPresent() ? 2 : 1;
        bool ds4w = state == 2;
        if (state != g_ds4wState) {
            g_ds4wState = state;
            static const char* const msg[3] = {"DS4Windows not running: native DualSense on",
                                               "DS4Windows running but stopped (no virtual pad): native DualSense on",
                                               "DS4Windows is exposing a virtual pad: native DualSense off"};
            Log("%s", msg[state]);
        }
        if (ds4w) ReapSlots(true);
        else { ReapSlots(false); Rescan(); }
        Sleep(2000);
    }
}

}  // namespace

bool DsInstall(bool enabled, int rumbleMode, DsLogFn log) {
    g_log = log;
    g_enabled = enabled;
    g_rumbleMode = rumbleMode;
    for (auto& s : g_slot) {
        InitializeCriticalSection(&s.cs);
        s.wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    if (!enabled) return false;
    HANDLE t = CreateThread(nullptr, 0, HotplugThread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

int DsCount() {
    int n = 0;
    for (auto& s : g_slot) n += s.have;
    return n;
}

bool DsState(int slot, DsPadState* out, DsExtra* extra) {
    if (!g_enabled || slot < 0 || slot >= kDsMax) return false;
    Slot& s = g_slot[slot];
    EnterCriticalSection(&s.cs);
    bool have = s.have;
    if (have) {
        *out = s.st;
        if (extra) *extra = s.ex;
    }
    LeaveCriticalSection(&s.cs);
    return have;
}

void DsSetOutput(int slot, const DsOutput& o) {
    if (!g_enabled || slot < 0 || slot >= kDsMax) return;
    Slot& s = g_slot[slot];
    EnterCriticalSection(&s.cs);
    s.want = o;
    LeaveCriticalSection(&s.cs);
    SetEvent(s.wake);
}
