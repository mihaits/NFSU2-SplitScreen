// Part of NFSU2SplitScreen.cpp: session points and the standings screen.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- session standings
// Every split race that reaches its results (not Outrun, not Free Run) gives the human players points by their place
// among the humans: 10 / 6 / 4 / 3 / 2 / 1, nothing without a finish (AI racers don't score). A session lasts until the
// main menu or a change in the number of players. After the race's Event Results the game's own tournament standings
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
    return SplitMode() && type != 8 && type != 9;
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
