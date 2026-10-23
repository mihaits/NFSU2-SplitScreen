// Part of NFSU2SplitScreen.cpp: race intro: scene choice, grid order, extra cars.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Race intro (NIS) selection 0x436840: context 0x431F10 (+0 racers, +4 humans, +8 humans' grid-slot mask, +0xC AI
// mask, +0x14 track byte) filtered against each scene (0x42FE00: slot mask, racer range +6..+7, ...); with exactly 2
// local players the 2P list (0x42FDA0) is used, otherwise the single-player one. Scenes exist for 2 and 4 cars, so
// 3 players got none (and the race started without an intro), nor did 5-6 cars with AI opponents (no intro, no grid
// girls); both are offered the 4-car scenes.
#define AT_INTRO_CONTEXT_CALL 0x4368F4
#define FN_INTRO_CONTEXT   0x431F10      // void __thiscall (ctx; trackId)
// With exactly 2 local players only IntroNis12 / IntroNis13 pass the 2P filter 0x42FDA0 (at 0x43695F); tracks whose
// eligible scenes include neither got no intro, and a 2P race that starts without an intro never switches to the
// world render (black screen with the HUD; user: Freeway West, reproduced on track 4021 with the filter forced off).
// When the 2P list comes out empty (0x4369FE) the scene loop runs again with the filter passing every scene, i.e. the
// scenes single player would pick from.
#define AT_INTRO_2P_FILTER   0x43695F    // call 0x42FDA0 (thiscall ctx; scene) -> bool
#define FN_INTRO_2P_FILTER   0x42FDA0
#define AT_INTRO_EMPTY       0x4369FE    // mov ecx,[ebp+18h]; xor edi,edi  (no scene)
#define AT_INTRO_EMPTY_RET   0x436A03
#define AT_INTRO_LOOP        0x4368F9    // list init + scene loop (edi = 0)
static bool g_intro2PFallback = false;
static bool g_introHideExtra = false;    // 5-6 cars got a 4-car scene: the extra cars are hidden during it
static bool __fastcall Intro2PFilter(void* ctx, void*, void* scene) {
    return g_intro2PFallback || reinterpret_cast<bool(__thiscall*)(void*, void*)>(FN_INTRO_2P_FILTER)(ctx, scene);
}
static int __cdecl IntroRetryWanted() {
    if (g_intro2PFallback || GetLocalPlayer(1) == nullptr || GetLocalPlayer(2) != nullptr) return 0;   // 2 players
    g_intro2PFallback = true;
    Log("intro: no 2-player scene for this track, using the single-player scenes");
    return 1;
}
static __declspec(naked) void IntroEmptyHook() {
    __asm {
        pushad
        call IntroRetryWanted
        test eax, eax
        popad
        jz none
        xor edi, edi
        push AT_INTRO_LOOP
        ret
    none:
        mov ecx, dword ptr [ebp + 0x18]
        xor edi, edi
        push AT_INTRO_EMPTY_RET
        ret
    }
}
static void __fastcall IntroContext(int* ctx, void*, int track) {
    g_intro2PFallback = false;
    g_introHideExtra = false;            // (also when the previous race's scene never started)
    reinterpret_cast<void(__thiscall*)(int*, int)>(FN_INTRO_CONTEXT)(ctx, track);
    Log("intro context: racers %d humans %d slots %X ai %X track byte %d (sound fs: %d free handles)", ctx[0], ctx[1],
        ctx[2], ctx[3], ctx[5], SoundFsFreeHandles());
    // 3 or 5-6 cars (AI opponents): the 4-car scenes (they animate their own 4 grid slots; cars in slots 5/6 stay on
    // the grid; a human in slot 5/6 fails the scenes' slot mask -> no intro, as before)
    if (SplitMode() && (ctx[0] == 3 || ctx[0] > 4)) {
        if (ctx[0] > 4) Log("intro: %d cars, offered the 4-car scenes", ctx[0]), g_introHideExtra = true;
        ctx[0] = 4;
    }
}

// Grid order: 0x5264A0(order) numbers the racer specs' grid slots (+3), order 1 = humans (spec +4 == 1) then AI (2),
// order 2 = AI first (the URL, drift and Street X starters). With 5-6 cars the humans then stood in slots 4-6, outside
// every intro scene's slots (4-car scenes: slots 1-4) -> no intro (user). Split races with more than 4 cars put the
// humans in front.
#define FN_ASSIGN_GRID     0x5264A0      // void __cdecl (int order): 6 bytes stolen
using AssignGridFn = void(__cdecl*)(int);
static AssignGridFn g_origAssignGrid;
static void __cdecl AssignGrid(int order) {
    if (order == 2 && SplitMode() && at<int>(NUM_PLAYERS) == 2) {
        auto* specs = at<uint8_t*>(0x89E810);
        int n = at<int>(0x89E814), cars = 0;
        for (int i = 0; specs && i < n; ++i) {
            int type = *reinterpret_cast<int*>(specs + i * 0x940 + 4);
            if (type == 1 || type == 2) ++cars;
        }
        if (cars > 4) {
            Log("grid: %d cars, humans in front", cars);
            order = 1;
        }
    }
    g_origAssignGrid(order);
}

// The intro scene also places its cars itself: its binder (0x439240, run at the scene's start and again on restarts,
// which skip the scene) teleports each bound car (0x4309D0 -> 0x5FBD60) to its track's spot, a 2x2 formation, and
// records it in the binding table (0x827B98 + track*0x14: +0 track, +4 x, +8 y, +0x10 car array index, -1 = none). On a track whose
// race grid is 4 abreast (drift 4311) the formation's second row is grid spots 5/6, where the extra cars stand: they
// started inside the scene's cars (user). An extra car that overlaps a bound car goes one row behind the formation.
#define FN_NIS_BIND_CARS   0x439240      // void __thiscall (scene; int)
static const uintptr_t kNisBindCarsCalls[] = {0x43AC10, 0x43AFDF, 0x43B196};
#define NIS_BINDINGS       0x827B98
#define FN_CAR_TELEPORT    0x5FBD60      // void __thiscall (car; const float* pos, 0, int heading, bool)
#define FN_HEADING_OF      0x43E6D0      // int __cdecl (float dx, float dy)
static void MoveExtraCarsBehindScene() {
    auto* list = at<uint8_t*>(0x890118);
    auto* cars = at<uint8_t*>(RACE_CARS);
    if (!list || !cars) return;
    int n = *reinterpret_cast<int*>(list + 0x24);
    auto car = [cars](int i) { return reinterpret_cast<uint8_t**>(cars + 0x1C)[i]; };
    float spot[4][2];
    bool bound[16] = {};
    for (int k = 0; k < 4; ++k) {
        auto* e = reinterpret_cast<uint8_t*>(NIS_BINDINGS + k * 0x14);
        int idx = *reinterpret_cast<int*>(e + 0x10);
        if (!*reinterpret_cast<void**>(e) || idx < 0 || idx >= n || idx >= 16) return;   // not a full 4-car scene
        bound[idx] = true;
        spot[k][0] = *reinterpret_cast<float*>(e + 4);
        spot[k][1] = *reinterpret_cast<float*>(e + 8);
    }
    // rows: tracks 0/1 in front, 2/3 behind
    float stepX = (spot[2][0] + spot[3][0] - spot[0][0] - spot[1][0]) / 2;
    float stepY = (spot[2][1] + spot[3][1] - spot[0][1] - spot[1][1]) / 2;
    if (stepX * stepX + stepY * stepY < 4.0f) return;
    int heading = reinterpret_cast<int(__cdecl*)(float, float)>(FN_HEADING_OF)(-stepX, -stepY);
    for (int i = 0; i < n && i < 16; ++i) {
        uint8_t* c = car(i);
        auto* spec = c ? *reinterpret_cast<uint8_t**>(c + 0x14) : nullptr;
        if (bound[i] || !spec || (*reinterpret_cast<int*>(spec + 4) != 1 && *reinterpret_cast<int*>(spec + 4) != 2)) continue;
        const float* p = reinterpret_cast<const float*>(c + 0x60);
        for (int k = 0; k < 4; ++k) {
            float dx = p[0] - spot[k][0], dy = p[1] - spot[k][1];
            if (dx * dx + dy * dy > 3.0f * 3.0f) continue;
            float to[3] = {spot[k][0] + stepX, spot[k][1] + stepY, p[2]};
            reinterpret_cast<void(__thiscall*)(void*, const float*, int, int, int)>(FN_CAR_TELEPORT)(c, to, 0, heading, 0);
            Log("intro: car %d stood on the scene's spot %d, moved a row back to %.1f, %.1f", i, k + 1, to[0], to[1]);
            break;
        }
    }
}
static void __fastcall NisBindCars(void* scene, void*, int arg) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_NIS_BIND_CARS)(scene, arg);
    if (SplitMode() && at<int>(NUM_PLAYERS) == 2) MoveExtraCarsBehindScene();
}

// A 4-car scene drives its 4 cars onto the grid through the extra cars already standing in grid slots 5/6 (user). Those
// are hidden while the scene runs: a car the scene doesn't drive (mover car+0x28 != 5, NISMvr) reports itself not
// visible to the per-view car check 0x60D4F0 (car body 0x6317A2, lights 0x60D594, headlight pools 0x6154D3), until
// the scene has released its cars; they appear on their grid spots when the race view starts.
#define FN_CAR_NOT_VISIBLE 0x60D4F0      // int __cdecl (car, view): nonzero = skip the car in this view (5 bytes stolen)
using CarNotVisibleFn = int(__cdecl*)(uint8_t*, void*);
static CarNotVisibleFn g_origCarNotVisible;
static bool g_introSceneSeen = false;    // a car was driven by the intro scene (g_introHideExtra set)
static int __cdecl CarNotVisible(uint8_t* car, void* view) {
    if (g_introHideExtra && g_introSceneSeen && car && *reinterpret_cast<int*>(car + 0x28) != MOVER_NIS) return 1;
    return g_origCarNotVisible(car, view);
}
static void UpdateIntroHiding() {        // per frame
    if (!g_introHideExtra) return;
    bool scene = false;
    if (uint8_t* mgr = at<uint8_t*>(RACE_CARS))
        for (auto* c = *reinterpret_cast<uint8_t**>(mgr); c && c != mgr; c = *reinterpret_cast<uint8_t**>(c))
            if (*reinterpret_cast<int*>(c + 0x28) == MOVER_NIS) scene = true;
    if (scene && !g_introSceneSeen) Log("intro: extra cars hidden during the scene");
    if (scene) g_introSceneSeen = true;
    else if (g_introSceneSeen || !SplitMode()) {
        if (g_introSceneSeen) Log("intro: scene over, extra cars shown");
        g_introHideExtra = g_introSceneSeen = false;
    }
}

// Debug: scene picks. The scene picker 0x436840(kind, ...) draws each scene with the game's random 0x43C1C0(count)
// (cdecl, 0..count-1 over the filtered list), one call per kind: 0 race intros (0x436A26), 1 post-race outros
// EndingNis01-04 (0x436BC5), 2 (0x436CDF) and 4 (0x4370E3) the other kinds. With Debug = 1 every pick is logged, and
// [DEBUG] ForceIntro / ForceOutro = list index (-1 = random) override kinds 0 / 1, to reproduce scene-specific bugs
// (the black post-race world needed one 2-car outro). The random number is still drawn: the game's sequence is kept.
#define FN_SCENE_RANDOM 0x43C1C0
static const uintptr_t kScenePickSites[4] = {0x436A26, 0x436BC5, 0x436CDF, 0x4370E3};
static const int kScenePickKind[4] = {0, 1, 2, 4};
static int g_forceScene[4] = {-1, -1, -1, -1};
static unsigned ScenePick(int site, unsigned count) {
    unsigned pick = reinterpret_cast<unsigned(__cdecl*)(unsigned)>(FN_SCENE_RANDOM)(count);
    int forced = g_forceScene[site];
    if (forced >= 0 && unsigned(forced) < count) pick = forced;
    Log("scene pick: kind %d, %u candidates -> %u%s", kScenePickKind[site], count, pick,
        forced >= 0 ? (unsigned(forced) < count ? " (forced)" : " (forced index out of range)") : "");
    return pick;
}
template <int Site> static unsigned __cdecl ScenePickAt(unsigned count) { return ScenePick(Site, count); }
static void InstallScenePicks() {
    if (!g_debug) return;
    g_forceScene[0] = GetPrivateProfileIntA("DEBUG", "ForceIntro", -1, g_iniPath);
    g_forceScene[1] = GetPrivateProfileIntA("DEBUG", "ForceOutro", -1, g_iniPath);
    const void* hooks[4] = {&ScenePickAt<0>, &ScenePickAt<1>, &ScenePickAt<2>, &ScenePickAt<3>};
    for (int i = 0; i < 4; ++i) {
        uint8_t* at = reinterpret_cast<uint8_t*>(kScenePickSites[i]);
        if (at[0] != 0xE8 || kScenePickSites[i] + 5 + *reinterpret_cast<int32_t*>(at + 1) != FN_SCENE_RANDOM) {
            Log("scene pick: site %p not the expected call, not hooked", at);
            continue;
        }
        WriteCall(kScenePickSites[i], hooks[i]);
    }
    Log("scene picks logged; forced intro %d, outro %d", g_forceScene[0], g_forceScene[1]);
}
