// Part of NFSU2SplitScreen.cpp: rendering: car reflections, headlight projectors, projection.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Car reflections: the cars sample a 128x128 cube render target (0x870858, faces 0x87085C[6], depth 0x870874[6])
// rendered every frame by views 0x10..0x15 (0x5CACF0 loop) from six cameras of the env camera object (0x871BC0) that
// 0x61AB50 places at player 1's car. Players 2..4 get their own cube, one player per frame in turn, rendered right
// after player 1's (same face block, cameras placed at their car); before each player view their faces are copied
// into the game's cube (StretchRect), so player 1's view sees its own and the others theirs.
#define OPT_CAR_ENV_MAP     0x870CB8
#define OPT_ENV_MAP_WORLD   0x870CBC
#define ENV_CUBE_FACES      0x87085C     // IDirect3DSurface9*[6]
#define ENV_CUBE_SIZE       0x7FEE6C
#define ENV_CUBE_FORMAT     0x7FEE70
#define ENV_CAMERAS         0x871BC0
#define ENV_CAR_OFFSET      0x803E50     // float[3] added to the car position
#define FN_PLACE_ENV_CAMERAS 0x5C3450    // void __thiscall (cameras; const float* eye, const float* target)
#define FN_PLACE_ENV_P1     0x61AB50     // the game's placement (player 1)
#define FN_ENV_PREPARE      0x5B9D90
#define AT_ENV_DONE         0x5CAE12     // call 0x5CF420 after the face loop
static void* g_envCube[MAX_LOCAL_PLAYERS];
static void* g_envFace[MAX_LOCAL_PLAYERS][6];
static bool g_envValid[MAX_LOCAL_PLAYERS];
static int g_envTurn = 1;

static void ReleaseEnvCubes() {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        for (void*& f : g_envFace[k]) ComRelease(f);
        ComRelease(g_envCube[k]);
        g_envValid[k] = false;
    }
}

static bool EnsureEnvCube(int k) {
    if (g_envCube[k]) return true;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return false;
    auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*)>(
        (*reinterpret_cast<void***>(dev))[25]);                      // CreateCubeTexture
    if (FAILED(create(dev, at<UINT>(ENV_CUBE_SIZE), 1, 1 /* D3DUSAGE_RENDERTARGET */, at<DWORD>(ENV_CUBE_FORMAT),
                      0 /* DEFAULT */, &g_envCube[k], nullptr)))
        return false;
    auto face = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, UINT, void**)>(
        (*reinterpret_cast<void***>(g_envCube[k]))[18]);            // GetCubeMapSurface
    for (int i = 0; i < 6; ++i)
        if (FAILED(face(g_envCube[k], i, 0, &g_envFace[k][i]))) { ReleaseEnvCubes(); return false; }
    Log("car env map for player %d created", k + 1);
    return true;
}

static void SetRenderState(DWORD state, DWORD value) {
    void* dev = at<void*>(D3D_DEVICE);
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>((*reinterpret_cast<void***>(dev))[57])(dev, state, value);
}

static void RenderEnvCube(int k) {
    // cameras at player k's car, looking from their camera (as 0x61AB50 does for player 1)
    auto* car = static_cast<uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, int)>(0x5F0E30)(
        at<void*>(0x890080), RacerIdOfPlayer(k)));
    uint8_t* cam = GetView(kPlayerViewId[k]).camera;
    if (!car || !cam) return;
    const float* pos = reinterpret_cast<const float*>(car + 0x50);
    const float* off = reinterpret_cast<const float*>(ENV_CAR_OFFSET);
    float target[3] = {pos[0] + off[0], pos[1] + off[1], pos[2] + off[2]};
    float eye[3];
    memcpy(eye, cam + 0x40, sizeof(eye));
    // the cube is in camera space (the cars sample it with a camera-space reflection vector): 0x5C3450 orients the
    // faces by view 1's camera -> player k's camera for the call
    View& v1 = GetView(1);
    uint8_t* cam1 = v1.camera;
    v1.camera = cam;
    reinterpret_cast<void(__thiscall*)(void*, const float*, const float*)>(FN_PLACE_ENV_CAMERAS)(
        reinterpret_cast<void*>(ENV_CAMERAS), eye, target);
    v1.camera = cam1;
    void** gameFaces = reinterpret_cast<void**>(ENV_CUBE_FACES);
    for (int id = 0x10; id <= 0x15; ++id) {
        View& v = GetView(id);
        if (!v.active || !v.rt || !v.rt->enabled || !v.platInfo) continue;
        int f = -1;
        for (int i = 0; i < 6; ++i) if (v.rt->surface == gameFaces[i]) f = i;
        if (f < 0) continue;
        void* saved = v.rt->surface;
        v.rt->surface = g_envFace[k][f];
        reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v.platInfo, &v, 0, 0, 0);
        RecullWorldEntry(v);
        reinterpret_cast<void(__cdecl*)(RenderTarget*, int)>(FN_RT_BEGIN)(v.rt, 1);
        SetRenderState(7, 0);            // D3DRS_ZENABLE: sky without depth
        SetRenderState(14, 0);           // D3DRS_ZWRITEENABLE
        reinterpret_cast<void(__cdecl*)(View*)>(FN_DRAW_SKY)(&v);
        SetRenderState(14, 1);
        SetRenderState(7, 1);
        reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
        reinterpret_cast<void(__cdecl*)()>(FN_ENV_PREPARE)();
        if (at<int>(OPT_ENV_MAP_WORLD))
            reinterpret_cast<void(__thiscall*)(void*, View*, int)>(FN_WORLD_DRAW)(g_worldViews, &v, 0);
        reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
        v.rt->surface = saved;
    }
    g_envValid[k] = true;
}

static void __cdecl EnvMapsForPlayers() {
    reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();   // replaced call
    if (!SplitRacing() || !at<int>(OPT_CAR_ENV_MAP) || at<int>(GAME_STATE) != 6 || !g_worldViews ||
        !at<void*>(ENV_CUBE_FACES) || g_screenViews < 2)
        return;
    if (g_envTurn < 1 || g_envTurn >= g_screenViews) g_envTurn = 1;
    int k = g_envTurn++;
    if (!GetView(kPlayerViewId[k]).camera || !EnsureEnvCube(k)) return;
    RenderEnvCube(k);
    for (int id = 0x10; id <= 0x15; ++id) {     // the faces' views back on player 1's cameras
        View& v = GetView(id);
        if (v.active && v.platInfo)
            reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v.platInfo, &v, 0, 0, 0);
    }
    reinterpret_cast<void(__cdecl*)()>(FN_PLACE_ENV_P1)();
}

// before player view k: its cube's faces into the game's cube (player 1: rendered there this frame, first view)
static void UseEnvMapOf(int k) {
    if (k < 1 || !g_envValid[k] || !g_envCube[k] || !at<int>(OPT_CAR_ENV_MAP)) return;
    void* dev = at<void*>(D3D_DEVICE);
    void** gameFaces = reinterpret_cast<void**>(ENV_CUBE_FACES);
    auto stretch = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, void*, const RECT*, DWORD)>(
        (*reinterpret_cast<void***>(dev))[34]);                      // StretchRect
    for (int i = 0; i < 6; ++i)
        if (gameFaces[i] && g_envFace[k][i]) stretch(dev, g_envFace[k][i], nullptr, gameFaces[i], nullptr, 0);
}

using ReleaseTargetsFn = void(__cdecl*)();
static ReleaseTargetsFn g_origReleaseTargets;
static void __cdecl ReleaseTargetsHook() {
    if (g_gameReflTex && at<void*>(REFLECT_TEXTURE) != g_gameReflTex) at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    g_gameReflTex = g_gameReflSurf = nullptr;
    ReleaseReflectionTextures();
    ReleaseEnvCubes();
    g_origReleaseTargets();
}

// Headlight projectors for every player. The game has one projector: the headlight pass (0x5D6D20(world; view 9,
// 0x488B90) at 0x5CB218) only flags the meshes in view 9's culled list (+8 -> +0x1B |= 0x80); the world draw turns the
// flag into render flag 0x200 (0x5D6B85), which picks the material's headlight shader variant (0x5C5953:
// worldheadlight.efx etc.) lit by view 9's projector, and clears the flag. The world draw only queues the meshes;
// 0x5CF420 draws the queue (normally at 0x5CB2A0), setting the projector params per draw (0x5BDAA0, colour from
// 0x60F510). One projector per draw, so the other players' beams are added in extra passes
// after the view's world draw (0x5CB23B), per other player whose lights are on, with view 9 aimed at that car:
//   1. its flagged meshes again with the headlight colour 0, subtracted from the frame (blend op REVSUBTRACT)
//   2. the same meshes with the beam, added (blend op ADD)
// = + mesh * beam (the base lighting and the fog colour cancel out). Only the flagged meshes are drawn: the world
// draw's include mask (0x2000 for flags & 0x10, imm at 0x5D6B0F) is 0x200 meanwhile. No depth writes, depth test
// with a small bias (both passes use the same headlight shader, the view's own pass may not have).
#define AT_MAIN_WORLD_DRAW    0x5CB23B   // call 0x5D6AD0 (world views; view, 0x20)
#define FN_FLAG_BEAM_MESHES   0x5D6D20   // void __thiscall (world views; View*, void(__cdecl*)(item*))
#define FN_FLAG_BEAM_MESH     0x488B90
#define AT_WORLD_INCLUDE_MASK 0x5D6B0F   // imm32 of `mov [ebp-0Ch], 2000h` (world draw flags & 0x10)
#define FN_DRAW_QUEUED        0x5CF420   // void __cdecl (): draws the queued meshes (per effect; headlight params per draw)
#define AT_BEAM_COLOR_CALL    0x5BDC20   // call 0x60F510: headlight colour (0x00BBGGRR)
#define FN_CAR_LIGHT_COLOR    0x60F510
static int g_beamPass;                   // 1: subtract (colour 0), 2: add
static bool g_beamSkipBlended;           // the material asked for alpha blending: not drawn in the beam passes
static HRESULT __stdcall SetRenderStateHook(void* dev, DWORD state, DWORD value) {
    if (g_beamPass && state == 27) g_beamSkipBlended = value != 0;
    if (g_beamPass) switch (state) {
        case 14: value = 0; break;                              // ZWRITEENABLE
        case 23: value = 4; break;                              // ZFUNC: LESSEQUAL
        case 27: value = 1; break;                              // ALPHABLENDENABLE
        case 19: case 20: value = 2; break;                     // SRCBLEND, DESTBLEND: ONE
        case 171: value = g_beamPass == 1 ? 3 : 1; break;       // BLENDOP: REVSUBTRACT / ADD
        case 206: value = 0; break;                             // SEPARATEALPHABLENDENABLE
        case 195: { float b = -0.00002f; memcpy(&value, &b, 4); break; }   // DEPTHBIAS
        case 175: { float b = -1.0f; memcpy(&value, &b, 4); break; }       // SLOPESCALEDEPTHBIAS
    }
    return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>(g_origSetRenderState)(dev, state, value);
}
// Semi-transparent materials don't cancel out (the subtracted copy clamps at black where it covers less than the
// whole pixel): their draws are skipped in the beam passes.
static HRESULT __stdcall DrawIndexedHook(void* dev, int type, int base, UINT minIdx, UINT n, UINT start, UINT count) {
    if (g_beamPass && g_beamSkipBlended) return 0;
    return reinterpret_cast<HRESULT(__stdcall*)(void*, int, int, UINT, UINT, UINT, UINT)>(g_origDrawIndexed)(
        dev, type, base, minIdx, n, start, count);
}
static uint32_t __cdecl BeamColor(void* car) {
    if (g_beamPass == 1) return 0;
    return reinterpret_cast<uint32_t(__cdecl*)(void*)>(FN_CAR_LIGHT_COLOR)(car);
}
static void __cdecl ClearBeamMesh(uint8_t* item) { (*reinterpret_cast<uint8_t**>(item + 8))[0x1B] &= 0x7F; }
static const DWORD kBeamStates[] = {14, 23, 27, 19, 20, 171, 206, 195, 175};

static void __fastcall WorldDrawWithBeams(void* world, void*, View* v, int flags) {
    using DrawFn = void(__thiscall*)(void*, View*, int);
    reinterpret_cast<DrawFn>(FN_WORLD_DRAW)(world, v, flags);
    // split racing, or the full screen race intro (view 1, player 1's projector)
    if (!SplitMode() || at<int>(NUM_PLAYERS) != 2 || !at<int>(OPT_CAR_HEADLIGHTS) || world != g_worldViews) return;
    int k = -1;
    for (int i = 0; i < g_screenViews && i < MAX_LOCAL_PLAYERS; ++i) if (&GetView(kPlayerViewId[i]) == v) k = i;
    View& v9 = GetView(9);
    void* dev = at<void*>(D3D_DEVICE);
    if (dev && (!g_origSetRenderState || !g_origDrawIndexed)) {   // the first race's intro comes before any split frame
        HookDeviceMethod(dev, 57, reinterpret_cast<void*>(&SetRenderStateHook), &g_origSetRenderState);
        HookDeviceMethod(dev, 82, reinterpret_cast<void*>(&DrawIndexedHook), &g_origDrawIndexed);
    }
    if (k < 0 || !v9.active || !v9.camera || !v9.platInfo || !v->camera || !dev || !g_origSetRenderState ||
        !g_origDrawIndexed)
        return;
    DWORD blending = 0;
    const float* eye = reinterpret_cast<const float*>(v->camera + 0x40);
    using FlagFn = void(__thiscall*)(void*, View*, void*);
    auto flag = reinterpret_cast<FlagFn>(FN_FLAG_BEAM_MESHES);
    auto drawQueued = reinterpret_cast<void(__cdecl*)()>(FN_DRAW_QUEUED);
    DWORD saved[sizeof(kBeamStates) / sizeof(kBeamStates[0])];
    auto get = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD*)>((*reinterpret_cast<void***>(dev))[58]);
    bool any = false;
    int players = 2 + (g_extraPlayers > 0 ? g_extraPlayers : 0);   // the intro has 1 view for all of them
    for (int j = 0; j < players && j < MAX_LOCAL_PLAYERS; ++j) {
        const uint8_t* car = j == ViewPlayer(k) ? nullptr : PlayerCar(j);
        if (!car || !car[0x550]) continue;
        const float* pos = reinterpret_cast<const float*>(car + 0x50);
        float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz > 250.0f * 250.0f) continue;   // beam out of sight
        if (!any) {
            for (size_t i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) get(dev, kBeamStates[i], &saved[i]);
            blending = saved[2];
            static const uint32_t headlightOnly = 0x200;
            Write(AT_WORLD_INCLUDE_MASK, &headlightOnly, 4);
            drawQueued();                // the view's own world draw first, as the game would
            any = true;
        }
        HeadlightsForPlayer(j);
        for (int pass = 1; pass <= 2; ++pass) {
            flag(world, &v9, reinterpret_cast<void*>(FN_FLAG_BEAM_MESH));
            reinterpret_cast<DrawFn>(FN_WORLD_DRAW)(world, v, flags | 0x10);   // queues the flagged meshes
            g_beamPass = pass;
            for (DWORD st : kBeamStates) SetRenderState(st, 0);   // forced to the pass's values by the hook
            g_beamSkipBlended = blending != 0;   // the game's own blend state (it may not set it again)
            drawQueued();
            g_beamPass = 0;
        }
        flag(world, &v9, reinterpret_cast<void*>(&ClearBeamMesh));   // meshes outside the player view's list
    }
    if (!any) return;
    static const uint32_t original = 0x2000;
    Write(AT_WORLD_INCLUDE_MASK, &original, 4);
    for (size_t i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) SetRenderState(kBeamStates[i], saved[i]);
    HeadlightsForPlayer(k);              // the projector back on the view's own car for the rest of the view
}

// The extra player views share one world view table entry (RegisterExtraWorldViews): before each of them after the
// first it points at that view and is re-culled into the free item buffer space (RecullWorldEntry finds an entry by its
// view), and the buffer cursor (+0x6C8) is moved past it so the headlight re-cull doesn't overwrite it. The cursor goes
// back at the next player view. A player view culls 1.5-3.3 KB of the 50.4 KB buffer (4P used 7.7-9.9 KB).
#define WORLD_CULL_CURSOR  0x6C8
static uint32_t g_cullBase;
static void WorldTableProbe(int viewId) {
    uint8_t* world = g_worldViews;
    if (!world || !g_debug || viewId != 1 || g_mmFrame % 600) return;
    int n = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    uint32_t cur = *reinterpret_cast<uint32_t*>(world + WORLD_CULL_CURSOR);
    char line[512];
    int len = wsprintfA(line, "world views: %d entries, cull buffer %u / %u bytes:", n, cur - 0x876DE0u, 0x8832C0u - 0x876DE0u);
    for (int i = 0; i < n && i < 12; ++i) {
        uint8_t* e = world + i * 0x90;
        View* v = *reinterpret_cast<View**>(e + 0x40);
        len += wsprintfA(line + len, " v%d=%u", v ? v->id : -1,
                         *reinterpret_cast<uint32_t*>(e + 0x4C) - *reinterpret_cast<uint32_t*>(e + 0x48));
    }
    Log("%s", line);
}
static void SharedWorldEntryFor(int k) {
    uint8_t* world = g_worldViews;
    if (!world) return;
    uint32_t& cursor = *reinterpret_cast<uint32_t*>(world + WORLD_CULL_CURSOR);
    if (k == 0) g_cullBase = cursor;
    else if (g_cullBase) cursor = g_cullBase;
    if (k < 2) return;
    View& v = GetView(kPlayerViewId[k]);
    int n = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    for (int i = 0; i < n && i < 12; ++i) {
        uint8_t* e = world + i * 0x90;
        View*& owner = *reinterpret_cast<View**>(e + 0x40);
        if (!owner || owner->id < 5 || owner->id > 8) continue;
        if (owner == &v) return;                 // view 5: culled by the game this frame
        owner = &v;
        RecullWorldEntry(v);
        cursor = *reinterpret_cast<uint32_t*>(e + 0x4C);
        return;
    }
}

static void __cdecl RefreshViewTarget(int viewId) {
    if (!SplitRacing()) return;
    WorldTableProbe(viewId);
    for (int k = 0; k < g_screenViews; ++k) if (kPlayerViewId[k] == viewId) SharedWorldEntryFor(k);
    if (g_screenViews > 2) memset(reinterpret_cast<void*>(GLARE_SLOT_COUNTS), 0, 8);
    auto* bb = reinterpret_cast<RenderTarget*>(RT_BACKBUFFER);
    for (int k = 0; k < g_screenViews; ++k)
        if (kPlayerViewId[k] == viewId && GetView(viewId).rt == &g_rt[k]) {
            g_rt[k].surface = bb->surface;
            g_rt[k].depth = bb->depth;
            HeadlightsForPlayer(k);
            UseReflectionOf(k);
            UseEnvMapOf(k);
        }
}

static __declspec(naked) void ViewLoopTopHook() {
    __asm {
        mov edx, dword ptr [esp + 0x10]  // loop counter 1..4 -> player view id (stolen: mov edx,[esp+10]; push edx)
        mov edx, dword ptr [kPlayerViewId + edx * 4 - 4]
        mov g_currentView, edx
        pushad
        push edx
        call RefreshViewTarget
        add esp, 4
        popad
        push edx
        push AT_VIEWLOOP_TOP_RET
        ret
    }
}

static __declspec(naked) void ViewLoopExitHook() {
    __asm {
        mov g_currentView, 0
        pushad
        call RestoreRoadReflectionView
        popad
        push AT_VIEWLOOP_EXIT_RET
        cmp byte ptr ds:[0x008709D0], bl // stolen; flags are consumed by the jz after the return point
        ret
    }
}

// Projection for the half-height split views: keep the full-screen horizontal FOV (hor+ for the screen aspect) and
// derive the vertical scale from the viewport aspect. Runs right before the game builds view-proj and frustum.
static void __cdecl AdjustProjection(View* v, float* plat) {
    bool player = false;
    for (int k = 0; k < g_screenViews; ++k) player |= v->id == kPlayerViewId[k];
    // view 4 (road reflection texture) must project exactly like the player view that samples it
    const RenderTarget* shape = v->rt;
    if (v->id == 4 && g_reflectionPlayer >= 0 && g_reflectionPlayer < g_screenViews) {
        player = true;
        shape = &g_rt[g_reflectionPlayer];
    }
    if (!SplitRacing() || !player || !shape || !v->camera || shape->height <= 0) return;
    unsigned fov = *reinterpret_cast<uint16_t*>(v->camera + 0xC4);
    if (!fov) return;
    const float kPi = 3.14159265f;
    float half = fov * (kPi / 65536.0f);                          // half of the camera's (4:3) horizontal FOV
    float screenAspect = static_cast<float>(at<int>(SCREEN_W)) / at<int>(SCREEN_H);
    float p00 = (1.0f / tanf(half)) * (4.0f / 3.0f) / screenAspect;
    float viewAspect = static_cast<float>(shape->width) / shape->height;
    plat[0x40 / 4] = p00;
    plat[0x54 / 4] = -p00 * viewAspect;
}

static __declspec(naked) void ProjectionHook() {
    __asm {
        pushad
        push esi                         // platInfo
        push ebp                         // view
        call AdjustProjection
        add esp, 8
        popad
        push ebx                         // stolen: push ebx ; xor edi,edi ; push esi ; lea ebp,[esi+80h]
        xor edi, edi
        push esi
        lea ebp, [esi + 0x80]
        push AT_PROJECTION_RET
        ret
    }
}
