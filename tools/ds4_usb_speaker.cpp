// Plays a tone on a USB DS4's audio endpoint, to check whether a cable reaches the built-in speaker. Second model
// (054C:09CC) only: the first has no USB audio. Report 0x05 carries the volumes: flags at 1 (0x10, 0x20 headphones,
// 0x80 speaker), headphones at 19 and 20, speaker at 22 (as report 0x11 on Bluetooth, 2 bytes earlier). Expected,
// from PadForge's docs: only the jack plays.
//   ds4_usb_speaker [offset=value ...]     report bytes over the defaults: 1=0xB0 22=0x4F
//   ds4_usb_speaker list                   the pad's report ids, the endpoint's volume and mute; no sound
//   ds4_usb_speaker features               reads the feature reports (not the pad's address and pairing ones)
//   ds4_usb_speaker sine [seconds]         the factory sine (feature report A0: 01 01, then 01 00), reads A4 after each.
//                                          Only this subcommand: others in A0 write flash or brick the USB mode.
//   ds4_usb_speaker mix [offset=value ...] 2 s factory sine, 2 s sine + a 1500 Hz tone on the endpoint, 3 s tone alone:
//                                          does the test mode route the USB stream to the speaker?
//   ds4_usb_speaker topology               the endpoint's audio parts: terminals, volumes, mutes, selectors; read only
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
#include <devicetopology.h>
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
        if (!HidD_GetFeature(dev, f, sizeof(f))) {
            if (GetLastError() != ERROR_INVALID_PARAMETER) printf("%02X: read failed, error %lu\n", id, GetLastError());
            continue;
        }
        printf("%02X:", id);
        for (int i = 1; i < 24; ++i) printf(" %02X", f[i]);
        puts("");
    }
}

static bool TestCommand(HANDLE dev, BYTE device, BYTE action) {
    BYTE f[7] = {0xA0, device, action};
    if (!HidD_SetFeature(dev, f, sizeof(f))) {
        printf("SET A0 %02X %02X failed, error %lu\n", device, action, GetLastError());
        return false;
    }
    Sleep(50);
    BYTE r[65] = {0xA4};
    if (HidD_GetFeature(dev, r, sizeof(r))) {
        printf("SET A0 %02X %02X ok, A4:", device, action);
        for (int i = 1; i < 9; ++i) printf(" %02X", r[i]);
        puts("");
    } else printf("SET A0 %02X %02X ok, A4 read failed, error %lu\n", device, action, GetLastError());
    return true;
}

static void WalkPart(IPart* part, int depth) {
    static UINT seen[256];
    static int seenCount;
    UINT id = 0;
    part->GetLocalId(&id);
    for (int i = 0; i < seenCount; ++i) if (seen[i] == id) return;
    if (seenCount < 256) seen[seenCount++] = id;
    LPWSTR name = nullptr;
    part->GetName(&name);
    PartType type;
    part->GetPartType(&type);
    GUID sub = {};
    part->GetSubType(&sub);
    UINT controls = 0;
    part->GetControlInterfaceCount(&controls);
    printf("%*s%s '%ls' id %08X subtype %08lX-%04X, %u controls\n", depth * 2, "", type == Connector ? "connector" : "subunit",
           name ? name : L"", id, sub.Data1, sub.Data2, controls);
    CoTaskMemFree(name);
    for (UINT i = 0; i < controls; ++i) {
        IControlInterface* c = nullptr;
        if (FAILED(part->GetControlInterface(i, &c))) continue;
        LPWSTR cn = nullptr;
        IID iid = {};
        c->GetName(&cn);
        c->GetIID(&iid);
        printf("%*scontrol '%ls' iid %08lX\n", depth * 2 + 2, "", cn ? cn : L"", iid.Data1);
        CoTaskMemFree(cn);
        c->Release();
    }
    for (int dir = 0; dir < 2; ++dir) {
        IPartsList* list = nullptr;
        if (FAILED(dir ? part->EnumPartsOutgoing(&list) : part->EnumPartsIncoming(&list)) || !list) continue;
        UINT n = 0;
        list->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IPart* q = nullptr;
            if (SUCCEEDED(list->GetPart(i, &q))) { WalkPart(q, depth + 1); q->Release(); }
        }
        list->Release();
    }
}

static void DumpTopology(IMMDevice* ep) {
    IDeviceTopology* t = nullptr;
    if (FAILED(ep->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&t)))) { puts("no topology"); return; }
    UINT n = 0;
    t->GetConnectorCount(&n);
    printf("endpoint connectors: %u\n", n);
    for (UINT i = 0; i < n; ++i) {
        IConnector* c = nullptr, *to = nullptr;
        if (FAILED(t->GetConnector(i, &c))) continue;
        ConnectorType ct = Unknown_Connector;
        c->GetType(&ct);
        printf("connector %u type %d\n", i, ct);
        if (SUCCEEDED(c->GetConnectedTo(&to)) && to) {
            IPart* part = nullptr;
            if (SUCCEEDED(to->QueryInterface(IID_PPV_ARGS(&part)))) {
                IDeviceTopology* dt = nullptr;
                if (SUCCEEDED(part->GetTopologyObject(&dt))) {
                    UINT sub = 0, cons = 0;
                    dt->GetSubunitCount(&sub);
                    dt->GetConnectorCount(&cons);
                    printf("device topology: %u subunits, %u connectors\n", sub, cons);
                    for (UINT k = 0; k < cons; ++k) {
                        IConnector* dc = nullptr;
                        IPart* dp = nullptr;
                        if (SUCCEEDED(dt->GetConnector(k, &dc)) && SUCCEEDED(dc->QueryInterface(IID_PPV_ARGS(&dp)))) {
                            WalkPart(dp, 0);
                            dp->Release();
                        }
                        if (dc) dc->Release();
                    }
                    dt->Release();
                }
                part->Release();
            }
            to->Release();
        }
        c->Release();
    }
    t->Release();
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
    if (argc > 1 && !strcmp(argv[1], "sine")) {
        int seconds = argc > 2 ? atoi(argv[2]) : 4;
        bool ok = TestCommand(dev, 1, 1);
        if (ok) puts("sine on");
        Sleep(seconds * 1000);
        TestCommand(dev, 1, 0);
        puts("sine off");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "features")) { ReadFeatures(dev); return 0; }
    if (argc > 1 && !strcmp(argv[1], "topology")) { DumpTopology(ep); return 0; }
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
    bool mix = argc > 1 && !strcmp(argv[1], "mix"), sineOn = false;
    double hz = mix ? 1500 : 440;
    if (mix) {
        sineOn = TestCommand(dev, 1, 1);
        puts("2 s: sine alone");
        Sleep(2000);
        puts("2 s: sine and tone");
    }
    client->Start();
    static const BYTE kSteps[] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0xFF};
    int stepsTotal = 0, offset = 2, step = 0;
    for (int i = 2; i < 32; ++i) if (i < 4 || i > 8) stepsTotal += sizeof(kSteps);
    DWORD t0 = GetTickCount(), nextStep = t0 + stepMs;
    BYTE base = r[offset];
    printf("%s\n", sweep ? "sweep" : "tone (4 s)");
    double phase = 0;
    int total = (mix ? 5 : 4) * fmt->nSamplesPerSec, done = 0;
    while (sweep ? step < stepsTotal : done < total) {
        if (sineOn && done >= static_cast<int>(2 * fmt->nSamplesPerSec)) {
            TestCommand(dev, 1, 0);
            sineOn = false;
            puts("3 s: tone alone");
        }
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
            phase += 2 * 3.14159265358979 * hz / fmt->nSamplesPerSec;
        }
        render->ReleaseBuffer(n, 0);
        done += n;
    }
    client->Stop();
    if (sineOn) TestCommand(dev, 1, 0);
    if (sweep) {
        memset(r + 1, 0, sizeof(r) - 1);
        r[1] = 0x07;                      // back to plain motors off, lightbar off
        WriteFile(dev, r, sizeof(r), &written, nullptr);
    }
    puts("done");
    return 0;
}
