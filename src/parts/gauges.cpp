// Part of NFSU2SplitScreen.cpp: single-player gauges and custom gauge textures.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---- Single-player gauges in split screen (user: the car-specific tach / boost / nitrous gauges and the 1P drag
// tach instead of the simplified 2P ones). The HUD ctor 0x4F8BF0 builds the same element classes for every package,
// binding objects by name, so the look comes from the package and the classes' NUM_PLAYERS == 1 branches. Each player
// additionally gets a Tachometer (or DragRaceTachometer), TurboMeter and NitrousGauge bound to a private copy of the 1P
// package (HUD_SingleRace / HUD_Drag / HUD_Drift -> "HUD_SingleRac1.fng"...), built, state-switched and updated with
// NUM_PLAYERS = 1. They read their player's car (index -> racer -> CUSTOM_HUD part 0xA3, "HUDINDEX"), so customized
// gauges work per player; each player's custom texture pack is loaded with P1's (LoadHudTextures). The 2P package's
// gauge objects and the copies' other objects are not drawn; the gauges are placed per view (GaugeTransform).
#define FN_FE_PUSH_PACKAGE 0x555F10      // void __cdecl (fng, 0, 0, 0)
#define FN_FE_POP_PACKAGE  0x5379A0      // void __cdecl (fng): what the HUD dtor does with its package
#define FN_GAME_FREE       0x575630
#define FN_FE_HIDE_OBJECT  0x50CA00      // void __cdecl (obj)
#define FN_ELEMENT_LIST_FREE 0x4835C0    // element dtor: frees its object list (head at +0, node +8 = object)
#define FN_ELEMENT_SET_STATE 0x4C6690    // thiscall (state lo, hi): shows/hides its objects by its masks
#define AT_HUD_UPDATE_CALL 0x6005DB      // player update: `call 0x4EC5A0` (HUD update, thiscall (player, arg))
#define FN_HUD_UPDATE      0x4EC5A0
#define AT_HUD_DTOR_CALL   0x4FD963      // HUD deleting dtor: `call 0x4FD720`
#define FN_HUD_DTOR_BODY   0x4FD720
#define AT_HUD_TEX_LOAD_CALL 0x57F3D8    // race load step: `call 0x577F60` (P1's custom HUD textures, thiscall (cb, ctx))
#define FN_HUD_TEX_LOAD    0x577F60
#define AT_HUD_TEX_UNLOAD_CALL 0x5816D4  // `call 0x580FC0` (thiscall: releases them)
#define FN_HUD_TEX_UNLOAD  0x580FC0
struct GaugeKind { uint32_t hudOffset; uintptr_t ctor; size_t size; uintptr_t update; };
static const GaugeKind kGauges[] = {
    {0x18, 0x4F5100, 0xC8, 0x4C68D0},    // Tachometer (not drag)
    {0x34, 0x4F6500, 0xA8, 0x4C7090},    // DragRaceTachometer (drag)
    {0x1C, 0x4F5A70, 0x38, 0x4C6B80},    // TurboMeter
    {0x28, 0x4F6A90, 0x38, 0x4C7B20},    // NitrousGauge
};
struct PlayerGauges { void* hud; void* elem[4]; char fng[32]; uint32_t state[2]; bool stateSet; bool dead; };
static PlayerGauges g_gauges[MAX_LOCAL_PLAYERS];
static bool g_spGauges = true;
static int g_gaugeMapFrames = 0;

static bool IsDragRace() { return at<uint8_t>(0x89E7D8) || at<int>(0x890108); }
static bool IsDriftRace() { return at<uint8_t>(0x89E7D9) || at<int>(0x890104); }

struct NumPlayersOne {
    int saved;
    NumPlayersOne() : saved(at<int>(NUM_PLAYERS)) { at<int>(NUM_PLAYERS) = 1; }
    ~NumPlayersOne() { at<int>(NUM_PLAYERS) = saved; }
};

static const uint8_t* ActivePackage(const char* name) {
    auto* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, const char*)>(FN_FIND_PACKAGE)(
        reinterpret_cast<void*>(FE_PACKAGE_CACHE), name);
    return e ? *reinterpret_cast<uint8_t**>(e + 0x18) : nullptr;
}

template <class F> static void ForElementObjects(const void* elem, F f) {
    if (!elem) return;
    auto* head = static_cast<const uint8_t*>(elem);
    int guard = 0;
    for (auto* node = *reinterpret_cast<uint8_t* const*>(head); node && node != head && ++guard < 512;
         node = *reinterpret_cast<uint8_t**>(node))
        if (auto* obj = *reinterpret_cast<uint8_t**>(node + 8)) f(obj);
}

static void AddTree(std::unordered_map<const void*, int>& m, const uint8_t* obj, int k, int depth = 0) {
    if (!obj || depth > 8) return;
    m[obj] = k;
    if (const void* data = *reinterpret_cast<const void* const*>(obj + 0x2C)) m[data] = k;
    if (*reinterpret_cast<const int*>(obj + 0x18) == 5)
        for (auto* c = *reinterpret_cast<const uint8_t* const*>(obj + 0x64); c; c = *reinterpret_cast<const uint8_t* const*>(c + 4))
            AddTree(m, c, k, depth + 1);
}

// objects of a 1P copy: groups holding a gauge object are drawn (placed as the player's), everything else is hidden
static bool HasOwned(const uint8_t* obj, int depth = 0) {
    if (!obj || depth > 8) return false;
    if (g_gaugeShow.count(obj)) return true;
    if (*reinterpret_cast<const int*>(obj + 0x18) == 5)
        for (auto* c = *reinterpret_cast<const uint8_t* const*>(obj + 0x64); c; c = *reinterpret_cast<const uint8_t* const*>(c + 4))
            if (HasOwned(c, depth + 1)) return true;
    return false;
}

static bool HasShown(const uint8_t* obj, int depth = 0) { return HasOwned(obj, depth); }

static void SplitCopyObjects(const uint8_t* obj, int k, int depth = 0) {
    for (; obj && depth <= 8; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4)) {
        if (g_gaugeShow.count(obj)) {
            *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C) |= 0x400000;   // cached transform
            continue;
        }
        if (HasShown(obj, depth)) {
            *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C) |= 0x400000;
            g_gaugeShow[obj] = k;
            if (const void* data = *reinterpret_cast<const void* const*>(obj + 0x2C)) g_gaugeShow[data] = k;
            if (*reinterpret_cast<const int*>(obj + 0x18) == 5) {
                // groups start hidden in the drag package (shown by the game for the 1P HUD); the elements only
                // switch their own objects
                // (only the group itself: 0x50CA50 also shows every child, which re-showed the elements' hidden
                // objects every remap - shift light blinking, N2O gamebreaker overlay stuck on)
                if (HasOwned(obj, depth)) {
                    auto& flags = *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C);
                    if (flags & 1) flags = (flags & ~1u) | 0x400000;
                }
                SplitCopyObjects(*reinterpret_cast<const uint8_t* const*>(obj + 0x64), k, depth + 1);
            }
        } else {
            AddTree(g_gaugeHide, obj, k);   // children are queued for rendering on their own
            if (depth > 0) reinterpret_cast<void(__cdecl*)(const void*)>(FN_FE_HIDE_OBJECT)(obj);   // drawn by its group
        }
    }
}

static void BuildGaugeMaps() {
    g_gaugeShow.clear();
    g_gaugeHide.clear();
    // copies whose HUD is gone stay hidden until the frontend has dropped them (their custom textures get unloaded)
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.dead) continue;
        const uint8_t* pkg = ActivePackage(g.fng);
        if (!pkg) { g.dead = false; continue; }
        for (auto* obj = *reinterpret_cast<const uint8_t* const*>(pkg + 0x4C); obj; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4))
            AddTree(g_gaugeHide, obj, k);
    }
    g_gaugeDrag = IsDragRace();             // (also during the intro: the copies' other objects must stay hidden)
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        for (void* e : g.elem) ForElementObjects(e, [k](const uint8_t* obj) { AddTree(g_gaugeShow, obj, k); });
    }
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        for (const GaugeKind& kind : kGauges)
            ForElementObjects(*reinterpret_cast<void**>(static_cast<uint8_t*>(g.hud) + kind.hudOffset),
                              [k](const uint8_t* obj) { if (!g_gaugeShow.count(obj)) AddTree(g_gaugeHide, obj, k); });
        const uint8_t* pkg = ActivePackage(g.fng);
        if (pkg) SplitCopyObjects(*reinterpret_cast<const uint8_t* const*>(pkg + 0x4C), k);
        if (g_debug && g_gaugeMapFrames == 1) {
            static int s_logs = 0;
            if (s_logs++ < 8) Log("1P gauges %d: package %p (%s)", k + 1, pkg, g.fng);
        }
    }
    static size_t s_last = 0;
    if (g_gaugeShow.size() != s_last) {
        s_last = g_gaugeShow.size();
        Log("1P gauges: %d shown, %d hidden", static_cast<int>(g_gaugeShow.size()), static_cast<int>(g_gaugeHide.size()));
    }
}

static void UpdateGaugeMaps() {
    if (++g_gaugeMapFrames < 30 && g_gaugeMapFrames > 1) return;   // elements add objects at runtime
    g_gaugeMapFrames = 1;
    BuildGaugeMaps();
}

static void DestroyGauges(int k) {
    for (void*& e : g_gauges[k].elem) {
        if (!e) continue;
        reinterpret_cast<void(__thiscall*)(void*)>(FN_ELEMENT_LIST_FREE)(e);
        reinterpret_cast<void(__cdecl*)(void*)>(FN_GAME_FREE)(e);
        e = nullptr;
    }
    g_gauges[k].hud = nullptr;
    g_gauges[k].dead = true;
    reinterpret_cast<void(__cdecl*)(const char*)>(FN_FE_POP_PACKAGE)(g_gauges[k].fng);
    BuildGaugeMaps();
    Log("1P gauges for player %d removed (%s %s)", k + 1, g_gauges[k].fng, ActivePackage(g_gauges[k].fng) ? "still active" : "popped");
}

static void CreateGauges(int k, void* hud) {
    if (!g_spGauges || !SplitMode() || at<int>(NUM_PLAYERS) != 2 || k < 0 || k >= MAX_LOCAL_PLAYERS || !hud) return;
    if (g_gauges[k].hud) DestroyGauges(k);
    bool drag = IsDragRace();
    const char* srcs[2] = {drag ? "HUD_Drag.fng" : IsDriftRace() ? "HUD_Drift.fng" : "HUD_SingleRace.fng",
                           "HUD_SingleRace.fng"};
    PlayerGauges& g = g_gauges[k];
    bool ok = false;
    for (const char* src : srcs) {
        strcpy_s(g.fng, src);
        char* dot = strrchr(g.fng, '.');
        dot[-1] = static_cast<char>('1' + k);
        if ((ok = CloneFePackage(src, g.fng))) break;
        Log("1P gauges: no %s", src);
    }
    if (!ok) return;
    reinterpret_cast<void(__cdecl*)(const char*, int, int, int)>(FN_FE_PUSH_PACKAGE)(g.fng, 0, 0, 0);
    NumPlayersOne one;
    for (int i = 0; i < 4; ++i) {
        g.elem[i] = nullptr;
        if ((i == 0 && drag) || (i == 1 && !drag)) continue;
        void* mem = reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(kGauges[i].size);
        if (mem) g.elem[i] = reinterpret_cast<void*(__thiscall*)(void*, const char*, int)>(kGauges[i].ctor)(mem, g.fng, k);
    }
    g.hud = hud;
    g.dead = false;
    g.stateSet = false;
    g_gaugeMapFrames = 0;
    Log("1P gauges for player %d from %s", k + 1, g.fng);
}

// the HUD's messages (show / intro animations: the 1P drag package starts with its tach faded out) reach the copies
static void ForwardToGauges(uint32_t msg, const char* fng) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        const char* hn = *reinterpret_cast<const char**>(static_cast<uint8_t*>(g.hud) + 0x10);
        bool match = hn && _stricmp(fng, hn) == 0;
        if (!match && k > 0 && g_hudCloneName[k][0] && _stricmp(fng, g_hudCloneSource[k]) == 0) match = true;
        if (match) g_origSendMsg(msg, g.fng);
    }
}

static void __fastcall HudUpdateHook(void* hud, void*, void* player, void* arg) {
    // the 2P package's own gauges (hidden) are not updated: both sets updating per frame for the same player
    // fought over shared state (the N2O gamebreaker overlay stayed on, the shift light blinked)
    void* saved[4] = {};
    bool mine = false;
    for (PlayerGauges& g : g_gauges) mine |= g.hud == hud;
    auto* h = static_cast<uint8_t*>(hud);
    if (mine)
        for (int i = 0; i < 4; ++i) std::swap(saved[i], *reinterpret_cast<void**>(h + kGauges[i].hudOffset));
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(FN_HUD_UPDATE)(hud, player, arg);
    if (mine)
        for (int i = 0; i < 4; ++i) std::swap(saved[i], *reinterpret_cast<void**>(h + kGauges[i].hudOffset));
    // Free Run has no race position or race time: the position element (HUD +0x20, ctor 0x4F4AC0) shows "1st /3" in
    // split screen (user) - its RacePositionNumber +0x28, RacePositionNth +0x2C, NumberRacers +0x30, PositionBacking
    // +0x3C hidden - and so are the timer and its backings (by name in the HUD's package)
    if (SplitMode() && at<int>(QR_RACE_TYPE) == 9) {
        if (auto* pos = *reinterpret_cast<uint8_t**>(h + 0x20))
            for (int off : {0x28, 0x2C, 0x30, 0x3C})
                if (auto* obj = *reinterpret_cast<uint8_t**>(pos + off)) *reinterpret_cast<uint32_t*>(obj + 0x1C) |= 1;
        static uint32_t timerHashes[4];
        if (!timerHashes[0]) {
            const char* names[4] = {"CurrentLapTime", "CurrentLapTimeText", "LapCounterBacking_2", "LapCounterBacking_3"};
            for (int i = 0; i < 4; ++i) timerHashes[i] = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(FN_FE_HASH)(names[i]);
        }
        const char* name = *reinterpret_cast<const char**>(h + 0x10);
        auto* entry = name ? reinterpret_cast<uint8_t*(__thiscall*)(void*, const char*)>(FN_FIND_PACKAGE)(
            reinterpret_cast<void*>(FE_PACKAGE_CACHE), name) : nullptr;
        if (auto* pkg = entry ? *reinterpret_cast<uint8_t**>(entry + 0x18) : nullptr)
            for (uint32_t hash : timerHashes)
                if (uint8_t* obj = FindFeObject(pkg, hash)) *reinterpret_cast<uint32_t*>(obj + 0x1C) |= 1;
    }
    for (PlayerGauges& g : g_gauges) {
        if (g.hud != hud) continue;
        uint32_t lo = *reinterpret_cast<uint32_t*>(h + 8), hi = *reinterpret_cast<uint32_t*>(h + 0xC);
        NumPlayersOne one;
        if (!g.stateSet || lo != g.state[0] || hi != g.state[1]) {
            g.state[0] = lo, g.state[1] = hi, g.stateSet = true;
            for (void* e : g.elem)
                if (e) reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(FN_ELEMENT_SET_STATE)(e, lo & 0xFDFBFFFF, hi);
        }
        for (int i = 0; i < 4; ++i)
            if (g.elem[i]) reinterpret_cast<void(__thiscall*)(void*, void*)>(kGauges[i].update)(g.elem[i], player);
    }
}

static void __fastcall HudDtorHook(void* hud) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k)
        if (g_gauges[k].hud == hud) DestroyGauges(k);
    reinterpret_cast<void(__thiscall*)(void*)>(FN_HUD_DTOR_BODY)(hud);
}

// Custom gauge textures: the race load (0x577F60) loads GLOBAL\HUD_CustomTextures_<HUDINDEX>.bin for P1's car only
// and makes its 8 textures resident (names from 0x5ED9B0); the other players' packs are loaded the same way first.
static uint32_t g_hudTexList[MAX_LOCAL_PLAYERS][8];
static int g_hudTexIndex[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1};
static bool g_hudTexFile[MAX_LOCAL_PLAYERS];   // this player's entry loaded the pack file (unloads it)
static void __cdecl HudTexLoaded(void*) {}

static int CustomHudIndex(int k, uint8_t** spec, uint8_t** car) {
    auto* racer = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(FN_RACER_OF_PLAYER)(reinterpret_cast<void*>(0x89E7A0), k);
    if (!racer || *reinterpret_cast<int8_t*>(racer) > 0) return -1;
    uint8_t* s = racer + 0x10;
    auto* c = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(0x5B5350)(s + 0x10, 1);
    if (!c) return -1;
    void* part = reinterpret_cast<void*(__thiscall*)(void*, int)>(0x6102F0)(s, 0xA3);
    int idx = 0;
    if (part) {
        uint32_t key = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(0x43DB50)("HUDINDEX");
        idx = reinterpret_cast<int(__thiscall*)(void*, uint32_t, int)>(0x61B500)(part, key, 0);   // (key, 0)
    }
    *spec = s;
    *car = c + 0x1F0;
    return idx;
}

static void HudTexPath(char* out, size_t n, int idx) { sprintf_s(out, n, "GLOBAL\\HUD_CustomTextures_%02d.bin", idx); }

static void __fastcall HudTexLoadHook(void* self, void*, void* cb, void* ctx) {
    // the 1P drag tach uses the drag set of the custom textures (0x5ED9B0 picks it for drag with NUM_PLAYERS == 1)
    bool asOne = g_spGauges && SplitMode() && at<int>(NUM_PLAYERS) == 2 && IsDragRace();
    int savedPlayers = at<int>(NUM_PLAYERS);
    if (asOne) at<int>(NUM_PLAYERS) = 1;
    if (g_spGauges && SplitMode() && savedPlayers == 2) {
        int loaded[MAX_LOCAL_PLAYERS];
        int n = 0;
        uint8_t *spec, *car;
        loaded[n++] = CustomHudIndex(0, &spec, &car);    // the game's (P1)
        for (int k = 1; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
            int idx = CustomHudIndex(k, &spec, &car);
            Log("custom HUD index: player %d %d (player 1 %d)", k + 1, idx, loaded[0]);
            if (idx < 0) continue;
            // the pack file once per index; the texture list per player: which of the pack's textures a car uses
            // depends on the car too (user: P4's stock Celica tach had no numbers, P3's stock IS300 shared its index)
            bool fileLoaded = false;
            for (int i = 0; i < n; ++i) fileLoaded |= loaded[i] == idx;
            if (!fileLoaded) {
                char path[64];
                HudTexPath(path, sizeof(path), idx);
                reinterpret_cast<int(__cdecl*)(const char*, int, int, int)>(0x4944D0)(path, 0, 0, 0);
                reinterpret_cast<void(__cdecl*)(int)>(0x490170)(0);
            }
            g_hudTexFile[k] = !fileLoaded;
            memset(g_hudTexList[k], 0, sizeof(g_hudTexList[k]));
            reinterpret_cast<void(__cdecl*)(int, void*, int, uint32_t*, int)>(0x5ED9B0)(   // (spec, car parts, units, out, index)
                *reinterpret_cast<int*>(spec), car, at<uint8_t>(0x83AA1A), g_hudTexList[k], idx);
            reinterpret_cast<void(__cdecl*)(uint32_t*, int, void(__cdecl*)(void*), void*, int)>(0x494D60)(
                g_hudTexList[k], 8, &HudTexLoaded, nullptr, 0);
            g_hudTexIndex[k] = idx;
            loaded[n++] = idx;
            Log("custom HUD textures %02d %s for player %d", idx, fileLoaded ? "(pack already loaded)" : "loaded", k + 1);
        }
    }
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(FN_HUD_TEX_LOAD)(self, cb, ctx);
    if (asOne) at<int>(NUM_PLAYERS) = savedPlayers;
}

static void __fastcall HudTexUnloadHook(void* self) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {     // every list first: a pack file can be shared (also P1's)
        if (g_hudTexIndex[k] < 0) continue;
        reinterpret_cast<void(__cdecl*)(uint32_t*, int)>(0x490150)(g_hudTexList[k], 8);
        memset(g_hudTexList[k], 0, sizeof(g_hudTexList[k]));
    }
    reinterpret_cast<void(__thiscall*)(void*)>(FN_HUD_TEX_UNLOAD)(self);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        if (g_hudTexIndex[k] < 0) continue;
        if (g_hudTexFile[k]) {
            char path[64];
            HudTexPath(path, sizeof(path), g_hudTexIndex[k]);
            reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x494550)(path, 0, 0);
        }
        g_hudTexIndex[k] = -1;
        g_hudTexFile[k] = false;
    }
}

#define AT_FE_RENDER_OBJ_CALL 0x537262   // `call 0x5370A0` (render one FE object, stdcall (obj, arg))
#define FN_FE_RENDER_OBJ   0x5370A0
// 0x5370A0 also takes the render context in ECX (set by the caller), so the wrapper must preserve it.
static void __cdecl SetRenderSlot(void* obj) {
    int slot = -1;
    if (g_quadHud) {
        auto it = g_hudObjSlot.find(obj);
        if (it != g_hudObjSlot.end()) slot = it->second;
    }
    g_renderSlot = slot;
}

// Never inlined: the compiler can't see that this asm calls a function, and inlined it kept SSE values (the minimap
// centre and radius constants) in registers across the game's render call, which clobbers them -> after the first
// marker every NearMinimap check compared garbage (own car markers missing / markers flickering, user reports).
static __declspec(noinline) void __stdcall CallFeRender(void* obj, int arg, void* ctx) {
    __asm {
        mov ecx, ctx
        push arg
        push obj
        mov eax, FN_FE_RENDER_OBJ
        call eax
    }
}

static bool IsMarkerName(const uint8_t* obj) {
    uint32_t h = *reinterpret_cast<const uint32_t*>(obj + 0x10);
    for (uint32_t m : g_mmMarkerHash)
        if (h == m) return true;
    return false;
}

static void __stdcall RenderFeObjectWith(void* obj, int arg, void* ctx) {
    if (g_debug) { static int s_d = -1; int d = X87Depth(); if (d != s_d) { Log("x87 depth %d entering FE object render", d); s_d = d; } }
    // P3/P4's car markers (recoloured and unhidden for the per-player minimaps) are only drawn by the quad HUD; after the
    // race (Street X results switch to layout 1) they showed up at their raw positions (user: blue marker on results)
    if (!g_quadHud && obj && (obj == g_mmExtraIcon[0] || obj == g_mmExtraIcon[1])) return;
    if ((!g_gaugeShow.empty() || !g_gaugeHide.empty()) && obj) {
        static unsigned s_n, s_show, s_hide;
        if (g_debug && ++s_n % 20000 == 0) Log("gauge render: %u objs, %u shown, %u hidden", s_n, s_show, s_hide);
        if (g_gaugeHide.count(obj)) { ++s_hide; return; }
        auto it = g_gaugeShow.find(obj);
        if (it != g_gaugeShow.end()) ++s_show;
        if (it != g_gaugeShow.end()) {
            if (!SplitRacing() || !g_hudWiden) return;      // intro cameras, pause menus: no gauges
            g_gaugeSlot = it->second;
            g_renderSlot = -1;
            CallFeRender(obj, arg, ctx);
            g_gaugeSlot = -1;
            return;
        }
    }
    SetRenderSlot(obj);
    int mm = -1;
    if (g_renderSlot >= 0 && g_mmPlayers > 0)
        for (int i = 0; i < g_mmObjCount; ++i)
            if (g_mmObj[i] == obj) mm = i;
    // minimap markers that are not on a minimap right now (spare ones, the game parks them away from the map) are not
    // drawn: placed as ordinary HUD objects they landed at random spots in the quadrants (user report)
    if (mm < 0 && g_mmTarget < 0 && g_renderSlot >= 0 && g_mmPlayers > 0 && IsMarkerName(static_cast<uint8_t*>(obj))) {
        g_renderSlot = -1;
        return;
    }
    int markers = 2 + (g_extraPlayers < 2 ? g_extraPlayers : 2);
    bool marker = false;
    for (int j = 0; j < markers; ++j) marker |= obj == g_mmMarker[j];
    if (mm >= 0 && marker && g_mmTarget < 0) {
        // the car markers are drawn together, once per frame, when P1's package reaches one of them (P3's package is
        // drawn before P1's map, which would cover its markers); in each minimap its own player's marker goes last
        if (g_mmMarkersFrame != g_mmFrame && (obj == g_mmMarker[0] || obj == g_mmMarker[1])) {
            g_mmMarkersFrame = g_mmFrame;
            for (int k = 0; k < g_mmPlayers; ++k) {
                if (g_mmFinished[k]) continue;                 // finished: no minimap of its own
                ApplyMinimapSnapshot(k);
                g_mmTarget = k;
                for (int n = 1; n <= markers; ++n) {
                    uint8_t* m = g_mmMarker[(k + n) % markers];     // k last
                    if (!m) continue;
                    // off this player's map (too far away): the game parks it far from the map centre
                    auto* md = *reinterpret_cast<const float**>(m + 0x2C);
                    static unsigned s_mmLog;
                    if (g_debug && (s_mmLog++ % 600) < markers * g_mmPlayers)
                        Log("minimap %d: marker %d at %.3f,%.3f (%08X %08X) d %.3f %.3f flags %08X %s", k + 1,
                            (k + n) % markers + 1, md ? md[7] : 0.0f, md ? md[8] : 0.0f,
                            md ? reinterpret_cast<const uint32_t*>(md)[7] : 0, md ? reinterpret_cast<const uint32_t*>(md)[8] : 0,
                            md ? fabsf(md[7] - MINIMAP_X) : -1.0f, md ? fabsf(md[8] - MINIMAP_Y) : -1.0f,
                            *reinterpret_cast<uint32_t*>(m + 0x1C), md && NearMinimap(md) ? "drawn" : "skipped");
                    if (!md || !NearMinimap(md)) continue;
                    *reinterpret_cast<uint32_t*>(m + 0x1C) |= 0x400000;
                    g_renderSlot = 0;
                    CallFeRender(m, arg, ctx);
                    if (g_debug) CheckX87("after marker render");
                }
            }
            ApplyMinimapSnapshot(0);
            g_mmTarget = -1;
        }
    } else if (mm < 0 || g_mmTarget >= 0) {     // not a minimap object, or nested in one being drawn per player
        CallFeRender(obj, arg, ctx);
    } else {
        for (int k = 0; k < g_mmPlayers; ++k) {
            if (g_mmFinished[k]) continue;
            ApplyMinimapSnapshot(k);
            for (int i = 0; i < g_mmObjCount; ++i)          // "changed": recompute the cached transform
                *reinterpret_cast<uint32_t*>(g_mmObj[i] + 0x1C) |= 0x400000;
            if (!DrawsOnMinimap(static_cast<uint8_t*>(obj))) continue;
            g_mmTarget = k;
            g_renderSlot = 0;
            CallFeRender(obj, arg, ctx);
        }
        ApplyMinimapSnapshot(0);
        g_mmTarget = -1;
    }
    g_renderSlot = -1;
}

static __declspec(naked) void RenderFeObject() {
    __asm {
        push ecx                         // render context
        push dword ptr [esp + 0xC]       // arg
        push dword ptr [esp + 0xC]       // obj
        call RenderFeObjectWith
        ret 8
    }
}

#define AT_MINIMAP_UPDATE_CALL 0x4DA650  // `call 0x4CA840` (minimap manager update, thiscall, no args)
#define FN_MINIMAP_UPDATE  0x4CA840
#define AT_MINIMAP_CENTRE_ID 0x4CA901    // imm8 of `push 0`: id of the car entity the map centres on
#define MARKER_POOL        0x836560      // list of free marker descriptors {obj, type} (filled by 0x4DB900(fng))
#define MARKER_ALLOCATOR   0x8364B4      // fixed allocator of the descriptors (0x66 x 8 bytes, made at 0x4AC6D0)
#define AT_MARKER_POOL_SIZE 0x4AC6D8     // imm8 of `push 0x66`
#define FN_POOL_ALLOC      0x43F8F0      // void* __cdecl (allocator)
#define FN_LIST_NODE       0x440EC0      // node* (): list node, payload at +8
#define FN_MINIMAP_ADD      0x4EC790     // entity* __thiscall (manager; type, id): takes a pool marker of that type
static void* g_mmExtendedMgr = nullptr;
