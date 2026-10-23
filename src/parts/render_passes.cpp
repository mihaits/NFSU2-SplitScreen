// Part of NFSU2SplitScreen.cpp: rendering: light streaks, speed blur, headlights, light pools.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Light streaks (g_LightStreaksEnable 0x870CE0): 0x5CBB00 keeps ONE streak buffer that persists across frames and is
// fed by every view, so in split screen each view shows the others' streaks. Until there is a buffer per view, split
// views take the game's no-streaks branch at 0x5CB577 (`cmp [0x870CE0], ebx; je 0x5CB5AC`). The option itself is not
// touched: its buffers are created from it at race load (toggling it mid-race left them null -> crash on quit).
#define AT_STREAKS_BRANCH  0x5CB577      // 8 bytes: cmp dword [0x870CE0], ebx (6) + je rel8 (2)
#define AT_STREAKS_ON      0x5CB57F
#define AT_STREAKS_OFF     0x5CB5AC
// Overbright (g_OverBrightEnable 0x870CFC, needs streaks + glow) uses that buffer (0x86FF70, screen size) as its
// persistent accumulation: the streak function fades it and adds each view's light glows, and the full-screen overbright
// post (0x5CCA00) blurs the scene into it and shows it (the streak composite is skipped then). Without the streak pass
// nothing faded it in split -> bright spots burned in. With overbright on, split views run the streak pass again, with
// everything it draws into the buffer confined to the view's part of it (StreakRegionViewport).
#define STREAK_SURFACE     0x86FF70
#define OPT_OVERBRIGHT     0x870CFC
static bool OverbrightActive() { return at<int>(OPT_OVERBRIGHT) && at<int>(0x870CE4) && at<int>(0x870CE0); }
#define AT_STREAK_VIEW_CALL 0x5CBC02    // `push 1; call 0x48B1E0`: the glows drawn into the buffer are view 1's
static void* __cdecl StreakView(int id) {
    for (int k = 0; k < g_screenViews; ++k)
        if (SplitRacing() && g_currentView == kPlayerViewId[k]) return &GetView(g_currentView);
    return &GetView(id);
}
// Without overbright the streak function ends with its own composite of the buffer onto the view (0x5C6BE0 at
// 0x5CBDA6: a full-clip-space quad, UVs 0..1 = the whole buffer). In a split view it would squeeze every player's part
// of the buffer into the view, so while that composite runs for a player view its UVs are remapped to the view's part:
// the blit builds the quad on its stack (4 vertices of 0x30 bytes at esp+0x30: position, then 4 UV pairs) and calls
// 0x5BCBC0 at 0x5C6EC6 before drawing it.
#define AT_STREAK_COMPOSITE_CALL 0x5CBDA6
#define FN_SCREEN_BLIT     0x5C6BE0      // cdecl, 8 args
#define AT_BLIT_QUAD_READY 0x5C6EC6      // call 0x5BCBC0 (vertices at the caller's esp+0x30)
#define FN_BLIT_QUAD_READY 0x5BCBC0
static int g_streakCompositeView = -1;   // player view whose streak composite is running (-1: none)
static void __cdecl StreakComposite(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                    uint32_t h) {
    for (int k = 0; k < g_screenViews; ++k)
        if (SplitRacing() && g_currentView == kPlayerViewId[k]) g_streakCompositeView = k;
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
        FN_SCREEN_BLIT)(a, b, c, d, e, f, g, h);
    g_streakCompositeView = -1;
}
static void __cdecl RemapStreakQuad(float* vertices) {
    int k = g_streakCompositeView;
    float w = static_cast<float>(at<int>(SCREEN_W)), h = static_cast<float>(at<int>(SCREEN_H));
    if (k < 0 || w <= 0 || h <= 0) return;
    float x0 = g_regionX[k] / w, y0 = g_regionY[k] / h, sx = g_rt[k].width / w, sy = g_rt[k].height / h;
    for (int v = 0; v < 4; ++v)
        for (int t = 0; t < 4; ++t) {
            float* uv = vertices + v * 12 + 4 + t * 2;
            uv[0] = x0 + uv[0] * sx;
            uv[1] = y0 + uv[1] * sy;
        }
}
static __declspec(naked) void BlitQuadReadyHook() {
    __asm {
        cmp g_streakCompositeView, 0
        jl done
        pushad
        lea eax, [esp + 0x20 + 0x40]     // caller's esp+0x30: past pushad, our return address and its 3 arguments
        push eax
        call RemapStreakQuad
        add esp, 4
        popad
    done:
        push FN_BLIT_QUAD_READY
        ret
    }
}
static int __cdecl StreaksAllowed() { return at<int>(0x870CE0) != 0; }
static __declspec(naked) void StreaksBranchHook() {
    __asm {
        pushad
        call StreaksAllowed
        test eax, eax
        popad
        jz off
        push AT_STREAKS_ON
        ret
    off:
        push AT_STREAKS_OFF
        ret
    }
}

static void ApplySplitLayout() {
    auto* bb = reinterpret_cast<RenderTarget*>(RT_BACKBUFFER);
    if (SplitRacing()) {
        HookDeviceSetViewport();
        int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
        bool quad = g_quadTest || g_extraPlayers > 0;
        g_screenViews = !quad ? 2 : g_extraPlayers == 1 ? 3 : 4;      // 3 players: bottom-right quadrant empty
        for (int k = 0; k < g_screenViews; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (k >= 2) {
                InitPlayerView(kPlayerViewId[k]);
                if (g_extraPlayers > 0) { if (void* c = ExtraCamera(k)) v.camera = static_cast<decltype(v.camera)>(c); }
                else v.camera = GetView(kPlayerViewId[k - 2]).camera;   // QuadTest: reuse P1/P2's cameras
            }
            g_rt[k] = *bb;               // refresh every frame: surfaces change after a device reset
            g_rt[k].ownerView = kPlayerViewId[k];
            g_rt[k].enabled = 1;
            g_rt[k].width = quad ? w / 2 : w;
            g_rt[k].height = h / 2;
            g_regionX[k] = quad ? (k & 1) * (w / 2) : 0;
            if (g_screenViews == 3 && k == 2) g_regionX[k] = w / 4;   // 3 players: P3 centred in the bottom half
            g_regionY[k] = quad ? (k >> 1) * (h / 2) : k * (h / 2);
            v.rt = &g_rt[k];
            v.active = 1;
        }
        for (int k = g_screenViews; k < MAX_LOCAL_PLAYERS; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (v.rt == &g_rt[k]) { v.rt = nullptr; v.active = 0; }
        }
        // Layouts 1/2 enable the road reflection view 4 (its 320x240 target) and the headlight view 9 (0x5C233D); layout
        // 3 has no case, so a split race inherited them from the intro (layout 1). Without an intro (the street drift
        // tracks 4174-4179 have no usable scene) they stayed off, and with road reflections on the main render returns
        // at once when view 4 is inactive (0x5CAB35) -> black world, and quitting crashed in the flare render.
        View& v4 = GetView(4);
        if (v4.rt) { v4.rt->enabled = 1; v4.rt->width = 320; v4.rt->height = 240; }
        v4.active = 1;
        GetView(9).active = 1;
    } else {
        g_screenViews = 2;
        g_viewTintInit[0] = g_viewTintInit[1] = false;      // per-race copies (they point into track data)
        View& v1 = GetView(1);
        if (v1.rt == &g_rt[0]) v1.rt = bb;
        for (int k = 1; k < MAX_LOCAL_PLAYERS; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (v.rt == &g_rt[k]) { v.rt = nullptr; v.active = 0; }
        }
    }
}

// Speed blur (g_MotionBlurEnable, 0x5CC460, called per player view at 0x5CB31F): reads view 1's camera for the speed
// and blur centre (0x48B1E0(1) at 0x5CC474 / 0x5CC490), smooths its strength in one global (0x870900) and copies the
// whole backbuffer into its blur texture (StretchRect at 0x5CC760) -> only P1 got it, centred on the screen centre.
// Per view: the view being drawn supplies the camera, keeps its own smoothed strength, and only its region is copied.
#define AT_BLUR_CALL       0x5CB31F
#define FN_MOTION_BLUR     0x5CC460
#define AT_BLUR_VIEW_CALL1 0x5CC474
#define AT_BLUR_VIEW_CALL2 0x5CC490
#define AT_BLUR_COPY       0x5CC760      // `call [ecx+0x88]` IDirect3DDevice9::StretchRect (6 bytes)
#define BLUR_STRENGTH      0x870900
static float g_blurStrength[MAX_LOCAL_PLAYERS];
struct BlurCopyArgs { void* dev; void* src; void* dst; int filter; };
static BlurCopyArgs g_blurCopy;                 // the view-region copy made during the current blur pass
using StretchRectFn = HRESULT(__stdcall*)(void*, void*, const RECT*, void*, const RECT*, int);
static StretchRectFn StretchRectOf(void* dev) {
    return reinterpret_cast<StretchRectFn>((*reinterpret_cast<void***>(dev))[0x88 / 4]);
}

static int CurrentPlayerSlot() {
    if (!SplitRacing()) return -1;
    for (int k = 0; k < g_screenViews; ++k)
        if (g_currentView == kPlayerViewId[k]) return k;
    return -1;
}

static View* __cdecl BlurView(int n) {
    int k = CurrentPlayerSlot();
    return &GetView(k >= 0 ? kPlayerViewId[k] : n);
}

static void __cdecl MotionBlurForView() {
    int k = CurrentPlayerSlot();
    float& strength = *reinterpret_cast<float*>(BLUR_STRENGTH);
    if (g_debug) CheckX87("motion blur in");
    if (k < 0) { reinterpret_cast<void(__cdecl*)()>(FN_MOTION_BLUR)(); return; }
    float shared = strength;
    strength = g_blurStrength[k];
    g_blurCopy = {};
    reinterpret_cast<void(__cdecl*)()>(FN_MOTION_BLUR)();
    g_blurStrength[k] = strength;
    strength = shared;
    if (g_debug) {                       // per view: camera speed and how often the blur actually drew
        static unsigned s_frames[MAX_LOCAL_PLAYERS], s_drawn[MAX_LOCAL_PLAYERS];
        static float s_maxSpeed[MAX_LOCAL_PLAYERS];
        ++s_frames[k];
        if (g_blurCopy.dev) ++s_drawn[k];
        if (uint8_t* cam = GetView(kPlayerViewId[k]).camera) {
            const float* v = reinterpret_cast<const float*>(cam + 0x1E0);
            float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (speed > s_maxSpeed[k]) s_maxSpeed[k] = speed;
        }
        if (s_frames[k] >= 180) {
            Log("blur view %d: max camera speed %.1f, drawn %u/%u frames", k + 1, s_maxSpeed[k], s_drawn[k], s_frames[k]);
            s_frames[k] = s_drawn[k] = 0;
            s_maxSpeed[k] = 0;
        }
    }
    // The blur texture is also the frame-end post chain's screen copy (0x86EB0C, e.g. depth of field reads it after
    // the views): leave it holding the whole screen, as the unpatched pass does.
    if (g_blurCopy.dev)
        StretchRectOf(g_blurCopy.dev)(g_blurCopy.dev, g_blurCopy.src, nullptr, g_blurCopy.dst, nullptr, g_blurCopy.filter);
}

static HRESULT __stdcall BlurCopy(void* dev, void* src, const RECT* srcRect, void* dst, const RECT* dstRect, int filter) {
    RECT region;
    int k = CurrentPlayerSlot();
    if (k >= 0 && !srcRect) {
        region = {g_regionX[k], g_regionY[k], g_regionX[k] + g_rt[k].width, g_regionY[k] + g_rt[k].height};
        srcRect = &region;
        if (!dstRect) g_blurCopy = {dev, src, dst, filter};
    }
    return StretchRectOf(dev)(dev, src, srcRect, dst, dstRect, filter);
}

typedef void(__cdecl* VoidFn)();
static VoidFn g_origLayoutApply;

static void EndResultsCount();
static void UpdateGaugeMaps();
static void SyncPlayerSettings(bool newRace);
static void UpdateFreeCamHook();
static void UpdateStartZoom();
static void UpdateIntroHiding();
static void UpdatePadLights();

static void __cdecl LayoutApplyHook() {
    if (g_debug) CheckX87("frame start");
    g_origLayoutApply();
    if (g_debug) CheckX87("after layout apply");
    HookDeviceDebug();
    XPadUpdate();
    UpdateFreeCamHook();
    UpdateStartZoom();
    UpdateIntroHiding();
    UpdateHudWiden();
    EndResultsCount();
    SyncPlayerSettings(false);
    UpdateQuadHud();
    UpdateGaugeMaps();
    ++g_mmFrame;
    if (g_vpLogFrames > 0 && --g_vpLogFrames == 0) Log("vp log end");
    UpdateEditingSlot2();
    UpdateBindSwap();
    UpdateSeats();
    UpdatePadLights();
    RunMailbox();
    UpdateModeTransitions();
    ApplySplitLayout();
}

// The split views draw into copies of the backbuffer target (g_rt). On alt-tab / device reset the game releases and
// recreates the backbuffer surfaces in the middle of a frame, so copies taken at frame start can point at released
// surfaces (crash in d3d9 under SetViewport, 0x5CB1EA). Re-copy right before each player view renders.
// Car light glare slots (0x48AB20: per light type, counters 0x82DAE8[2], capacity 0x7F3D44 {200, 50}) are reset once
// per frame (0x48AC00 at 0x5CBAE1) but made and drawn per player view: with 3-4 views the per-frame budget ran out as
// cars came into view and the later views lost their glares (user: P3's and then P1's nitrous glare vanished). Each
// player view starts with an empty budget.
#define GLARE_SLOT_COUNTS  0x82DAE8

// World headlights: one projector, view 9 (camera 0x870A20), placed once per frame by 0x5C2AE0(view 9, human index 0)
// (called at 0x5D2955; human index -> racer id via the race's human slots 0x89E808[], car found by that id in the race
// manager's list) and its matrices built by 0x5C7EC0. Every player view projected P1's headlights. Each player view
// now re-aims the projector at its own car right before it renders.
#define FN_HEADLIGHT_CAMERA 0x5C2AE0     // void __cdecl (View* view9, int humanIndex)
#define FN_VIEW_UPDATE      0x5C7EC0     // void __thiscall (platInfo; View*, 0, 0, 0)
#define OPT_CAR_HEADLIGHTS  0x870CC4
#define HUMAN_RACER_SLOTS   0x89E808     // racer ids of humans 0, 1
// The headlight pass draws view 9's entry of the frame's world view table (0x5D6D20 at 0x5CB218), culled once per
// frame from the projector on P1 (0x5E9A30: pass 1 stores the camera position / direction and the scenery section,
// pass 2 culls each entry into the shared item buffer 0x876DE0 via 0x5E99C0) -> other players' lights only reached
// geometry near P1. Re-cull view 9's entry from the re-aimed projector into the free buffer space after the frame's
// lists (the buffer cursor is left alone, so each player view reuses that space).
#define FN_WORLD_ENTRY_FRUSTUM 0x5C26E0  // void __cdecl (View*, entry)
#define FN_WORLD_CULL_ENTRY    0x5E99C0  // void __thiscall (world; entry)
#define WORLD_SECTION_MODE     0x800DF8
#define WORLD_SECTION_MASK     0x8832C0
static void RecullHeadlightView(View& v9) {
    uint8_t* world = g_worldViews;
    if (!world || !v9.camera) return;
    int n = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    uint8_t* e = nullptr;
    for (int i = 0; i < n && i < 12; ++i)
        if (*reinterpret_cast<View**>(world + i * 0x90 + 0x40) == &v9) e = world + i * 0x90;
    if (!e) return;
    reinterpret_cast<void(__cdecl*)(View*, void*)>(FN_WORLD_ENTRY_FRUSTUM)(&v9, e);
    const float* cam = reinterpret_cast<const float*>(v9.camera + 0x40);
    float* pos = reinterpret_cast<float*>(e + 0x60);
    pos[0] = cam[0]; pos[1] = cam[1]; pos[2] = cam[2];          // +0x60 position, +0x6C direction (camera +0x50)
    pos[3] = cam[4]; pos[4] = cam[5]; pos[5] = cam[6];
    *reinterpret_cast<int*>(e + 0x80) = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(&v9) + 0xC);
    int mode = at<int>(WORLD_SECTION_MODE), section = -1;
    if (mode == 1 || (mode == 2 && at<int>(0x7FB708) % 0x3D >= 0xF)) {   // as pass 1 (mode 3 left unrestricted)
        int x = static_cast<int>(pos[0]), y = static_cast<int>(pos[1]);
        int cell = (((y >> 5) & 0x7F) << 7) + ((x >> 5) & 0x7F);
        if (!(reinterpret_cast<const uint8_t*>(WORLD_SECTION_MASK)[cell >> 3] & (1 << (cell & 7))))
            section = (((y >> 5) & 0x1F) << 5) + ((x >> 5) & 0x1F);
    }
    *reinterpret_cast<int*>(e + 0x84) = section;
    uint32_t cursor = *reinterpret_cast<uint32_t*>(world + 0x6C8);
    *reinterpret_cast<uint32_t*>(e + 0x48) = cursor;
    *reinterpret_cast<uint32_t*>(e + 0x4C) = cursor;
    *reinterpret_cast<uint32_t*>(e + 0x50) = *reinterpret_cast<uint32_t*>(world + 0x6CC);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_WORLD_CULL_ENTRY)(world, e);
}

// Per-car headlight pools on the road ("2PLAYERHEADLIGHT1" decal, 0x615490(view): every car with lights on that is
// not hidden in the view) are drawn by the 1-player render path only (0x5CAC07); the split per-view path skips them,
// so only the projector car lit the road. Draw them in the split path's matching pass, after 0x566400 (0x5CB2E3).
#define AT_SPLIT_PASS_CALL   0x5CB2E3
#define FN_SPLIT_PASS        0x566400    // void __cdecl (View*)
#define FN_HEADLIGHT_POOLS   0x615490    // void __cdecl (View*)
// As in single player, the view's own car gets no pool (its projector lights the road; the pool is a flat quad that
// tilts with the car and clipped into the road under braking): its lights flag (+0x550) is off during the call.
// Every player's car has a projector (WorldDrawWithBeams, also in the full screen race intro), so no player car gets
// a pool: their lights flags are off during the call. Traffic keeps its pools, as in single player.
static uint8_t* PlayerCar(int k) {
    int id = k < 2 ? reinterpret_cast<const int*>(HUMAN_RACER_SLOTS)[k] : k;
    uint8_t* mgr = at<uint8_t*>(0x890080);
    if (!mgr) return nullptr;
    for (auto* c = *reinterpret_cast<uint8_t**>(mgr); c && c != mgr; c = *reinterpret_cast<uint8_t**>(c))
        if (*reinterpret_cast<int16_t*>(c + 0xA) == id) return c;
    return nullptr;
}
static void __cdecl PoolsForTraffic(View* v) {
    if (!SplitMode() || at<int>(NUM_PLAYERS) != 2) {
        reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v);
        return;
    }
    uint8_t* car[MAX_LOCAL_PLAYERS] = {};
    uint8_t lights[MAX_LOCAL_PLAYERS] = {};
    for (int k = 0; k < 2 + (g_extraPlayers > 0 ? g_extraPlayers : 0) && k < MAX_LOCAL_PLAYERS; ++k)
        if ((car[k] = PlayerCar(k)) != nullptr) { lights[k] = car[k][0x550]; car[k][0x550] = 0; }
    reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) if (car[k]) car[k][0x550] = lights[k];
}
static void __cdecl SplitPassWithHeadlightPools(View* v) {
    reinterpret_cast<void(__cdecl*)(View*)>(FN_SPLIT_PASS)(v);
    if (SplitRacing() && at<int>(OPT_CAR_HEADLIGHTS)) PoolsForTraffic(v);
}
// The race intro is drawn full screen by the 1-player path, which draws the pools itself (0x5CAC07).
#define AT_1P_POOLS_CALL     0x5CAC07
