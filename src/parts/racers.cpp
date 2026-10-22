// Part of NFSU2SplitScreen.cpp: particles, extra local players, standings, results, post-race scene, names, skins, AI cars.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- particles near any player
// The particle emitter update 0x491E50 only spawns when the emitter (+0x70 pos) is within its range (effect +0x80)
// of one of the 3 camera positions the particle system snapshots each frame (0x48BD90: views 1, 2, 0 at system
// +0x10/+0x20/+0x30). P3/P4's cameras (views 5/6) were missing -> their tyre smoke and nitrous flames stopped
// when far from P1/P2. The range test now also checks the extra players' cameras.
#define AT_EMITTER_RANGE      0x491E8F   // mov eax,[esi+0x80] (6 bytes); the 3-camera loop follows
#define AT_EMITTER_RANGE_DONE 0x491EE4   // edi = in range
static int __cdecl EmitterNearCamera(const uint8_t* system, const float* pos, float range) {
    if (!(range > 0)) return 0;
    auto inRange = [&](const float* c) {
        float dx = pos[0] - c[0], dy = pos[1] - c[1], dz = pos[2] - c[2];
        return dx * dx + dy * dy + dz * dz < range * range;
    };
    for (int i = 0; i < 3; ++i)
        if (inRange(reinterpret_cast<const float*>(system + 0x10 + i * 0x10))) return 1;
    if (SplitRacing())
        for (int k = 2; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
            const View& v = GetView(kPlayerViewId[k]);
            if (v.active && v.camera && inRange(reinterpret_cast<const float*>(v.camera + 0x40))) return 1;
        }
    return 0;
}
// "Distance to the nearest racer" 0x5F1160 (race manager [0x890080], racer positions at +0x80 + i*0x10, count +0x14)
// measures racers 0/1 only unless some mode flags (0x89CF69, 0x8669F4, 0x866A14) are set. Callers use it for
// "is this near a player": car effects (0x60A1AB: no tyre smoke / nitrous flames beyond 150 m), the per-car
// update 0x60A0EB, sound 0x46A262, ... -> P3/P4 counted as far from everyone. With extra players: all racers.
#define FN_NEAREST_RACER_DIST 0x5F1160   // float __thiscall (manager; const float* pos)
typedef float(__thiscall* NearestRacerDistFn)(void*, const float*);
static NearestRacerDistFn g_origNearestRacerDist;
static float __fastcall NearestRacerDistance(uint8_t* mgr, void*, const float* pos) {
    if (!SplitRacing() || g_extraPlayers <= 0) return g_origNearestRacerDist(mgr, pos);
    int n = *reinterpret_cast<int*>(mgr + 0x14);
    if (n > 6) n = 6;
    float best = 3.0e38f;
    for (int i = 0; i < n || i < 2; ++i) {
        const float* p = reinterpret_cast<const float*>(mgr + 0x80 + i * 0x10);
        float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

static __declspec(naked) void EmitterRangeHook() {
    __asm {
        mov eax, dword ptr [esi + 0x80]
        mov dword ptr [esp + 0x20], eax
        pushad
        push eax                         // range
        lea edx, [ebx + 0x70]
        push edx                         // emitter position
        push ecx                         // particle system
        call EmitterNearCamera
        add esp, 12
        mov dword ptr [esp], eax         // popad restores it into edi
        popad
        push AT_EMITTER_RANGE_DONE
        ret
    }
}

// ---------------------------------------------------------------- extra local players (3, 4)
// The race runs as a 2-player race (all the game's "== 2 players" logic stays on its 2P paths); players 3 and 4 are
// added next to it: human racers in race setup, player objects at race start, views 5/6 with their own cameras.
#define FN_CREATE_HUMAN_RACERS 0x525ED0  // void __cdecl (count, p2, p3): human racer specs for players 0..count-1
#define FN_RACER_SPEC_CTOR 0x6373E0      // __thiscall (spec; playerIndex, human, carRecord, profile, -2, 0)
#define AT_RACER_SPEC_CALL 0x52602A      // the ctor call inside the per-player loop
#define FN_CREATE_PLAYERS  0x5F4250      // race start: local player objects for the human racers
#define AT_CREATE_PLAYERS_CALL 0x60A798
#define FN_FIND_RACER      0x5ED920      // racer* __thiscall (raceParams; playerIndex)
#define RACE_PARAMS        0x89E7A0
#define FN_PLAYER_CTOR     0x5F1670      // __thiscall (player(0x9C0); port, index)
#define FN_PLAYER_ATTACH   0x5F3E10      // __thiscall (player; car)
#define RACE_CARS          0x890080      // list of race cars; car+0xA (short) = player index of its racer
#define FN_LIST_END        0x502FD0
#define FN_CAMERA_CTOR     0x44F5A0      // __thiscall (camera), static cameras are 0x320 apart
#define AT_HUMAN_MANAGER_CAP 0x4856C8    // imm8 of `push 2`: capacity of the per-human-racer component manager
static const uintptr_t kCreateHumanRacersCalls[] = {0x53F2B5, 0x53F800, 0x53FA03, 0x52694E, 0x53F4E3, 0x53FB8B,
                                                    0x526814, 0x53F656};
static bool g_extraRacerPass = false;
static void* g_extraCamera[MAX_LOCAL_PLAYERS];

static void PrepareExtraNames();
#define AI_COUNT           0x89E7C4      // AI racers; the race starters set it to 0 with 2 local players and create
                                         // the AI (0x53EE90(count)) right after the human racers
static void __cdecl CreateHumanRacers(int count, int p2, int p3) {
    using Fn = void(__cdecl*)(int, int, int);
    // Free Run's starter (0x53F580) sets 1 local player and creates 1 human: a split-screen Free Run gets 2, like the
    // other starters, and players 3/4 below
    if (count == 1 && SplitMode() && at<int>(0x83AAB4) == 9) {
        at<int>(NUM_PLAYERS) = 2;
        count = 2;
        Log("free run: %d players", 2 + g_extraPlayers);
    }
    if (count == 2 && SplitMode()) {
        at<int>(AI_COUNT) = SplitOpponents();
        if (SplitOpponents()) Log("race: %d AI opponents", SplitOpponents());
    }
    reinterpret_cast<Fn>(FN_CREATE_HUMAN_RACERS)(count, p2, p3);
    int extra = g_extraPlayers < 2 ? g_extraPlayers : 2;
    if (count != 2 || extra <= 0) return;
    // second pass over players 0..extra-1 with the spec's player index shifted by 2 (RacerSpecIndexShift):
    // same profile, car and port as players 1/2
    int ownCar[2] = {at<int>(CAR_IDS), at<int>(CAR_IDS + 4)};
    for (int k = 0; k < extra; ++k)
        if (g_seatCar[2 + k]) at<int>(CAR_IDS + k * 4) = g_seatCar[2 + k];   // picked on the car select
    Log("extra racers: cars %08X %08X (players 1/2: %08X %08X)", at<int>(CAR_IDS), at<int>(CAR_IDS + 4), ownCar[0],
        ownCar[1]);
    Log("race: type %d (0x83AAB4), track %d (0x89E7A0), qr type %d (0x850088), racers %d", at<int>(0x83AAB4),
        at<int>(RACE_PARAMS), at<int>(0x850088), at<int>(RACE_PARAMS + 0x74));
    PrepareExtraNames();
    g_extraRacerPass = true;
    reinterpret_cast<Fn>(FN_CREATE_HUMAN_RACERS)(extra, p2, p3);
    g_extraRacerPass = false;
    at<int>(CAR_IDS) = ownCar[0];
    at<int>(CAR_IDS + 4) = ownCar[1];
    Log("extra human racers added: %d (racers now %d)", extra, at<int>(RACE_PARAMS + 0x74));
}

// Race standings (0x56D850, rebuilt during the race; drift scoring at 0x56DDD0 looks every racer up in it and
// crashes on a missing one) are built for [0x89E7C0] + [0x89E7C8] racers: the extra human racers are counted in for
// the call.
#define FN_BUILD_STANDINGS 0x56D850      // void __cdecl (int)
#define RACE_COUNT_B       0x89E7C8      // racers counted besides the local players
static const uintptr_t kBuildStandingsCalls[] = {0x5F7B3F, 0x5F94C7, 0x6052F8};
static void __cdecl BuildStandings(int arg) {
    int counted = at<int>(NUM_PLAYERS) + at<int>(RACE_COUNT_B);
    int missing = at<int>(RACE_PARAMS + 0x74) - counted;
    if (missing < 0 || !SplitMode() || g_extraPlayers <= 0) missing = 0;
    // only the extra human players: the street drift (touge) tracks 4174-4179 add non-human racers (ids 50+) to the
    // race count, and counting them overran the standings' static entry array 0x864640 (broken list -> crash in the
    // drift scoring sort 0x56DDD0 / later on the way back to the menus)
    if (missing > g_extraPlayers) missing = g_extraPlayers;
    at<int>(RACE_COUNT_B) += missing;
    reinterpret_cast<void(__cdecl*)(int)>(FN_BUILD_STANDINGS)(arg);
    at<int>(RACE_COUNT_B) -= missing;
}

// Race results (0x54CCA0 at 0x551447, thiscall (results; race)): one entry per racer, but the pointer list the results
// screens read (results +0xCD8, 6 slots; e.g. 0x430960 loops the racer count) is filled for [0x89E7C0..CC] racers
// only -> P3/P4's slots stayed null and the results screen crashed (0x430987). Extra racers counted in for the call.
#define AT_RACE_RESULTS_CALL 0x551447
#define FN_RACE_RESULTS      0x54CCA0
// The post-race screens (results rows 0x4BF0A0 and others, stats, back to results) size their lists with the same sum
// -> the extra racers stay counted from the results until the race is left: split race over, a restart (CreatePlayers),
// or right before the race setup paths that read the counters (0x5260B0 at 0x5324F2, 0x526970 at 0x530A91 / 0x581902; the quick race
// menus rewrite them anyway, 0x526720).
static int g_resultsExtra = 0;
static int g_resultsCountB = 0;          // the counter as set by RaceResults
// only while the counter still holds the value set for the results: a new race's setup writes it afresh (user: after
// going back to the menus, the next race's counter was uncounted to -1 -> crash in the drift scoring 0x56DE6E)
static void UncountResultsRacers() {
    if (!g_resultsExtra) return;
    if (at<int>(RACE_COUNT_B) == g_resultsCountB) {
        at<int>(RACE_COUNT_B) -= g_resultsExtra;
        Log("results: extra racers uncounted (%d)", g_resultsExtra);
    } else {
        Log("results: extra racers already reset by the race setup");
    }
    g_resultsExtra = 0;
}
// The results list holds one entry per race car, but its size comes from the counters. A split race once ended with
// an AI racer that never got a car (3 AI specs, 5 cars; cause unknown, not reproduced) and the results rows read past
// the list (crash 0x4BF2D2). The AI counter is capped at the AI cars that exist (race cars: count [[0x890118]+0x24],
// car i at [[0x890080]+0x1C+4i], spec car+0x14, spec+4 == 2 for AI).
static void CapAiCountToCars() {
    auto* list = at<uint8_t*>(0x890118);
    auto* cars = at<uint8_t*>(0x890080);
    if (!list || !cars) return;
    int n = *reinterpret_cast<int*>(list + 0x24), ai = 0;
    for (int i = 0; i < n; ++i) {
        auto* car = reinterpret_cast<uint8_t**>(cars + 0x1C)[i];
        auto* spec = car ? *reinterpret_cast<uint8_t**>(car + 0x14) : nullptr;
        if (spec && *reinterpret_cast<int*>(spec + 4) == 2) ++ai;
    }
    if (at<int>(AI_COUNT) > ai) {
        Log("results: %d AI counted but %d AI cars (of %d cars) - counter capped", at<int>(AI_COUNT), ai, n);
        at<int>(AI_COUNT) = ai;
    }
}
static bool g_postRace = false;          // results computed until the next race's players (CreatePlayers)
static void __fastcall RaceResults(void* results, void*, void* race) {
    g_postRace = true;
    UncountResultsRacers();
    if (SplitMode()) CapAiCountToCars();
    int counted = at<int>(0x89E7C0) + at<int>(0x89E7C4) + at<int>(0x89E7C8) + at<int>(0x89E7CC);
    int missing = at<int>(RACE_PARAMS + 0x74) - counted;
    if (missing < 0 || !SplitMode() || g_extraPlayers <= 0) missing = 0;    // only P3/P4 are missing from the counts
    if (missing > g_extraPlayers) missing = g_extraPlayers;                 // (touge drift: non-human racers too)
    Log("results: racers %d, counted %d (%d %d %d %d), adding %d", at<int>(RACE_PARAMS + 0x74), counted,
        at<int>(0x89E7C0), at<int>(0x89E7C4), at<int>(0x89E7C8), at<int>(0x89E7CC), missing);
    at<int>(RACE_COUNT_B) += missing;
    g_resultsExtra = missing;
    g_resultsCountB = at<int>(RACE_COUNT_B);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_RACE_RESULTS)(results, race);
}
// A human car the post-race scene doesn't feature is driven to the world origin and falls there forever (no ground):
// its own sounds play to its player's listener wherever it is, so the fall's wind rush was heard (user, P1 last). It is
// held still under the map: physics body = [[car+0x34] (mover) + 0x20], position z +0x28 (and +0x108), velocity z +0xA8.
#define MOVER_NIS          5        // car mover mode (car+0x28): driven by a scene (NISMvr)
static void HoldSceneParkedCars() {         // per frame
    if (!g_postRace || !SplitMode() || at<int>(NUM_PLAYERS) != 2 || SplitRacing()) return;
    auto* cars = at<uint8_t*>(RACE_CARS);
    auto* list = at<uint8_t*>(0x890118);
    if (!cars || !list) return;
    int n = *reinterpret_cast<int*>(list + 0x24);
    for (int i = 0; i < n; ++i) {
        auto* car = reinterpret_cast<uint8_t**>(cars + 0x1C)[i];
        auto* spec = car ? *reinterpret_cast<uint8_t**>(car + 0x14) : nullptr;
        if (!spec || *reinterpret_cast<int*>(spec + 4) != 1 || *reinterpret_cast<int*>(car + 0x28) != MOVER_NIS) continue;
        const float* p = reinterpret_cast<const float*>(car + 0x60);
        auto* mover = *reinterpret_cast<uint8_t**>(car + 0x34);
        auto* body = mover ? *reinterpret_cast<uint8_t**>(mover + 0x20) : nullptr;
        if (!body || fabsf(p[0]) > 1.0f || fabsf(p[1]) > 1.0f) continue;
        *reinterpret_cast<float*>(body + 0x28) = -50.0f;
        *reinterpret_cast<float*>(body + 0x108) = -50.0f;
        *reinterpret_cast<float*>(body + 0xA8) = 0.0f;
    }
}
static void EndResultsCount() {
    HoldSceneParkedCars();
    if (g_resultsExtra && !SplitMode()) UncountResultsRacers();   // not SplitRacing: Street X results use layout 1
}

// Post-race scene (NIS): its car tracks are bound to the race cars by car array index (table 0x827B9C, 0x14 a track,
// +0xC car index; 0x4309D0 sets mover mode 5 = NISMvr) and the cars it doesn't feature are held at the world origin.
// With 3-4 players + AI, array entries 2/3 (P3, the first AI) are such cars: harmless for AI, but a human car gets
// the fall-through check (0x5ECBA0: below the ground -> +0x96C) and is reset onto the road every ~0.5 s (0x6009A0 via
// 0x601324), each reset switching its mover 5 -> 0 -> 5 = a screen flash and an audio duck (user: rhythmic sound gate
// with flashing on the post-race menu). A car driven by the NIS is not reset.
#define AT_FALL_RESET_CALL 0x601324      // call 0x6009A0 (thiscall [car+0x18]); esi = car, mover mode at car+0x28
#define FN_FALL_RESET      0x6009A0
static __declspec(naked) void FallResetHook() {
    __asm {
        cmp dword ptr [esi + 0x28], MOVER_NIS
        je skip
        mov eax, FN_FALL_RESET
        jmp eax
    skip:
        ret
    }
}
static const uintptr_t kRaceSetupCalls[][2] = {{0x5324F2, 0x5260B0}, {0x530A91, 0x526970}, {0x581902, 0x526970}};
static int __cdecl RaceSetup5260B0(int a) { UncountResultsRacers(); return reinterpret_cast<int(__cdecl*)(int)>(0x5260B0)(a); }
static int __cdecl RaceSetup526970(int a) { UncountResultsRacers(); return reinterpret_cast<int(__cdecl*)(int)>(0x526970)(a); }

// Racer names come from the per-player block (0x83A9E0 + index*0xAB4C, name at +0): players 3/4 reused P1's / P2's
// (the same name twice on the results). They get P2's name with its digit swapped ("Player 2" -> "Player 3").
#define PLAYER_BLOCKS      0x83A9E0
#define PLAYER_BLOCK_SIZE  0xAB4C
static void PrepareExtraNames() {
    const char* p2 = reinterpret_cast<const char*>(PLAYER_BLOCKS + PLAYER_BLOCK_SIZE);
    for (int k = 0; k < 2; ++k) {
        char* n = g_extraNames[k];
        strncpy_s(n, 16, p2, _TRUNCATE);
        size_t len = strlen(n);
        if (len && n[len - 1] == '2') n[len - 1] = static_cast<char>('3' + k);
        else sprintf_s(n, 16, "Player %d", 3 + k);
    }
}
// Car skins: a car's paint and vinyls are composited into the texture DUMMY_SKIN<n> named by its ride info (+0x584,
// set by 0x61C280(ride; n)); the garage entries pick n from the player index (0x503A80: index + 1, 0x503880: 1 or 2),
// so players 3/4 got P1's / P2's skin texture and rendered with mismatched skin textures (user: P3's
// stock 3000GT dark with stray textures, e.g. a door card texture on the hood). Their ride infos get DUMMY_SKIN3/4 (the single-player opponents' slots; split races have no
// AI) before the racer spec copies them. Free Run (race type 9) sets skin 1 for every human (0x526007; a single player)
// - skipped in split screen (FreeRunSkinHook), every player keeps their own (user: P3's paint broken in Free Run).
#define FN_RIDE_SET_SKIN   0x61C280      // void __thiscall (RideInfo*; int n)
static void __cdecl ExtraRacerSkin(void* ride, int playerIndex) {
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_RIDE_SET_SKIN)(ride, playerIndex + 1);
}
#define AT_FREE_RUN_SKIN   0x525FFE      // cmp dword ptr [0x83AAB4], 9 (7 bytes); jne 0x526012
#define FREE_RUN_SKIN_SET  0x526007
#define FREE_RUN_SKIN_SKIP 0x526012
static __declspec(naked) void FreeRunSkinHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        cmp dword ptr ds:[0x83AAB4], 9
        jne skip
        push FREE_RUN_SKIN_SET
        ret
    skip:
        push FREE_RUN_SKIN_SKIP
        ret
    }
}
// AI cars are picked to match a reference car (0x4FEAF0: the first car type in a shuffled list whose stock performance
// 0x5B6990(ride + 0x10; 1, -1.0) is within 3% (doubling) of the reference's): racer 0's, player 1's (0x53EE90 at
// 0x53EEE9: 0x5ED900(race; 0) + 0x10). In split races it is the fastest human car.
#define AT_AI_REFERENCE_CALL 0x53EEE9    // call 0x5ED900 (thiscall race; racer index) -> spec (+0x10 ride)
#define FN_RACER_SPEC        0x5ED900
#define FN_CAR_PERFORMANCE   0x5B6990    // float __thiscall (ride + 0x10; 1, -1.0)
static uint8_t* __fastcall AiReferenceRacer(void* race, void*, int index) {
    using SpecFn = uint8_t*(__thiscall*)(void*, int);
    uint8_t* best = reinterpret_cast<SpecFn>(FN_RACER_SPEC)(race, index);
    if (index != 0 || !SplitMode() || at<int>(NUM_PLAYERS) != 2) return best;
    float bestRating = -1e9f;
    for (int k = 0; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
        uint8_t* spec = reinterpret_cast<SpecFn>(FN_RACER_SPEC)(race, k);
        if (!spec || *reinterpret_cast<int*>(spec + 4) != 1) continue;          // humans only
        float rating = reinterpret_cast<float(__thiscall*)(void*, int, float)>(FN_CAR_PERFORMANCE)(spec + 0x20, 1, -1.0f);
        Log("AI reference: racer %d car rating %.3f", k, rating);
        if (rating > bestRating) bestRating = rating, best = spec;
    }
    return best;
}

// AI racers' cars (0x4FEAF0(out, 100 + i, 2 + i, ...), from 0x53EE90) get DUMMY_SKIN<2 + i> (call at 0x4FEB51): after
// the one human of single player. In split races that is player 2's (and 3/4's) skin: they start after the humans.
#define AT_AI_SET_SKIN     0x4FEB51
static void __fastcall AiSetSkin(void* ride, void*, int n) {
    if (SplitMode() && at<int>(NUM_PLAYERS) == 2) {
        n += 2 + g_extraPlayers - 1;     // 2 + i -> humans + 1 + i
        Log("AI car: DUMMY_SKIN%d", n);
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_RIDE_SET_SKIN)(ride, n);
}
static __declspec(naked) void RacerSpecIndexShift() {
    __asm {
        cmp g_extraRacerPass, 0
        je ctor
        add dword ptr [esp + 4], 2       // playerIndex (first stack argument)
        mov eax, dword ptr [esp + 4]
        and eax, 1
        shl eax, 4
        add eax, offset g_extraNames
        mov dword ptr [esp + 0x10], eax  // name (4th argument)
        push ecx
        push dword ptr [esp + 8]         // playerIndex
        push dword ptr [esp + 0x14]      // ride info (3rd argument)
        call ExtraRacerSkin
        add esp, 8
        pop ecx
    ctor:
        mov eax, FN_RACER_SPEC_CTOR
        jmp eax
    }
}
