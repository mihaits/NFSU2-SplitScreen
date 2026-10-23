// Part of NFSU2SplitScreen.cpp: "Hurry!" timer, catch-up.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- "Hurry!" timer with 3-4 players
// When the first racer finishes (0x606FD1, place 1), the race end countdown starts if the finisher is an AI (type 2) or
// a human with exactly 2 local players ([0x8900BC], the relocated player count) -> never with 3-4 humans (user: no
// "Hurry!" and no race end 30 s after the first finish). Now 2 or more.
#define AT_HURRY_PLAYER_COUNT 0x606FE8   // jne (count != 2) -> jl
static void InstallHurry() {
    const uint8_t jl = 0x7C;
    Write(AT_HURRY_PLAYER_COUNT, &jl, 1);
}

// ---------------------------------------------------------------- catch-up with 3-4 players
// Catch-up (Quick Race option, 0x5028F0 -> race params 0x89E7F8) between the human players: 0x5F6740 (per human driver:
// +0x18 player, +0x1C racer, factor at +0x468) makes the factor a curve (0x81CB60 + 0xA6C) of (own progress - the
// other human's progress) * 0x88F2B0, progress = racer +0x14 (other: player +4 -> car +0x1C). It only ran with exactly 2
// local players (0x5F676A) -> no catch-up at all with 3-4. Now it runs with 2 or more, and each human measures against
// the furthest ahead other human (with 2 players: the other one, as before).
#define AT_HUMAN_CATCHUP_COUNT 0x5F6771  // jne (count != 2) -> jl
#define AT_HUMAN_CATCHUP_OTHER 0x5F6773  // other human -> edx, ecx = [esi+0x1C]                      (25 bytes)
#define HUMAN_CATCHUP_OTHER_RET 0x5F678C

static const uint8_t* RacerOfLocalPlayer(void* player) {
    auto* car = player ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4) : nullptr;
    return car ? *reinterpret_cast<uint8_t**>(car + 0x1C) : nullptr;
}

static void* __cdecl CatchupOtherHuman(const uint8_t* driver) {
    void* self = *reinterpret_cast<void* const*>(driver + 0x18);
    void* other = nullptr;
    float best = -1e30f;
    for (int k = 0; k < g_localPlayers.count && k < MAX_LOCAL_PLAYERS; ++k) {
        void* p = GetLocalPlayer(k);
        const uint8_t* racer = RacerOfLocalPlayer(p);
        if (!p || p == self || !racer) continue;
        float progress = *reinterpret_cast<const float*>(racer + 0x14);
        if (!other || progress > best) { best = progress; other = p; }
    }
    static DWORD logged[MAX_LOCAL_PLAYERS];
    int me = self ? *reinterpret_cast<int*>(static_cast<uint8_t*>(self) + 0x10) : 0;
    if (me < 0 || me >= MAX_LOCAL_PLAYERS) me = 0;
    if (g_debug && other && GetTickCount() - logged[me] > 3000) {
        logged[me] = GetTickCount();
        const uint8_t* mine = *reinterpret_cast<uint8_t* const*>(driver + 0x1C);
        Log("catch-up: P%d (progress %.3f) vs P%d (progress %.3f), factor %.3f", me + 1,
            mine ? *reinterpret_cast<const float*>(mine + 0x14) : 0.0f,
            *reinterpret_cast<int*>(static_cast<uint8_t*>(other) + 0x10) + 1, best,
            *reinterpret_cast<const float*>(driver + 0x468));
    }
    return other ? other : GetLocalPlayer(0);
}

static __declspec(naked) void HumanCatchupOtherHook() {
    __asm {
        push esi
        call CatchupOtherHuman
        add esp, 4
        mov edx, eax
        mov ecx, dword ptr [esi + 0x1C]
        push HUMAN_CATCHUP_OTHER_RET
        ret
    }
}

// AI rubber-banding (AI opponents in split races): 0x4173ED.. picks the human each AI car measures against (AI +0x748,
// a local player): 1 player -> that one; 2 -> the leader, or the trailer when the AI is behind a threshold (AI count /
// 2, +1 per human within it; racer +0xA = position). With 3-4 the target stayed 0 and was read at 0x4174E2 (crash).
// The same rule over every human: `AiTargetHook` at 0x4174C5 (reached with the target set, or unset for 3-4).
#define AT_AI_TARGET_DONE  0x4174C5      // mov ecx, 0x89E7A0 (5 bytes); ebx = AI, [esp+0x14] = its racer
#define AI_TARGET_RET      0x4174CA
static void __cdecl AiRubberTarget(uint8_t* ai, const uint8_t* aiRacer) {
    void*& target = *reinterpret_cast<void**>(ai + 0x748);
    int n = at<int>(NUM_PLAYERS) == 2 ? 2 + g_extraPlayers : 0;
    if (target || n < 3 || !aiRacer) return;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    int threshold = at<int>(AI_COUNT) / 2;
    void *leader = nullptr, *trailer = nullptr;
    int best = 127, worst = -128;
    for (int k = 0; k < n; ++k) {
        void* p = GetLocalPlayer(k);
        const uint8_t* r = RacerOfLocalPlayer(p);
        if (!r) continue;
        int pos = static_cast<int8_t>(r[0xA]);
        if (pos <= threshold) ++threshold;
        if (pos < best) best = pos, leader = p;
        if (pos > worst) worst = pos, trailer = p;
    }
    target = static_cast<int8_t>(aiRacer[0xA]) > threshold ? trailer : leader;
}
static __declspec(naked) void AiTargetHook() {
    __asm {
        pushad
        push dword ptr [esp + 0x20 + 0x14]   // the AI's racer
        push ebx                         // the AI
        call AiRubberTarget
        add esp, 8
        popad
        mov ecx, 0x89E7A0
        push AI_TARGET_RET
        ret
    }
}

static void InstallCatchup() {
    const uint8_t jl = 0x7C;
    Write(AT_HUMAN_CATCHUP_COUNT, &jl, 1);
    WriteJump(AT_HUMAN_CATCHUP_OTHER, &HumanCatchupOtherHook, 25);
    WriteJump(AT_AI_TARGET_DONE, &AiTargetHook, 5);
}

static void __cdecl CreatePlayers() {
    g_postRace = false;
    // new HUDs (a restart from the pause menu has no post-race frame in between): minimap state of the old one dropped
    g_mmOwnerHidden = false;
    g_mmVisibleCount = 0;
    for (const void*& pkg : g_hudPackages) pkg = nullptr;
    // a restart from the post-race menu skips the race setup paths that uncount the results' extra racers: the
    // restarted race counted one racer too many (drift: a racer message without a name, crash 0x43E99A)
    UncountResultsRacers();
    SyncPlayerSettings(true);
    reinterpret_cast<void(__cdecl*)()>(FN_CREATE_PLAYERS)();
    if (g_debug && GetPrivateProfileIntA("DEBUG", "WatchHud", 0, g_iniPath) && g_localPlayers.byIndex[0]) {
        auto* hud = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g_localPlayers.byIndex[0]) + 0x9A4);
        if (hud && *reinterpret_cast<uint8_t**>(hud + 0x18))
            SetWatch(0, reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(hud + 0x18) + 0x18));
    }
    if (at<int>(NUM_PLAYERS) != 2 || !SplitMode()) return;
    for (int idx = 2; idx < 2 + g_extraPlayers && idx < MAX_LOCAL_PLAYERS; ++idx) {
        auto* racer = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(FN_FIND_RACER)(
            reinterpret_cast<void*>(RACE_PARAMS), idx);
        void* car = FindRaceCar(idx);
        if (!racer || *reinterpret_cast<int*>(racer + 4) != 1 || !car) {
            Log("player %d: no racer/car (racer %p car %p)", idx + 1, racer, car);
            continue;
        }
        auto* player = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x9C0));
        if (!player) continue;
        int port = idx;                                          // input port 2..5 (PollExtraPort)
        // constructed as player 1/2 so the ctor picks P1's / P2's 2P HUD package (PlayerHudCtor clones it for this
        // player), then re-indexed
        void* twin = g_localPlayers.byIndex[idx & 1];
        g_hudCloneFor = idx;                                     // its HUD gets a private package copy
        reinterpret_cast<void*(__thiscall*)(void*, int, int)>(FN_PLAYER_CTOR)(player, port, idx & 1);
        g_hudCloneFor = -1;
        g_localPlayers.byIndex[idx & 1] = twin;
        g_localPlayers.byIndex[idx] = player;
        *reinterpret_cast<int*>(player + 0x10) = idx;
        PrepareExtraNames();                                     // its name (+0x30, 15 chars; results, labels)
        strncpy_s(reinterpret_cast<char*>(player + 0x30), 16, g_extraNames[idx - 2], _TRUNCATE);
        InitPlayerView(kPlayerViewId[idx]);                     // camera movers need the view's camera
        if (void* cam = ExtraCamera(idx))
            GetView(kPlayerViewId[idx]).camera = static_cast<uint8_t*>(cam);
        *reinterpret_cast<int*>(player + 0x18) = kPlayerViewId[idx];   // its view (the ctor picks 1/2 or 1)
        *reinterpret_cast<int*>(player + 0x1C) = 0;                      // no mirror view
        reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_PLAYER_ATTACH)(player, car);
        Log("player %d created (car %p, view %d)", idx + 1, car, kPlayerViewId[idx]);
    }
}

static void* ExtraCamera(int k) {
    if (!g_extraCamera[k]) {
        void* c = reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x320);
        if (!c) return nullptr;
        memset(c, 0, 0x320);
        reinterpret_cast<void*(__thiscall*)(void*)>(FN_CAMERA_CTOR)(c);
        g_extraCamera[k] = c;
    }
    return g_extraCamera[k];
}
