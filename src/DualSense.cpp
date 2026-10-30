// Native DualSense backend: HID over SetupAPI + hid.dll. Byte layouts, init sequence and CRC are described in
// docs/input.md ("Native DualSense").

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include "DualSense.h"
#include "SbcEncoder.h"
#include <opus.h>

namespace {

enum : WORD {   // XInput button bits
    XB_DPAD_UP = 0x0001, XB_DPAD_DOWN = 0x0002, XB_DPAD_LEFT = 0x0004, XB_DPAD_RIGHT = 0x0008,
    XB_START = 0x0010, XB_BACK = 0x0020, XB_LTHUMB = 0x0040, XB_RTHUMB = 0x0080,
    XB_LB = 0x0100, XB_RB = 0x0200, XB_A = 0x1000, XB_B = 0x2000, XB_X = 0x4000, XB_Y = 0x8000,
};

constexpr WORD kVid = 0x054C, kPidDualSense = 0x0CE6, kPidEdge = 0x0DF2;
constexpr DWORD kInMax = 600;             // largest input report (a Bluetooth descriptor may list 547)
constexpr DWORD kKeepAliveBtMs = 500;     // BT: re-send when nothing else was sent
constexpr DWORD kKeepAliveRumbleMs = 4000;
constexpr DWORD kMinGapBtMs = 25;         // BT: at most 40 reports/s per pad (the lightbar changes every frame)
// while the pad's speaker streams (47 audio reports/s), extra output reports cost audio: lights at most every 125 ms,
// rumble start/stop and trigger changes after 50 ms (with the lightbar at 40/s the speaker dropped out now and then)
constexpr DWORD kMinGapSpeakerMs = 125, kMinGapSpeakerUrgentMs = 50;
constexpr DWORD kRetryMs = 250;           // after a failed output report
// A pad streams full input reports every few ms. When they stop without an error (the Bluetooth link stalls; with 5
// pads on one adapter, one pad dropping froze every pad's input for ~19 s and cars spun on held steering), the
// game keeps the last input through short gaps and gets neutral input after kStallMs until reports come back. Gaps
// from kStallLogMs are logged with their length.
constexpr DWORD kStallLogMs = 250, kStallMs = 1000;

struct Slot {
    volatile LONG alive;                  // I/O thread running
    volatile LONG stop;
    HANDLE thread;
    HANDLE dev;
    HANDLE wake;                          // auto-reset: new wanted output
    char path[MAX_PATH];
    char mac[13];                         // the pad's Bluetooth address (hex); kept after it drops: its slot is kept for it
    DWORD lostAt;
    bool usb, edge, ds4;
    int rumbleV2;
    USHORT inLen, outLen;
    CRITICAL_SECTION cs;
    bool have;
    DWORD inAt;                           // tick of the last full input report
    bool stalled;                         // I/O thread only: no input report for kStallLogMs, logged
    DsPadState st;
    DsExtra ex;
    DsOutput want;
    // speaker: DsSpeakerWrite queues mono samples, SpeakerThread (Bluetooth) or UsbSpeakerThread streams them
    CRITICAL_SECTION spkCs;
    float spkBuf[16384];                  // ring, at spkRate
    int spkHead, spkCount, spkRate;
    DWORD spkLastWrite;
    HANDLE spkThread;
    volatile LONG spkStop;
    // USB: no usable audio endpoint. With spkWaitEndpoint the hotplug thread clears it once the endpoint shows up,
    // which is some seconds after the HID interface when a pad is plugged in; otherwise it stays until a reconnect.
    volatile bool spkNoDevice, spkWaitEndpoint;
};

Slot g_slot[kDsMax];
bool g_enabled;
int g_rumbleMode = -1;
DsLogFn g_log;
int g_ds4wState = -1;                     // 0 not running, 1 running without a virtual pad, 2 virtual pad exposed

const char* Model(const Slot* s) { return s->ds4 ? "DS4" : "DualSense"; }

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
    } else if (s->ds4) {
        if (n < 13 || buf[0] < 0x11 || buf[0] > 0x19) return;   // BT: ignore the short report (id 0x01)
        b = buf + 2;
    } else {
        if (n < 12 || buf[0] != 0x31) return;   // BT: ignore the short report (id 0x01)
        b = buf + 1;
    }
    // same bits on both models, at other offsets: DS4 face 5, mid 6, special 7, triggers 8/9, battery 30
    const int oFace = s->ds4 ? 5 : 8, oTrig = s->ds4 ? 8 : 5, oBattery = s->ds4 ? 30 : 53;
    DsPadState st = {};
    st.thumbLX = Stick(b[1], false); st.thumbLY = Stick(b[2], true);
    st.thumbRX = Stick(b[3], false); st.thumbRY = Stick(b[4], true);
    st.leftTrigger = b[oTrig]; st.rightTrigger = b[oTrig + 1];
    uint8_t face = b[oFace], mid = b[oFace + 1], special = b[oFace + 2];
    WORD btn = 0;
    if (face & 0x10) btn |= XB_X;         // Square
    if (face & 0x20) btn |= XB_A;         // Cross
    if (face & 0x40) btn |= XB_B;         // Circle
    if (face & 0x80) btn |= XB_Y;         // Triangle
    if (mid & 0x01) btn |= XB_LB;
    if (mid & 0x02) btn |= XB_RB;
    if (mid & 0x10) btn |= XB_BACK;       // Create (DS4: Share)
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
    ex.ps = special & 0x01; ex.touchClick = special & 0x02; ex.mute = !s->ds4 && (special & 0x04);
    ex.battery = n > static_cast<DWORD>(oBattery) ? b[oBattery] & 0x0F : 0;
    ex.usb = s->usb;
    DWORD now = GetTickCount();
    if (s->stalled) {
        s->stalled = false;
        Log("%s %d: input again after %lu ms", Model(s), static_cast<int>(s - g_slot), now - s->inAt);
    }
    EnterCriticalSection(&s->cs);
    st.packet = s->st.packet + 1;
    s->st = st;
    s->ex = ex;
    s->have = true;
    s->inAt = now;
    LeaveCriticalSection(&s->cs);
}

// ---------------------------------------------------------------- output
// flag0: 0x01 compatible vibration, 0x02 haptics select, 0x04 right trigger, 0x08 left trigger
// flag1: 0x04 lightbar colour, 0x10 player LEDs; flag2: 0x01 LED brightness, 0x02 lightbar setup, 0x04 vibration v2
// DS4: USB 0x05 (flags at 1, motors at 4) or BT 0x11 (flags at 3, motors at 6, CRC as the DualSense); flags 0x01 motors,
// 0x02 lightbar, 0x04 hardware flash; from the motors on: small, large, R, G, B, flash on, flash off.
DWORD BuildDs4Report(const Slot* s, const DsOutput& o, uint8_t* r) {
    memset(r, 0, s->outLen);
    uint8_t* c;
    if (s->usb) { r[0] = 0x05; r[1] = 0x07; c = r + 4; }
    else { r[0] = 0x11; r[1] = 0xC0 | 0x04; r[2] = 0xA0; r[3] = 0x03; c = r + 6; }
    c[0] = o.smallMotor;
    c[1] = o.largeMotor;
    c[2] = o.rgb[0]; c[3] = o.rgb[1]; c[4] = o.rgb[2];
    if (!s->usb) BtSign(r);
    return s->outLen;
}

DWORD BuildReport(const Slot* s, const DsOutput& o, bool setup, uint8_t* r, int seq) {
    if (s->ds4) return BuildDs4Report(s, o, r);
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
        c[1] |= 0x01;                     // mute LED
        c[8] = o.muteLed;
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
    uint8_t in[kInMax], out[128];

    // init: firmware info (also switches a Bluetooth pad to the full input report), calibration, light out. A DS4
    // has none of these: its calibration report 0x02 does that switch, and so does any 0x11 output report.
    uint32_t hw = 0, fw = 0;
    unsigned upd = 0;
    bool gotFw = false, ds4Init = false;
    for (int t = 0; t < 5 && s->ds4 && !ds4Init; ++t) {
        uint8_t f[64] = {};
        f[0] = 0x02;
        ds4Init = HidD_GetFeature(s->dev, f, sizeof(f)) != 0;
    }
    for (int t = 0; t < 5 && !s->ds4 && !gotFw; ++t) {
        uint8_t f[64] = {};
        f[0] = 0x20;
        if (HidD_GetFeature(s->dev, f, sizeof(f))) {
            memcpy(&hw, f + 24, 4); memcpy(&fw, f + 28, 4);
            upd = f[44] | f[45] << 8;
            gotFw = true;
        }
    }
    for (int t = 0; t < 5 && !s->ds4; ++t) {
        uint8_t f[41] = {};
        f[0] = 0x05;
        if (HidD_GetFeature(s->dev, f, sizeof(f))) break;
    }
    int v2 = g_rumbleMode == 2 || (g_rumbleMode < 0 && (s->edge || !gotFw || upd >= 0x0215));
    s->rumbleV2 = v2;
    if (s->ds4)
        Log("DS4 %d: %s, report sizes in %u out %u, calibration report %s", idx, s->usb ? "USB" : "Bluetooth", s->inLen,
            s->outLen, ds4Init ? "read" : "unreadable");
    else if (gotFw)
        Log("DualSense %d: %s%s, hardware %08X, firmware %08X, update %04X, rumble v%d%s", idx,
            s->usb ? "USB" : "Bluetooth", s->edge ? " (Edge)" : "", hw, fw, upd, v2 ? 2 : 1,
            g_rumbleMode < 0 ? " (auto)" : " (ini)");
    else
        Log("DualSense %d: %s%s, firmware info unreadable (error %lu), rumble v%d", idx, s->usb ? "USB" : "Bluetooth",
            s->edge ? " (Edge)" : "", GetLastError(), v2 ? 2 : 1);

    int seq = 0;
    bool wrote = WriteReport(s, writeEv, out, BuildReport(s, s->want, true, out, seq++));
    if (!wrote) Log("%s %d: first output report failed (error %lu)", Model(s), idx, GetLastError());

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
        bool speaker = s->spkThread != nullptr && !s->usb;
        DWORD gap = failing ? kRetryMs : speaker ? (urgent ? kMinGapSpeakerUrgentMs : kMinGapSpeakerMs) : kMinGapBtMs;
        bool gapOk = failing || speaker ? now - lastSend >= gap : s->usb || urgent || now - lastSend >= kMinGapBtMs;
        bool due = (changed && gapOk) ||
                   now - lastSend >= (rumbling ? kKeepAliveRumbleMs : 0xFFFFFFFFu) ||
                   (!s->usb && now - lastSend >= kKeepAliveBtMs);
        DWORD waitMs = changed && !due ? gap - (now - lastSend) : 100;   // wake when the held change is due
        if (due) {
            if (WriteReport(s, writeEv, out, BuildReport(s, want, false, out, seq++))) {
                if (failing) Log("%s %d: output reports work again", Model(s), idx);
                sent = want; haveSent = true; failing = false;
            } else if (!failing) {
                Log("%s %d: output report failed (error %lu), retrying every %lu ms", Model(s), idx, GetLastError(),
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
        if (s->have && !s->stalled && GetTickCount() - s->inAt >= kStallLogMs) {
            s->stalled = true;
            Log("%s %d: no input for %lu ms", Model(s), idx, GetTickCount() - s->inAt);
        }
    }
    if (pending) {
        CancelIoEx(s->dev, &ro);
        DWORD n;
        GetOverlappedResult(s->dev, &ro, &n, TRUE);
    }
    if (!s->stop) Log("%s %d: lost (error %lu)", Model(s), idx, GetLastError());
    EnterCriticalSection(&s->cs);
    s->have = false;
    memset(&s->st, 0, sizeof(s->st));
    LeaveCriticalSection(&s->cs);
    EnterCriticalSection(&s->spkCs);       // the speaker thread writes on s->dev too
    InterlockedExchange(&s->spkStop, 1);
    HANDLE spk = s->spkThread;
    s->spkThread = nullptr;
    LeaveCriticalSection(&s->spkCs);
    if (spk) { WaitForSingleObject(spk, 2000); CloseHandle(spk); }
    CloseHandle(s->dev);
    CloseHandle(readEv);
    CloseHandle(writeEv);
    s->lostAt = GetTickCount();
    InterlockedExchange(&s->alive, 0);
    return 0;
}

// ---------------------------------------------------------------- speaker (Bluetooth)
// Windows has no audio device for a Bluetooth DualSense; its speaker takes Opus in output report 0x39 (547 bytes, as
// awalol/DS5Dongle and KytyPS5 send it): [1] seq << 4, [2..9] config sub-packet 0x91 (len 6: 0x7E = no microphone
// stream back, 4x 48, [9] counter += 2), [10..75] haptics 0xD2 (len 64, silent), [140..541] speaker 0xD3 (len 200)
// with two 480-frame Opus packets (48 kHz stereo, CBR 160 kbit/s = 200 bytes each), CRC at 543. One report per
// 21.333 ms; the pad plays them at 45 kHz, so the queue is resampled to 45 kHz and encoded as if it were 48 kHz.
// Output report 0x31 with audio_control = speaker path (+ volume, preamp) routes it to the speaker first.
constexpr int kSpkRing = 16384;           // queued input samples (at the input rate)
constexpr int kSpkReport = 547, kSpkOpusBytes = 200, kSpkOut = 960;   // 960 output samples (45 kHz) per report
constexpr int kSpkTarget = 2048, kSpkMaxQueue = 8820;   // input samples: ~46 ms to start, 200 ms max
constexpr int kSpkLowTarget = 768;        // what's left after a report, at its lowest per half second (~17 ms)
constexpr int kSpkWindow = 24 * kSpkOut;  // output samples per rate correction

int SpeakerRoute(const Slot* s, uint8_t* r, int seq, int path) {  // path 3: speaker, 0: jack (USB only)
    memset(r, 0, s->outLen);
    uint8_t* c;
    if (s->usb) { r[0] = 0x02; c = r + 1; }
    else { r[0] = 0x31; r[1] = static_cast<uint8_t>((seq & 0x0F) << 4); r[2] = 0x10; c = r + 3; }
    c[0] = 0x80 | 0x20;                   // valid: audio control, speaker volume
    c[1] = 0x80;                          // valid: audio control 2
    c[5] = 0x64;                          // speaker volume (max)
    c[7] = static_cast<uint8_t>(path << 4);   // audio control: output path
    c[37] = 0x02;                         // audio control 2: speaker preamp gain
    if (!s->usb) BtSign(r);
    return s->outLen;
}

// The speaker thread's side of the queue (spkBuf), for both transports.
struct SpkQueue {
    double frac = 0;                      // resampler position past the queue head, in input samples
    double drift = 0, trim = 0;           // rate correction (clock drift, see below): integral part, total
    int winMin = 1 << 30, winMax = 0, winSamples = 0, statMin = 1 << 30, statMax = 0;
    float env = 0;                        // limiter envelope
    bool started = false, refill = false;
    int underruns = 0, dropped = 0;
    float peak = 0;
};

void SpkStatsReset(SpkQueue& q) {
    q.statMin = 1 << 30;
    q.statMax = 0;
    q.underruns = q.dropped = 0;
    q.peak = 0;
}

// Writes n (at most kSpkOut) limited mono samples at outRate, silence when the queue has none. False: idle for 1 s,
// the stream should end.
bool SpkTake(Slot* s, SpkQueue& q, float* out, int n, double outRate) {
    static float lin[kDsMax][1100];
    int idx = static_cast<int>(s - g_slot);
    // input for n output samples at positions frac + i * step (linear interpolation)
    bool have = false;
    double step = 1;
    EnterCriticalSection(&s->spkCs);
    if (GetTickCount() - s->spkLastWrite > 1000) {                       // idle: end the stream
        s->spkCount = 0;                                                  // (a few leftover samples never drain)
        LeaveCriticalSection(&s->spkCs);
        return false;
    }
    // The game's sound card and the pad's clock drift apart: read up to 1 % faster or slower so the queue neither
    // runs dry (a gap) nor fills up (dropped samples). The game fills its sound buffer in bursts (up to ~65 ms at
    // once), so the queue is a sawtooth: what has to stay above 0 is its lowest point after a take, held near
    // kSpkLowTarget per half second (proportional + integral); a gap slows it down at once.
    step = s->spkRate / outRate * (1 + q.trim);
    int need = static_cast<int>(q.frac + (n - 1) * step) + 2;
    if (q.started) {
        if (s->spkCount - need < q.winMin) q.winMin = s->spkCount - need;
        if (s->spkCount > q.winMax) q.winMax = s->spkCount;
        q.winSamples += n;
        if (q.winSamples >= kSpkWindow) {
            double err = static_cast<double>(q.winMin - kSpkLowTarget) / kSpkTarget;
            q.drift += err * 0.001;
            if (q.drift > 0.005) q.drift = 0.005;
            if (q.drift < -0.005) q.drift = -0.005;
            q.trim = q.drift + err * 0.003;
            if (q.trim > 0.01) q.trim = 0.01;
            if (q.trim < -0.01) q.trim = -0.01;
            if (q.winMin < q.statMin) q.statMin = q.winMin;
            if (q.winMax > q.statMax) q.statMax = q.winMax;
            q.winMin = 1 << 30;
            q.winMax = q.winSamples = 0;
        }
    }
    if (s->spkCount > kSpkMaxQueue) {                                    // fell behind: keep the newest
        int drop = s->spkCount - kSpkTarget;
        s->spkHead = (s->spkHead + drop) % kSpkRing;
        s->spkCount -= drop;
        q.dropped += drop;
    }
    // first start: a full kSpkTarget; after a gap only a take plus the reserve, so the windows that follow
    // don't see a refill-sized reserve and speed up again
    if (!q.started && s->spkCount >= (q.refill ? need + kSpkLowTarget : kSpkTarget)) q.started = true;
    if (q.started && s->spkCount >= need && need <= 1100) {
        for (int i = 0; i < need; ++i) lin[idx][i] = s->spkBuf[(s->spkHead + i) % kSpkRing];
        have = true;
        int used = static_cast<int>(q.frac + n * step);
        s->spkHead = (s->spkHead + used) % kSpkRing;
        s->spkCount -= used;
    } else if (q.started) {
        q.started = false;
        q.refill = true;
        ++q.underruns;
        q.drift -= 0.0005;
        if (q.drift < -0.005) q.drift = -0.005;
        q.trim = q.drift;
        q.winMin = 1 << 30;
        q.winMax = q.winSamples = 0;
    }
    LeaveCriticalSection(&s->spkCs);
    for (int i = 0; i < n; ++i) {
        float v = 0;
        if (have) {
            double x = q.frac + i * step;
            int j = static_cast<int>(x);
            float t = static_cast<float>(x - j);
            v = lin[idx][j] + (lin[idx][j + 1] - lin[idx][j]) * t;
            float a = v < 0 ? -v : v;   // limiter: instant attack, ~50 ms release, ceiling 0.9 (no hard clipping)
            if (a > q.peak) q.peak = a;
            q.env = a > q.env ? a : q.env * 0.9995f;
            if (q.env > 0.9f) v *= 0.9f / q.env;
        }
        out[i] = v;
    }
    if (have) q.frac = q.frac + n * step - static_cast<int>(q.frac + n * step);
    return true;
}

// Last step of a speaker thread: while the handle is set no new stream starts (the USB route restore can't land after
// a new stream's route). A lost pad's I/O thread has already taken the handle.
void SpkFinish(Slot* s) {
    EnterCriticalSection(&s->spkCs);
    if (s->spkThread) { CloseHandle(s->spkThread); s->spkThread = nullptr; }
    LeaveCriticalSection(&s->spkCs);
}

DWORD WINAPI SpeakerThread(LPVOID param) {
    Slot* s = static_cast<Slot*>(param);
    int idx = static_cast<int>(s - g_slot);
    int err = 0;
    OpusEncoder* enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    if (enc) {
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(kSpkOpusBytes * 8 * 100));
        opus_encoder_ctl(enc, OPUS_SET_VBR(0));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    }
    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    static uint8_t reports[kDsMax][kSpkReport];
    static float mono[kDsMax][kSpkOut], pcm[kDsMax][kSpkOut * 2];
    uint8_t* rep = reports[idx];
    bool ok = enc && WriteReport(s, ev, rep, SpeakerRoute(s, rep, 0, 3));
    Log("DualSense %d: speaker stream %s", idx, ok ? "started" : enc ? "route report failed" : "no Opus encoder");
    timeBeginPeriod(1);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double period = 64.0 / 3000;
    double next = static_cast<double>(now.QuadPart) / freq.QuadPart;
    SpkQueue q;
    bool failing = false;
    uint8_t seq = 1, counter = 0;
    int sent = 0, shortPackets = 0;
    double lateMax = 0;
    DWORD statsAt = GetTickCount();
    while (ok && !s->spkStop) {
        if (!SpkTake(s, q, mono[idx], kSpkOut, 45000.0)) break;
        float* out = pcm[idx];
        for (int i = 0; i < kSpkOut; ++i) out[2 * i] = out[2 * i + 1] = mono[idx][i];
        memset(rep, 0, kSpkReport);
        rep[0] = 0x39; rep[1] = static_cast<uint8_t>(seq++ << 4);
        rep[2] = 0x91; rep[3] = 6; rep[4] = 0x7E; rep[5] = rep[6] = rep[7] = rep[8] = 48;
        counter += 2;
        rep[9] = counter;
        rep[10] = 0xD2; rep[11] = 64;
        rep[140] = 0xD3; rep[141] = kSpkOpusBytes;
        for (int k = 0; k < 2; ++k)            // CBR: 200 bytes each; a short packet is zero-padded
            if (opus_encode_float(enc, out + k * kSpkOut, kSpkOut / 2, rep + 142 + k * kSpkOpusBytes, kSpkOpusBytes) !=
                kSpkOpusBytes)
                ++shortPackets;
        uint8_t seed = 0xA2;
        uint32_t c = ~Crc32Update(Crc32Update(0xFFFFFFFFu, &seed, 1), rep, kSpkReport - 4);
        rep[543] = static_cast<uint8_t>(c); rep[544] = static_cast<uint8_t>(c >> 8);
        rep[545] = static_cast<uint8_t>(c >> 16); rep[546] = static_cast<uint8_t>(c >> 24);
        QueryPerformanceCounter(&now);
        double late = static_cast<double>(now.QuadPart) / freq.QuadPart - next;
        if (late > lateMax) lateMax = late;
        if (WriteReport(s, ev, rep, kSpkReport)) {
            if (failing) Log("DualSense %d: speaker reports work again", idx);
            failing = false;
            ++sent;
        } else if (!failing) {
            Log("DualSense %d: speaker report failed (error %lu)", idx, GetLastError());
            failing = true;
        }
        DWORD t = GetTickCount();
        if (t - statsAt >= 5000) {
            Log("DualSense %d: speaker %d reports, %d underruns, %d samples dropped, peak %.2f (limited above 0.90), "
                "left %d, queue max %d, %d short Opus packets, max late %.1f ms, rate %+.3f %%", idx, sent, q.underruns,
                q.dropped, q.peak, q.statMin == 1 << 30 ? 0 : q.statMin, q.statMax, shortPackets, lateMax * 1000,
                q.trim * 100);
            SpkStatsReset(q);
            sent = shortPackets = 0;
            lateMax = 0;
            statsAt = t;
        }
        next += period;
        for (;;) {
            QueryPerformanceCounter(&now);
            double d = next - static_cast<double>(now.QuadPart) / freq.QuadPart;
            if (d <= 0) { if (d < -0.1) next -= d; break; }   // far behind (a stall): don't send a burst
            if (d > 0.002) Sleep(1);
        }
    }
    timeEndPeriod(1);
    if (enc) opus_encoder_destroy(enc);
    CloseHandle(ev);
    Log("DualSense %d: speaker stream stopped", idx);
    SpkFinish(s);
    return 0;
}

// ---------------------------------------------------------------- speaker (DS4, Bluetooth)
// The speaker takes SBC in output report 0x17 (462 bytes, as PadForge documents it, checked by ear with
// tools/ds4_bt_speaker.py): [1] 0x40, [2] 0xA0, [3..4] frame counter += 4, [5] 0x02 speaker, [6..] 4 frames, CRC32 seed 0xA2
// at 458. Each frame is 128 samples at 32 kHz, so one report per 16 ms. Report 0x11 with flag 0x80 and the speaker
// volume at 24 turns it on first. Queue, rate trim and limiter are shared (SpkTake, at 32 kHz here).
constexpr int kDs4SpkReport = 462, kDs4SpkFrames = 4, kDs4SpkVolume = 0x4F;

DWORD WINAPI Ds4SpeakerThread(LPVOID param) {
    Slot* s = static_cast<Slot*>(param);
    int idx = static_cast<int>(s - g_slot);
    HANDLE ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    static uint8_t reports[kDsMax][kDs4SpkReport];
    static float mono[kDsMax][kSbcFrameSamples * kDs4SpkFrames];
    static SbcEncoder encoders[kDsMax];
    SbcEncoder& enc = encoders[idx];
    enc = SbcEncoder();
    uint8_t* rep = reports[idx];
    uint8_t on[78] = {0x11, 0xC0, 0xA0, 0x80};
    on[24] = kDs4SpkVolume;
    BtSign(on);
    bool ok = WriteReport(s, ev, on, sizeof(on));
    Log("DS4 %d: speaker stream %s", idx, ok ? "started" : "enable report failed");
    timeBeginPeriod(1);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const double period = 16.0 / 1000;
    double next = static_cast<double>(now.QuadPart) / freq.QuadPart;
    SpkQueue q;
    bool failing = false;
    uint16_t counter = 0;
    int sent = 0;
    double lateMax = 0;
    DWORD statsAt = GetTickCount();
    while (ok && !s->spkStop) {
        if (!SpkTake(s, q, mono[idx], kSbcFrameSamples * kDs4SpkFrames, 32000.0)) break;
        memset(rep, 0, kDs4SpkReport);
        rep[0] = 0x17; rep[1] = 0x40; rep[2] = 0xA0;
        rep[3] = static_cast<uint8_t>(counter); rep[4] = static_cast<uint8_t>(counter >> 8);
        rep[5] = 0x02;
        counter += kDs4SpkFrames;
        for (int k = 0; k < kDs4SpkFrames; ++k)
            enc.Encode(mono[idx] + k * kSbcFrameSamples, rep + 6 + k * kSbcFrameBytes);
        uint8_t seed = 0xA2;
        uint32_t c = ~Crc32Update(Crc32Update(0xFFFFFFFFu, &seed, 1), rep, kDs4SpkReport - 4);
        memcpy(rep + kDs4SpkReport - 4, &c, 4);
        QueryPerformanceCounter(&now);
        double late = static_cast<double>(now.QuadPart) / freq.QuadPart - next;
        if (late > lateMax) lateMax = late;
        if (WriteReport(s, ev, rep, kDs4SpkReport)) {
            if (failing) Log("DS4 %d: speaker reports work again", idx);
            failing = false;
            ++sent;
        } else if (!failing) {
            Log("DS4 %d: speaker report failed (error %lu)", idx, GetLastError());
            failing = true;
        }
        DWORD t = GetTickCount();
        if (t - statsAt >= 5000) {
            Log("DS4 %d: speaker %d reports, %d underruns, %d samples dropped, peak %.2f (limited above 0.90), left %d, "
                "queue max %d, max late %.1f ms, rate %+.3f %%", idx, sent, q.underruns, q.dropped, q.peak,
                q.statMin == 1 << 30 ? 0 : q.statMin, q.statMax, lateMax * 1000, q.trim * 100);
            SpkStatsReset(q);
            sent = 0;
            lateMax = 0;
            statsAt = t;
        }
        next += period;
        for (;;) {
            QueryPerformanceCounter(&now);
            double d = next - static_cast<double>(now.QuadPart) / freq.QuadPart;
            if (d <= 0) { if (d < -0.1) next -= d; break; }   // far behind (a stall): don't send a burst
            if (d > 0.002) Sleep(1);
        }
    }
    timeEndPeriod(1);
    CloseHandle(ev);
    Log("DS4 %d: speaker stream stopped", idx);
    SpkFinish(s);
    return 0;
}

// ---------------------------------------------------------------- speaker (USB)
// Over USB the pad is also a Windows audio device: a render endpoint of 4 channels at 48 kHz (speaker tap L/R, haptics
// L/R), which plays on the jack unless output path 3 is set (SpeakerRoute): that mutes the jack and plays channel 2 on
// the speaker. Channels 3-4 stay zero. The endpoint is the render device with the pad's container id.
const PROPERTYKEY kPkeyContainerId = {{0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
const DEVPROPKEY kDevpkeyContainerId = {{0x8C7ED206, 0x3F8A, 0x4827, {0xB3, 0xAB, 0xAE, 0x9E, 0x1F, 0xAE, 0xFC, 0x6C}}, 2};
const DEVPROPKEY kDevpkeyInstanceId = {{0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}}, 256};
constexpr REFERENCE_TIME kUsbSpkBuffer = 300000;   // 30 ms

template <class T> void ComRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

bool PadContainerId(const Slot* s, GUID* id) {
    WCHAR path[MAX_PATH], inst[256];
    MultiByteToWideChar(CP_ACP, 0, s->path, -1, path, MAX_PATH);
    DEVPROPTYPE type;
    ULONG size = sizeof(inst), idSize = sizeof(GUID);
    DEVINST node;
    return CM_Get_Device_Interface_PropertyW(path, &kDevpkeyInstanceId, &type, reinterpret_cast<PBYTE>(inst), &size, 0) ==
               CR_SUCCESS &&
           CM_Locate_DevNodeW(&node, inst, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
           CM_Get_DevNode_PropertyW(node, &kDevpkeyContainerId, &type, reinterpret_cast<PBYTE>(id), &idSize, 0) ==
               CR_SUCCESS;
}

IMMDevice* FindPadEndpoint(const GUID& container, int warnIdx = -1) {   // warnIdx: slot to log the default output for
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    IMMDevice* found = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        UINT n = 0;
        list->GetCount(&n);
        for (UINT i = 0; i < n && !found; ++i) {
            IMMDevice* d = nullptr;
            IPropertyStore* ps = nullptr;
            if (FAILED(list->Item(i, &d))) continue;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(kPkeyContainerId, &v)) && v.vt == VT_CLSID && IsEqualGUID(*v.puuid, container))
                    found = d;
                PropVariantClear(&v);
                ps->Release();
            }
            if (!found) d->Release();
        }
        IMMDevice* def = nullptr;
        LPWSTR a = nullptr, b = nullptr;
        if (found && warnIdx >= 0 && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def)) &&
            SUCCEEDED(found->GetId(&a)) && SUCCEEDED(def->GetId(&b)) && !wcscmp(a, b))
            Log("DualSense %d: this pad is the Windows default output, so the PC mix plays on its speaker too", warnIdx);
        CoTaskMemFree(a);
        CoTaskMemFree(b);
        ComRelease(def);
    }
    ComRelease(list);
    ComRelease(en);
    return found;
}

DWORD WINAPI UsbSpeakerThread(LPVOID param) {
    Slot* s = static_cast<Slot*>(param);
    int idx = static_cast<int>(s - g_slot);
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr), writeEv = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    UINT32 frames = 0;
    uint8_t rep[128];
    GUID container;
    const char* why = nullptr;            // what failed
    if (FAILED(com)) why = "COM";
    else if (!PadContainerId(s, &container)) why = "no container id";
    else if (!(dev = FindPadEndpoint(container))) why = "no active audio endpoint";
    else if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
             FAILED(client->GetMixFormat(&fmt))) why = "no audio client";
    else {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
        bool isFloat = fmt->wBitsPerSample == 32 && (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT));
        if (!isFloat || fmt->nChannels < 2) why = "unexpected mix format";
        else if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kUsbSpkBuffer, 0,
                                           fmt, nullptr)) ||
                 FAILED(client->SetEventHandle(ev)) || FAILED(client->GetBufferSize(&frames)) ||
                 FAILED(client->GetService(IID_PPV_ARGS(&render)))) why = "audio client setup failed";
        else if (!WriteReport(s, writeEv, rep, SpeakerRoute(s, rep, 0, 3))) why = "route report failed";
        else if (FAILED(client->Start())) why = "audio client didn't start";
    }
    Log("DualSense %d: USB speaker stream %s%s", idx, why ? "failed: " : "started", why ? why : "");
    SpkQueue q;
    int channels = fmt ? fmt->nChannels : 0;
    double rate = fmt ? fmt->nSamplesPerSec : 0;
    int sent = 0;
    DWORD statsAt = GetTickCount();
    static float mono[kDsMax][kSpkOut];
    bool idle = false;
    while (!why && !idle && !s->spkStop) {
        if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) { why = "no audio event"; break; }
        UINT32 pad = 0;
        BYTE* data;
        if (FAILED(client->GetCurrentPadding(&pad))) { why = "endpoint gone"; break; }   // the pad dropped
        UINT32 n = frames - pad;
        if (!n) continue;
        if (FAILED(render->GetBuffer(n, &data))) { why = "endpoint gone"; break; }
        float* f = reinterpret_cast<float*>(data);
        memset(f, 0, n * channels * sizeof(float));
        for (UINT32 done = 0; done < n && !idle;) {
            int chunk = n - done > kSpkOut ? kSpkOut : static_cast<int>(n - done);
            if (!SpkTake(s, q, mono[idx], chunk, rate)) { idle = true; break; }
            for (int i = 0; i < chunk; ++i) f[(done + i) * channels + 1] = mono[idx][i];
            done += chunk;
        }
        render->ReleaseBuffer(n, 0);
        sent += n;
        DWORD t = GetTickCount();
        if (t - statsAt >= 5000) {
            Log("DualSense %d: USB speaker %d frames, %d underruns, %d samples dropped, peak %.2f (limited above 0.90), "
                "left %d, queue max %d, rate %+.3f %%", idx, sent, q.underruns, q.dropped, q.peak,
                q.statMin == 1 << 30 ? 0 : q.statMin, q.statMax, q.trim * 100);
            SpkStatsReset(q);
            sent = 0;
            statsAt = t;
        }
    }
    if (client) client->Stop();
    if (!s->spkStop) WriteReport(s, writeEv, rep, SpeakerRoute(s, rep, 0, 0));   // back on the jack
    if (why) {
        Log("DualSense %d: USB speaker stopped: %s; engine stays on the PC", idx, why);
        s->spkWaitEndpoint = !dev || !strcmp(why, "endpoint gone");
        s->spkNoDevice = true;
    } else Log("DualSense %d: speaker stream stopped", idx);
    ComRelease(render);
    ComRelease(client);
    ComRelease(dev);
    CoTaskMemFree(fmt);
    CloseHandle(ev);
    CloseHandle(writeEv);
    if (SUCCEEDED(com)) CoUninitialize();
    SpkFinish(s);
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

// The pad's Bluetooth address as 12 hex digits, the same over USB and Bluetooth: feature report 0x09 on a DualSense or
// 0x12 on a DS4 (pairing info, address at 1..6, last byte first, as hid-playstation and SDL read it), else the HID
// serial string (Bluetooth).
bool ReadMac(HANDLE h, bool ds4, char out[13]) {
    uint8_t f[64] = {};
    f[0] = ds4 ? 0x12 : 0x09;
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

void OpenPad(const char* path, bool edge, bool ds4) {
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
    if (!ReadMac(h, ds4, mac)) mac[0] = 0;
    // the same pad on its other transport (a cable plugged into a Bluetooth pad) while the old link is still open:
    // wait for that one to drop, then this one takes its slot on a later rescan
    for (auto& c : g_slot)
        if (mac[0] && c.thread && !strcmp(c.mac, mac)) {
            static char waiting[MAX_PATH];
            if (strcmp(waiting, path)) {
                strcpy_s(waiting, path);
                Log("%s %d: also seen on another connection, kept on the open one", Model(&c), static_cast<int>(&c - g_slot));
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
    s->ds4 = ds4;
    s->inLen = caps.InputReportByteLength;
    s->outLen = caps.OutputReportByteLength;
    if (!s->usb && s->outLen > 78) s->outLen = 78;   // BT descriptor lists the largest report (547); 0x31 is 78 bytes
    if (s->inLen > kInMax || s->outLen > 128 || s->outLen < (s->usb ? (ds4 ? 32 : 48) : 78)) {
        Log("%s: unexpected report sizes in %u out %u", Model(s), s->inLen, s->outLen);
        CloseHandle(h);
        return;
    }
    strcpy_s(s->path, path);
    // the log shows only the address's last 4 digits (logs get attached to bug reports)
    Log("%s %d: pad ..%s, %s", Model(s), static_cast<int>(s - g_slot), mac[0] ? mac + 8 : "(no address)",
        same ? "back in its slot" : s->mac[0] ? "takes a slot another pad had" : "new slot");
    strcpy_s(s->mac, mac);
    s->stop = 0;
    EnterCriticalSection(&s->cs);
    s->want.smallMotor = s->want.largeMotor = 0;   // no stale rumble from before an unplug
    memset(&s->st, 0, sizeof(s->st));
    s->have = false;
    s->stalled = false;
    LeaveCriticalSection(&s->cs);
    EnterCriticalSection(&s->spkCs);
    s->spkCount = 0;
    s->spkStop = 0;
    s->spkNoDevice = s->usb;              // a DS4 on a cable stays this way: only Bluetooth reaches its speaker
    s->spkWaitEndpoint = s->usb && !ds4;
    LeaveCriticalSection(&s->spkCs);
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
        bool ds4 = sony && (strstr(lower, "pid_05c4") || strstr(lower, "pid&05c4") || strstr(lower, "pid_09cc") ||
                            strstr(lower, "pid&09cc"));
        if (!ds && !edge && !ds4) continue;
        bool open = false;
        for (auto& s : g_slot)
            if (s.thread && !_stricmp(s.path, d->DevicePath)) open = true;
        if (!open) OpenPad(d->DevicePath, edge, ds4);
    }
    SetupDiDestroyDeviceInfoList(set);
}

void CheckUsbEndpoints() {
    for (auto& s : g_slot) {
        if (!s.alive || !s.usb || !s.spkNoDevice || !s.spkWaitEndpoint) continue;
        GUID container;
        IMMDevice* d = PadContainerId(&s, &container) ? FindPadEndpoint(container, static_cast<int>(&s - g_slot)) : nullptr;
        if (!d) continue;
        d->Release();
        Log("DualSense %d: USB audio endpoint ready", static_cast<int>(&s - g_slot));
        s.spkWaitEndpoint = false;
        s.spkNoDevice = false;
    }
}

DWORD WINAPI HotplugThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
        else { ReapSlots(false); Rescan(); CheckUsbEndpoints(); }
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
        InitializeCriticalSection(&s.spkCs);
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
        if (GetTickCount() - s.inAt >= kStallMs) {   // stalled: release held buttons, sticks and triggers
            uint32_t packet = out->packet;
            *out = {};
            out->packet = packet;
            if (extra) extra->ps = extra->touchClick = extra->mute = false;
        }
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

bool DsSpeakerWrite(int slot, const float* mono, int frames, int rate) {
    if (!g_enabled || slot < 0 || slot >= kDsMax || frames <= 0 || rate <= 0) return false;
    Slot& s = g_slot[slot];
    if (!s.alive || s.spkNoDevice) return false;
    EnterCriticalSection(&s.spkCs);
    bool ok = !s.spkStop;
    if (ok) {
        if (s.spkRate != rate) { s.spkRate = rate; s.spkCount = 0; }
        for (int i = 0; i < frames; ++i) {
            if (s.spkCount == kSpkRing) { s.spkHead = (s.spkHead + 1) % kSpkRing; --s.spkCount; }
            s.spkBuf[(s.spkHead + s.spkCount++) % kSpkRing] = mono[i];
        }
        s.spkLastWrite = GetTickCount();
        LPTHREAD_START_ROUTINE fn = s.ds4 ? Ds4SpeakerThread : s.usb ? UsbSpeakerThread : SpeakerThread;
        if (!s.spkThread) s.spkThread = CreateThread(nullptr, 0, fn, &s, 0, nullptr);
    }
    LeaveCriticalSection(&s.spkCs);
    return ok;
}

bool DsIsDs4(int slot) { return g_enabled && slot >= 0 && slot < kDsMax && g_slot[slot].alive && g_slot[slot].ds4; }

bool DsIsBluetooth(int slot) { return g_enabled && slot >= 0 && slot < kDsMax && g_slot[slot].alive && !g_slot[slot].usb; }

bool DsSpeakerReady(int slot) {
    return g_enabled && slot >= 0 && slot < kDsMax && g_slot[slot].alive && !g_slot[slot].spkNoDevice;
}

