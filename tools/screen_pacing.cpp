// Records what actually reaches the primary monitor, through DXGI Desktop Duplication: every composed update with its
// present time and how many frames it swallowed. Prints a line a second and then every hitch, so stutter that the game's
// own timing can't see shows up. No game state is touched. Built by build_screen_pacing.bat (64-bit).
// screen_pacing.exe [seconds=30]
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

struct Hitch { double at, gap; UINT swallowed; };

int main(int argc, char** argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 30.0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 1;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput* output = nullptr;
    DXGI_OUTPUT_DESC desc = {};
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* o = nullptr;
        for (UINT i = 0; adapter->EnumOutputs(i, &o) != DXGI_ERROR_NOT_FOUND; ++i) {
            o->GetDesc(&desc);
            if (desc.DesktopCoordinates.left == 0 && desc.DesktopCoordinates.top == 0) { output = o; break; }
            o->Release();
        }
        if (!output) adapter->Release();
    }
    if (!output) { puts("FAIL: no primary output"); return 1; }
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) { printf("FAIL: device %08lX\n", hr); return 1; }
    IDXGIOutput1* output1 = nullptr;
    output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    IDXGIOutputDuplication* dup = nullptr;
    hr = output1->DuplicateOutput(device, &dup);
    if (FAILED(hr)) { printf("FAIL: DuplicateOutput %08lX\n", hr); return 1; }
    printf("recording %ls %ldx%ld\n", desc.DeviceName, desc.DesktopCoordinates.right, desc.DesktopCoordinates.bottom);
    fflush(stdout);

    LARGE_INTEGER freq, now, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    double ms = 1000.0 / freq.QuadPart;
    LONGLONG last = 0;
    int shown = 0, second = 0, slow = 0, swallowed = 0;
    double worst = 0, sum = 0;
    int bins[6] = {};                    // gaps: <14, 14-20, 20-30, 30-50, 50-100, 100+ ms
    std::vector<Hitch> hitches;
    for (;;) {
        QueryPerformanceCounter(&now);
        double t = (now.QuadPart - start.QuadPart) * ms / 1000.0;
        if (t >= seconds) break;
        DXGI_OUTDUPL_FRAME_INFO info = {};
        IDXGIResource* res = nullptr;
        hr = dup->AcquireNextFrame(100, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST) { printf("%6.2f access lost, restarting\n", t); dup->Release(); output1->DuplicateOutput(device, &dup); continue; }
        if (FAILED(hr)) { printf("FAIL: acquire %08lX\n", hr); return 1; }
        if (info.LastPresentTime.QuadPart) {
            if (last) {
                double gap = (info.LastPresentTime.QuadPart - last) * ms;
                sum += gap, ++shown;
                if (gap > worst) worst = gap;
                ++bins[gap < 14 ? 0 : gap < 20 ? 1 : gap < 30 ? 2 : gap < 50 ? 3 : gap < 100 ? 4 : 5];
                if (gap > 20 || info.AccumulatedFrames > 1) {
                    ++slow;
                    if (info.AccumulatedFrames > 1) swallowed += info.AccumulatedFrames - 1;
                    hitches.push_back({(info.LastPresentTime.QuadPart - start.QuadPart) * ms / 1000.0, gap, info.AccumulatedFrames});
                }
            }
            last = info.LastPresentTime.QuadPart;
        }
        res->Release();
        dup->ReleaseFrame();
        if (static_cast<int>(t) != second) {
            second = static_cast<int>(t);
            printf("%3d s: %d shown, avg %.2f ms, worst %.1f, hitches %d, swallowed %d\n", second, shown, shown ? sum / shown : 0, worst, slow, swallowed);
            fflush(stdout);
            shown = 0, sum = 0, worst = 0, slow = 0, swallowed = 0;
        }
    }
    printf("\ngaps: <14 ms %d | 14-20 %d | 20-30 %d | 30-50 %d | 50-100 %d | 100+ %d\nhitches (time s, gap ms, frames swallowed):\n",
           bins[0], bins[1], bins[2], bins[3], bins[4], bins[5]);
    for (size_t i = 0; i < hitches.size() && i < 80; ++i) printf("  %7.3f  %6.1f  %u\n", hitches[i].at, hitches[i].gap, hitches[i].swallowed);
    return 0;
}
