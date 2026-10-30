// Plays a tone on a USB DS4's audio endpoint, to check whether a cable reaches the built-in speaker. Second model
// (054C:09CC) only: the first has no USB audio. Report 0x05 carries the volumes: flags at 1 (0x10, 0x20 headphones,
// 0x80 speaker), headphones at 19 and 20, speaker at 22 (as report 0x11 on Bluetooth, 2 bytes earlier). Expected,
// from PadForge's docs: only the jack plays.
//   ds4_usb_speaker [offset=value ...]     report bytes over the defaults: 1=0xB0 22=0x4F
//   ds4_usb_speaker list                   the pad's report ids, the endpoint's volume and mute; no sound
//   ds4_usb_speaker features               reads the feature reports (not the pad's address and pairing ones)
//   ds4_usb_speaker sweep [ms]             a tone while each report byte steps through bit values, 200 ms a step. The
//                                          motors and the lightbar (4..8) stay out of it. Only output report 0x05 is
//                                          written: never write feature reports, they hold calibration and pairing.
//
// Build (32-bit VS prompt): cl /nologo /O2 /EHsc /Fo:build\ /Fe:build\ds4_usb_speaker.exe tools\ds4_usb_speaker.cpp
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <hidpi.h>
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

static HANDLE FindPad(GUID* container) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof(ifd)};
    static BYTE buf[2048];
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf);
    HANDLE found = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, sizeof(buf), nullptr, nullptr)) continue;
        if (!wcsstr(detail->DevicePath, L"vid_054c&pid_09cc")) continue;
        HANDLE h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        WCHAR inst[256];
        ULONG size = sizeof(inst), gsize = sizeof(GUID);
        DEVPROPTYPE type;
        DEVINST node;
        if (CM_Get_Device_Interface_PropertyW(detail->DevicePath, &DEVPKEY_Device_InstanceId, &type,
                                              reinterpret_cast<PBYTE>(inst), &size, 0) == CR_SUCCESS &&
            CM_Locate_DevNodeW(&node, inst, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
            CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(container),
                                     &gsize, 0) == CR_SUCCESS)
            found = h;
        else CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
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
            printf("endpoint: %ls\n", name.pwszVal);
            PropVariantClear(&name);
            found = d;
        } else d->Release();
        PropVariantClear(&v);
        ps->Release();
    }
    list->Release();
    return found;
}

static void ListReports(HANDLE dev) {
    PHIDP_PREPARSED_DATA pre;
    HIDP_CAPS caps = {};
    if (!HidD_GetPreparsedData(dev, &pre)) return;
    HidP_GetCaps(pre, &caps);
    printf("report sizes: in %d, out %d, feature %d\n", caps.InputReportByteLength, caps.OutputReportByteLength,
           caps.FeatureReportByteLength);
    static const struct { HIDP_REPORT_TYPE type; USHORT n; const char* name; } kinds[] = {
        {HidP_Input, caps.NumberInputValueCaps, "input"},
        {HidP_Output, caps.NumberOutputValueCaps, "output"},
        {HidP_Feature, caps.NumberFeatureValueCaps, "feature"}};
    for (auto& k : kinds) {
        static HIDP_VALUE_CAPS v[256];
        USHORT n = k.n > 256 ? 256 : k.n;
        printf("%s ids:", k.name);
        if (HidP_GetValueCaps(k.type, v, &n, pre) == HIDP_STATUS_SUCCESS)
            for (int i = 0; i < n; ++i) printf(" %02X(%d)", v[i].ReportID, v[i].BitSize * v[i].ReportCount / 8);
        puts("");
    }
    HidD_FreePreparsedData(pre);
}

static void ReadFeatures(HANDLE dev) {
    static const BYTE kSkip[] = {0x12, 0x13, 0x81, 0x83, 0x84, 0x85, 0xA3, 0xF0, 0xF1, 0xF2};   // address, pairing, auth
    for (int id = 1; id < 256; ++id) {
        if (memchr(kSkip, id, sizeof(kSkip))) continue;
        BYTE f[65] = {static_cast<BYTE>(id)};
        if (!HidD_GetFeature(dev, f, sizeof(f))) continue;
        printf("%02X:", id);
        for (int i = 1; i < 24; ++i) printf(" %02X", f[i]);
        puts("");
    }
}

int main(int argc, char** argv) {
    bool list = argc > 1 && !strcmp(argv[1], "list"), sweep = argc > 1 && !strcmp(argv[1], "sweep");
    int stepMs = sweep && argc > 2 ? atoi(argv[2]) : 200;
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    GUID container;
    HANDLE dev = FindPad(&container);
    if (dev == INVALID_HANDLE_VALUE) { puts("FAIL: no DS4 (09CC) on USB"); return 1; }
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    IMMDevice* ep = FindEndpoint(en, container);
    if (!ep) { puts("FAIL: no active render endpoint with the pad's container id"); return 1; }
    IAudioEndpointVolume* vol = nullptr;
    ep->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol));
    float level = -1;
    BOOL muted = FALSE;
    if (vol) { vol->GetMasterVolumeLevelScalar(&level); vol->GetMute(&muted); }
    printf("endpoint volume %.0f %%, muted %d\n", level * 100, muted);
    ListReports(dev);
    if (argc > 1 && !strcmp(argv[1], "features")) { ReadFeatures(dev); return 0; }
    if (list) return 0;
    IAudioClient* client = nullptr;
    ep->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    WAVEFORMATEX* fmt = nullptr;
    client->GetMixFormat(&fmt);
    printf("mix format: %d channels, %lu Hz, %d bit\n", fmt->nChannels, fmt->nSamplesPerSec, fmt->wBitsPerSample);
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 300000, 0, fmt, nullptr);
    if (FAILED(hr)) { printf("FAIL: Initialize 0x%08lX\n", hr); return 1; }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client->SetEventHandle(ev);
    UINT32 frames = 0;
    client->GetBufferSize(&frames);
    IAudioRenderClient* render = nullptr;
    client->GetService(IID_PPV_ARGS(&render));

    BYTE r[32] = {};
    r[0] = 0x05;
    r[1] = 0x10 | 0x20 | 0x80;
    r[22] = 0x4F;
    r[2] = 0x04; r[1] = 0xF0; r[19] = r[20] = 0x43; r[22] = 0xFF; r[23] = 0x85;   // the loudest setting tried
    for (int i = 1; i < argc; ++i) {      // offset=value
        char* eq = strchr(argv[i], '=');
        int at = eq ? strtol(argv[i], nullptr, 0) : 0;
        if (at > 0 && at < 32) r[at] = static_cast<BYTE>(strtol(eq + 1, nullptr, 0));
    }
    DWORD written;
    if (!WriteFile(dev, r, sizeof(r), &written, nullptr)) { printf("FAIL: report, error %lu\n", GetLastError()); return 1; }
    printf("report:");
    for (int i = 0; i < 32; ++i) if (r[i]) printf(" [%d]=%02X", i, r[i]);
    puts("");
    client->Start();
    static const BYTE kSteps[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFF};
    int stepsTotal = 0, offset = 2, step = 0;
    for (int i = 2; i < 32; ++i) if (i < 4 || i > 8) stepsTotal += sizeof(kSteps);
    DWORD t0 = GetTickCount(), nextStep = t0 + stepMs;
    BYTE base = r[offset];
    printf("%s\n", sweep ? "sweep" : "tone (4 s)");
    double phase = 0;
    int total = 4 * fmt->nSamplesPerSec, done = 0;
    while (sweep ? step < stepsTotal : done < total) {
        if (sweep && GetTickCount() >= nextStep) {
            nextStep += stepMs;
            r[offset] = base;
            int k = step++ % sizeof(kSteps);
            if (k == 0 && step > 1) {
                offset = offset == 3 ? 9 : offset + 1;
                base = r[offset];
                printf("[%5.1f s] byte %d\n", (GetTickCount() - t0) / 1000.0, offset);
            }
            r[offset] = kSteps[k];
            WriteFile(dev, r, sizeof(r), &written, nullptr);
        }
        if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0) { puts("FAIL: no audio event"); return 1; }
        UINT32 pad = 0;
        if (FAILED(client->GetCurrentPadding(&pad))) { puts("FAIL: endpoint gone"); return 1; }
        UINT32 n = frames - pad;
        BYTE* data;
        if (!n || FAILED(render->GetBuffer(n, &data))) continue;
        auto* f = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < n; ++i) {
            float v = static_cast<float>(0.4 * sin(phase));
            for (int c = 0; c < fmt->nChannels; ++c) f[i * fmt->nChannels + c] = v;
            phase += 2 * 3.14159265358979 * 440 / fmt->nSamplesPerSec;
        }
        render->ReleaseBuffer(n, 0);
        done += n;
    }
    client->Stop();
    if (sweep) {
        memset(r + 1, 0, sizeof(r) - 1);
        r[1] = 0x07;                      // back to plain motors off, lightbar off
        WriteFile(dev, r, sizeof(r), &written, nullptr);
    }
    puts("done");
    return 0;
}
