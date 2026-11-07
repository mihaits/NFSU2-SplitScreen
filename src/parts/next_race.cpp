// Part of NFSU2SplitScreen.cpp: Next Race on the split Post Race menu.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- next race
// Post Race (builder 0x4BEE10, fastcall menu) adds its icon options with 0x51F670(option, icon, text, 0) and
// menu->vtbl[0x18](option): career's Next Race (icon 0x12BB5EA2, text "Next Race" 0x23F359B8), Restart, Race Stats,
// Replay (not in split) and Quit to Main Menu. In split, Next Race is added first (not in outrun). Its react sends
// UI_PostRace.fng what Quit's confirm dialog sends on OK (0x30ED2368); the screen's handler 0x4F39A0 then leaves the
// race (0x24DD40CF, 0xD78D7BF1 -> 0x4A3E80) for the screen 0x502FF0 names: the quick race mode select in split. Once
// that is up (with the garage scene and the race's mode selected), NextRaceTick accepts it like A would, which opens
// the track select of the same mode through the game's own screen chain (Back returns to the mode select). With
// g_nextRace set, the track select's next screen (0x498240) is the car select instead of the options, and the car
// select's player 1 popup takes the last round (seats.cpp, PressStartFilter): same players, pads and cars, random
// cars dealt again.
#define AT_POSTRACE_BUILD     0x4BEE26    // `cmp dword ptr [0x850078], 1` (7 bytes), ecx = the menu
#define AT_POSTRACE_BUILD_RET 0x4BEE2D
#define FN_QR_TRACK_NEXT      0x498240    // the screen after the track select: `mov eax, [0x850078]` (5 bytes)
#define QR_CARSEL_NAME        0x7F446C    // char*: "UI_QRCarSelect.fng"
#define NEXT_RACE_ICON        0x12BB5EA2
#define NEXT_RACE_TEXT        0x23F359B8  // "Next Race" (localized)
#define MSG_POSTRACE_QUIT     0x30ED2368  // Quit to Main Menu confirmed
#define QR_MODESEL_NAME       0x7F445C    // char*: "UI_QRModeSelect.fng"

static bool g_nextRaceToTrack = false;    // Next Race chosen: accept the mode select once it's up

static void __fastcall NextRaceReact(void*, void*, const char*, uint32_t msg, uint32_t, uint32_t, uint32_t) {
    if (msg != MSG_ACCEPT) return;
    g_nextRace = g_nextRaceToTrack = true;
    Log("next race: leaving the race");
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(MSG_POSTRACE_QUIT, "UI_PostRace.fng");
}

static void NextRaceTick() {
    using ActiveFn = bool(__cdecl*)(const char*);
    static int frames = 0;
    if (g_nextRace && reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("UI_Main.fng")) {
        g_nextRace = false;                    // backed out to the main menu: the next split visit starts fresh
        Log("next race: main menu, ended");
    }
    if (!g_nextRaceToTrack || !g_nextRace) { g_nextRaceToTrack = false; frames = 0; return; }
    const char* modes = at<const char*>(QR_MODESEL_NAME);
    if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)(modes)) { frames = 0; return; }
    if (++frames < 3) return;                  // let the screen take input first
    g_nextRaceToTrack = false;
    frames = 0;
    Log("next race: mode select accepted");
    g_feSoundMuted = true;                     // the player already heard Next Race's accept
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(MSG_ACCEPT, modes);
    g_feSoundMuted = false;
}

static void* g_nextRaceVtbl[2] = {reinterpret_cast<void*>(FN_MENU_OPTION_DTOR), &NextRaceReact};

static void __cdecl AddNextRaceOption(void* menu) {
    using MallocFn = void*(__cdecl*)(size_t);
    using InitFn = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t);
    using AddFn = void(__thiscall*)(void*, void*);
    g_nextRace = false;
    if (at<int>(GAME_MODE) != 4 || at<int>(QR_RACE_TYPE) == 8) return;
    void* option = reinterpret_cast<MallocFn>(FN_MALLOC)(0x48);
    if (!option) return;
    reinterpret_cast<InitFn>(FN_MENU_OPTION_INIT)(option, NEXT_RACE_ICON, NEXT_RACE_TEXT, 0);
    *static_cast<void**>(option) = g_nextRaceVtbl;
    void** menuVtbl = *static_cast<void***>(menu);
    reinterpret_cast<AddFn>(menuVtbl[0x18 / 4])(menu, option);
}

static __declspec(naked) void PostRaceBuildHook() {
    __asm {
        pushad
        push ecx
        call AddNextRaceOption
        add esp, 4
        popad
        cmp dword ptr ds:[GAME_MODE], 1     // the displaced compare (its flags are used below)
        push AT_POSTRACE_BUILD_RET
        ret
    }
}

static __declspec(naked) void QrTrackNextHook() {
    __asm {
        cmp byte ptr g_nextRace, 0
        je normal
        mov eax, dword ptr ds:[QR_CARSEL_NAME]
        ret
    normal:
        mov eax, dword ptr ds:[GAME_MODE]
        push FN_QR_TRACK_NEXT + 5
        ret
    }
}

// Next Race skips the options screen, so a new mode takes the last race's options it also has. Quick race settings
// per type at QR_SETTINGS + type * 0x14; the split options rows of each type (measured; Lap KO from its row builder):
//   +0xC direction (dword)  circuit, sprint, drag, Street X, URL, Lap KO: when the track can be raced reversed
//   +0xA laps (word)        circuit, drift, Street X, URL
//   +4 traffic              circuit, sprint, drag
//   +5 catch-up             circuit, sprint, drag, Street X, URL, Lap KO
//   +2 AI difficulty        every type with AI opponents (players, cars and opponents are the plugin's rows)
// Types: 0 circuit, 1 sprint, 2 drag, 3 drift, 4 URL, 7 Lap KO, 10 Street X.
#define FN_TRACK_INFO         0x5D3E40    // track record* __cdecl (track id); +0x86 == 1: can be raced reversed
static void CarryRaceOptions(int from, int to) {
    struct Field { int off, size; uint32_t types; const char* name; };
    static const Field kFields[] = {
        {0xC, 4, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 10, "direction"},
        {0xA, 2, 1u << 0 | 1u << 3 | 1u << 4 | 1u << 10, "laps"},
        {0x4, 1, 1u << 0 | 1u << 1 | 1u << 2, "traffic"},
        {0x5, 1, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 10, "catch-up"},
        {0x2, 1, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4 | 1u << 5 | 1u << 7 | 1u << 10, "difficulty"},
    };
    if (from == to || from < 0 || from >= QR_RACE_TYPES || to < 0 || to >= QR_RACE_TYPES) return;
    uint8_t* src = reinterpret_cast<uint8_t*>(QR_SETTINGS + from * 0x14);
    uint8_t* dst = reinterpret_cast<uint8_t*>(QR_SETTINGS + to * 0x14);
    for (const Field& f : kFields) {
        if (!(f.types >> from & 1) || !(f.types >> to & 1)) continue;
        if (f.off == 0xC && *reinterpret_cast<int*>(src + 0xC)) {
            uint8_t* track = reinterpret_cast<uint8_t*(__cdecl*)(int)>(FN_TRACK_INFO)(*reinterpret_cast<uint16_t*>(dst + 8));
            if (!track || track[0x86] != 1) continue;
        }
        memcpy(dst + f.off, src + f.off, f.size);
        Log("next race: %s from type %d to %d", f.name, from, to);
    }
}

// The mode list (UIQRModeSelect::Setup) adds Outrun (0x4B2F2A..0x4B2F85) and Free Run (0x4B2F8A..0x4B2FDD) last.
// After Next Race both are left out: Outrun is a tournament of its own and Free Run isn't a race.
#define AT_MODE_LIST_OUTRUN   0x4B2F2A    // mov eax, [0x850078] (5 bytes)
#define AT_MODE_LIST_END      0x4B2FE0    // after the Free Run entry
static __declspec(naked) void ModeListOutrunHook() {
    __asm {
        cmp byte ptr g_nextRace, 0
        je normal
        push AT_MODE_LIST_END
        ret
    normal:
        mov eax, dword ptr ds:[GAME_MODE]
        push AT_MODE_LIST_OUTRUN + 5
        ret
    }
}

static void InstallNextRace() {
    const uint8_t build[] = {0x83, 0x3D, 0x78, 0x00, 0x85, 0x00, 0x01};
    const uint8_t trackNext[] = {0xA1, 0x78, 0x00, 0x85, 0x00};
    if (memcmp(reinterpret_cast<void*>(AT_POSTRACE_BUILD), build, sizeof(build)) != 0 ||
        memcmp(reinterpret_cast<void*>(FN_QR_TRACK_NEXT), trackNext, sizeof(trackNext)) != 0) {
        Log("next race: unexpected code, not installed");
        return;
    }
    WriteJump(AT_POSTRACE_BUILD, &PostRaceBuildHook, 7);
    WriteJump(FN_QR_TRACK_NEXT, &QrTrackNextHook, 5);
    if (memcmp(reinterpret_cast<void*>(AT_MODE_LIST_OUTRUN), trackNext, sizeof(trackNext)) == 0)
        WriteJump(AT_MODE_LIST_OUTRUN, &ModeListOutrunHook, 5);
    else Log("next race: unexpected mode list code, Outrun and Free Run stay listed");
}
