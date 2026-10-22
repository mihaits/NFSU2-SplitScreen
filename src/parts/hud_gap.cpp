// Part of NFSU2SplitScreen.cpp: drift and race HUD gap.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Drift HUD gap ("+N" / "-N" under the position, 0x4DA414): the 2-player path scores the racers of human slots 0 and 1
// (0x56DD50(racer) +0x10) and shows slot0 - slot1 for player 0, the reverse for player 1 and 0 for anyone else.
// Now for every local player: leading -> own score - the next best, otherwise own score - the leader's (same numbers
// as the game's with 2 players).
#define AT_DRIFT_GAP       0x4DA445      // `push 0` (2-player branch; ebx = player)
#define AT_DRIFT_GAP_DONE  0x4DA49A      // edx = gap
#define HUMAN_RACER_SLOTS_EARLY 0x89E808
#define FN_DRIFT_SCORE     0x56DD50      // entry* __cdecl (racer); +0x10 float score
static int RacerIdOfPlayer(int k) { return k < 2 ? reinterpret_cast<const int*>(HUMAN_RACER_SLOTS_EARLY)[k] : k; }
static int __cdecl DriftGap(const uint8_t* player) {
    int me = *reinterpret_cast<const int*>(player + 0x10), n = 2 + g_extraPlayers;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    auto score = [](int k) {
        auto* e = reinterpret_cast<const uint8_t*(__cdecl*)(int)>(FN_DRIFT_SCORE)(RacerIdOfPlayer(k));
        return e ? static_cast<int>(*reinterpret_cast<const float*>(e + 0x10)) : 0;
    };
    int own = score(me), best = INT_MIN;
    bool any = false;
    for (int k = 0; k < n; ++k)
        if (k != me) { int s = score(k); if (!any || s > best) best = s; any = true; }
    if (!any) return 0;
    return own - best;                    // leading: positive gap to the next best; else negative gap to the leader
}
static __declspec(naked) void DriftGapHook() {
    __asm {
        pushad
        push ebx
        call DriftGap
        add esp, 4
        mov dword ptr [esp + 0x14], eax  // popad restores it into edx
        popad
        push AT_DRIFT_GAP_DONE
        ret
    }
}

// Race HUD gap (circuit / sprint / Street X / URL, same element, branch at 0x4DA4DC): the game gets the time gap
// between local players 0 and 1 (0x5F82D0(status; &out, other, self), positive = self ahead, players' +4 -> +0x1C) and
// shows it for player 0 / 1, 0 for anyone else. Now for every local player the smallest gap over the others: leading
// -> + gap to the next, otherwise - gap to the leader (with 2 players the game's own numbers).
#define AT_RACE_GAP        0x4DA4DC
#define AT_RACE_GAP_DONE   0x4DA526      // `test eax, eax` with eax = [esp + 0x34] = gap
#define FN_RACE_TIME_GAP   0x5F82D0      // void __thiscall (status; int* out, info other, info self)
#define RACE_STATUS        0x890118
static int __cdecl RaceGap(const uint8_t* element) {
    int me = *reinterpret_cast<const int*>(element + 0x10), n = 2 + g_extraPlayers;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    auto info = [](int k) -> void* {
        auto* p = static_cast<uint8_t*>(GetLocalPlayer(k));
        auto* q = p ? *reinterpret_cast<uint8_t**>(p + 4) : nullptr;
        return q ? *reinterpret_cast<void**>(q + 0x1C) : nullptr;
    };
    void* self = info(me);
    auto* status = *reinterpret_cast<void**>(RACE_STATUS);
    if (!self || !status) return 0;
    int best = 0;
    bool any = false;
    for (int k = 0; k < n; ++k) {
        void* other = k != me ? info(k) : nullptr;
        if (!other) continue;
        int gap = 0;
        reinterpret_cast<void(__thiscall*)(void*, int*, void*, void*)>(FN_RACE_TIME_GAP)(status, &gap, other, self);
        if (!any || gap < best) best = gap;
        any = true;
    }
    return best;
}
static __declspec(naked) void RaceGapHook() {
    __asm {
        pushad
        push ebx
        call RaceGap
        add esp, 4
        mov dword ptr [esp + 0x20 + 0x34], eax
        popad
        mov eax, dword ptr [esp + 0x34]
        push AT_RACE_GAP_DONE
        ret
    }
}
