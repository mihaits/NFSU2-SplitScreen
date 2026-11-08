// Part of NFSU2SplitScreen.cpp: Next Race on the split Post Race menu.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- next race
// Post Race (builder 0x4BEE10, fastcall menu) adds its icon options with 0x51F670(option, icon, text, 0) and
// menu->vtbl[0x18](option): career's Next Race (icon 0x12BB5EA2, text "Next Race" 0x23F359B8), Restart, Race Stats,
// Replay (not in split) and Quit to Main Menu. In split, Next Race is added first. Its react sends
// UI_PostRace.fng what Quit's confirm dialog sends on OK (0x30ED2368); the screen's handler 0x4F39A0 then leaves the
// race (0x24DD40CF, 0xD78D7BF1 -> 0x4A3E80) for the screen 0x502FF0 names: the quick race mode select in split. Once
// that is up (with the garage scene and the race's mode selected), NextRaceTick accepts it like A would, which opens
// the track select of the same mode through the game's own screen chain (Back returns to the mode select). With
// g_nextRace set, the track select's next screen (0x498240) is the car select instead of the options, and the car
// select's player 1 popup takes the last round (seats.cpp, PressStartFilter): same players, pads and cars, random
// cars dealt again. Outrun has no track select (its track is random); the screen after its mode select is its
// options, which Next Race skips like any mode's: RandomTick accepts it by itself while g_nextRace is set.
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

static bool g_randomSeries = false;       // the race came from a mode roll: Next Race rolls mode and track again
static void StartModeRoll();
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
    if (g_randomSeries) { StartModeRoll(); return; }   // a random series: roll mode and track again
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
    if (at<int>(GAME_MODE) != 4) return;
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
// After Next Race Free Run is left out: it isn't a race.
#define AT_FREE_RUN_ENTRY 0x4B2F8A    // cmp eax, 8; je 0x4B2FE0 (5 bytes; eax = the game mode)
#define AT_MODE_LIST_END      0x4B2FE0    // after the Free Run entry
static __declspec(naked) void ModeListFreeRunHook() {
    __asm {
        cmp byte ptr g_nextRace, 0
        jne skip
        cmp eax, 8
        je skip
        push AT_FREE_RUN_ENTRY + 5
        ret
    skip:
        push AT_MODE_LIST_END
        ret
    }
}

// Random: Y on any pad, Space or a click on the help bar's "(Y) Random" (its Customize button, HelpBarSet) rolls.
// - On the split track select: one of the listed, unlocked tracks (not the track just raced), stepped to with the
//   screen's own left / right, one per frame, then accepted: after Next Race the race loads, otherwise the options
//   come next. The screen: [0x836344] - 0x118 (set by its ctor 0x4E2F30, vtable 0x7974F0); its icon list at +0x4C
//   (node: +0 next, +8 locked, +0x14 track id, +0x2C track record or 0, record +0x8A track id), the selected node +0x54.
// - On the split mode select: one of the seven race modes (entries 1-7: circuit, sprint, drag, drift, Street X, URL,
//   Outrun; in a series not the mode just raced), stepped to and accepted the same way; the track select that opens
//   rolls its track by itself. Outrun has no track select: its options screen comes up instead (accepted by itself
//   after Next Race, like any Outrun reached by it). That starts a random series: Next Race then rolls mode and track
//   again with no input (NextRaceTick). The series ends when the mode select comes up any other way (Quit, Back) or
//   at the main menu.
//   The highlighted entry: the frontend's per-screen table at 0x7F7DC8 (stride 0x1C up to 0x7F9178: +0 screen name,
//   +0x10 entry, 1-based, 0 = the first; read by 0x50B8A0 when a screen is built, updated as the player moves).
#define QR_TRACKSEL_NAME      0x7F4464    // char*: "UI_QRTrackSelect.fng"
#define QR_OPTIONS_NAME       0x7F4460    // char*: "UI_QRModeOptions.fng"
#define QR_TRACKSEL_SCREEN    0x836344    // screen + 0x118
#define QR_TRACKSEL_VTBL      0x7974F0
#define FE_ENTRY_TABLE        0x7F7DC8
#define FE_ENTRY_TABLE_END    0x7F9178
static const int kRollModeTypes[7] = {0, 1, 2, 3, 10, 4, 8};   // mode select entries 1-7
static int g_debugRollMode = 0;           // [DEBUG] RollMode = 1..7: mode rolls pick that entry (tests)
static bool g_randomClicked = false;      // that button, labelled Random on the mode / track select (SendFeMessage)
static bool g_randomTrackPending = false; // a mode roll was accepted: its track select rolls by itself
// The mode select's entries set the race type in their react (0x4B2C00(type): type 0x83AAB4 and its quick race
// settings) when A is pressed; the screen then goes on by that type (0x4FA470). An accept message skips the react, so a
// mode roll sets the type itself before accepting.
#define FN_QR_SET_MODE        0x4B2C00    // void __cdecl (race type)
static struct { const char* fng; int steps, accept, type; } g_roll = {};
static int TrackNodeId(uint8_t* node) {
    auto* record = *reinterpret_cast<uint8_t**>(node + 0x2C);
    return record ? *reinterpret_cast<int16_t*>(record + 0x8A) : *reinterpret_cast<int*>(node + 0x14);
}
static void StartRoll(const char* fng, int steps, int type = -1) {
    g_roll.fng = fng;
    g_roll.steps = steps;
    g_roll.accept = steps ? 0 : 3;
    g_roll.type = type;
}
static void StartModeRoll() {
    const char* modes = at<const char*>(QR_MODESEL_NAME);
    int current = 1;
    for (uintptr_t e = FE_ENTRY_TABLE; e < FE_ENTRY_TABLE_END; e += 0x1C) {
        const char* name = *reinterpret_cast<const char**>(e);
        if (name && _stricmp(name, modes) == 0) { current = *reinterpret_cast<int*>(e + 0x10); break; }
    }
    if (current < 1) current = 1;
    int pool[7], n = 0;
    for (int i = 0; i < 7; ++i)
        if (!(g_randomSeries && kRollModeTypes[i] == g_lastRaceType)) pool[n++] = i + 1;
    int target = pool[RandomInt(n)];
    if (g_debugRollMode >= 1 && g_debugRollMode <= 7) target = g_debugRollMode;
    g_randomSeries = g_randomTrackPending = true;
    StartRoll(modes, target - current, kRollModeTypes[target - 1]);
    Log("random mode: entry %d (type %d) from %d", target, kRollModeTypes[target - 1], current);
}
static void StartTrackRoll();
static void ResetSession(const char* why);
static void RandomTick() {
    using ActiveFn = bool(__cdecl*)(const char*);
    using SendFn = void(__cdecl*)(uint32_t, const char*);
    auto active = [](const char* fng) { return reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)(fng); };
    static bool spaceDown = false, wasOnModes = false;
    static int trackWait = 0;
    bool space = KeyPressed(VK_SPACE, spaceDown);
    bool clicked = g_randomClicked;
    g_randomClicked = false;
    const char* tracks = at<const char*>(QR_TRACKSEL_NAME);
    const char* modes = at<const char*>(QR_MODESEL_NAME);
    bool onTracks = SplitMode() && active(tracks), onModes = SplitMode() && active(modes);
    if (onModes && !wasOnModes && !g_nextRace && (g_randomSeries || g_randomTrackPending)) {
        g_randomSeries = g_randomTrackPending = false;   // the mode select came up by Quit or Back
        Log("random: series ended");
    }
    wasOnModes = onModes;
    if (!SplitMode() || active("UI_Main.fng")) {
        g_randomSeries = g_randomTrackPending = false;
        ResetSession("left split screen");     // standings.cpp
    }
    if (g_roll.fng && !active(g_roll.fng)) g_roll = {};
    if (g_roll.fng) {                          // one step per frame, like a player pressing left / right
        if (g_roll.steps) {
            // silent steps: the mode select plays its menu tick for each step message (the track select doesn't),
            // which at one step per frame is a buzz; the accept still sounds
            g_feSoundMuted = true;
            reinterpret_cast<SendFn>(FN_FE_SEND_MSG)(g_roll.steps > 0 ? MSG_RIGHT : MSG_LEFT, g_roll.fng);
            g_feSoundMuted = false;
            g_roll.steps += g_roll.steps > 0 ? -1 : 1;
            if (!g_roll.steps) g_roll.accept = 3;
        } else if (--g_roll.accept <= 0) {
            const char* fng = g_roll.fng;
            if (g_roll.type >= 0) reinterpret_cast<void(__cdecl*)(int)>(FN_QR_SET_MODE)(g_roll.type);
            g_roll = {};
            Log("random: %s accepted (type %d, track %d)", fng, at<int>(QR_RACE_TYPE),
                at<uint16_t>(QR_SETTINGS + at<int>(QR_RACE_TYPE) * 0x14 + 8));
            reinterpret_cast<SendFn>(FN_FE_SEND_MSG)(MSG_ACCEPT, fng);
        }
        return;
    }
    bool onOutrunOptions = SplitMode() && at<int>(QR_RACE_TYPE) == RACE_TYPE_OUTRUN && active(at<const char*>(QR_OPTIONS_NAME));
    if (onOutrunOptions && (g_nextRace || g_randomTrackPending)) {   // no track to pick or roll
        if (++trackWait < 3) return;           // let the screen take input first
        trackWait = 0;
        g_randomTrackPending = false;
        if (g_nextRace) {                      // Next Race skips the options: on with the same players
            Log("next race: Outrun options accepted");
            reinterpret_cast<SendFn>(FN_FE_SEND_MSG)(MSG_ACCEPT, at<const char*>(QR_OPTIONS_NAME));
        }
        return;
    }
    if (onTracks && g_randomTrackPending) {    // after a mode roll; let the screen take input first
        if (++trackWait < 3) return;
        trackWait = 0;
        g_randomTrackPending = false;
        StartTrackRoll();
        return;
    }
    trackWait = 0;
    bool y = space || clicked;
    for (int pad = 0; pad < XPadMax() && !y; ++pad) y = XPadYPressed(pad);
    if (!y) return;
    if (onTracks) StartTrackRoll();
    else if (onModes) StartModeRoll();
}
static void StartTrackRoll() {
    uint8_t* screen = at<uint8_t*>(QR_TRACKSEL_SCREEN);
    if (!screen) return;
    screen -= 0x118;
    if (at<uint32_t>(reinterpret_cast<uintptr_t>(screen)) != QR_TRACKSEL_VTBL) return;
    uint8_t* head = screen + 0x4C;
    uint8_t* current = *reinterpret_cast<uint8_t**>(screen + 0x54);
    int count = 0, currentIndex = -1, pool[128], n = 0;
    bool sameType = at<int>(QR_RACE_TYPE) == g_lastRaceType;
    for (auto* node = *reinterpret_cast<uint8_t**>(head); node && node != head && count < 128;
         node = *reinterpret_cast<uint8_t**>(node), ++count) {
        if (node == current) currentIndex = count;
        if (node[8]) continue;                 // locked
        if (sameType && TrackNodeId(node) == g_lastTrack) continue;
        pool[n++] = count;
    }
    if (!n || currentIndex < 0) return;
    int target = pool[RandomInt(n)];
    StartRoll(at<const char*>(QR_TRACKSEL_NAME), target - currentIndex);
    Log("random track: %d of %d tracks, %d steps", n, count, target - currentIndex);
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
    const uint8_t freeRunEntry[] = {0x83, 0xF8, 0x08, 0x74, 0x51};   // cmp eax, 8; je 0x4B2FE0
    if (memcmp(reinterpret_cast<void*>(AT_FREE_RUN_ENTRY), freeRunEntry, sizeof(freeRunEntry)) == 0)
        WriteJump(AT_FREE_RUN_ENTRY, &ModeListFreeRunHook, 5);
    else Log("next race: unexpected mode list code, Free Run stays listed after Next Race");
}
