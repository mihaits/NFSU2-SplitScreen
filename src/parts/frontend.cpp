// Part of NFSU2SplitScreen.cpp: frontend: main menu entry, players row, garage, help bar.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- frontend
static void ResetFrontendPlayers() {
    at<int>(FE_PLAYER) = 0;
    at<uint32_t>(FE_PORTS) = 0xFFFFFFFF;
    at<uint32_t>(FE_PORTS + 4) = 0xFFFFFFFF;
}

static void SetQuickRacePlayers(uint8_t n) {
    for (int i = 0; i < QR_RACE_TYPES; ++i)
        at<uint8_t>(QR_SETTINGS + i * 0x14) = n;
}

// The console backed player 1's garage up when entering split screen (PC lost that code but kept the restore);
// without it backing out restores an empty garage: no car in the main menu, and Customize crashes.
static void BackupGarage() {
    memcpy(reinterpret_cast<void*>(GARAGE + GARAGE_LIST_BACKUP), reinterpret_cast<void*>(GARAGE + GARAGE_LIST),
           GARAGE_LIST_SIZE);
    at<uint32_t>(GARAGE + GARAGE_CAR_BACKUP) = at<uint32_t>(GARAGE + GARAGE_CAR);
}

static void __cdecl OnQuickRaceSelected() {
    bool split = g_menuSplit;
    g_menuSplit = false;
    if (split) {
        if (at<int>(GAME_MODE) != 4) BackupGarage();
        at<int>(GAME_MODE) = 4;
        SetQuickRacePlayers(2);
        // input port N carries only the devices in DirectInput slot N, so the players' controller masks are fixed
        at<int>(FE_PLAYER) = 0;
        at<uint32_t>(FE_PORTS) = 1;
        at<uint32_t>(FE_PORTS + 4) = 2;
        ResetAssignments();
    } else {
        at<int>(GAME_MODE) = 2;
    }
}

static __declspec(naked) void QuickRaceModeHook() {
    __asm {
        pushfd                       // the jnz after the patched mov uses flags from the earlier test
        pushad
        call OnQuickRaceSelected
        popad
        popfd
        push AT_QR_SET_MODE_RET
        ret
    }
}

// Main menu "Split Screen" option (cut from PC, restored from the console menu): a Quick Race option whose react
// starts the quick race flow in split-screen mode.
static __declspec(naked) void SplitMenuReact() {
    __asm {
        cmp dword ptr [esp + 8], MSG_ACCEPT
        jne quickrace
        mov byte ptr g_menuSplit, 1
    quickrace:
        mov eax, FN_QR_REACT
        jmp eax
    }
}

static void* g_splitOptionVtbl[2] = {reinterpret_cast<void*>(FN_MENU_OPTION_DTOR), &SplitMenuReact};

// The option's text is the console's "2 Player Split-Screen" (MMSPLIT_TEXT); with 2-4 players it becomes
// "Split-Screen". The loaded language table (entries {hash, text*} sorted by hash at [0x8383D8], count [0x8383D0];
// lookup 0x4FF9D0) gets its entry pointed at a shortened text when it holds a known original; other languages keep
// theirs.
#define LANGUAGE_TABLE     0x8383D8
#define LANGUAGE_COUNT     0x8383D0
static void RenameSplitMenuText() {
    static const char* const kNames[][2] = {
        {"2 Player Split-Screen", "Split-Screen"},
        {"2 Spieler-Splitscreen", "Splitscreen"},
        {"Ecran partag\xE9 2 joueurs", "Ecran partag\xE9"},
        {"2 giocatori in split-screen", "Split-screen"},
        {"2 jugadores con pantalla dividida", "Pantalla dividida"},
        {"Twee spelers op gesplitst scherm", "Gesplitst scherm"},
        {"Opdelt sk\xE6rm for 2 spillere", "Opdelt sk\xE6rm"},
        {"2 spelare p\xE5 delad sk\xE4rm", "Delad sk\xE4rm"},
    };
    auto* table = at<uint32_t*>(LANGUAGE_TABLE);
    int lo = 0, hi = at<int>(LANGUAGE_COUNT) - 1;
    while (table && lo <= hi) {
        int mid = (lo + hi) / 2;
        uint32_t hash = table[mid * 2];
        if (hash < MMSPLIT_TEXT) { lo = mid + 1; continue; }
        if (hash > MMSPLIT_TEXT) { hi = mid - 1; continue; }
        const char* text = reinterpret_cast<const char*>(table[mid * 2 + 1]);
        for (const auto& name : kNames)
            if (text && strcmp(text, name[0]) == 0) table[mid * 2 + 1] = reinterpret_cast<uint32_t>(name[1]);
        return;
    }
}

static void __cdecl AddSplitMenuOption(void* menu) {
    using MallocFn = void*(__cdecl*)(size_t);
    using InitFn = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    using AddFn = void(__thiscall*)(void*, void*);
    RenameSplitMenuText();
    void* option = reinterpret_cast<MallocFn>(FN_MALLOC)(0x48);
    if (!option) return;
    reinterpret_cast<InitFn>(FN_MENU_OPTION_INIT)(option, MMSPLIT_ICON, MMSPLIT_TEXT, 0);
    *static_cast<void**>(option) = g_splitOptionVtbl;
    void** menuVtbl = *static_cast<void***>(menu);
    reinterpret_cast<AddFn>(menuVtbl[0x18 / 4])(menu, option);
}

static __declspec(naked) void MainMenuHook() {
    __asm {
        pushad
        push esi                     // the menu being built
        call AddSplitMenuOption
        add esp, 4
        popad
        push AT_MAINMENU_QR_RET      // the displaced call
        push FN_MAINMENU_CHECK
        ret
    }
}

// Controls: both joypad pages are titled "Joypad Configuration"; append the player the page belongs to (page N
// edits joystick slot N = the pad player N joins with; the extra pages of players 3/4, see ControlsPageSeat).
static int ControlsPageSeat();
static void UpdateBindSwap();
static void __cdecl SetJoypadPageTitle(void* title, uint32_t hash) {
    using SetHashFn = void(__cdecl*)(void*, uint32_t);
    using SetFmtFn = int(__cdecl*)(void*, const char*, ...);
    using LocalizeFn = const char*(__cdecl*)(uint32_t);
    UpdateBindSwap();                    // the page is being built: its record must hold its player's bindings now
    reinterpret_cast<SetHashFn>(FN_SET_TEXT_HASH)(title, hash);
    int device = at<int>(CONTROLS_PAGE_DEV + at<int>(CONTROLS_PAGE) * 12);
    if (!title || device < 0 || device >= INPUT_DEVICE_MAX) return;
    int seat = ControlsPageSeat();
    if (seat < 0) seat = at<int>(INPUT_DEVICES + device * 0x370 + 0x230);
    const char* text = reinterpret_cast<LocalizeFn>(FN_LOCALIZE)(hash);
    if (seat >= 0 && text)
        reinterpret_cast<SetFmtFn>(FN_SET_TEXT_FMT)(title, "%s - P%d", text, seat + 1);
}

static void WriteCall(uintptr_t where, const void* to) {
    uint8_t buf[5] = {0xE8};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (where + 5));
    memcpy(buf + 1, &rel, 4);
    Write(where, buf, 5);
}

// Once per frame. Leaving split screen must reset the 2P frontend state, otherwise the next single-player car
// select reads player 2's garage slot and crashes.
static void UpdateModeTransitions() {
    int mode = at<int>(GAME_MODE);
    if (g_prevMode == 4 && mode != 4) {
        ResetFrontendPlayers();
        ResetAssignments();
        SetQuickRacePlayers(1);
    }
    g_prevMode = mode;
}
