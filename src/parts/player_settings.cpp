// Part of NFSU2SplitScreen.cpp: per-player settings, keyboard Feedback, HUD state.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Per-player settings (camera, HUD options, feedback...: 0x83AA64 + index*0x28, 2 entries; the race data follows at
// 0x83AAB4) -> players 3/4 read race data as settings (e.g. camera 0 instead of P1's drift camera). The in-race readers
// use a 4-entry mirror instead: entries 0/1 synced both ways with the game's table every frame (options/pause menus
// write the table, the in-race camera change 0x5F2FA0 writes the mirror), entries 2/3 are players 3/4's own (seat
// store; the first race without any copies P1's / P2's).
// The pause menu's player options edit the pausing player's entry. Most of them (0x4C31D0..0x4C4700) pick it by
// comparing the pausing port [0x8900D8]+0x20 with players 1/2's ports, so any other port gets entry 0; the camera
// options use 0x4A7AE0 (index of the pausing player among players 1/2, 0 for anyone else). While a player 3..6 owns
// the pause, their entry is swapped into the game's table at entry 0 (which 0x4A7AE0 then returns), so the menus show
// and change their settings.
#define PLAYER_SETTINGS    0x83AA64
#define PLAYER_FEEDBACK    4             // settings +4: Feedback (rumble) option
#define FN_PAUSING_PLAYER  0x4A7AE0      // int __cdecl (): 5 bytes stolen (mov eax, [0x8900D8])
static uint8_t g_settings[MAX_LOCAL_PLAYERS][PLAYER_SETTING_SIZE];
static uint8_t g_settingsSeen[MAX_LOCAL_PLAYERS][PLAYER_SETTING_SIZE];   // the mirror as last synced
static int g_settingsSwap = -1;          // player 3..6 whose entry sits in the game's table (entry 0)
static const uintptr_t kSettingsReaders[] = {
    0x41E23E, 0x41E245, 0x41E28D, 0x497D59, 0x4DA0F1, 0x4DA8DA, 0x5EA7DD, 0x5EA8C7, 0x5EB406, 0x5EB426, 0x5F17E9,
    0x5F17F2, 0x5F2A93, 0x5F2FB6, 0x5F2FC8, 0x5F325F, 0x5F32AA, 0x5F33CE, 0x5F33DD, 0x5F3402, 0x5F345E, 0x5F3A33,
    0x5F6388, 0x5FFE31, 0x6001FB, 0x600497, 0x60353D};
static int SettingsSeat(int entry) { return g_settingsSwap >= 0 && entry == 0 ? g_settingsSwap : entry; }

// The keyboard player's own pause menus show Feedback greyed and off (FfCapable; display only: the game's entry has it
// cleared while they are open, the player's setting is kept).
static bool PauseMenuOpen();
static int g_kbFeedbackEntry = -1;       // game entry whose Feedback is shown off
static uint8_t g_kbFeedbackValue;
static bool KeyboardPlayerOptions() {
    return SplitRacing() && g_keyboardSeat >= 0 && g_pausePort == g_keyboardSeat && PauseMenuOpen();
}
static void SyncPlayerSettings(bool newRace) {
    auto* game = reinterpret_cast<uint8_t*>(PLAYER_SETTINGS);
    if (g_kbFeedbackEntry >= 0) {
        game[g_kbFeedbackEntry * PLAYER_SETTING_SIZE + PLAYER_FEEDBACK] = g_kbFeedbackValue;
        g_kbFeedbackEntry = -1;
    }
    for (int k = 0; k < 2; ++k) {
        uint8_t* g = game + k * PLAYER_SETTING_SIZE;
        int seat = SettingsSeat(k);
        if (memcmp(g_settings[seat], g_settingsSeen[seat], PLAYER_SETTING_SIZE) != 0)
            memcpy(g, g_settings[seat], PLAYER_SETTING_SIZE);       // changed in race (camera) -> keep it
        else
            memcpy(g_settings[seat], g, PLAYER_SETTING_SIZE);       // menus may have changed the game's
        memcpy(g_settingsSeen[seat], g_settings[seat], PLAYER_SETTING_SIZE);
    }
    int swap = SplitRacing() && g_pauseOwner >= 2 ? g_pauseOwner : -1;
    if (swap != g_settingsSwap) {
        g_settingsSwap = swap;
        int seat = SettingsSeat(0);
        memcpy(game, g_settings[seat], PLAYER_SETTING_SIZE);
        memcpy(g_settingsSeen[seat], g_settings[seat], PLAYER_SETTING_SIZE);
        Log("player settings: %s", swap >= 0 ? "pause menu edits the pausing player 3..6" : "game table back to players 1/2");
    }
    for (int k = 2; k < MAX_LOCAL_PLAYERS; ++k) {
        SeatStore& st = g_seatStore[k - 2];
        if (!st.hasSettings) {
            if (!newRace) continue;
            memcpy(g_settings[k], g_settings[k & 1], PLAYER_SETTING_SIZE);   // first race: start from P1's / P2's
            memcpy(g_settingsSeen[k], g_settings[k], PLAYER_SETTING_SIZE);
        } else if (memcmp(st.settings, g_settings[k], PLAYER_SETTING_SIZE) == 0) {
            continue;
        }
        memcpy(st.settings, g_settings[k], PLAYER_SETTING_SIZE);
        st.hasSettings = true;
        SaveSeatStore(k - 2);
    }
    if (KeyboardPlayerOptions()) {
        g_kbFeedbackEntry = g_settingsSwap >= 0 ? 0 : g_keyboardSeat;
        g_kbFeedbackValue = game[g_kbFeedbackEntry * PLAYER_SETTING_SIZE + PLAYER_FEEDBACK];
        game[g_kbFeedbackEntry * PLAYER_SETTING_SIZE + PLAYER_FEEDBACK] = 0;
    }
}
static bool __cdecl FfCapable() {
    return !KeyboardPlayerOptions();
}

// HUD state of a player (which HUD parts show): 0x600090 reads the player's HUD options (settings +0, +3, +0x10;
// readers redirected to the mirror) and has 2-player rules on the index (e.g. index != 0 with 2 players hides a part).
// Players 3/4 run it as player 1 / player 2 for those rules, with their own options in that mirror entry for the call.
#define AT_HUD_STATE_CALL  0x600595      // `call 0x600090` (uint64 __fastcall (player))
#define FN_HUD_STATE       0x600090
static uint64_t __fastcall HudStateOf(uint8_t* player) {
    int& idx = *reinterpret_cast<int*>(player + 0x10);
    int saved = idx;
    if (saved < 2 || saved >= MAX_LOCAL_PLAYERS)
        return reinterpret_cast<uint64_t(__fastcall*)(uint8_t*)>(FN_HUD_STATE)(player);
    uint8_t own[PLAYER_SETTING_SIZE];
    int as = saved & 1;
    memcpy(own, g_settings[as], PLAYER_SETTING_SIZE);
    memcpy(g_settings[as], g_settings[saved], PLAYER_SETTING_SIZE);
    idx = as;
    uint64_t state = reinterpret_cast<uint64_t(__fastcall*)(uint8_t*)>(FN_HUD_STATE)(player);
    idx = saved;
    memcpy(g_settings[as], own, PLAYER_SETTING_SIZE);
    return state;
}

static void* g_origPausingPlayer;
static int __cdecl PausingPlayer() {
    if (g_settingsSwap >= 0) return 0;
    return reinterpret_cast<int(__cdecl*)()>(g_origPausingPlayer)();
}

// The Transmission and Stability Control pause options also apply the change to the car at once: message 0x578EF0
// (type 1 / 2, player index, value) -> 0x4A7B30 sets player +0x20 / +0x21 and the car's gearbox / car +0x464 on
// byIndex[index]. The index is the same port comparison (eax at 0x4C32B3 / 0x4C3D53), 0 for players 3..6: their
// change went to player 1's car. Hooked on `mov ecx, [0x8650B8]` (6 bytes) before the pushes.
#define AT_PAUSE_STABILITY_MSG 0x4C32B3
#define AT_PAUSE_GEARBOX_MSG   0x4C3D53
static __declspec(naked) void PauseStabilityMsgHook() {
    __asm {
        cmp g_settingsSwap, 0
        jl keep
        mov eax, g_settingsSwap          // the pausing player 3..6's own car
    keep:
        mov ecx, dword ptr ds:[0x8650B8]
        push AT_PAUSE_STABILITY_MSG + 6
        ret
    }
}
static __declspec(naked) void PauseGearboxMsgHook() {
    __asm {
        cmp g_settingsSwap, 0
        jl keep
        mov eax, g_settingsSwap
    keep:
        mov ecx, dword ptr ds:[0x8650B8]
        push AT_PAUSE_GEARBOX_MSG + 6
        ret
    }
}

static int RedirectPlayerSettings() {
    memcpy(g_settings, reinterpret_cast<void*>(PLAYER_SETTINGS), 2 * PLAYER_SETTING_SIZE);
    for (int k = 2; k < MAX_LOCAL_PLAYERS; ++k)
        if (g_seatStore[k - 2].hasSettings) memcpy(g_settings[k], g_seatStore[k - 2].settings, PLAYER_SETTING_SIZE);
    memcpy(g_settingsSeen, g_settings, sizeof(g_settingsSeen));
    int n = 0;
    for (uintptr_t at : kSettingsReaders)
        for (uintptr_t a = at; a < at + 8; ++a) {
            uint32_t v = *reinterpret_cast<const uint32_t*>(a);
            if (v < PLAYER_SETTINGS || v >= PLAYER_SETTINGS + 2 * PLAYER_SETTING_SIZE) continue;
            uint32_t nv = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_settings[0][0]) + (v - PLAYER_SETTINGS));
            Write(a, &nv, 4);
            ++n;
            break;
        }
    return n;
}
