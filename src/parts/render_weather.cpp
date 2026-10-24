// Part of NFSU2SplitScreen.cpp: rendering: rain, weather caches, road reflections, nitrous and overbright, 3P bars.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Rain: every view has its own rain object (view +0x64: falling rain + the drops on the lens, updated from that
// view's camera by 0x6140F0), but the frame draws only view 1's, once over the whole screen: falling rain at
// 0x5CB46B (0x61F120, also updates the drops) and the lens drops at 0x5CB9A6 (0x5C7AB0: copies the screen, then a
// quad per drop at x/y 0..1 that also samples the copy there). Split: each player view gets its own pass - falling
// rain in its region (viewport moved by SetViewportHook via g_currentView), drops remapped into its region.
#define AT_RAIN_CALL       0x5CB46B      // `call 0x61F120` (thiscall rain object)
#define FN_RAIN_UPDATE     0x61F120
#define RAIN_TIME_ACCUM    0x8A1D50      // global accumulator advanced by 0x61F120 (once per frame)
#define AT_RAIN_DROPS_CALL 0x5CB9A6      // `call 0x5C7AB0` (cdecl, byte arg)
#define FN_RAIN_DROPS      0x5C7AB0
#define AT_RAIN_DROP_GET   0x5C7C52      // `call 0x6142C0` (thiscall (i, &x, &y, &alpha, &size), ret 0x14)
#define FN_RAIN_DROP_GET   0x6142C0
static const uintptr_t kRainDropsViewCalls[] = {0x5C7AB8, 0x5C7C04, 0x5C7C47, 0x5C7DD4};   // `call 0x48B1E0` (push 1)
static int g_rainSlot = -1;              // player slot whose drops are being drawn

// Weather region caches ("Wind" 0x8A3180, "Normal Fog" 0x8A1E90, "Night Normal" 0x8A31F0, 0x8A3250) are 2-entry
// static arrays indexed by view id - 1. Each entry is a query cache (region the given position was last found in),
// recomputed from the position passed in, so views 5/6 (players 3/4) share players 1/2's entries instead of reading
// past the arrays (0x6136B0: falling rain update; 0x614300: per-view splashes, already run for views 5/6).
#define AT_RAIN_REGION_SLOT   0x6136C3   // mov eax, [ecx+4]; dec eax; push edi        (ecx = view)
#define AT_SPLASH_REGION_SLOT 0x61437E   // mov ebp, [eax+4]; dec ebp; mov [esp+0x38], ebp   (eax = view)
// The falling rain is emitted around the view's car (0x61E700: frame +0x5010 from the car's matrix): view id 1 ->
// human slot 0, anything else -> slot 1, so P3/P4's rain fell around P2's car (not in their view).
#define AT_RAIN_CAR        0x61E724      // cmp [eax+4], 1 ... push 0/1; call 0x5ED980  (eax = view)
#define AT_RAIN_CAR_DONE   0x61E73A      // eax = racer id
static int RacerIdOfPlayer(int k);
static int __fastcall RainCarRacer(int id) {
    int k = id == 1 ? 0 : 1;
    for (int i = 0; i < MAX_LOCAL_PLAYERS; ++i)
        if (id == kPlayerViewId[i]) k = i;
    return RacerIdOfPlayer(k);
}
static __declspec(naked) void RainCarHook() {
    __asm {
        mov ecx, dword ptr [eax + 4]
        call RainCarRacer
        push AT_RAIN_CAR_DONE
        ret
    }
}
static int __fastcall WeatherSlotOfView(int id) {
    for (int k = 2; k < MAX_LOCAL_PLAYERS; ++k)
        if (id == kPlayerViewId[k]) return (k - 2) & 1;      // views 7/8 share slots 0/1 with views 5/6
    return id - 1;
}
static __declspec(naked) void RainRegionSlot() {
    __asm {
        push edx
        push ecx
        mov ecx, dword ptr [ecx + 4]
        call WeatherSlotOfView
        pop ecx
        pop edx
        xchg edi, dword ptr [esp]        // return address <-> edi: push edi, then return
        push edi
        mov edi, dword ptr [esp + 4]
        ret
    }
}
static __declspec(naked) void SplashRegionSlot() {
    __asm {
        push ecx
        push edx
        push eax
        mov ecx, dword ptr [eax + 4]
        call WeatherSlotOfView
        mov ebp, eax
        pop eax
        pop edx
        pop ecx
        mov dword ptr [esp + 4 + 0x38], ebp
        ret
    }
}

static void __fastcall RainForViews(void* rain1, void*) {
    if (!SplitRacing()) { reinterpret_cast<void(__thiscall*)(void*)>(FN_RAIN_UPDATE)(rain1); return; }
    void* dev = at<void*>(D3D_DEVICE);
    float accum = at<float>(RAIN_TIME_ACCUM);
    int saved = g_currentView;
    Viewport9 before = {};                // later passes (flares, particles) rely on the viewport left by the views
    bool haveBefore = dev && SUCCEEDED(reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(
                                           (*reinterpret_cast<void***>(dev))[48])(dev, &before));   // GetViewport
    for (int k = 0; k < g_screenViews; ++k) {
        View& v = GetView(kPlayerViewId[k]);
        auto* rain = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(&v) + 0x64);
        if (!v.active || !rain) continue;
        if (k > 0) at<float>(RAIN_TIME_ACCUM) = accum;
        g_currentView = kPlayerViewId[k];
        Viewport9 vp = {static_cast<DWORD>(g_regionX[k]), static_cast<DWORD>(g_regionY[k]),
                        static_cast<DWORD>(g_rt[k].width), static_cast<DWORD>(g_rt[k].height), 0.0f, 1.0f};
        if (dev && g_origSetViewport) reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
        reinterpret_cast<void(__thiscall*)(void*)>(FN_RAIN_UPDATE)(rain);
    }
    g_currentView = saved;
    if (haveBefore && g_origSetViewport) reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &before);
}

static void* __cdecl RainDropsView(int id) {
    return &GetView(g_rainSlot >= 0 ? kPlayerViewId[g_rainSlot] : id);
}

static void __fastcall RainDropGet(void* rain, void*, int i, float* x, float* y, float* alpha, float* size) {
    reinterpret_cast<void(__thiscall*)(void*, int, float*, float*, float*, float*)>(FN_RAIN_DROP_GET)(rain, i, x, y,
                                                                                                    alpha, size);
    if (g_rainSlot < 0) return;
    float w = static_cast<float>(at<int>(SCREEN_W)), h = static_cast<float>(at<int>(SCREEN_H));
    if (w <= 0 || h <= 0) return;
    int k = g_rainSlot;
    float rh = g_rt[k].height / h;
    *x = g_regionX[k] / w + *x * (g_rt[k].width / w);
    *y = g_regionY[k] / h + *y * rh;
    *size *= rh;                          // same pixel shape, scaled with the view
}

static void __cdecl RainDropsForViews(int arg) {
    if (!SplitRacing()) { reinterpret_cast<void(__cdecl*)(int)>(FN_RAIN_DROPS)(arg); return; }
    for (int k = 0; k < g_screenViews; ++k) {
        if (!GetView(kPlayerViewId[k]).active) continue;
        g_rainSlot = k;
        reinterpret_cast<void(__cdecl*)(int)>(FN_RAIN_DROPS)(arg);
    }
    g_rainSlot = -1;
}

static uint8_t* g_ownLightCar;           // the car view 9 is aimed at: its colour is HeadlightColor (OwnLightColor)
static void HeadlightsForPlayer(int k) {
    k = ViewPlayer(k);
    View& v9 = GetView(9);
    if (!at<int>(OPT_CAR_HEADLIGHTS) || !v9.active || !v9.camera || !v9.platInfo) return;
    g_ownLightCar = PlayerCar(k);
    int* slot0 = reinterpret_cast<int*>(HUMAN_RACER_SLOTS);
    int saved = *slot0;
    *slot0 = k < 2 ? slot0[k] : k;       // extra players' racers follow the two 2P humans (ids 2, 3)
    reinterpret_cast<void(__cdecl*)(View*, int)>(FN_HEADLIGHT_CAMERA)(&v9, 0);
    *slot0 = saved;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v9.platInfo, &v9, 0, 0, 0);
    RecullWorldEntry(v9);
}

// Road reflections: view 4 renders the scene from player 1's camera (it shares view 1's camera object) into a 320x240
// texture (0x870894, surface 0x870890 = view 4's target) before the player views (0x5CAB15..0x5CACCC, blurred by
// 0x5C7E40 through the second texture 0x87089C/0x870898); road materials bind [0x870894] when they draw (0x5BC7D1...)
// and sample it at each pixel's projected position, so every view showed player 1's reflection. Now players 2..4 get
// their own texture, rendered right after player 1's (same place in the frame: rendering it inside the view loop left
// the view's state broken), with their camera and their view's projection (AdjustProjection), and [0x870894] is
// switched to the player's texture before each player view. The textures are D3DPOOL_DEFAULT like the game's and are
// released with them on device loss (0x5BA610), recreated on demand.
#define OPT_ROAD_REFLECTION 0x870CD4     // 0 off, 1 .., 2/3 full
#define GAME_STATE          0x8654A4     // 6 = racing
#define CURRENT_PASS        0x870910
#define RENDER_PASS_MAIN    0x86EB28
#define FN_RT_BEGIN         0x5BA3C0     // void __cdecl (RenderTarget*, int clear)
#define FN_WORLD_DRAW       0x5D6AD0     // void __thiscall (world views; View*, flags)
#define FN_REFLECT_CARS     0x631250     // void __cdecl (View*)
#define FN_DRAW_CARS        0x630DE0     // void __cdecl (View*, int)
#define FN_FLUSH_DRAWS      0x5CF420
#define FN_REFLECT_POST     0x5C7E40     // racing and option 3: blur
#define REFLECT_SURFACE     0x870890
#define REFLECT_TEXTURE     0x870894
#define REFLECT_FORMAT      0x7FEE78
#define AT_REFLECTIONS_DONE 0x5CACCC     // cmp [0x870CB8], ebx (6 bytes) - after view 4, before the env map
#define FN_RELEASE_TARGETS  0x5BA610     // device lost: releases the render target textures
static int g_reflectionPlayer = 0;       // player whose camera/projection view 4 has
#define RENDER_PASS_GLOW    0x86EB5C
#define OPT_GLOW            0x870CE4
static void* g_reflTex[MAX_LOCAL_PLAYERS];   // [0] unused (player 1 uses the game's)
static void* g_reflSurf[MAX_LOCAL_PLAYERS];
static void* g_gameReflTex;              // the game's texture/surface while ours are swapped in
static void* g_gameReflSurf;

template <class F> static void InRenderPass(uintptr_t passAddr, F draw) {
    auto* pass = at<uint8_t*>(passAddr);
    void* dev = at<void*>(D3D_DEVICE);
    if (!pass || !dev) return;
    at<void*>(CURRENT_PASS) = pass;
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(pass))[1])(pass);
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(dev))[0x15C / 4])(
        dev, *reinterpret_cast<void**>(pass + 0x24));
    auto* effect = *reinterpret_cast<void**>(pass + 0x20);
    unsigned passes = 0;
    reinterpret_cast<HRESULT(__stdcall*)(void*, unsigned*, DWORD)>((*reinterpret_cast<void***>(effect))[0x104 / 4])(
        effect, &passes, 0);
    draw();
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(pass))[2])(pass);
    reinterpret_cast<HRESULT(__stdcall*)(void*)>((*reinterpret_cast<void***>(effect))[0x114 / 4])(effect);
    at<void*>(CURRENT_PASS) = nullptr;
}

static void ComRelease(void*& p) {
    if (p) reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(p))[2])(p);
    p = nullptr;
}

static void ReleaseReflectionTextures() {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) { ComRelease(g_reflSurf[k]); ComRelease(g_reflTex[k]); }
}

static bool EnsureReflectionTexture(int k) {
    if (g_reflTex[k] && g_reflSurf[k]) return true;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return false;
    auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*)>(
        (*reinterpret_cast<void***>(dev))[23]);                      // CreateTexture
    if (FAILED(create(dev, 320, 240, 1, 1 /* D3DUSAGE_RENDERTARGET */, at<DWORD>(REFLECT_FORMAT), 0 /* DEFAULT */,
                      &g_reflTex[k], nullptr)))
        return false;
    auto level = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, void**)>(
        (*reinterpret_cast<void***>(g_reflTex[k]))[18]);            // GetSurfaceLevel
    if (FAILED(level(g_reflTex[k], 0, &g_reflSurf[k]))) { ComRelease(g_reflTex[k]); return false; }
    Log("road reflection texture for player %d created", k + 1);
    return true;
}

static void RenderRoadReflection(int k) {
    View& v4 = GetView(4);
    int opt = at<int>(OPT_ROAD_REFLECTION);
    View* v = &v4;
    v4.camera = GetView(kPlayerViewId[k]).camera;
    g_reflectionPlayer = k;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v4.platInfo, v, 0, 0, 0);
    RecullWorldEntry(v4);
    reinterpret_cast<void(__cdecl*)(RenderTarget*, int)>(FN_RT_BEGIN)(v4.rt, 1);
    if (opt > 1) {
        reinterpret_cast<void(__thiscall*)(void*, View*, int)>(FN_WORLD_DRAW)(g_worldViews, v, 8);
        reinterpret_cast<void(__cdecl*)(View*)>(FN_SPLIT_PASS)(v);
        reinterpret_cast<void(__cdecl*)(View*)>(FN_REFLECT_CARS)(v);
        InRenderPass(RENDER_PASS_MAIN, [v] { reinterpret_cast<void(__cdecl*)(View*, int)>(FN_DRAW_CARS)(v, 1); });
    }
    InRenderPass(RENDER_PASS_MAIN, [v] { reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v); });
    reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
    if (at<int>(GAME_STATE) == 6 && opt == 3) reinterpret_cast<void(__cdecl*)()>(FN_REFLECT_POST)();
    InRenderPass(RENDER_PASS_GLOW, [v] {        // glow sprites: tail/head light reflections
        if (!at<int>(OPT_GLOW)) return;
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x491860)(v, 2);
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x61AE90)(v, 1);
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x61ACE0)(v, 1);
        reinterpret_cast<void(__cdecl*)(View*, int, int)>(0x494270)(v, 0, 1);
    });
}

// The car's own reflection on the road (0x631250(view): local player 0 / human slot 0 for view 4, player 1 / slot 1 for
// anything else) drew player 1's car into every player's road reflection -> the player view 4 is rendered for.
#define AT_REFLECT_CAR_PICK  0x631289    // mov eax,[edi+4] ... call 0x5ED980 (edi = view)
#define AT_REFLECT_CAR_DONE  0x6312B0    // esi = local player, eax = racer id
void* GetLocalPlayer(int k);
static void* __fastcall ReflectedCarPlayer(int viewId, int* racer) {
    int k = viewId == 4 ? ViewPlayer(g_reflectionPlayer) : 1;
    *racer = RacerIdOfPlayer(k);
    return GetLocalPlayer(k);
}
static __declspec(naked) void ReflectCarPickHook() {
    __asm {
        sub esp, 4
        mov edx, esp
        mov ecx, dword ptr [edi + 4]
        call ReflectedCarPlayer
        mov esi, eax
        pop eax
        push AT_REFLECT_CAR_DONE
        ret
    }
}

static void __cdecl RoadReflectionsForPlayers() {
    View& v4 = GetView(4);
    if (!SplitRacing() || !at<int>(OPT_ROAD_REFLECTION) || !v4.active || !v4.rt || !v4.rt->enabled || !v4.platInfo ||
        !g_worldViews || !at<void*>(REFLECT_TEXTURE))
        return;
    g_gameReflTex = at<void*>(REFLECT_TEXTURE);
    g_gameReflSurf = at<void*>(REFLECT_SURFACE);
    uint8_t* cam1 = v4.camera;
    for (int k = 1; k < g_screenViews; ++k) {
        if (!GetView(kPlayerViewId[k]).camera || !EnsureReflectionTexture(k)) continue;
        at<void*>(REFLECT_TEXTURE) = g_reflTex[k];
        at<void*>(REFLECT_SURFACE) = g_reflSurf[k];
        v4.rt->surface = g_reflSurf[k];
        RenderRoadReflection(k);
    }
    at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    at<void*>(REFLECT_SURFACE) = g_gameReflSurf;
    v4.rt->surface = g_gameReflSurf;
    v4.camera = cam1;
    g_reflectionPlayer = 0;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v4.platInfo, &v4, 0, 0, 0);
}

static __declspec(naked) void ReflectionsDoneHook() {
    __asm {
        pushad
        call RoadReflectionsForPlayers
        popad
        cmp dword ptr ds:[0x00870CB8], ebx  // stolen
        push AT_REFLECTIONS_DONE + 6
        ret
    }
}

// player view k samples its own reflection texture (player 1: the game's)
static void UseReflectionOf(int k) {
    if (!g_gameReflTex || at<void*>(REFLECT_SURFACE) != g_gameReflSurf) return;   // not this frame / device reset
    at<void*>(REFLECT_TEXTURE) = k > 0 && g_reflTex[k] ? g_reflTex[k] : g_gameReflTex;
}

// The nitrous screen effect's strength (effect channel handler 0x5D7770, writes the view effect's channel 4 from the
// car's nitrous bar via 0x5925B0) picks the car by view: view 1 -> local player 1 (in creation order), any other view
// -> player 2, so P3's view showed P2's bar (frozen) and P3's own nitrous never registered (user). The view's own player.
#define AT_NOS_FX_PLAYER      0x5D778D   // cmp edx,1; setne cl; mov edx,[ecx*4+inOrder]  (13 bytes, edx = view id)
#define AT_NOS_FX_PLAYER_RET  0x5D779A
void* LocalPlayerInOrder(int k);
static void* __fastcall NosFxPlayer(int viewId) {
    int k = viewId == 1 ? 0 : 1;
    for (int i = 0; i < MAX_LOCAL_PLAYERS; ++i)
        if (viewId == kPlayerViewId[i]) k = i;
    return LocalPlayerInOrder(ViewPlayer(k));
}
static __declspec(naked) void NosFxPlayerHook() {
    __asm {
        mov ecx, edx
        call NosFxPlayer
        mov edx, eax
        push AT_NOS_FX_PLAYER_RET
        ret
    }
}

// Overbright strength follows the nitrous of player 1 only: once per frame 0x5BC180 reads view 1's effect state
// (+0x60 object: +0x3C nitrous on, +0x1E0 its intensity), smooths it (0x876848, rate 0x800B90) and sets the amount the
// full-screen overbright pass adds into its accumulation (0x7FF114, blend factor at 0x5CCB7E) -> P1's nitrous
// brightened every view. Now each player view has its own smoothed amount, and the accumulation blit (0x5C6BE0 at
// 0x5CCBA5) runs once per view with that view's blend factor, limited to its region by a scissor rectangle (the blit
// itself stays full-screen so its sampling is unchanged; SetRenderTargetHook re-applies the scissor).
#define AT_OVERBRIGHT_AMOUNT_CALL 0x5D2B5A   // call 0x5BC180 (once per frame)
#define FN_OVERBRIGHT_AMOUNT      0x5BC180
#define AT_OVERBRIGHT_ACCUM_CALL  0x5CCBA5   // call 0x5C6BE0 (cdecl, 8 args) into the streak surface
static void SetRenderState(DWORD state, DWORD value);
static float g_obSmoothed[MAX_LOCAL_PLAYERS];
static int g_obAmount[MAX_LOCAL_PLAYERS];
static int g_obThreshold[MAX_LOCAL_PLAYERS];   // bright-pass threshold 0x7FF10C (216 -> 62 with nitrous)
int g_obScissorSlotExt = -1;
#define g_obScissorSlot g_obScissorSlotExt
static void __cdecl OverbrightAmounts() {
    reinterpret_cast<void(__cdecl*)()>(FN_OVERBRIGHT_AMOUNT)();
    g_obAmount[0] = at<int>(0x7FF114);
    g_obThreshold[0] = at<int>(0x7FF10C);
    g_obSmoothed[0] = at<float>(0x876848);
    if (!SplitRacing()) return;
    for (int k = 1; k < g_screenViews; ++k) {
        auto* fx = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(&GetView(kPlayerViewId[k])) + 0x60);
        float t = at<float>(0x7D7D3C);
        if (at<uint8_t>(0x870904)) t = at<float>(0x7FF7C8);
        else if (at<int>(GAME_STATE) != 6) t = at<float>(0x7A20A8);
        else if (fx && *reinterpret_cast<int*>(fx + 0x3C)) t = *reinterpret_cast<float*>(fx + 0x1E0);
        float lo = at<float>(0x7FF0FC), x = lo + (at<float>(0x7FF100) - lo) * t;
        float rate = at<float>(0x800B90);
        g_obSmoothed[k] = (1.0f - rate) * g_obSmoothed[k] + rate * x;
        int a = at<int>(0x7FF118), b = at<int>(0x7FF11C);
        g_obAmount[k] = static_cast<int>(a + (b - a) * g_obSmoothed[k]);
        int c = at<int>(0x7FF120), d = at<int>(0x7FF124);
        g_obThreshold[k] = static_cast<int>(c + (d - c) * g_obSmoothed[k]);
    }
}
static RECT g_obScissorRect;
static void SetScissor(void* dev, int k, float scale = 1.0f) {   // region of player k, scaled to the target's size
    g_obScissorRect = {static_cast<LONG>(g_regionX[k] * scale), static_cast<LONG>(g_regionY[k] * scale),
                       static_cast<LONG>((g_regionX[k] + g_rt[k].width) * scale),
                       static_cast<LONG>((g_regionY[k] + g_rt[k].height) * scale)};
    reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*)>((*reinterpret_cast<void***>(dev))[75])(dev, &g_obScissorRect);
}
void ReapplyOverbrightScissor(void* dev) {
    reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*)>((*reinterpret_cast<void***>(dev))[75])(dev, &g_obScissorRect);
}
// The bright pass of the overbright post (0x5C5F70 with its last argument 1: half-size scene 0x86FF7C -> 0x86FFA8,
// calls at 0x5CCA40 / 0x5CCA61) uses the threshold 0x7FF10C, which 0x5BC180 lowers with player 1's nitrous -> every
// view got brighter. Run once per view with its own threshold, scissored to its region (scaled to the target).
#define AT_BRIGHT_PASS_CALL1 0x5CCA40
#define AT_BRIGHT_PASS_CALL2 0x5CCA61
#define AT_BRIGHT_PASS_CALL0 0x5CCA25    // the downsample before it (last argument 0), same treatment in case it is the one
static void __cdecl BrightPassPerView(void* dst, void* src, int mode, void* params, int flag) {
    auto blit = reinterpret_cast<void(__cdecl*)(void*, void*, int, void*, int)>(0x5C5F70);
    void* dev = at<void*>(D3D_DEVICE);
    if (!SplitRacing() || g_screenViews < 2 || !dev || !dst || at<int>(GAME_STATE) != 6) { blit(dst, src, mode, params, flag); return; }
    struct { DWORD Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality; UINT Width, Height; } desc = {};
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(dst))[12])(dst, &desc);
    float scale = at<int>(SCREEN_W) > 0 ? static_cast<float>(desc.Width) / at<int>(SCREEN_W) : 1.0f;
    int saved = at<int>(0x7FF10C);
    SetRenderState(174, 1);
    for (int k = 0; k < g_screenViews; ++k) {
        at<int>(0x7FF10C) = g_obThreshold[k];
        g_obScissorSlot = k;
        SetScissor(dev, k, scale);
        blit(dst, src, mode, params, flag);
    }
    g_obScissorSlot = -1;
    SetRenderState(174, 0);
    at<int>(0x7FF10C) = saved;
}
static void __cdecl OverbrightAccumulate(void* a1, int a2, void* a3, void* a4, int a5, int a6, int a7, int a8) {
    auto blit = reinterpret_cast<void(__cdecl*)(void*, int, void*, void*, int, int, int, int)>(0x5C6BE0);
    void* dev = at<void*>(D3D_DEVICE);
    if (!SplitRacing() || g_screenViews < 2 || !dev || at<int>(GAME_STATE) != 6) { blit(a1, a2, a3, a4, a5, a6, a7, a8); return; }
    SetRenderState(174, 1);                                         // D3DRS_SCISSORTESTENABLE
    for (int k = 0; k < g_screenViews; ++k) {
        uint32_t v = static_cast<uint32_t>(g_obAmount[k]) & 0xFF;
        SetRenderState(0xC1, v | v << 8 | v << 16 | v << 24);      // D3DRS_BLENDFACTOR
        g_obScissorSlot = k;
        SetScissor(dev, k);
        blit(a1, a2, a3, a4, a5, a6, a7, a8);
    }
    g_obScissorSlot = -1;
    SetRenderState(174, 0);
}

// 3 players: P3 is centred in the bottom half, leaving a bar on each side. Nothing draws there, so both bars are
// filled black every frame after the views - in the picture and in the streak/overbright buffer (whose full-screen
// pass would otherwise show leftovers).
static void ClearUnusedAreas() {
    if (!SplitRacing() || (g_screenViews != 3 && g_screenViews != 5)) return;   // a short, centred bottom row
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return;
    int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
    Cell first = LayoutCell(g_screenViews, g_screenViews == 3 ? 2 : 3, w, h), last = LayoutCell(g_screenViews, g_screenViews - 1, w, h);
    RECT bars[2] = {{0, first.y, first.x, h}, {last.x + last.w, last.y, w, h}};
    auto fill = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, DWORD)>((*reinterpret_cast<void***>(dev))[35]);
    void* targets[2] = {reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface,
                        at<int>(0x870CFC) ? at<void*>(0x86FF70) : nullptr};
    for (void* t : targets)
        if (t) for (const RECT& r : bars) fill(dev, t, &r, 0);
}

// after the player views: the game's texture back
static void RestoreRoadReflectionView() {
    ClearUnusedAreas();
    if (g_gameReflTex && at<void*>(REFLECT_SURFACE) == g_gameReflSurf) at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    g_gameReflTex = nullptr;
    g_gameReflSurf = nullptr;
}
