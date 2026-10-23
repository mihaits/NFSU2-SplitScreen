// Part of NFSU2SplitScreen.cpp: rendering: layout, viewports, device hooks, world views, sky, extra views.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- rendering
// Player view k renders into its screen region (top/bottom halves, or quadrants); the game always passes (0,0).
static int g_regionX[MAX_LOCAL_PLAYERS], g_regionY[MAX_LOCAL_PLAYERS];

static void* CurrentRenderTarget(void* dev) {
    void* rt0 = nullptr;           // IDirect3DDevice9::GetRenderTarget (slot 38); only the pointer is needed
    auto getRt = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void**)>((*reinterpret_cast<void***>(dev))[38]);
    if (SUCCEEDED(getRt(dev, 0, &rt0)) && rt0)
        reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(rt0))[2])(rt0);   // Release
    return rt0;
}

#define STREAK_SURFACE_ADDR 0x86FF70
#define STREAK_AA_SURFACE   0x86F2E0     // with anti-aliasing ([0x870D08] > 0) the streak pass draws here, then copies
static bool g_streakFullCopy = false;    // the AA path's copy of the whole previous streak image (0x5C7120)
static bool IsStreakSurface(void* surf) {
    return surf && !g_streakFullCopy && (surf == at<void*>(STREAK_SURFACE_ADDR) || surf == at<void*>(STREAK_AA_SURFACE));
}
// With AA the streak pass first copies the whole previous streak image (texture 0x86F2E4) into 0x86F2E0
// (0x5C7120 at 0x5CBBB0, a full-screen quad) - that copy must stay full-screen, only what the view draws is confined.
#define AT_STREAK_AA_COPY  0x5CBBB0
static void __cdecl StreakAACopy(void* surf, void* tex) {
    g_streakFullCopy = true;
    reinterpret_cast<void(__cdecl*)(void*, void*)>(0x5C7120)(surf, tex);
    g_streakFullCopy = false;
}
static void StreakRegionViewport(int k, Viewport9* vp) {    // streak buffer = screen size (0x86FF70)
    int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
    if (vp->X != 0 || vp->Y != 0 || vp->Width != static_cast<DWORD>(w) || vp->Height != static_cast<DWORD>(h)) return;
    vp->X = g_regionX[k];
    vp->Y = g_regionY[k];
    vp->Width = g_rt[k].width;
    vp->Height = g_rt[k].height;
}

static HRESULT __stdcall SetViewportHook(void* dev, Viewport9* vp) {
    // Only viewports on the backbuffer are moved: passes inside a view also render into their own (often
    // half-screen sized) textures, e.g. the speed blur, which must keep their (0,0) viewport.
    int k = -1;
    for (int i = 0; i < g_screenViews; ++i)
        if (SplitRacing() && g_currentView == kPlayerViewId[i] && GetView(kPlayerViewId[i]).rt == &g_rt[i]) k = i;
    if (vp && k >= 0 && vp->X == 0 && vp->Y == 0 &&
        CurrentRenderTarget(dev) == reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface) {
        if (vp->Width == static_cast<DWORD>(g_rt[k].width) && vp->Height == static_cast<DWORD>(g_rt[k].height)) {
            vp->X = g_regionX[k];
            vp->Y = g_regionY[k];
        }
        // Post passes that target the whole backbuffer (light streaks composite at 0x5CBD5C / 0x5C7108) would stretch
        // this view's result over every other view: keep them inside the view's region.
        else if (vp->Width == static_cast<DWORD>(at<int>(SCREEN_W)) && vp->Height == static_cast<DWORD>(at<int>(SCREEN_H))) {
            vp->X = g_regionX[k];
            vp->Y = g_regionY[k];
            vp->Width = g_rt[k].width;
            vp->Height = g_rt[k].height;
        }
    }
    // the streak/overbright buffer (half resolution, whole screen): a player view only touches its own part
    if (vp && k >= 0 && IsStreakSurface(CurrentRenderTarget(dev))) StreakRegionViewport(k, vp);
    if (g_vpLogFrames > 0 && vp)
        Log("vp view %d: %u,%u %ux%u rt %p (caller %p)", g_currentView, vp->X, vp->Y, vp->Width, vp->Height,
            CurrentRenderTarget(dev), _ReturnAddress());
    return reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, vp);
}

// SetRenderTarget resets the viewport to the whole surface without a SetViewport call. Inside a player view, the
// backbuffer's "whole surface" is the view's region: post passes that render elsewhere and come back to the
// backbuffer (speed blur composite 0x5C6BE0) would otherwise draw over the whole screen.
static void* g_origSetRenderTarget = nullptr;
extern int g_obScissorSlotExt;
void ReapplyOverbrightScissor(void* dev);
static HRESULT __stdcall SetRenderTargetHook(void* dev, DWORD index, void* surface) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(g_origSetRenderTarget)(dev, index, surface);
    if (SUCCEEDED(hr) && index == 0 && g_obScissorSlotExt >= 0) ReapplyOverbrightScissor(dev);
    if (SUCCEEDED(hr) && index == 0 && surface && SplitRacing() && IsStreakSurface(surface))
        for (int k = 0; k < g_screenViews; ++k)
            if (g_currentView == kPlayerViewId[k] && GetView(kPlayerViewId[k]).rt == &g_rt[k]) {
                Viewport9 vp = {0, 0, static_cast<DWORD>(at<int>(SCREEN_W)), static_cast<DWORD>(at<int>(SCREEN_H)), 0.0f,
                                1.0f};
                StreakRegionViewport(k, &vp);
                reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
            }
    if (SUCCEEDED(hr) && index == 0 && surface && SplitRacing() &&
        surface == reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface)
        for (int k = 0; k < g_screenViews; ++k)
            if (g_currentView == kPlayerViewId[k] && GetView(kPlayerViewId[k]).rt == &g_rt[k]) {
                Viewport9 vp = {static_cast<DWORD>(g_regionX[k]), static_cast<DWORD>(g_regionY[k]),
                                static_cast<DWORD>(g_rt[k].width), static_cast<DWORD>(g_rt[k].height), 0.0f, 1.0f};
                reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
            }
    return hr;
}

static void HookDeviceMethod(void* dev, int slotIndex, void* hook, void** orig) {
    void** slot = *reinterpret_cast<void***>(dev) + slotIndex;
    if (*slot == hook) return;
    *orig = *slot;
    Write(reinterpret_cast<uintptr_t>(slot), &hook, sizeof(hook));
}

// Debug: device lost / reset tracing (the game resets on TestCooperativeLevel == D3DERR_DEVICENOTRESET, 0x5D2AFF /
// 0x5D2BE2, or after a video option change, 0x5D288B).
static void* g_origPresent = nullptr;
static void* g_origReset = nullptr;
static void* g_origTestCoop = nullptr;
static HRESULT __stdcall PresentHook(void* dev, const void* a, const void* b, HWND c, const void* d) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, const void*, const void*, HWND, const void*)>(g_origPresent)(dev, a, b, c, d);
    if (FAILED(hr)) Log("d3d: Present %08X (fg %p)", hr, GetForegroundWindow());
    return hr;
}
static void AddressSpace(unsigned* freeMb, unsigned* largestMb) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t total = 0, largest = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
         a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
        if (mbi.State == MEM_FREE) { total += mbi.RegionSize; if (mbi.RegionSize > largest) largest = mbi.RegionSize; }
    *freeMb = static_cast<unsigned>(total >> 20);
    *largestMb = static_cast<unsigned>(largest >> 20);
}
static HRESULT __stdcall TestCoopHook(void* dev) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*)>(g_origTestCoop)(dev);
    static HRESULT last = 0;
    if (hr != last) {
        unsigned freeMb, largestMb;
        AddressSpace(&freeMb, &largestMb);
        Log("d3d: TestCooperativeLevel %08X (caller %p) address space free %u MB, largest %u MB", hr, _ReturnAddress(),
            freeMb, largestMb);
    }
    last = hr;
    return hr;
}
static HRESULT __stdcall ResetHook(void* dev, void* params) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_origReset)(dev, params);
    void* stack[12];
    USHORT n = RtlCaptureStackBackTrace(1, 12, stack, nullptr);
    char trace[200] = "";
    for (USHORT i = 0; i < n && strlen(trace) < 180; ++i) sprintf_s(trace + strlen(trace), sizeof(trace) - strlen(trace), " %p", stack[i]);
    Log("d3d: Reset %08X (fg %p) stack%s", hr, GetForegroundWindow(), trace);
    return hr;
}

static WNDPROC g_origWndProc = nullptr;
static LRESULT CALLBACK DebugWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATEAPP: Log("wnd: WM_ACTIVATEAPP %d (other thread %u)", static_cast<int>(wp), static_cast<unsigned>(lp)); break;
    case WM_ACTIVATE: Log("wnd: WM_ACTIVATE %d (other %p)", LOWORD(wp), reinterpret_cast<void*>(lp)); break;
    case WM_SETFOCUS: Log("wnd: WM_SETFOCUS (from %p)", reinterpret_cast<void*>(wp)); break;
    case WM_KILLFOCUS: Log("wnd: WM_KILLFOCUS (to %p)", reinterpret_cast<void*>(wp)); break;
    case WM_DISPLAYCHANGE: Log("wnd: WM_DISPLAYCHANGE %dx%d %d bpp", LOWORD(lp), HIWORD(lp), static_cast<int>(wp)); break;
    case WM_SIZE: Log("wnd: WM_SIZE %d %dx%d", static_cast<int>(wp), LOWORD(lp), HIWORD(lp)); break;
    case WM_WINDOWPOSCHANGED: {
        auto* wpos = reinterpret_cast<const WINDOWPOS*>(lp);
        Log("wnd: WM_WINDOWPOSCHANGED %d,%d %dx%d flags %X after %p", wpos->x, wpos->y, wpos->cx, wpos->cy, wpos->flags,
            reinterpret_cast<void*>(wpos->hwndInsertAfter));
        break;
    }
    case WM_SYSCOMMAND: Log("wnd: WM_SYSCOMMAND %X", static_cast<unsigned>(wp)); break;
    case WM_NCACTIVATE: Log("wnd: WM_NCACTIVATE %d", static_cast<int>(wp)); break;
    case WM_DEVICECHANGE: Log("wnd: WM_DEVICECHANGE %X", static_cast<unsigned>(wp)); break;
    case WM_SETTINGCHANGE: Log("wnd: WM_SETTINGCHANGE %X", static_cast<unsigned>(wp)); break;
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void HookDeviceDebug() {
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev || !g_debug) return;
    if (!g_origWndProc)
        if (HWND w = FindWindowA("GameFrame", nullptr))
            g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DebugWndProc)));
    HookDeviceMethod(dev, 3, reinterpret_cast<void*>(&TestCoopHook), &g_origTestCoop);
    HookDeviceMethod(dev, 16, reinterpret_cast<void*>(&ResetHook), &g_origReset);
    HookDeviceMethod(dev, 17, reinterpret_cast<void*>(&PresentHook), &g_origPresent);
}

static void* g_origSetRenderState;
static void* g_origDrawIndexed;
static HRESULT __stdcall SetRenderStateHook(void* dev, DWORD state, DWORD value);
static HRESULT __stdcall DrawIndexedHook(void* dev, int type, int base, UINT minIdx, UINT n, UINT start, UINT count);
static void HookDeviceSetViewport() {
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return;
    HookDeviceMethod(dev, 0xBC / 4, reinterpret_cast<void*>(&SetViewportHook), &g_origSetViewport);
    HookDeviceMethod(dev, 0x94 / 4, reinterpret_cast<void*>(&SetRenderTargetHook), &g_origSetRenderTarget);
    HookDeviceMethod(dev, 57, reinterpret_cast<void*>(&SetRenderStateHook), &g_origSetRenderState);
    HookDeviceMethod(dev, 82, reinterpret_cast<void*>(&DrawIndexedHook), &g_origDrawIndexed);
}

// The world renderer collects the views to draw every frame (0x5B9C20: views 1..3, env map 0x10..0x15, 4, 9; 12
// entries, on the main render's stack). The extra player views get one entry after 1..3 (the mirror view 3 is off in
// split screen): view 5's, shared by views 6.. (SharedWorldEntryFor re-points and re-culls it per view).
static uint8_t* g_worldViews;            // this frame's world view table (on the frame render's stack)
static void __cdecl RegisterExtraWorldViews(uint8_t* world) {
    g_worldViews = world;
    int& count = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    for (int k = 2; k < g_screenViews && k < 3; ++k) {
        View& v = GetView(kPlayerViewId[k]);
        if (!v.active || !v.rt || !v.rt->surface || count >= 12 - 8) continue;   // keep room for 0x10..0x15, 4, 9
        uint8_t* entry = world + count * 0x90;
        ++count;
        reinterpret_cast<void(__cdecl*)(View*, void*, uint32_t)>(FN_WORLD_ADD_VIEW)(&v, entry,
                                                                                at<int>(0x8707A4) ? 0x1000 : 0x6000);
    }
}

static __declspec(naked) void WorldViewsHook() {
    __asm {
        pushad
        push esi                         // world renderer
        call RegisterExtraWorldViews
        add esp, 4
        popad
        mov eax, dword ptr ds:[0x00870CB8]   // stolen
        push AT_WORLD_VIEWS_RET
        ret
    }
}

// The sky dome is only drawn for view ids 1..3 (and env map faces); the extra player views pose as view 1.
static void __cdecl DrawSky(View* v) {
    int id = v->id;
    if (id >= 5 && id <= 8) v->id = 1;
    reinterpret_cast<void(__cdecl*)(View*)>(FN_DRAW_SKY)(v);
    v->id = id;
}

// Views 5..8 exist but the renderer only sets up 1..4 (0x5C2F90); give a spare view what view 2 gets there.
static void InitPlayerView(int id) {
    View& v = GetView(id);
    auto* raw = reinterpret_cast<uint8_t*>(&v);
    if (*reinterpret_cast<void**>(raw + 0x60)) return;
    using MallocFn = void*(__cdecl*)(size_t);
    void* cull = reinterpret_cast<MallocFn>(FN_MALLOC)(0x3B0);
    void* rend = reinterpret_cast<MallocFn>(FN_MALLOC)(0x51C0);
    if (!cull || !rend) return;
    memset(cull, 0, 0x3B0);              // the game's views got fresh zeroed memory at boot; the ctors rely on it
    memset(rend, 0, 0x51C0);
    reinterpret_cast<void*(__thiscall*)(void*)>(FN_VIEW_CULL_CTOR)(cull);
    *static_cast<View**>(cull) = &v;
    reinterpret_cast<void*(__thiscall*)(void*, View*, int)>(FN_VIEW_RENDER_CTOR)(rend, &v, 0);
    // as 0x61E0F0 does for views 1 and 2
    *reinterpret_cast<float*>(static_cast<uint8_t*>(rend) + 0x509C) = 1.0f;
    reinterpret_cast<void(__thiscall*)(void*, int, float)>(0x613050)(rend, 5, 1.0f);
    *reinterpret_cast<void**>(raw + 0x60) = cull;
    *reinterpret_cast<void**>(raw + 0x64) = rend;
    Log("view %d initialised", id);
}
