// Part of NFSU2SplitScreen.cpp: session points and the standings screen.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- session standings
// Every split race that reaches its results (each Outrun too, not Free Run) gives the human players points by
// their place among the humans: 10 / 6 / 4 / 3 / 2 / 1, nothing without a finish (AI racers don't score). A session
// lasts until the main menu or a change in the number of players. After the race's Event Results the game's own tournament standings
// screen (UI_PostRace_TournStandings.fng, class cPostRaceTournamentStandings, ctor 0x4F4610) shows the totals.
// - The post-race screen list (0x4F3EA0) adds that screen when 0x4C00C0 says so (career tournaments, split Outrun);
//   its one call (0x4F3F9A) goes through `ShowStandings`.
// - The screen's setup 0x4EA570 (fastcall screen; its one call 0x4F4657) builds the rows from tournament records and
//   points (0x4C07A0, 0x4D77A0); `StandingsSetup` fills them from the session instead. Row text objects: RANKING_DATA_n,
//   PLAYER_DATA_n (name), CAR_DATA_n (car name, by language hash CAR_NAME_<car key>), PLAYER_POINT_DATA_n; then
//   0x4A4770(screen; 6, rows) sizes the table (the list's package goes to [0x8363DC] + 0x1C first).
// - Names as the game's row setter 0x4C03A0 picks them: a car whose spec (car +0x14) is the local human's (+4 == 1,
//   +0x900 == 0) shows the profile name (0x5F41B0, thiscall car +0x18), any other its spec +0x90C. Car key: car +0xC.
#define AT_SHOW_STANDINGS_CALL  0x4F3F9A  // call 0x4C00C0 (bool __cdecl ())
#define FN_SHOW_STANDINGS       0x4C00C0
#define AT_STANDINGS_SETUP_CALL 0x4F4657  // call 0x4EA570 (__fastcall screen)
#define FN_STANDINGS_SETUP      0x4EA570
#define FN_FE_HASHF             0x50B990  // uint32_t __cdecl (fmt, ...): hash of the formatted name
#define FN_FE_TEXTF             0x537B80  // __cdecl (package, object hash, fmt, ...)
#define FN_FE_OBJECT            0x537A10  // FEObject* __cdecl (package, object hash)
#define FN_STANDINGS_ROWS       0x4A4770  // __thiscall (screen; 6, rows)
#define STANDINGS_LIST          0x8363DC  // +0x1C: the list's package
#define FN_PROFILE_NAME         0x5F41B0  // const char* __thiscall (car +0x18)

static const int kSessionPoints[MAX_LOCAL_PLAYERS] = {10, 6, 4, 3, 2, 1};
static struct {
    int players, races;
    int points[MAX_LOCAL_PLAYERS], last[MAX_LOCAL_PLAYERS];
    char name[MAX_LOCAL_PLAYERS][32], car[MAX_LOCAL_PLAYERS][32];
} g_session = {};

static void ResetSession(const char* why) {
    if (g_session.races) Log("session: ended after %d races (%s)", g_session.races, why);
    g_session = {};
}
static bool SessionRace() {
    int type = at<int>(QR_RACE_TYPE);
    return SplitMode() && type != 9;
}
static bool SessionShown() { return SessionRace() && g_session.races > 0 && g_session.players == SplitPlayers(); }

// At the results (RaceResults): each player's place among the humans who finished.
static void AwardSessionPoints() {
    if (!SessionRace()) return;
    int n = SplitPlayers();
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    if (g_session.players != n) {
        ResetSession("player count changed");
        g_session.players = n;
    }
    int place[MAX_LOCAL_PLAYERS] = {};
    for (int k = 0; k < n; ++k) {
        void* player = GetLocalPlayer(k);
        place[k] = player ? FinishedPosition(player) : 0;
        auto* car = player ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4) : nullptr;
        auto* spec = car ? *reinterpret_cast<uint8_t**>(car + 0x14) : nullptr;
        if (!spec) continue;
        const char* name = *reinterpret_cast<int*>(spec + 4) == 1 && !spec[0x900]
            ? reinterpret_cast<const char*(__thiscall*)(void*)>(FN_PROFILE_NAME)(*reinterpret_cast<void**>(car + 0x18))
            : reinterpret_cast<const char*>(spec + 0x90C);
        const char* key = *reinterpret_cast<const char**>(car + 0xC);
        strncpy_s(g_session.name[k], name ? name : "", _TRUNCATE);
        strncpy_s(g_session.car[k], key ? key : "", _TRUNCATE);
    }
    for (int k = 0; k < n; ++k) {
        int rank = 1;
        for (int j = 0; j < n; ++j) if (place[j] && place[j] < place[k]) ++rank;
        g_session.last[k] = place[k] ? kSessionPoints[rank - 1] : 0;
        g_session.points[k] += g_session.last[k];
    }
    ++g_session.races;
    for (int k = 0; k < n; ++k)
        Log("session: race %d, player %d place %d: +%d = %d", g_session.races, k + 1, place[k], g_session.last[k],
            g_session.points[k]);
}

static bool __cdecl ShowStandings() {
    if (SessionShown()) return true;
    return reinterpret_cast<bool(__cdecl*)()>(FN_SHOW_STANDINGS)();
}

static void __fastcall StandingsSetup(uint8_t* screen) {
    if (!SessionShown()) {
        reinterpret_cast<void(__fastcall*)(void*)>(FN_STANDINGS_SETUP)(screen);
        return;
    }
    using HashFn = uint32_t(__cdecl*)(const char*, ...);
    using TextFn = void(__cdecl*)(void*, uint32_t, const char*, ...);
    auto hash = reinterpret_cast<HashFn>(FN_FE_HASHF);
    auto text = reinterpret_cast<TextFn>(FN_FE_TEXTF);
    void* package = *reinterpret_cast<void**>(screen + 4);
    int n = g_session.players;
    int order[MAX_LOCAL_PLAYERS];
    for (int k = 0; k < n; ++k) order[k] = k;
    std::stable_sort(order, order + n, [](int a, int b) {
        if (g_session.points[a] != g_session.points[b]) return g_session.points[a] > g_session.points[b];
        return g_session.last[a] > g_session.last[b];
    });
    for (int i = 0; i < n; ++i) {
        int k = order[i], rank = 1;
        for (int j = 0; j < n; ++j) if (g_session.points[j] > g_session.points[k]) ++rank;
        text(package, hash("RANKING_DATA_%d", i + 1), "%d", rank);
        text(package, hash("PLAYER_DATA_%d", i + 1), "%s", g_session.name[k]);
        if (void* car = reinterpret_cast<void*(__cdecl*)(void*, uint32_t)>(FN_FE_OBJECT)(package, hash("CAR_DATA_%d", i + 1)))
            reinterpret_cast<void(__cdecl*)(void*, uint32_t)>(FN_SET_TEXT_HASH)(car, hash("CAR_NAME_%s", g_session.car[k]));
        text(package, hash("PLAYER_POINT_DATA_%d", i + 1), "%d", g_session.points[k]);
    }
    if (auto* list = at<uint8_t*>(STANDINGS_LIST)) *reinterpret_cast<void**>(list + 0x1C) = package;
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(FN_STANDINGS_ROWS)(screen, 6, n);
    Log("session: standings after %d races shown", g_session.races);
}

// ---------------------------------------------------------------- split Outrun
// The game's Outrun is a tournament of rounds; each ends when every chaser has dropped out (300 m behind the leader,
// see race-flow.md), the post-race screens follow and A on the last one loads the next round, until a player reaches
// the options' Number of Points to Win. Split Outrun is a single round that ends like any race: Event Results, the
// session standings, Post Race. In split screen the post-race list (0x4F3EA0) would show the Tournament Race Results
// (0x4C0050 is true for mode 4 + Outrun) instead of Event Results, with rows from two lobby records and the game's
// 2-player points (+10 / +6 into 0x83ABC8 / 0x83ABC9, every racer after the first into the second byte), and would
// leave out Post Race. All three checks test mode 4 last, so a jmp / nop there leaves split screen on the race path.
#define AT_EVENT_RESULTS_MODE4 0x4F3F3C   // jne 0x4F3F47 (Event Results unless mode 4 Outrun) -> jmp
#define AT_TOURN_RESULTS_MODE4 0x4C00A5   // jne 0x4C00AF (false unless mode 4 Outrun) -> jmp
#define AT_POST_RACE_MODE4     0x4F40A9   // je 0x4F40CE after `cmp [0x83AAB4], 8` (no Post Race) -> nops
// Every round's starter (0x526830; a new round skips creating the racers) ends in a jump to the grid setup 0x5264A0(1):
// it numbers the human racer specs' grid slots 1..n (slot 1 = the front spot), then in split Outrun swaps players 1 and
// 2 so that the loser of the last round (0x83ABCC, 0xFF in the first round = a random pick) starts in front, and copies
// each human's slot to 0x89E7D0[player]. `OutrunGrid` replaces that swap for any number of players: the grid is the
// session's last race reversed (the fewest points of it in front), random in a new session.
#define AT_OUTRUN_GRID_JUMP 0x526965      // jmp 0x5264A0 (cdecl, arg 1 on the stack)
#define FN_OUTRUN_GRID      0x5264A0
#define RACER_SPECS         0x89E810      // spec*, count at +4, stride 0x940: +1 player index, +3 grid slot, +4 1 = human
#define RACER_SPEC_SIZE     0x940
#define HUMAN_GRID_SLOTS    0x89E7D0      // byte per player index

static void __cdecl OutrunGrid(int arg) {
    reinterpret_cast<void(__cdecl*)(int)>(FN_OUTRUN_GRID)(arg);
    if (!SplitMode() || at<int>(QR_RACE_TYPE) != 8) return;
    auto* specs = at<uint8_t*>(RACER_SPECS);
    int count = at<int>(RACER_SPECS + 4), n = 0;
    uint8_t* human[MAX_LOCAL_PLAYERS];
    int slots[MAX_LOCAL_PLAYERS];
    for (int i = 0; specs && i < count && n < MAX_LOCAL_PLAYERS; ++i) {
        uint8_t* spec = specs + i * RACER_SPEC_SIZE;
        if (*reinterpret_cast<int*>(spec + 4) != 1) continue;
        human[n] = spec;
        slots[n++] = spec[3];
    }
    std::sort(slots, slots + n);
    bool seeded = g_session.races > 0 && g_session.players == n;
    if (seeded) {
        std::stable_sort(human, human + n, [](uint8_t* a, uint8_t* b) {
            int pa = static_cast<int8_t>(a[1]), pb = static_cast<int8_t>(b[1]);
            return g_session.last[pa] < g_session.last[pb];
        });
    } else {
        for (int i = n - 1; i > 0; --i) {
            int j = reinterpret_cast<int(__cdecl*)(int)>(FN_GAME_RANDOM)(i + 1);
            std::swap(human[i], human[j]);
        }
    }
    char order[64] = {};
    for (int i = 0; i < n; ++i) {
        int player = static_cast<int8_t>(human[i][1]);
        human[i][3] = static_cast<uint8_t>(slots[i]);
        if (player >= 0 && player < MAX_LOCAL_PLAYERS) at<uint8_t>(HUMAN_GRID_SLOTS + player) = static_cast<uint8_t>(slots[i]);
        sprintf_s(order + strlen(order), sizeof(order) - strlen(order), " P%d", player + 1);
    }
    Log("outrun: grid front to back%s (%s)", order, seeded ? "last race reversed" : "random");
}

// The tournament is over when exactly one player has the most points and they reach the options' Number of Points to
// Win: 0x5027B0 (thiscall on the race settings 0x83AAB4: +0x108 the option, 6 = endless; target byte table 0x7992CC
// = 10 / 20 / 30 / 40 / 50 / 100) reads the game's points of players 1..4 (0x83ABC8..CB). It decides A at the end of
// the post-race list (0x4BFFC4: next round, or back to the options screen) and the standings' button text (0x4EA659).
// `OutrunOver` says over after every split round. The options' row builder for Outrun (0x4CDA20) adds Number of
// Points to Win (vtable 0x7914A0), then Time Limit; `OutrunPointsRowHook` leaves the first out in split screen.
#define FN_OUTRUN_OVER       0x5027B0      // bool __thiscall (settings)
#define AT_OUTRUN_POINTS_ROW 0x4CDA52      // push edi; push 0x54; call 0x575620 (8 bytes): the Points to Win row
#define OUTRUN_POINTS_ROW_RUN  0x4CDA5A
#define OUTRUN_POINTS_ROW_SKIP 0x4CDAB0    // the Time Limit row (edi pushed, popped at 0x4CDB0D)
#define FN_GAME_ALLOC        0x575620      // void* __cdecl (size)
static void* g_origOutrunOver = nullptr;

static bool __fastcall OutrunOver(uint8_t* settings, void*) {
    if (!SplitMode() || at<int>(QR_RACE_TYPE) != 8)
        return reinterpret_cast<bool(__fastcall*)(void*, void*)>(g_origOutrunOver)(settings, nullptr);
    return true;
}
static __declspec(naked) void OutrunPointsRowHook() {
    __asm {
        push edi
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        push 0x54
        mov eax, FN_GAME_ALLOC
        call eax
        push OUTRUN_POINTS_ROW_RUN
        ret
    skip:
        push OUTRUN_POINTS_ROW_SKIP
        ret
    }
}

// Time Limit (option settings +0x10C, minutes byte table 0x7992D4 = 0 / 2 / 5 / 8 / 10): once the race time
// (manager +0x14, 4000 per second) reaches it, the race update (0x6082D1..0x60831F, reached for each chaser still in)
// finishes racers 0 and 1: the one with more time in the lead wins (racer [+0] +0x18 -> +0; the game sets the
// leader's to the race time minus a chaser's, so it is the time in the lead only with 2 racers), the other is second.
// With more players the rest kept racing and the last of them became a second winner. In split Outrun the plugin
// counts each player's time as racer 0 itself (`OutrunTimeLimitFrame`, each frame before the outrun logic), the
// option's getter (call 0x6082E7) answers "no limit", and from the limit on the player with the least time in the
// lead gets a gap far past 300 m each frame: the game's own drop-out finishes it with the last free place, until the
// one with the most time in the lead (kept in slot 0, which the gaps are measured from) is left and wins.
#define AT_TIME_LIMIT_OPTION 0x6082E7      // call 0x502780 (int __thiscall (settings): +0x10C)
#define FN_TIME_LIMIT_OPTION 0x502780
#define TIME_LIMIT_MINUTES   0x7992D4      // int8 [5]
#define RACE_MANAGER         0x890118
#define RACERS_ORDERED       0xBD0         // racer* [count +0x24], racer 0 leads
#define OUTRUN_DROP_GAP      1.0e6f
static int g_leadTicks[MAX_LOCAL_PLAYERS];
static int g_leadLastTime = -1;
static bool g_timeLimitLogged = false;

static bool SplitOutrunRacing(uint8_t** mgr) {
    *mgr = at<uint8_t*>(RACE_MANAGER);
    return SplitMode() && at<int>(QR_RACE_TYPE) == 8 && at<int>(0x89E7B0) == 7 && *mgr;
}
static int __fastcall TimeLimitOption(uint8_t* settings, void*) {
    uint8_t* mgr;
    return SplitOutrunRacing(&mgr) ? 0 : *reinterpret_cast<int*>(settings + 0x10C);
}
static int RacerPlayer(uint8_t* racer) {
    auto* car = *reinterpret_cast<uint8_t**>(racer);
    auto* info = car ? *reinterpret_cast<uint8_t**>(car + 0x18) : nullptr;
    int k = info ? info[0x10] : -1;
    return k >= 0 && k < MAX_LOCAL_PLAYERS ? k : -1;
}
static int LeadTicks(uint8_t* racer) { int k = RacerPlayer(racer); return k >= 0 ? g_leadTicks[k] : 0; }

static void __cdecl OutrunTimeLimitFrame() {
    uint8_t* mgr;
    if (!SplitOutrunRacing(&mgr)) return;
    auto** slots = reinterpret_cast<uint8_t**>(mgr + RACERS_ORDERED);
    int count = *reinterpret_cast<int*>(mgr + 0x24), now = *reinterpret_cast<int*>(mgr + 0x14);
    if (now < g_leadLastTime || g_leadLastTime < 0) {          // a new round
        memset(g_leadTicks, 0, sizeof(g_leadTicks));
        g_leadLastTime = now;
        g_timeLimitLogged = false;
    }
    uint8_t* in[MAX_RACERS];
    int n = 0;
    for (int i = 0; i < count && n < MAX_RACERS; ++i)
        if (slots[i] && *reinterpret_cast<int16_t*>(slots[i] + 0x10) == 0) in[n++] = slots[i];
    if (count > 0 && slots[0] && *reinterpret_cast<int16_t*>(slots[0] + 0x10) == 0) {
        int k = RacerPlayer(slots[0]);
        if (k >= 0) g_leadTicks[k] += now - g_leadLastTime;
    }
    g_leadLastTime = now;
    int option = at<int>(QR_RACE_TYPE + 0x10C);
    int minutes = option > 0 && option < 5 ? reinterpret_cast<const int8_t*>(TIME_LIMIT_MINUTES)[option] : 0;
    if (!minutes || now < minutes * 60 * 4000 || n < 2) return;
    if (!g_timeLimitLogged) {
        char buf[128] = {};
        for (int i = 0; i < n; ++i)
            sprintf_s(buf + strlen(buf), sizeof(buf) - strlen(buf), " P%d %.2f", RacerPlayer(in[i]) + 1, LeadTicks(in[i]) / 4000.0);
        Log("outrun: time limit, seconds in the lead:%s", buf);
        g_timeLimitLogged = true;
    }
    uint8_t* most = in[0];
    uint8_t* least = nullptr;
    for (int i = 0; i < n; ++i) if (LeadTicks(in[i]) > LeadTicks(most)) most = in[i];
    for (int i = 0; i < count; ++i)
        if (slots[i] == most) { std::swap(slots[0], slots[i]); break; }
    for (int i = 0; i < n; ++i)
        if (in[i] != most && (!least || LeadTicks(in[i]) < LeadTicks(least))) least = in[i];
    *reinterpret_cast<float*>(least + 0x24) = OUTRUN_DROP_GAP;
}

static void InstallOutrun() {
    auto* events = reinterpret_cast<uint8_t*>(AT_EVENT_RESULTS_MODE4);
    auto* tourn = reinterpret_cast<uint8_t*>(AT_TOURN_RESULTS_MODE4);
    auto* grid = reinterpret_cast<uint8_t*>(AT_OUTRUN_GRID_JUMP);
    static const uint8_t overStart[] = {0x57, 0x8B, 0xB9, 0x08, 0x01, 0x00, 0x00};   // push edi; mov edi, [ecx+0x108]
    if (events[0] != 0x75 || events[1] != 0x09 || tourn[0] != 0x75 || tourn[1] != 0x08 || grid[0] != 0xE9 ||
        AT_OUTRUN_GRID_JUMP + 5 + *reinterpret_cast<int32_t*>(grid + 1) != FN_OUTRUN_GRID ||
        memcmp(reinterpret_cast<void*>(AT_POST_RACE_MODE4), "\x74\x23", 2) != 0 ||
        memcmp(reinterpret_cast<void*>(AT_OUTRUN_POINTS_ROW), "\x57\x6A\x54\xE8", 4) != 0 ||
        memcmp(reinterpret_cast<void*>(FN_OUTRUN_OVER), overStart, sizeof(overStart)) != 0 ||
        *reinterpret_cast<uint8_t*>(AT_TIME_LIMIT_OPTION) != 0xE8 ||
        AT_TIME_LIMIT_OPTION + 5 + *reinterpret_cast<int32_t*>(AT_TIME_LIMIT_OPTION + 1) != FN_TIME_LIMIT_OPTION) {
        Log("outrun: unexpected code, split Outrun keeps the game's 2-player results");
        return;
    }
    const uint8_t jmp = 0xEB;
    Write(AT_EVENT_RESULTS_MODE4, &jmp, 1);
    Write(AT_TOURN_RESULTS_MODE4, &jmp, 1);
    WriteJump(AT_OUTRUN_GRID_JUMP, &OutrunGrid, 5);
    const uint8_t nops[2] = {0x90, 0x90};
    Write(AT_POST_RACE_MODE4, nops, 2);
    WriteJump(AT_OUTRUN_POINTS_ROW, &OutrunPointsRowHook, 8);
    WriteCall(AT_TIME_LIMIT_OPTION, &TimeLimitOption);
    g_origOutrunOver = MakeTrampoline(FN_OUTRUN_OVER, sizeof(overStart));
    WriteJump(FN_OUTRUN_OVER, &OutrunOver, sizeof(overStart));
}

static void InstallStandings() {
    auto isCallTo = [](uintptr_t at_, uintptr_t to) {
        auto* b = reinterpret_cast<const uint8_t*>(at_);
        return b[0] == 0xE8 && at_ + 5 + *reinterpret_cast<const int32_t*>(b + 1) == to;
    };
    if (!isCallTo(AT_SHOW_STANDINGS_CALL, FN_SHOW_STANDINGS) || !isCallTo(AT_STANDINGS_SETUP_CALL, FN_STANDINGS_SETUP)) {
        Log("session: unexpected code, standings not installed");
        return;
    }
    WriteCall(AT_SHOW_STANDINGS_CALL, &ShowStandings);
    WriteCall(AT_STANDINGS_SETUP_CALL, &StandingsSetup);
}
