// Plays test tones on a USB DualSense through its Windows audio endpoint, to check the speaker route: which output
// path and channel reach the speaker, whether other output reports keep the route, and that the endpoint matches by
// container id. Takes the test number (1-4), one per run, and prints what to expect; the listener says what they
// heard after each. Ends with the path back on the jack.
//
// Build (32-bit VS prompt): cl /nologo /O2 /EHsc tools\ds_usb_speaker.cpp /Fe:build\ds_usb_speaker.exe /Fo:build\
//                           ole32.lib hid.lib setupapi.lib cfgmgr32.lib
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

static const PROPERTYKEY kContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
static DWORD g_t0;
static HANDLE g_dev;
static int g_outLen;

static void Say(const char* s) {
    printf("[%6.1f s] %s\n", (GetTickCount() - g_t0) / 1000.0, s);
    fflush(stdout);
}

static bool FindPad(GUID* container) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    static BYTE buf[2048];
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, nullptr)) continue;
        if (!wcsstr(detail->DevicePath, L"vid_054c&pid_0ce6") && !wcsstr(detail->DevicePath, L"vid_054c&pid_0df2")) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { printf("open failed, error %lu\n", GetLastError()); continue; }
        PHIDP_PREPARSED_DATA pre;
        HIDP_CAPS caps = {};
        if (HidD_GetPreparsedData(h, &pre)) { HidP_GetCaps(pre, &caps); HidD_FreePreparsedData(pre); }
        if (caps.InputReportByteLength != 64) { CloseHandle(h); continue; }   // Bluetooth
        WCHAR inst[256];
        ULONG size = sizeof(inst);
        DEVPROPTYPE type;
        DEVINST node;
        ULONG gsize = sizeof(GUID);
        if (CM_Get_Device_Interface_PropertyW(detail->DevicePath, &DEVPKEY_Device_InstanceId, &type,
                                              reinterpret_cast<PBYTE>(inst), &size, 0) != CR_SUCCESS ||
            CM_Locate_DevNodeW(&node, inst, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS ||
            CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(container),
                                     &gsize, 0) != CR_SUCCESS) {
            printf("no container id for %ls\n", detail->DevicePath);
            CloseHandle(h);
            continue;
        }
        printf("pad: %ls, output report %d bytes\n", inst, caps.OutputReportByteLength);
        g_dev = h;
        g_outLen = caps.OutputReportByteLength;
        SetupDiDestroyDeviceInfoList(set);
        return true;
    }
    SetupDiDestroyDeviceInfoList(set);
    return false;
}

static IMMDevice* FindEndpoint(IMMDeviceEnumerator* en, const GUID& container) {
    IMMDeviceCollection* list = nullptr;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list);
    UINT n = 0;
    list->GetCount(&n);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < n && !found; ++i) {
        IMMDevice* d;
        IPropertyStore* ps;
        list->Item(i, &d);
        d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(kContainerId, &v)) && v.vt == VT_CLSID && IsEqualGUID(*v.puuid, container)) {
            PROPVARIANT name;
            PropVariantInit(&name);
            ps->GetValue(PKEY_Device_FriendlyName, &name);
            printf("endpoint by container id: %ls\n", name.pwszVal);
            PropVariantClear(&name);
            found = d;
        } else d->Release();
        PropVariantClear(&v);
        ps->Release();
    }
    list->Release();
    return found;
}

// path 3: right channel on the speaker, headphones muted; path 0: L+R on the headphones (the pad's default)
static bool Route(int path) {
    BYTE r[128] = {};
    r[0] = 0x02;
    BYTE* c = r + 1;
    c[0] = 0x80 | 0x20;                   // valid: audio control, speaker volume
    c[1] = 0x80;                          // valid: audio control 2
    c[5] = 0x64;                          // speaker volume (max)
    c[7] = static_cast<BYTE>(path << 4);  // audio control: output path
    c[37] = 0x02;                         // audio control 2: speaker preamp gain
    DWORD n;
    if (!WriteFile(g_dev, r, g_outLen, &n, nullptr)) { printf("route report failed, error %lu\n", GetLastError()); return false; }
    return true;
}

static void Rumble(int strength) {        // a normal report, without the audio flags
    BYTE r[128] = {};
    r[0] = 0x02;
    r[1] = 0x01 | 0x02;
    r[3] = 0;
    r[4] = static_cast<BYTE>(strength);
    DWORD n;
    if (!WriteFile(g_dev, r, g_outLen, &n, nullptr)) printf("rumble report failed, error %lu\n", GetLastError());
}

static IAudioClient* g_client;
static IAudioRenderClient* g_render;
static HANDLE g_ev;
static UINT32 g_frames;
static int g_ch, g_rate;
static double g_phase;

static void Fail(const char* what, HRESULT hr) {
    printf("[%6.1f s] FAIL: %s (0x%08lX), tick %lu\n", (GetTickCount() - g_t0) / 1000.0, what, hr, GetTickCount());
    fflush(stdout);
    ExitProcess(1);
}

static void Play(double seconds, int channel, double hz, bool rumble) {
    int total = static_cast<int>(seconds * g_rate), done = 0;
    bool buzzing = false;
    while (done < total) {
        if (WaitForSingleObject(g_ev, 500) != WAIT_OBJECT_0) Fail("no audio event for 500 ms", 0);
        UINT32 pad = 0;
        HRESULT hr = g_client->GetCurrentPadding(&pad);
        if (FAILED(hr)) Fail("GetCurrentPadding", hr);       // AUDCLNT_E_DEVICE_INVALIDATED: the pad dropped
        UINT32 n = g_frames - pad;
        if (!n) continue;
        BYTE* data;
        hr = g_render->GetBuffer(n, &data);
        if (FAILED(hr)) Fail("GetBuffer", hr);
        float* f = reinterpret_cast<float*>(data);
        memset(f, 0, n * g_ch * sizeof(float));
        for (UINT32 i = 0; i < n; ++i) {
            f[i * g_ch + channel] = static_cast<float>(0.4 * sin(g_phase));
            g_phase += 2 * 3.14159265358979 * hz / g_rate;
        }
        g_render->ReleaseBuffer(n, 0);
        done += n;
        if (rumble) {
            bool want = done > total / 3 && done < total * 2 / 3;
            if (want != buzzing) { buzzing = want; Rumble(want ? 120 : 0); }
        }
    }
}

int main(int argc, char** argv) {
    int test = argc > 1 ? atoi(argv[1]) : 1;
    g_t0 = GetTickCount();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    GUID container;
    if (!FindPad(&container)) { Say("FAIL: no DualSense on USB"); return 1; }
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    IMMDevice* dev = FindEndpoint(en, container);
    if (!dev) { Say("FAIL: no active render endpoint with the pad's container id"); return 1; }
    LPWSTR id = nullptr, defId = nullptr;
    IMMDevice* def = nullptr;
    dev->GetId(&id);
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) def->GetId(&defId);
    printf("pad is the Windows default output: %s\n", defId && !wcscmp(id, defId) ? "yes" : "no");

    dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&g_client));
    WAVEFORMATEX* fmt = nullptr;
    g_client->GetMixFormat(&fmt);
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    printf("mix format: %d channels, %lu Hz, %d bit, float %d\n", fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample, isFloat);
    if (!isFloat || fmt->nChannels < 2) { Say("FAIL: unexpected mix format"); return 1; }
    g_ch = fmt->nChannels;
    g_rate = fmt->nSamplesPerSec;
    HRESULT hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 300000, 0, fmt, nullptr);
    if (FAILED(hr)) { printf("Initialize failed: 0x%08lX\n", hr); return 1; }
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_client->SetEventHandle(g_ev);
    g_client->GetBufferSize(&g_frames);
    g_client->GetService(IID_PPV_ARGS(&g_render));
    printf("buffer: %u frames\n", g_frames);
    g_client->Start();

    // one test per run, so each result can be reported right after it: 1 beep, 2 left channel, 3 rumble, 4 jack path
    if (!Route(3)) return 1;
    Say("BEEP (1 s): the speaker works");
    Play(1, 1, 440, false);
    if (test == 2) {
        Say("TEST 2 (4 s): tone on the LEFT channel. Expect: SILENCE after the beep");
        Play(4, 0, 660, false);
    } else if (test == 3) {
        Say("TEST 3 (5 s): high tone, rumble in the middle. Expect: TONE KEEPS PLAYING THROUGH THE BUZZ");
        Play(5, 1, 880, true);
    } else if (test == 4) {
        Say("TEST 4 (4 s): path 0, tone on the jack. Expect: SILENCE after the beep");
        if (!Route(0)) return 1;
        Play(4, 1, 440, false);
    }
    g_client->Stop();
    Rumble(0);
    Route(0);
    Say("DONE: path back on the jack");
    return 0;
}
