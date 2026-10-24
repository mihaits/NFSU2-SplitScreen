// Part of NFSU2SplitScreen.cpp: rendering: every player's headlight projector, in the game's own headlight draw.
//
// The game has one projector, view 9 (re-aimed per player view: HeadlightsForPlayer). The headlight pass (0x5D6D20 at
// 0x5CB218, callback 0x488B90) flags the meshes in view 9's culled list (instance +0x1B |= 0x80); the world draw
// (0x5CB23B) queues them with render flag 0x200, which picks the material's headlight effect (0x5C5953:
// worldheadlight, -nodepth, -reflect, depth_worldheadlight, -reflect), and the flush at 0x5CB2A0 draws them.
// 0x5BDAA0 sets the projector per draw: HeadlightWVP = X * VP9 * B (VP9 at view 9's platInfo +0x80, B a translation by
// the floats at 0x80052C) and HeadlightColor from 0x60F510(player 1's car). The pixel shaders (ps_1_1) compute
//   L = beam(q.xy / q.w) * ramp(0.005 q.w + 0.5) * HeadlightColor,   rgb = sat(2 * texture * (vertex colour + L))
// (q = pos * HeadlightWVP; the ramp clips behind the projector and fades out towards 100 m).
//
// The other players' lights go into the same draw (the method of th0mk/nfsu2-web, runtime/headlights.c): per player
// view, view 9 is aimed at each other player in reach and its culled list flagged too (MarkLitMesh), and their
// projector coordinates are q_k = q * T_k with T_k = inv(VP_own B) (VP_k B), one matrix per light and view. While
// that view's world queue is drawn, the game's headlight shaders are swapped for:
//   - vs: the game's own, with src/shaders/hl_vs_extra.asm appended (q_k into oT4..oT6, T_k in c20..c31)
//   - ps_2_0 per variant (src/shaders/hl_ps_*.asm): L = min(L_own + sum of gain * L_k, cap), the rest as the game's
// Gain and cap keep overlapping beams from blowing out the road (the game is tuned for one light: one beam's L already
// takes the texture to full brightness).
#include "headlight_shaders.h"

#define AT_MAIN_WORLD_DRAW  0x5CB23B     // call 0x5D6AD0 (world views; view, 0x20): queues the view's world meshes
#define AT_LIT_WORLD_FLUSH  0x5CB2A0     // call 0x5CF420: draws the view's world queue (the lit meshes too)
#define AT_LIGHT_COLOR_CALL 0x5BDC20     // call 0x60F510: HeadlightColor, always player 1's car
#define FN_CAR_LIGHT_COLOR  0x60F510     // uint32_t __cdecl (car): 0x00BBGGRR
#define HEADLIGHT_BIAS      0x80052C     // B: x, y added after view 9's projection (0x5BDAA0)
#define FN_FLAG_LIT_MESHES  0x5D6D20     // void __thiscall (world views; View*, void(__cdecl*)(item*))
#define MAX_OTHER_LIGHTS    3
#define LIGHT_REACH         250.0f       // m from the view's camera (a beam reaches 100 m ahead of its car)

struct LightSet {
    bool active;                         // from the view's world draw until its queue is drawn
    float vsT[MAX_OTHER_LIGHTS * 4][4];  // c20..c31: T_k columns
    float ps[MAX_OTHER_LIGHTS + 1][4];   // c1..c3 colours, c4 the cap
};
static LightSet g_lights;
static float g_lightGain = 0.8f, g_lightCap = 1.0f;

static uint32_t __cdecl OwnLightColor(void* car) {
    return reinterpret_cast<uint32_t(__cdecl*)(void*)>(FN_CAR_LIGHT_COLOR)(
        SplitMode() && g_ownLightCar ? g_ownLightCar : car);
}

// ---- shader swap
static const uint32_t kLitVsHashes[] = {0x11E68A13, 0xC6C07610, 0xB98AA1F3, 0x49C43D70};   // fxdump hash
static const uint32_t kLitPsHashes[] = {0x8B4A5AE8, 0x4A4A5AE8, 0x071164AC, 0x2178C399};   // world, depth, reflect,
static const DWORD* const kLitPs[] = {kHlPs_world, kHlPs_depth, kHlPs_reflect, kHlPs_depthreflect};   // depth refl.
static std::unordered_map<void*, void*> g_litVs;   // game vs -> ours (null: not a headlight shader / failed)
static std::unordered_map<void*, int> g_litPsKind; // game ps -> index in kLitPs (-1: not a headlight shader)
static void* g_litPs[4];
static void* g_origSetVertexShader;
static void* g_origSetPixelShader;

// FNV-1a over the tokens, comments skipped (as tools/fxdump hash)
static uint32_t ShaderHash(const DWORD* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        DWORD t = p[i];
        if ((t & 0xFFFF) == 0xFFFE && i) { i += (t >> 16) & 0x7FFF; continue; }
        h = (h ^ t) * 16777619u;
        if (t == 0x0000FFFF) break;
    }
    return h;
}
static bool ShaderFunction(void* shader, DWORD* buf, UINT* size) {     // IDirect3D{Vertex,Pixel}Shader9::GetFunction
    auto get = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, UINT*)>((*reinterpret_cast<void***>(shader))[4]);
    UINT n = 0;
    if (FAILED(get(shader, nullptr, &n)) || n == 0 || n > *size) return false;
    *size = n;
    return SUCCEEDED(get(shader, buf, size));
}

static void* LitVertexShader(void* dev, void* vs) {
    auto it = g_litVs.find(vs);
    if (it != g_litVs.end()) return it->second;
    void* ours = nullptr;
    static DWORD code[2048];
    UINT size = sizeof(code);
    if (ShaderFunction(vs, code, &size)) {
        size_t n = size / 4;
        uint32_t h = ShaderHash(code, n);
        for (uint32_t known : kLitVsHashes) {
            if (h != known) continue;
            const size_t extra = sizeof(kHlVsExtra) / 4;
            if (code[n - 1] != 0x0000FFFF || n + extra > sizeof(code) / 4) break;
            memcpy(code + n - 1, kHlVsExtra, sizeof(kHlVsExtra));
            code[n - 1 + extra] = 0x0000FFFF;
            auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, const DWORD*, void**)>(
                (*reinterpret_cast<void***>(dev))[91]);                       // CreateVertexShader
            HRESULT hr = create(dev, code, &ours);
            Log("headlights: vertex shader %08X %s (0x%08lX)", h, SUCCEEDED(hr) ? "extended" : "NOT extended", hr);
            if (FAILED(hr)) ours = nullptr;
        }
    }
    g_litVs[vs] = ours;
    return ours;
}

static void* LitPixelShader(void* dev, void* ps) {
    auto it = g_litPsKind.find(ps);
    int kind = -1;
    if (it != g_litPsKind.end()) kind = it->second;
    else {
        static DWORD code[1024];
        UINT size = sizeof(code);
        if (ShaderFunction(ps, code, &size)) {
            uint32_t h = ShaderHash(code, size / 4);
            for (int i = 0; i < 4; ++i) if (h == kLitPsHashes[i]) kind = i;
        }
        g_litPsKind[ps] = kind;
    }
    if (kind < 0) return nullptr;
    if (!g_litPs[kind]) {
        auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, const DWORD*, void**)>(
            (*reinterpret_cast<void***>(dev))[106]);                          // CreatePixelShader
        HRESULT hr = create(dev, kLitPs[kind], &g_litPs[kind]);
        DWORD flags = 0;
        reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD*)>((*reinterpret_cast<void***>(dev))[66])(
            dev, 1, 24, &flags);                                              // stage 1 TEXTURETRANSFORMFLAGS
        Log("headlights: pixel shader %d %s (0x%08lX), beam stage transform flags 0x%lX", kind,
            SUCCEEDED(hr) ? "created" : "NOT created", hr, flags);
        if (FAILED(hr)) { g_litPs[kind] = nullptr; g_litPsKind[ps] = -1; }
    }
    return g_litPs[kind];
}

static HRESULT __stdcall SetVertexShaderHook(void* dev, void* vs) {
    if (g_lights.active && vs)
        if (void* ours = LitVertexShader(dev, vs)) {
            reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const float*, UINT)>(
                (*reinterpret_cast<void***>(dev))[94])(dev, 20, &g_lights.vsT[0][0], MAX_OTHER_LIGHTS * 4);
            vs = ours;
        }
    return reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_origSetVertexShader)(dev, vs);
}
static HRESULT __stdcall SetPixelShaderHook(void* dev, void* ps) {
    if (g_lights.active && ps)
        if (void* ours = LitPixelShader(dev, ps)) {
            reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const float*, UINT)>(
                (*reinterpret_cast<void***>(dev))[109])(dev, 1, &g_lights.ps[0][0], MAX_OTHER_LIGHTS + 1);
            ps = ours;
        }
    return reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_origSetPixelShader)(dev, ps);
}

// ---- the view's light set
static uint8_t* g_marked[4096];          // instances flagged for the other lights only
static int g_markedCount;
static void __cdecl MarkLitMesh(uint8_t* item) {
    uint8_t* inst = *reinterpret_cast<uint8_t**>(item + 8);
    if (inst[0x1B] & 0x80) return;
    inst[0x1B] |= 0x80;
    if (g_markedCount < 4096) g_marked[g_markedCount++] = inst;
}

// view 9's view * projection, then * B
static void ProjectorMatrix(const View& v9, double m[4][4]) {
    const float* vp = reinterpret_cast<const float*>(v9.platInfo + 0x80);
    const float* b = reinterpret_cast<const float*>(HEADLIGHT_BIAS);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) m[i][j] = vp[i * 4 + j] + (j < 2 ? vp[i * 4 + 3] * b[j] : 0.0);
}
static bool Invert(const double a[4][4], double r[4][4]) {    // Gauss-Jordan, partial pivoting
    double w[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) { w[i][j] = a[i][j]; w[i][4 + j] = i == j; }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int i = c + 1; i < 4; ++i) if (fabs(w[i][c]) > fabs(w[p][c])) p = i;
        if (fabs(w[p][c]) < 1e-12) return false;
        if (p != c) for (int j = 0; j < 8; ++j) std::swap(w[c][j], w[p][j]);
        double d = 1.0 / w[c][c];
        for (int j = 0; j < 8; ++j) w[c][j] *= d;
        for (int i = 0; i < 4; ++i) {
            if (i == c || w[i][c] == 0) continue;
            double f = w[i][c];
            for (int j = 0; j < 8; ++j) w[i][j] -= f * w[c][j];
        }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) r[i][j] = w[i][4 + j];
    return true;
}

// Flags the other players' lit meshes and fills g_lights for view v (player k). Returns the number of other lights.
static int PrepareOtherLights(void* world, View* v) {
    int k = -1;
    for (int i = 0; i < g_screenViews && i < MAX_LOCAL_PLAYERS; ++i) if (&GetView(kPlayerViewId[i]) == v) k = i;
    View& v9 = GetView(9);
    if (k < 0 || !v9.active || !v9.camera || !v9.platInfo || !v->camera) return 0;
    const float* eye = reinterpret_cast<const float*>(v->camera + 0x40);
    double other[MAX_OTHER_LIGHTS][4][4];
    uint32_t colour[MAX_OTHER_LIGHTS];
    int n = 0;
    int players = 2 + (g_extraPlayers > 0 ? g_extraPlayers : 0);   // the intro has 1 view for all of them
    for (int j = 0; j < players && j < MAX_LOCAL_PLAYERS && n < MAX_OTHER_LIGHTS; ++j) {
        uint8_t* car = j == k ? nullptr : PlayerCar(j);
        if (!car || !car[0x550]) continue;
        const float* pos = reinterpret_cast<const float*>(car + 0x50);
        float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz > LIGHT_REACH * LIGHT_REACH) continue;
        HeadlightsForPlayer(j);
        ProjectorMatrix(v9, other[n]);
        colour[n] = reinterpret_cast<uint32_t(__cdecl*)(void*)>(FN_CAR_LIGHT_COLOR)(car);
        reinterpret_cast<void(__thiscall*)(void*, View*, void*)>(FN_FLAG_LIT_MESHES)(world, &v9, &MarkLitMesh);
        ++n;
    }
    if (!n) return 0;
    HeadlightsForPlayer(k);              // back on the view's car: the game's per-draw HeadlightWVP / colour
    double own[4][4], inv[4][4];
    ProjectorMatrix(v9, own);
    if (!Invert(own, inv)) return 0;
    for (int s = 0; s < MAX_OTHER_LIGHTS; ++s) {
        int l = s < n ? s : 0;           // unused slots repeat light 0 with no colour (finite coordinates)
        for (int c = 0; c < 4; ++c)      // T = inv * other[l], uploaded by columns
            for (int r = 0; r < 4; ++r) {
                double t = 0;
                for (int i = 0; i < 4; ++i) t += inv[r][i] * other[l][i][c];
                g_lights.vsT[s * 4 + c][r] = static_cast<float>(t);
            }
        float g = s < n ? g_lightGain / 255.0f : 0.0f;
        uint32_t col = colour[l];
        g_lights.ps[s][0] = g * (col & 0xFF);
        g_lights.ps[s][1] = g * ((col >> 8) & 0xFF);
        g_lights.ps[s][2] = g * ((col >> 16) & 0xFF);
        g_lights.ps[s][3] = 0;
    }
    for (int i = 0; i < 4; ++i) g_lights.ps[MAX_OTHER_LIGHTS][i] = g_lightCap;
    return n;
}

static void __fastcall WorldDrawWithLights(void* world, void*, View* v, int flags) {
    g_lights.active = false;
    g_markedCount = 0;
    int n = 0;
    void* dev = at<void*>(D3D_DEVICE);
    // split racing, or the full screen race intro (view 1, every player's light)
    if (SplitMode() && at<int>(NUM_PLAYERS) == 2 && at<int>(OPT_CAR_HEADLIGHTS) && world == g_worldViews && dev) {
        HookDeviceMethod(dev, 92, reinterpret_cast<void*>(&SetVertexShaderHook), &g_origSetVertexShader);
        HookDeviceMethod(dev, 107, reinterpret_cast<void*>(&SetPixelShaderHook), &g_origSetPixelShader);
        n = PrepareOtherLights(world, v);
    }
    reinterpret_cast<void(__thiscall*)(void*, View*, int)>(FN_WORLD_DRAW)(world, v, flags);
    for (int i = 0; i < g_markedCount; ++i) g_marked[i][0x1B] &= 0x7F;   // flagged, not in the view's list
    g_lights.active = n > 0;
}
static void __cdecl FlushLitWorld() {
    reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
    g_lights.active = false;
}

static void InstallHeadlights() {
    g_lightGain = GetPrivateProfileIntA("DEBUG", "HeadlightGain", 80, g_iniPath) / 100.0f;
    g_lightCap = GetPrivateProfileIntA("DEBUG", "HeadlightCap", 100, g_iniPath) / 100.0f;
    WriteCall(AT_MAIN_WORLD_DRAW, &WorldDrawWithLights);
    WriteCall(AT_LIT_WORLD_FLUSH, &FlushLitWorld);
    WriteCall(AT_LIGHT_COLOR_CALL, &OwnLightColor);
    Log("headlights: other players' gain %.2f, light cap %.2f", g_lightGain, g_lightCap);
}
