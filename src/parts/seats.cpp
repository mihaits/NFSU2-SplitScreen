// Part of NFSU2SplitScreen.cpp: players 3-6 data, controller assignment, car select seats.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- players 3-6: their own data
// The game keeps per-player data for 2 players only (profile blocks, player options, joypad records, picked car).
// Players 3-6 get their own copy of each, kept in NFSU2SplitScreen_Players.ini next to the plugin:
//  - player options: an entry like the game's per-player settings (0x83AA64 + i*0x28: camera, HUD, feedback, ...);
//  - joypad bindings: the binding part of a joypad device record (action tables +0x18 and axis setup +0x344, the
//    data the profile saves per device, see the Controls section);
//  - the car they picked last.
// A seat with nothing stored yet starts from player 1's / player 2's (settings) or player 1's (bindings).
#define PLAYER_SETTING_SIZE 0x28
#define REC_BIND_TABLES    0x18          // 4 action tables x 32 inputs (16 buttons, then axis directions / POV)
#define REC_BIND_TABLES_SIZE 0x200
#define REC_BIND_AXES      0x344         // axis offsets / scales / enable flags (+0x344..+0x368)
#define REC_BIND_AXES_SIZE 0x25
struct SeatStore {
    bool hasSettings, hasBindings;
    uint8_t settings[PLAYER_SETTING_SIZE];
    uint8_t tables[REC_BIND_TABLES_SIZE];
    uint8_t axes[REC_BIND_AXES_SIZE];
    int car;                             // garage car id (0 = none)
};
static SeatStore g_seatStore[MAX_EXTRA_PLAYERS];   // players 3..6
static char g_playersPath[MAX_PATH];

static void ToHex(const uint8_t* p, size_t n, char* out) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = digits[p[i] >> 4]; out[2 * i + 1] = digits[p[i] & 15]; }
    out[2 * n] = 0;
}

static bool FromHex(const char* s, uint8_t* p, size_t n) {
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
                                      c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    if (strlen(s) != 2 * n) return false;
    for (size_t i = 0; i < n; ++i) {
        int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        p[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

static void LoadSeatStore() {
    static char buf[2 * REC_BIND_TABLES_SIZE + 16];
    for (int i = 0; i < MAX_EXTRA_PLAYERS; ++i) {
        SeatStore& s = g_seatStore[i];
        char section[16];
        sprintf_s(section, "Player%d", i + 3);
        GetPrivateProfileStringA(section, "Settings", "", buf, sizeof(buf), g_playersPath);
        s.hasSettings = FromHex(buf, s.settings, sizeof(s.settings));
        GetPrivateProfileStringA(section, "JoypadTables", "", buf, sizeof(buf), g_playersPath);
        bool tables = FromHex(buf, s.tables, sizeof(s.tables));
        GetPrivateProfileStringA(section, "JoypadAxes", "", buf, sizeof(buf), g_playersPath);
        s.hasBindings = tables && FromHex(buf, s.axes, sizeof(s.axes));
        GetPrivateProfileStringA(section, "Car", "0", buf, sizeof(buf), g_playersPath);
        s.car = static_cast<int>(strtoul(buf, nullptr, 16));
    }
}

static void SaveSeatStore(int i) {
    static char buf[2 * REC_BIND_TABLES_SIZE + 16];
    const SeatStore& s = g_seatStore[i];
    char section[16];
    sprintf_s(section, "Player%d", i + 3);
    if (s.hasSettings) { ToHex(s.settings, sizeof(s.settings), buf); WritePrivateProfileStringA(section, "Settings", buf, g_playersPath); }
    if (s.hasBindings) {
        ToHex(s.tables, sizeof(s.tables), buf);
        WritePrivateProfileStringA(section, "JoypadTables", buf, g_playersPath);
        ToHex(s.axes, sizeof(s.axes), buf);
        WritePrivateProfileStringA(section, "JoypadAxes", buf, g_playersPath);
    }
    sprintf_s(buf, "%08X", static_cast<unsigned>(s.car));
    WritePrivateProfileStringA(section, "Car", buf, g_playersPath);
}

// ---------------------------------------------------------------- controller assignment
// Console-style: every "Player N Press START" popup waits for Start on a pad that hasn't joined yet; that pad
// becomes player N's controller (the virtual joystick slot N reads it, see PadForDevice). Keyboard Enter joins any
// player like a pad (one keyboard player per round; see KeyboardPort). Outside split screen every pad works and the
// last one used drives.
#define PAD_NONE -1
static int g_assignedPad[MAX_LOCAL_PLAYERS] = {PAD_NONE, PAD_NONE, PAD_NONE, PAD_NONE, PAD_NONE, PAD_NONE};
static int g_keyboardSeat = -1;          // the player who joined with the keyboard this round (-1: nobody)
// The last round that started a race (RememberRound): Y / Space on player 1's Press START popup gives every player
// their pad, car and gearbox from it again and starts the race (QuickStart).
static int g_lastPlayers = 0;            // 0: no round yet
static int g_lastPad[MAX_LOCAL_PLAYERS], g_lastKeyboardSeat = -1;
static int g_lastCar[MAX_LOCAL_PLAYERS], g_lastPort[MAX_LOCAL_PLAYERS], g_lastGearbox[MAX_LOCAL_PLAYERS];
// Next Race (Post Race, next_race.cpp): set when it's chosen, until the car select's player 1 popup takes the last
// round by itself (or can't: then the round is picked as usual).
static bool g_nextRace = false;
static int g_lastRaceType = -1;          // quick race type (0x83AAB4) of the last round
static int g_lastTrack = -1;             // and its track
static void CarryRaceOptions(int from, int to);   // next_race.cpp

static int g_seat = 0;                   // car select: whose turn (0..5); the frontend serves FE player seat & 1
static int g_splitPlayers = 2;           // players chosen on the split-screen options screen (2..6)
static int g_splitOpponents = 0;         // AI opponents chosen there (see SplitOpponents)
// "Cars" row: each player picks (the car select as it is), everyone races player 1's pick, or everyone gets a random
// car of about the same performance. With the last two a player's turn is only the join (AutoPick).
enum { CARS_EACH, CARS_SAME, CARS_RANDOM, CARS_MODES };
static int g_carsMode = CARS_EACH;
static bool CarsAuto(int seat) { return g_carsMode == CARS_RANDOM || (g_carsMode == CARS_SAME && seat > 0); }
static bool g_autoSeat[MAX_LOCAL_PLAYERS];   // this round, the seat's car was given (no pick screen)
static bool g_autoPrompted[MAX_LOCAL_PLAYERS];   // and the gearbox dialog came up for it (AutoPickTick)
static int g_autoPickSeat = -1;          // joined just now: its car is given on the next car select tick
static int g_autoBackSeat = -1;          // Back returned to a seat that has no pick: its join is undone too
static int g_firstCar = 0;               // player 1's pick of this round (CARS_SAME)
static int g_randomCar[MAX_LOCAL_PLAYERS];
static const char* const kCarsModeText[CARS_MODES] = {"Pick Each", "Pick One", "Random"};
#define MAX_RACERS 6                     // the race start placement reads 6 start spots (0x5EB5D0(6, out))
static int SplitPlayers() { return g_splitPlayers; }

static void ResetAssignments() {
    for (int& pad : g_assignedPad) pad = PAD_NONE;
    g_keyboardSeat = -1;
}

// The pause screen (ctor 0x4EB410) only accepts input from the pausing player: package input mask =
// 0x50D2D0(port of the pausing player [0x8900D8]+0x20) at 0x4EB469. Frontend input only ever comes from joystick
// slots 0/1 (masks 1/2), so a pause by players 3-6 (ports 2-5, masks 4-32) locked the menu (user report). Their pad
// drives slot (port & 1) while they own the pause, and the mask is that slot's.
#define AT_PAUSE_MASK_CALL 0x4EB469
#define FN_PORT_MASK       0x50D2D0      // int __cdecl (port) -> 1 << port
// The Start press that paused is still held when the menu starts reading the owner's pad through slot (port & 1);
// that slot never saw it held, so the menu took it as a new Start and closed again (user: P3's pause opened and closed
// at once). The owner's pad stays idle on the menu until all its buttons are released.
static bool g_pauseOwnerReleased = false;
static void SyncPlayerSettings(bool newRace);
static int __cdecl PauseInputMask(int port) {
    g_pauseOwner = SplitRacing() && port >= 2 && port < MAX_LOCAL_PLAYERS ? port : -1;
    g_pausePort = SplitRacing() && port >= 0 && port < MAX_LOCAL_PLAYERS ? port : -1;
    g_pauseOwnerReleased = false;
    g_pauseSeen = false;
    SyncPlayerSettings(false);           // the pause menus show the pausing player's own settings
    if (g_pauseOwner >= 0) Log("pause: player %d's pad drives the menu as slot %d", port + 1, port & 1);
    return reinterpret_cast<int(__cdecl*)(int)>(FN_PORT_MASK)(g_pauseOwner >= 0 ? port & 1 : port);
}

// Car select: the seat whose pad / keyboard drives the frontend. On a later seat's Press START popup that is the
// previous seat: joining is read from XInput directly, so the frontend only needs it for Back, which only the player
// who joined last may press. The popup's own seat may still hold a stale pad / keyboard from going back. Player 1's
// popup starts a new round: -1 = any pad or the keyboard (the previous round's pads no longer count).
static int CarselDriverSeat() {
    int fe = at<int>(FE_PLAYER);
    int seat = (g_seat & 1) == fe ? g_seat : fe;
    if (reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng"))
        return seat - 1;
    return seat;
}

// Pad takeover: a player's pad that turns off (battery, range) keeps its seat until it comes back. In a split race,
// START on a connected pad no player has gives it the seat of the first player whose pad is off. It reads idle until
// its buttons are released, so the same START press doesn't pause.
static int g_takeoverPad = PAD_NONE;
static int RacePad(int k) {
    int pad = g_assignedPad[k];
    return pad != PAD_NONE && pad == g_takeoverPad ? PAD_NONE : pad;
}
static void UpdatePadTakeover() {      // per frame, after XPadUpdate
    uint16_t buttons; uint8_t lt, rt; int16_t lx;
    if (g_takeoverPad != PAD_NONE && (!XPadRawState(g_takeoverPad, &buttons, &lt, &rt, &lx) || !buttons))
        g_takeoverPad = PAD_NONE;
    if (!SplitRacing()) return;
    int players = 2 + g_extraPlayers < MAX_LOCAL_PLAYERS ? 2 + g_extraPlayers : MAX_LOCAL_PLAYERS;
    int lost = -1;
    for (int k = 0; k < players && lost < 0; ++k)
        if (g_assignedPad[k] != PAD_NONE && !XPadRawState(g_assignedPad[k], &buttons, &lt, &rt, &lx)) lost = k;
    if (lost < 0) return;
    for (int pad = 0; pad < XPadMax(); ++pad) {
        bool taken = false;
        for (int k = 0; k < players; ++k) taken |= g_assignedPad[k] == pad;
        if (taken || !XPadStartPressed(pad)) continue;
        Log("player %d's pad %d is off: pad %d takes over", lost + 1, g_assignedPad[lost] + 1, pad + 1);
        g_assignedPad[lost] = pad;
        if (lost < g_lastPlayers) g_lastPad[lost] = pad;
        g_takeoverPad = pad;
        return;
    }
}

static int __cdecl PadForDevice(int device) {
    if (device < 0 || device > 1) return PAD_NONE;
    // outside split the last pad used drives; joystick slot 2 only gets it while its bindings are being edited
    // (otherwise both slots' bindings would apply in 1P)
    if (!SplitMode()) return device == 0 || g_editingSlot2 ? XPadLastActive() : PAD_NONE;
    // in the split-screen menus only the player whose turn it is controls the frontend
    if (SplitRacing()) {
        if (g_pauseOwner < 0 || device != (g_pauseOwner & 1)) return RacePad(device);
        int pad = RacePad(g_pauseOwner);
        if (!g_pauseOwnerReleased) {
            uint16_t buttons = 0; uint8_t lt, rt; int16_t lx;
            if (XPadRawState(pad, &buttons, &lt, &rt, &lx) && buttons) return PAD_NONE;
            g_pauseOwnerReleased = true;
        }
        return pad;
    }
    // split menus: only the player whose turn it is; their own pad on the car select once joined, otherwise
    // (mode/track/options, Press START popups) the last pad used
    // car select: the player whose turn it is always drives joystick slot 0 (slot 1's bindings lack the frontend's
    // LB/RB category switch -> player 2 couldn't change categories); CarSelectHook tags the messages with their port
    if (g_inCarSelect) {
        if (device != 0) return PAD_NONE;
        int seat = CarselDriverSeat();
        if (seat < 0) return XPadLastActive();                  // player 1's popup: anyone
        if (seat == g_keyboardSeat) return PAD_NONE;            // the keyboard player's turn: keyboard only
        int pad = g_assignedPad[seat];
        return pad != PAD_NONE ? pad : XPadLastActive();
    }
    if (device != at<int>(FE_PLAYER)) return PAD_NONE;
    return XPadLastActive();
}

#define FN_PLAY_FE_SOUND   0x50D180      // void __cdecl (sound id; -1 = none)

static bool KeyPressed(int vk, bool& wasDown) {
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0 && GetForegroundWindow() == FindWindowA("GameFrame", nullptr);
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}
static bool KeyboardEnterPressed() { static bool wasDown = false; return KeyPressed(VK_RETURN, wasDown); }
static bool KeyboardSpacePressed() { static bool wasDown = false; return KeyPressed(VK_SPACE, wasDown); }

// Last round again: only with the same number of players, every pad of that round connected and every car still in
// the garage.
static bool GarageHasCar(int id);
static bool g_quickStart = false;        // player 1 joined through Y / Space: CarselFilter starts the race
static bool LastRoundReady() {
    if (!g_lastPlayers || g_lastPlayers != SplitPlayers()) return false;
    for (int k = 0; k < g_lastPlayers; ++k) {
        uint16_t buttons; uint8_t lt, rt; int16_t lx;
        if (k != g_lastKeyboardSeat && !XPadRawState(g_lastPad[k], &buttons, &lt, &rt, &lx)) return false;
        if (!GarageHasCar(g_lastCar[k])) return false;
    }
    return true;
}

// 2P_PressStart handler filter (thiscall: this, msg, p3, port). The PC frontend never sends START itself in a
// usable way (all input is merged and tagged as player 1), so joins are detected here from XInput directly and the
// game's own START/accept messages are ignored. Returns the message the handler should see.
static bool __stdcall CarselFilter(uint8_t* carsel, uint32_t msg);
static uint8_t* g_carsel = nullptr;      // the car select of the current round (set when switching seats)
static int g_autoJoinSeat = -1;           // back from customize: this seat's Press START popup accepts by itself

// A join (not the rematch, not the rejoin after customize): with a given car (Cars row) the seat's car comes on the
// next car select tick instead of its pick screen. Player 1's join starts the round: random cars are dealt then.
static void PickRandomCars(int players);
static void JoinedSeat(int seat) {
    if (seat == 0) {
        for (bool& a : g_autoSeat) a = false;
        for (bool& a : g_autoPrompted) a = false;
        if (g_carsMode == CARS_RANDOM) PickRandomCars(SplitPlayers());
    }
    g_autoSeat[seat] = CarsAuto(seat);
    g_autoPickSeat = g_autoSeat[seat] ? seat : -1;
    Log("cars: %s, player %d %s", kCarsModeText[g_carsMode], seat + 1, g_autoSeat[seat] ? "gets a car" : "picks");
}

static uint32_t __stdcall PressStartFilter(uint8_t* popup, uint32_t msg, uint32_t* port) {
    int player = at<int>(FE_PLAYER);
    if (player < 0 || player > 1) return msg;
    int seat = (g_seat & 1) == player ? g_seat : player;      // players 3-6 join on FE players 1/2's popups
    if (msg == MSG_FRAME_TICK) {
        bool keyboard = KeyboardEnterPressed() && (seat == 0 || g_keyboardSeat < 0 || g_keyboardSeat >= seat);
        bool space = KeyboardSpacePressed();
        if (!popup[0x4C]) return msg;                 // first tick sets the popup's ready flag
        if (g_autoJoinSeat == seat) {
            g_autoJoinSeat = -1;
            *port = 1u << player;
            Log("player %d rejoined after customize", seat + 1);
            return MSG_START;
        }
        if (seat == 0 && g_nextRace) {            // the options screen was skipped: a new mode takes the last race's
            CarryRaceOptions(g_lastRaceType, at<int>(QR_RACE_TYPE));   // options, whoever joins
            if (!LastRoundReady()) {
                g_nextRace = false;
                Log("next race: the last round can't start again (players, pads or cars changed): joins as usual");
            }
        }
        if (seat == 0 && LastRoundReady()) {
            bool again = g_nextRace || (space && g_lastKeyboardSeat >= 0);
            for (int k = 0; k < g_lastPlayers && !again; ++k)
                again = k != g_lastKeyboardSeat && XPadYPressed(g_lastPad[k]);
            if (again && g_nextRace) {                // Next Race: random cars are dealt again, given cars are kept
                g_nextRace = false;
                if (g_carsMode == CARS_RANDOM) {
                    PickRandomCars(g_lastPlayers);
                    for (int k = 0; k < g_lastPlayers; ++k) if (g_randomCar[k]) g_lastCar[k] = g_randomCar[k];
                }
                Log("next race: %s", g_carsMode == CARS_RANDOM ? "new random cars" : "same cars");
            }
            if (again) {                              // player 1 joins; the race starts on the next car select tick
                for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) g_assignedPad[k] = k < g_lastPlayers ? g_lastPad[k] : PAD_NONE;
                g_keyboardSeat = g_lastKeyboardSeat;
                g_quickStart = true;
                *port = 1u << player;
                Log("last round again: %d players", g_lastPlayers);
                reinterpret_cast<void(__cdecl*)(int)>(FN_PLAY_FE_SOUND)(6);
                return MSG_START;
            }
        }
        for (int pad = 0; pad < XPadMax(); ++pad) {
            // player 1 joining starts a new round: any pad may join; later players can't take a joined pad
            bool taken = false;
            for (int k = 0; k < seat; ++k) taken |= pad == g_assignedPad[k];
            if (!XPadStartPressed(pad) || taken) continue;
            if (seat == 0) ResetAssignments();
            g_assignedPad[seat] = pad;
            if (g_keyboardSeat >= seat) g_keyboardSeat = -1;    // (left over from going back)
            *port = 1u << player;
            Log("player %d joined with pad %d", seat + 1, pad + 1);
            reinterpret_cast<void(__cdecl*)(int)>(FN_PLAY_FE_SOUND)(6);    // join sound (see FeSound)
            JoinedSeat(seat);
            return MSG_START;
        }
        if (keyboard) {
            if (seat == 0) ResetAssignments();
            g_assignedPad[seat] = PAD_NONE;
            g_keyboardSeat = seat;
            *port = 1u << player;
            Log("player %d joined with keyboard", seat + 1);
            reinterpret_cast<void(__cdecl*)(int)>(FN_PLAY_FE_SOUND)(6);    // join sound (see FeSound)
            JoinedSeat(seat);
            return MSG_START;
        }
        return msg;
    }
    if (msg == MSG_START || msg == MSG_ACCEPT || msg == MSG_PC_ACTIVATE) return 0;   // ignored by the handler
    if (msg == MSG_BACK && seat >= 2 && !(seat & 1) && g_carsel && CarselFilter(g_carsel, msg)) return 0;   // back to P2 / P4
    return msg;
}

static __declspec(naked) void PressStartHook() {
    __asm {
        pushad
        lea eax, [esp + 0x2C]            // &port (msg/p3/port at esp+24/28/2C after pushad)
        push eax
        push dword ptr [esp + 0x28]      // msg (+4 for the previous push)
        push ecx                         // popup (this)
        call PressStartFilter            // stdcall
        mov dword ptr [esp + 0x24], eax  // msg
        popad
        mov eax, dword ptr [esp + 4]     // stolen: mov eax,[esp+4] ; cmp eax,MSG_BACK
        cmp eax, MSG_BACK
        push AT_PRESSSTART_RET           // push/ret keep the flags for the jz that follows
        ret
    }
}

// The PC frontend reads one merged input and tags every message with controller mask 1. In split screen,
// retag messages with the mask of the player the frontend currently serves (PollPortHook makes sure only that
// player's devices reach the frontend), so player 2's screens accept them.
static void __stdcall LogCarSelectMessage(uint32_t msg, uint32_t p3, uint32_t port) {
    if (msg != MSG_FRAME_TICK && msg != 0x32C72D8F && msg != 0xFC946BFA)
        Log("carsel msg=%08X p3=%08X port=%X feplayer=%d", msg, p3, port, at<int>(FE_PLAYER));
}

static __declspec(naked) void CarSelectHook() {
    __asm {
        pushad
        push dword ptr [esp + 0x2C]      // port (msg/p3/port at esp+24/28/2C after pushad)
        push dword ptr [esp + 0x2C]      // p3   (+4)
        push dword ptr [esp + 0x2C]      // msg  (+8)
        call LogCarSelectMessage         // (clobbers ecx)
        push dword ptr [esp + 0x24]      // msg
        push dword ptr [esp + 0x1C]      // car select (ecx saved by pushad at +0x18, +4 for the push)
        call CarselFilter                // handled -> the game's handler is skipped
        test al, al
        popad
        jz unhandled
        ret 0x10
    unhandled:
        cmp dword ptr ds:[GAME_MODE], 4
        jne original
        push eax                         // thiscall: msg, p3, port -> retag port whatever the source
        mov eax, dword ptr ds:[FE_PLAYER]
        cmp eax, 1
        ja restore
        mov eax, dword ptr [FE_PORTS + eax * 4]
        cmp eax, 0xFFFFFFFF
        jne store
        xor eax, eax                     // player not joined yet (ports are reset after a race): port 0 = any;
    store:                               // keeping 1 made the car select drop everything -> P2 Back softlock
        mov dword ptr [esp + 0x10], eax  // port (+4 for the push)
    restore:
        pop eax
    original:
        push ebp                         // stolen: push ebp ; mov ebp,[esp+10h]
        mov ebp, dword ptr [esp + 0x10]
        push AT_CARSELECT_RET
        ret
    }
}

// ---------------------------------------------------------------- 3-6 players: seats in the car select
// The car select serves FE players 0/1 (0x4FA2A0(player) switches: FE_PLAYER, title, car list; 0x4FC3A8/0x4FC9B6
// switch to player 2 after player 1's pick, 0x4FC723 goes back; 0x4FC3BE/0x4FC9C2 start the race (0x53FC20) after
// player 2's pick). With 3-6 players the picks continue: seats 2 / 4 (players 3 / 5) on FE player 0, seats 3 / 5
// (players 4 / 6) on FE player 1, picking from the shared garage. Each pick (car id 0x83AAB8[fe], port 0x83A9D8[fe]) is
// saved per seat and player 1's / player 2's own restored; players 3-6's racers get theirs (CreateHumanRacers).
#define FN_FE_PRINTF       0x537BE0      // void __cdecl (obj, fmt, ...): sets the object's text
#define FN_CARSEL_SWITCH   0x4FA2A0      // void __thiscall (carsel; fePlayer)
#define FN_START_RACE      0x53FC20      // void __cdecl ()
#define CAR_IDS            0x83AAB8      // int [2]: car of each player (in that player's garage)
#define PLAYER_PORTS       0x83A9D8      // int [2]: input port mask of each player
static const uintptr_t kCarselToP2Calls[] = {0x4FC3A8, 0x4FC9B6};
static const uintptr_t kCarselStartCalls[] = {0x4FC3BE, 0x4FC9C2};
#define AT_CARSEL_BACK_TO_P1 0x4FC723
static int g_seatCar[MAX_LOCAL_PLAYERS], g_seatPort[MAX_LOCAL_PLAYERS];
// The gearbox dialog after a pick writes the FE player's settings +0x15 (0x4FC267 / 0x4FC5B3): kept per seat as well
#define SETTING_GEARBOX    0x15
#define GEARBOX_SETTINGS   (0x83AA64 + SETTING_GEARBOX)   // FE player k's at + k * PLAYER_SETTING_SIZE
#define GEARBOX_ASK        0x83AA6A      // FE player k's at + k * PLAYER_SETTING_SIZE: a pick asks manual / automatic (0x4FC3DA)
#define FN_CARSEL_GEARBOX_DIALOG 0x497D50 // __thiscall (carsel): opens the gearbox dialog
static int g_seatGearbox[MAX_LOCAL_PLAYERS];   // + 1 (0: no pick yet)

// Players 1/2's last cars are kept by the game (CAR_IDS in their profile blocks); players 3-6's in their seat store.
// A car id is only used while it is still in the (shared) garage: P1's quick race garage car list, entries with the
// id at +8 (the lookup of 0x525FBB).
#define GARAGE_CARS        0x844968      // P1's block 0x83A9E0 + garage 0x3B0 + car list 0x9BD8: entry* [0x5B]
static bool GarageHasCar(int id) {
    for (int i = 0; id && i < 0x5B; ++i) {
        const uint8_t* car = reinterpret_cast<const uint8_t* const*>(GARAGE_CARS)[i];
        if (car && *reinterpret_cast<const int*>(car + 8) == id) return true;
    }
    return false;
}

// Random cars (CARS_RANDOM): the cars the car select offers in its categories Stock (1), My Cars (2), Career (4) and
// bonus (8), by the game's own tests (list builder 0x4E22E0, category count 0x534850): entry +0x10 set, car type
// (vtable slot 1) valid, its car type record ([0x8A1CCC] + type * 0x890) usable (+0x840 slot 0x5022C0, 0x610180), and
// unlocked (0x534180(context 0x497DA0(), id, 0)); a career car instead needs its +0x7F5.
// The rating is the game's performance rating of the car as built, upgrades included (`CarRating`). Entry +0xC holds a
// rating too, but a My Cars / career entry's setup (0x516B90) rates a fresh stock build of the car type
// (0x610270(type, 1, 0, 0)) and nothing rates it again, so a maxed career car kept its stock rating. `CarRating` does
// what that setup does with the entry's own build instead: RideInfo ctor 0x5AC6E0 (on ride +0x10), the entry's vtable
// slot 3 (thiscall (entry; mode 0, ride, 0): the build from its parts, 0x516AC0 for My Cars / career, 0x503A80 stock,
// 0x5039D0 bonus), 0x5B6990(ride +0x10; 0, -1.0f) and the release 0x4022C0 (ride +0x384).
#define GARAGE_CAR_TYPES   0x8A1CCC      // car type records, stride 0x890
#define FN_RIDE_CTOR       0x5AC6E0
#define FN_CAR_RATING      0x5B6990
#define FN_RIDE_RELEASE    0x4022C0
static float CarRating(uint8_t* car) {
    alignas(16) static uint8_t ride[0x1000];
    memset(ride, 0, sizeof(ride));
    auto* vt = *reinterpret_cast<uintptr_t**>(car);
    reinterpret_cast<void(__thiscall*)(void*)>(FN_RIDE_CTOR)(ride + 0x10);
    reinterpret_cast<void(__thiscall*)(void*, int, void*, int)>(vt[3])(car, 0, ride, 0);
    float rating = reinterpret_cast<float(__thiscall*)(void*, int, float)>(FN_CAR_RATING)(ride + 0x10, 0, -1.0f);
    reinterpret_cast<void(__thiscall*)(void*)>(FN_RIDE_RELEASE)(ride + 0x384);
    return rating;
}
static int CarPool(int* ids, float* ratings, int max) {
    int ctx = reinterpret_cast<int(__cdecl*)()>(0x497DA0)();
    int n = 0;
    for (int i = 0; i < 0x5B && n < max; ++i) {
        uint8_t* car = reinterpret_cast<uint8_t* const*>(GARAGE_CARS)[i];
        if (!car || !*reinterpret_cast<int*>(car + 0x10)) continue;
        int flags = *reinterpret_cast<int*>(car + 0x14), id = *reinterpret_cast<int*>(car + 8);
        if (!(flags & 0xF)) continue;
        auto* vt = *reinterpret_cast<uintptr_t**>(car);
        int type = reinterpret_cast<int(__thiscall*)(void*)>(vt[1])(car);
        if (type < 0) continue;
        uint8_t* rec = at<uint8_t*>(GARAGE_CAR_TYPES) + type * 0x890;
        int slot = *reinterpret_cast<int*>(rec + 0x840);
        if (slot == -1 || !reinterpret_cast<bool(__cdecl*)(int)>(0x5022C0)(slot) ||
            !reinterpret_cast<bool(__thiscall*)(void*)>(0x610180)(rec)) continue;
        bool open = flags & 4 ? car[0x7F5] != 0 : reinterpret_cast<bool(__cdecl*)(int, int, int)>(0x534180)(ctx, id, 0);
        float rating = open ? CarRating(car) : 0.0f;
        if (g_debug && open) Log("random cars: %08X (category %d) rating %.1f (stored %.1f)", id, flags, rating,
                                 *reinterpret_cast<float*>(car + 0xC));
        if (!open || !(rating > 0)) {
            Log("random cars: %08X (category %d) left out: %s", id, flags, open ? "no rating" : "locked");
            continue;
        }
        ids[n] = id, ratings[n] = rating, ++n;
    }
    return n;
}

static uint32_t RandomInt(uint32_t n) {         // xorshift, seeded from the clock
    static uint32_t s = 0;
    if (!s) s = GetTickCount() | 1;
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    return n ? s % n : 0;
}

// One car at random, then the cars within 4% of its rating (doubling until there are enough for everyone); each player
// gets a different one of them while there are enough.
static void PickRandomCars(int players) {
    static int ids[0x5B], near_[0x5B];
    static float ratings[0x5B];
    int n = CarPool(ids, ratings, 0x5B);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) g_randomCar[k] = 0;
    if (!n) { Log("random cars: no car available"); return; }
    float anchor = ratings[RandomInt(n)], tol = 0.04f;
    int m = 0;
    for (;;) {
        m = 0;
        for (int i = 0; i < n; ++i)
            if (fabsf(ratings[i] - anchor) <= tol * anchor) near_[m++] = i;
        if (m >= players || tol > 1.0f) break;
        tol *= 2;
    }
    for (int i = m - 1; i > 0; --i) { int j = RandomInt(i + 1), t = near_[i]; near_[i] = near_[j]; near_[j] = t; }
    for (int k = 0; k < players && k < MAX_LOCAL_PLAYERS; ++k) {
        int i = near_[k % m];
        g_randomCar[k] = ids[i];
        Log("random cars: player %d gets %08X (rating %.1f, anchor %.1f +-%.0f%%, %d of %d cars)", k + 1, ids[i],
            ratings[i], anchor, tol * 100, m, n);
    }
}

static void SaveSeat(int seat) {
    g_seatCar[seat] = at<int>(CAR_IDS + (seat & 1) * 4);
    g_seatPort[seat] = at<int>(PLAYER_PORTS + (seat & 1) * 4);
    // a given car without the gearbox dialog: players 1/2 keep their own setting (their FE slot), players 3-6 theirs
    // (0 = their player settings, see CreatePlayers); with the dialog the answer is in the FE slot, as for a pick
    g_seatGearbox[seat] = g_autoSeat[seat] && !g_autoPrompted[seat] && seat >= 2 ? 0
                        : at<uint8_t>(GEARBOX_SETTINGS + (seat & 1) * PLAYER_SETTING_SIZE) + 1;
    if (seat >= 2 && g_seatStore[seat - 2].car != g_seatCar[seat]) {
        g_seatStore[seat - 2].car = g_seatCar[seat];
        SaveSeatStore(seat - 2);
    }
    Log("player %d picked car %08X (port %d)", seat + 1, g_seatCar[seat], g_seatPort[seat]);
}

static void RestoreSeat(int seat) {            // players 1/2's picks back into the game's slots
    at<int>(CAR_IDS + seat * 4) = g_seatCar[seat];
    at<int>(PLAYER_PORTS + seat * 4) = g_seatPort[seat];
    if (g_seatGearbox[seat])
        at<uint8_t>(GEARBOX_SETTINGS + seat * PLAYER_SETTING_SIZE) = static_cast<uint8_t>(g_seatGearbox[seat] - 1);
}

// Called right before a split race starts from the car select: players 1/2's picks are in the game's slots (restored),
// the others' in the seat arrays.
static void RememberRound(int players) {
    g_lastPlayers = players;
    g_lastRaceType = at<int>(QR_RACE_TYPE);
    g_lastTrack = g_lastRaceType >= 0 && g_lastRaceType < QR_RACE_TYPES ?
                  at<uint16_t>(QR_SETTINGS + g_lastRaceType * 0x14 + 8) : -1;
    g_lastKeyboardSeat = g_keyboardSeat;
    for (int k = 0; k < players; ++k) {
        g_lastPad[k] = g_assignedPad[k];
        g_lastCar[k] = k < 2 ? at<int>(CAR_IDS + k * 4) : g_seatCar[k];
        g_lastPort[k] = k < 2 ? at<int>(PLAYER_PORTS + k * 4) : g_seatPort[k];
        g_lastGearbox[k] = k < 2 ? at<uint8_t>(GEARBOX_SETTINGS + k * PLAYER_SETTING_SIZE) + 1 : g_seatGearbox[k];
        Log("race: player %d car %08X gearbox %d", k + 1, g_lastCar[k], g_lastGearbox[k] - 1);
    }
}

static void StartSplitRace() {
    g_extraPlayers = SplitPlayers() - 2;
    g_seat = 0;
    RememberRound(SplitPlayers());
    reinterpret_cast<void(__cdecl*)()>(FN_START_RACE)();
}

// Player 1 joined through Y / Space (PressStartFilter): everyone's last car, port and gearbox, then the race.
static void QuickStart() {
    g_quickStart = false;
    for (int k = 0; k < g_lastPlayers; ++k) {
        g_seatCar[k] = g_lastCar[k];
        g_seatPort[k] = g_lastPort[k];
        g_seatGearbox[k] = g_lastGearbox[k];
        if (k < 2) RestoreSeat(k);
        else if (g_seatStore[k - 2].car != g_lastCar[k]) { g_seatStore[k - 2].car = g_lastCar[k]; SaveSeatStore(k - 2); }
    }
    Log("last round again: race starts");
    StartSplitRace();
}

static void SwitchSeat(uint8_t* carsel, int seat) {
    g_seat = seat;
    g_carsel = carsel;
    if (seat >= 2 && GarageHasCar(g_seatStore[seat - 2].car))   // the list opens on the seat's last car
        at<int>(CAR_IDS + (seat & 1) * 4) = g_seatStore[seat - 2].car;
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_CARSEL_SWITCH)(carsel, seat & 1);
    if (seat >= 2) {                           // title: "PLAYER 3" / "PLAYER 4"
        using HashFn = uint32_t(__cdecl*)(const char*);
        uint32_t hash = reinterpret_cast<HashFn>(0x505450)("Player_Name");
        void* obj = reinterpret_cast<void*(__cdecl*)(const void*, uint32_t)>(0x537A10)(
            *reinterpret_cast<void**>(carsel + 4), hash);
        if (obj) reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF)(obj, "PLAYER %d", seat + 1);
    }
    Log("car select: seat %d (FE player %d)", seat + 1, seat & 1);
}

// Seats pick in order P1 (FE 0), P2 (FE 1), P3 (FE 0), P4 (FE 1), P5 (FE 0), P6 (FE 1); each pick of a seat >= 2 is
// saved and the FE slot gets players 1/2's own pick back.
static void __fastcall CarselToP2(uint8_t* carsel, void*, int) {      // after FE player 1's pick
    if (g_seat == 0) g_firstCar = at<int>(CAR_IDS);
    if (SplitPlayers() > 2 && g_seat >= 2) {   // player 3 / 5 picked
        int seat = g_seat;
        SaveSeat(seat);
        RestoreSeat(0);
        if (seat + 1 >= SplitPlayers()) {      // the last seat
            StartSplitRace();
            return;
        }
        SwitchSeat(carsel, seat + 1);
        return;
    }
    SwitchSeat(carsel, 1);
}

static void __stdcall CarselStart(uint8_t* carsel) {                   // after FE player 2's pick
    if (SplitPlayers() > 2 && SplitMode()) {
        if (g_seat == 1) {                     // players 1 and 2 picked: on to player 3
            SaveSeat(0);
            SaveSeat(1);
            SwitchSeat(carsel, 2);
            return;
        }
        if (g_seat >= 3) {
            int seat = g_seat;
            SaveSeat(seat);
            RestoreSeat(1);
            if (seat + 1 < SplitPlayers()) {   // on to player 5
                SwitchSeat(carsel, seat + 1);
                return;
            }
        }
    }
    if (SplitMode()) {
        StartSplitRace();
        return;
    }
    g_extraPlayers = 0;
    g_seat = 0;
    reinterpret_cast<void(__cdecl*)()>(FN_START_RACE)();
}

static __declspec(naked) void CarselStartHook() {
    __asm {
        push esi                         // the car select (this of 0x4FC1F0)
        call CarselStart
        ret
    }
}

static void BackToSeat(uint8_t* carsel, int seat) {
    SwitchSeat(carsel, seat);
    g_autoPickSeat = -1;
    g_autoBackSeat = g_autoSeat[seat] ? seat : -1;   // no pick screen to go back to: its join is undone too
}

static void __fastcall CarselBackToP1(uint8_t* carsel, void*, int) {  // Back on FE player 2's pick
    if (g_seat >= 3) {                         // player 4 / 6: back to player 3 / 5
        RestoreSeat(1);
        BackToSeat(carsel, g_seat - 1);
        return;
    }
    BackToSeat(carsel, 0);
}

// Cars row: a seat with a given car gets it on the first car select tick after its join, as if it had picked it
// (the game's after-pick steps CarselToP2 / CarselStart: on to the next seat's Press START, or the race). A seat that
// Back returned to gets the game's own Back on its pick screen, which reopens its Press START popup.
static bool AutoPickTick(uint8_t* carsel) {
    using ActiveFn = bool(__cdecl*)(const char*);
    if (at<int>(FE_PLAYER) != (g_seat & 1) || reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng"))
        return false;
    if (g_autoBackSeat == g_seat) {
        g_autoBackSeat = -1;
        Log("cars: back to player %d's Press START", g_seat + 1);
        reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(MSG_BACK, "UI_QRCarSelect.fng");
        return true;
    }
    if (g_autoPickSeat != g_seat) return false;
    g_autoPickSeat = -1;
    int car = g_carsMode == CARS_SAME ? g_firstCar : g_randomCar[g_seat];
    if (!GarageHasCar(car)) {                  // (nothing to give: the player picks)
        g_autoSeat[g_seat] = false;
        Log("cars: no car %08X for player %d, picks", car, g_seat + 1);
        return false;
    }
    at<int>(CAR_IDS + (g_seat & 1) * 4) = car;
    Log("cars: player %d gets %08X (%s)", g_seat + 1, car, g_carsMode == CARS_SAME ? "same as player 1" : "random");
    // the gearbox dialog when a pick would bring it up (0x4FC3D4: not drag, the FE slot's ask setting +0x12 on):
    // 0x497D50 opens it; its answer (0x1A2826E1 / 0x5F5E3886 -> 0xBA5B5FCE) runs the same after-pick calls (0x4FC9B6 /
    // 0x4FC9C2, CarselToP2 / CarselStartHook). The pick also stores the port of the pressing device (0x50D2A0(port));
    // the seat's port is already its join's.
    if (at<int>(QR_RACE_TYPE) != 2 && at<uint8_t>(GEARBOX_ASK + (g_seat & 1) * PLAYER_SETTING_SIZE)) {
        g_autoPrompted[g_seat] = true;
        reinterpret_cast<void(__thiscall*)(void*)>(FN_CARSEL_GEARBOX_DIALOG)(carsel);
        Log("cars: player %d's gearbox dialog", g_seat + 1);
        return true;
    }
    if (g_seat & 1) CarselStart(carsel);
    else CarselToP2(carsel, nullptr, 0);
    return true;
}

// Back on player 3 (FE player 1, whose Back would leave the car select) returns to player 2, like player 2's Back
// returns to player 1 (0x4FC723: closes the Press START popup 0x5379A0, switches, sound 0x50D180(5)).
// Customize (X) from any player's pick: split mode used to open the memory card first (0x4FCB7A: mode 4 -> load a
// profile for this FE player); with the shared garage it goes straight to the car shop like the other modes
// (jne -> jmp). The seat is kept while the shop is open, and when the car select comes back (it restarts at player 1's
// Press START) its first frame closes that popup and returns to the seat.
#define MSG_CUSTOMIZE      0xC519BFC0
#define AT_CUSTOMIZE_MODE4 0x4FCB7A      // cmp edi,4 ; jne +42h
static int g_customizeSeat = -1;
static bool g_customizeLeft = false;
static int g_returnSeat = -1;            // back from the shop: seat to return to once player 1 rejoined   // the car select was left for the shop since
static bool __stdcall CarselFilter(uint8_t* carsel, uint32_t msg) {
    if (!SplitMode()) return false;
    if (msg == MSG_CUSTOMIZE && g_customizeSeat < 0) {
        g_customizeSeat = g_seat;
        g_customizeLeft = false;
        if (g_seat == 1) SaveSeat(0);
        if (g_seat >= 2) g_seatStore[g_seat - 2].car = at<int>(CAR_IDS + (g_seat & 1) * 4);   // back on this car
        // the shop shows the car in its FE player's display slot: as FE player 2 it alternated between the
        // customised car and player 1's (user: RX-7 / Supra flipping) -> the shop always opens as player 1 (shared
        // garage); player 1's own pick is restored when the round goes on
        if (g_seat & 1) {
            at<int>(CAR_IDS) = at<int>(CAR_IDS + 4);
            *reinterpret_cast<int*>(carsel + 0x5C) = 0;
            at<int>(FE_PLAYER) = 0;
        }
        Log("customize from player %d's pick", g_seat + 1);
        return false;
    }
    using ActiveFn = bool(__cdecl*)(const char*);
    auto active = reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE);
    if (msg == MSG_FRAME_TICK && g_customizeSeat >= 0 && g_customizeLeft) {
        // back: the car select restarted at player 1's Press START -> player 1 rejoins by itself, then (next step)
        // the round goes on to the seat the shop was opened from, as after a pick
        g_returnSeat = g_customizeSeat;
        g_customizeSeat = -1;
        g_autoJoinSeat = 0;
        Log("car select back from customize: player %d", g_returnSeat + 1);
        return false;
    }
    if (msg == MSG_FRAME_TICK && g_returnSeat >= 1 && g_autoJoinSeat < 0 && at<int>(FE_PLAYER) == 0 &&
        !active("2P_PressStart.fng")) {
        int seat = g_returnSeat;
        g_returnSeat = -1;
        RestoreSeat(0);
        if (seat >= 3) RestoreSeat(1);
        g_autoJoinSeat = seat;
        SwitchSeat(carsel, seat);
        return false;
    }
    if (msg == MSG_FRAME_TICK && g_returnSeat == 0 && g_autoJoinSeat < 0) g_returnSeat = -1;
    if (msg == MSG_FRAME_TICK && g_quickStart && at<int>(FE_PLAYER) == 0 && !active("2P_PressStart.fng")) {
        QuickStart();
        return true;
    }
    if (msg == MSG_FRAME_TICK && g_autoJoinSeat < 0 && g_returnSeat < 0 && !g_quickStart && AutoPickTick(carsel))
        return true;
    if (msg != MSG_BACK || g_seat < 2 || (g_seat & 1)) return false;   // Back on player 3 / 5's popup: to player 2 / 4
    // Back on the pick screen itself undoes only this player's join: the game's own Back (0x4FC795) reopens the popup
    using ActiveFn = bool(__cdecl*)(const char*);
    if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng")) return false;
    reinterpret_cast<void(__cdecl*)(const char*)>(0x5379A0)("2P_PressStart.fng");
    RestoreSeat(0);
    BackToSeat(carsel, g_seat - 1);
    reinterpret_cast<void(__cdecl*)(int)>(0x50D180)(5);
    return true;
}

// Leaving the car select (Back on player 1 or 3, Customize, ...) ends the round: players 1/2 keep their own cars.
// The Press START popup's text (object 0x2EBB974D) names the FE player; players 3-6 get their own.
// Help bar (bottom buttons, [0x8384D8], visible +0x6C; show 0x54E6A0; set buttons 0x54E6E0 does nothing while hidden):
// leaving a split race rebuilds it hidden (ctor 0x551F80) and returns to the split mode select, whose screens then set
// their buttons on a hidden bar (ignored) -> no buttons until Customize. In the split menus (no race loaded) a hidden
// bar is shown before the buttons are set.
#define FN_HELP_BAR_SHOW   0x54E6A0      // void __thiscall (bar)
#define FN_HELP_BAR_SET    0x54E6E0      // void __thiscall (bar; flags, arg) - 5 bytes stolen
#define FN_FE_SET_STRING   0x535FE0      // void __thiscall (FEString*, const char*): sets its text, grows the buffer
#define HELP_RANDOM_TRACK  0x1000        // the help bar's Customize button: Random on the split track select (a click
                                         // sends the screen MSG_CUSTOMIZE, see RandomTick)
typedef void(__thiscall* HelpBarSetFn)(void*, uint32_t, uint32_t);
// The Random button's text. The released WidescreenFix ([MISC] ImproveGamepadSupport 1-3) gives the help bar's buttons
// "(A)"-style prefixes by swapping known English labels, so with it the button gets the same style here: "(Y) Random",
// or "(Triangle) Random" with PlayStation text (2). Otherwise "$BUTTON_Y$ Random": "$...$" is a console button token
// the PC font skips (plain "Random", like the game's own buttons) and FusionFix draws as the Y button.
static const char* RandomButtonText() {
    static int style = -1;                 // 0 none, 1 Xbox text, 2 PlayStation text
    if (style < 0) {
        style = 0;
        char path[MAX_PATH];
        HMODULE wsf = GetModuleHandleA("NFSUnderground2.WidescreenFix.asi");
        if (wsf && GetModuleFileNameA(wsf, path, MAX_PATH)) {
            if (char* dot = strrchr(path, '.')) strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
            int mode = GetPrivateProfileIntA("MISC", "ImproveGamepadSupport", 0, path);
            style = mode == 2 ? 2 : mode == 1 || mode == 3 ? 1 : 0;
        }
        Log("help bar: Random button style %d (WidescreenFix %s)", style, wsf ? "loaded" : "not loaded");
    }
    return style == 1 ? "(Y) Random" : style == 2 ? "(Triangle) Random" : "$BUTTON_Y$ Random";
}
static HelpBarSetFn g_origHelpBarSet;
static void __fastcall HelpBarSet(uint8_t* bar, void*, uint32_t flags, uint32_t arg) {
    extern void* GetLocalPlayer(int);
    if (bar && !bar[0x6C] && SplitMode() && !GetLocalPlayer(0)) {
        reinterpret_cast<void(__thiscall*)(void*)>(FN_HELP_BAR_SHOW)(bar);
        Log("help bar shown again (split menus, buttons %X)", flags);
    }
    using ActiveFn = bool(__cdecl*)(const char*);   // Random on the split track and mode selects (RandomTick)
    bool trackHint = bar && SplitMode() &&
        (reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("UI_QRTrackSelect.fng") ||
         reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("UI_QRModeSelect.fng"));
    if (trackHint) flags |= HELP_RANDOM_TRACK;
    g_origHelpBarSet(bar, flags, arg);
    if (!trackHint) return;
    // the bar's buttons: list at +0x4C (node - 4 = button; +4 next node, +0x10 text object, +0x1C its flag)
    auto* head = bar + 0x48;
    for (auto* node = *reinterpret_cast<uint8_t**>(bar + 0x4C); node && node - 4 != head;
         node = *reinterpret_cast<uint8_t**>(node - 4 + 4)) {
        uint8_t* button = node - 4;
        if (*reinterpret_cast<uint32_t*>(button + 0x1C) != HELP_RANDOM_TRACK) continue;
        // Set with the store 0x535FE0, which grows the string to fit; the printf 0x537BE0 cuts at the object's current
        // length (16 here).
        if (auto* text = *reinterpret_cast<uint8_t**>(button + 0x10)) {
            reinterpret_cast<void(__thiscall*)(void*, const char*)>(FN_FE_SET_STRING)(text, RandomButtonText());
            *reinterpret_cast<uint32_t*>(text + 0x1C) |= 2;   // text set by code, as 0x537B30 marks it
        }
        break;
    }
}

// Car select sounds: one per action. Every FE sound goes through 0x458520 (thiscall sound manager [0x82B884]; id),
// 5 bytes stolen. The generic package layer (0x51DD40: the package's handler, then the sound of the input it handled,
// table 0x837FE0, call at 0x51DD81) plays Back (5) / Start (6) for whatever the controlling device presses, while the
// car select plays its own for each action it takes (0x50D180 from 0x4FC728 / 0x4FC753 / 0x4FC7AF, CarselFilter). So
// Back played twice on popups and FE player 0's pick, once on FE player 1's pick; a join played 6 only for the device
// that happened to drive the frontend (not for other pads or the keyboard), and an ignored Start played it too. In the
// split car select the generic 5/6 is dropped and joins play 6 themselves (PressStartFilter).
// Debug (Log only writes with Debug = 1): every car select sound and press with the seat state.
#define FN_FE_SOUND        0x458520
#define AT_FE_GENERIC_SOUND_RET 0x51DD86
typedef void(__thiscall* FeSoundFn)(void*, int);
static FeSoundFn g_origFeSound;
static const char* CarselStateText() {
    static char s[96];
    int fe = at<int>(FE_PLAYER);
    bool popup = reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng");
    sprintf_s(s, "seat %d fe %d %s driver %d kb %d pads %d,%d,%d,%d", g_seat + 1, fe, popup ? "popup" : "pick",
              CarselDriverSeat() + 1, g_keyboardSeat + 1, g_assignedPad[0], g_assignedPad[1], g_assignedPad[2],
              g_assignedPad[3]);
    return s;
}
static bool g_feSoundMuted = false;      // a message the plugin sends for the player (no sound of its own)

// List screens: one sound per mouse click. The list screens' shared handler 0x556020 (thiscall screen; msg, ...)
// plays the sound of the input a message stands for at its end (table 0x837FE0, call at 0x55647B, the message is the
// saved edi on top of the caller's stack), and the accept input (8, sound 4) stands for both the accept 0x0C407210 and
// the package click 0x406415E3. A left click on the help bar's Continue delivers both, the click one frame before the
// accept, so it sounded twice; a click on a row delivers only the click. The accept's sound is dropped when this site
// played the same sound for a click within the last 200 ms.
#define AT_FE_LIST_SOUND_RET 0x556480
#define MSG_FE_CLICK       0x406415E3
static bool ListClickEcho(int id, const uint32_t* ret) {
    static int clickSound = -1;
    static ULONGLONG clickTime;
    if (*ret != AT_FE_LIST_SOUND_RET) return false;
    uint32_t msg = ret[2];
    ULONGLONG now = GetTickCount64();
    if (g_debug) Log("list sound %d for message %08X", id, msg);
    if (msg == MSG_FE_CLICK) { clickSound = id; clickTime = now; return false; }
    bool echo = msg == MSG_ACCEPT && id == clickSound && now - clickTime < 200;
    clickSound = -1;
    return echo;
}

static void __fastcall FeSound(void* mgr, void*, int id) {
    if (g_feSoundMuted) {
        if (g_debug) Log("sound %d from %08X dropped (plugin's own accept)", id, reinterpret_cast<uintptr_t>(_ReturnAddress()) - 5);
        return;
    }
    if (ListClickEcho(id, reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress()))) {
        if (g_debug) Log("sound %d dropped (the click already played it)", id);
        return;
    }
    if (g_inCarSelect) {
        uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        bool generic = ret == AT_FE_GENERIC_SOUND_RET && (id == 5 || id == 6);
        if (g_debug) Log("sound %d from %08X%s (%s)", id, ret - 5, generic ? " dropped" : "", CarselStateText());
        if (generic) return;
    }
    g_origFeSound(mgr, id);
}
static void LogCarselInput() {
    static uint16_t prev[32];
    static bool prevKey[3];
    if (!g_debug || !g_inCarSelect) { memset(prev, 0, sizeof(prev)); return; }
    for (int pad = 0; pad < XPadMax() && pad < 32; ++pad) {
        uint16_t b = 0; uint8_t lt, rt; int16_t lx;
        if (!XPadRawState(pad, &b, &lt, &rt, &lx)) b = 0;
        uint16_t down = b & ~prev[pad] & (0x0010 | 0x0020 | 0x1000 | 0x2000);   // Start, Back, A, B
        prev[pad] = b;
        if (down) Log("pad %d pressed%s%s%s%s (%s)", pad, down & 0x10 ? " Start" : "", down & 0x20 ? " Back" : "",
                      down & 0x1000 ? " A" : "", down & 0x2000 ? " B" : "", CarselStateText());
    }
    static const int keys[3] = {VK_RETURN, VK_ESCAPE, VK_BACK};
    static const char* const names[3] = {"Enter", "Esc", "Backspace"};
    for (int i = 0; i < 3; ++i) {
        bool d = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (d && !prevKey[i]) Log("key %s (%s)", names[i], CarselStateText());
        prevKey[i] = d;
    }
}

static void PrepareExtraNames();
static char g_extraNames[MAX_EXTRA_PLAYERS][16];
static const char* const kSeatWords[MAX_EXTRA_PLAYERS] = {"Three", "Four", "Five", "Six"};
static void UpdateSeats() {
    using ActiveFn = bool(__cdecl*)(const char*);
    UpdatePadTakeover();
    if (g_inCarSelect) {                       // name box: whose pick it is (the game shows the profile for everyone)
        int fe = at<int>(FE_PLAYER);
        int seat = (g_seat & 1) == fe ? g_seat : fe;
        PrepareExtraNames();
        const char* name = seat >= 2 ? g_extraNames[seat - 2]
                                     : reinterpret_cast<const char*>(0x83A9E0 + (seat & 1) * 0xAB4C);
        using HashFn = uint32_t(__cdecl*)(const char*);
        static uint32_t hash = reinterpret_cast<HashFn>(0x505450)("Player_Name");
        for (const char* pkg : {"UI_QRCarSelect.fng", "GarageMain.fng", "UI_PC_Help_Bar.fng"}) {
            if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)(pkg)) continue;
            void* obj = reinterpret_cast<void*(__cdecl*)(const char*, uint32_t)>(0x537A10)(pkg, hash);
            if (obj && seat >= 0 && seat < MAX_LOCAL_PLAYERS)
                reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF)(obj, "%s", name);
        }
    }
    if (g_seat >= 2 && reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng")) {
        void* obj = reinterpret_cast<void*(__cdecl*)(const char*, uint32_t)>(0x537A10)("2P_PressStart.fng", 0x2EBB974D);
        if (obj) reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF)(
            obj, "Player %s Press START", kSeatWords[g_seat - 2]);
    }
    if (g_inCarSelect && g_seat == 0 && at<int>(FE_PLAYER) == 0 && g_autoJoinSeat < 0 && LastRoundReady() &&
        reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng")) {
        void* obj = reinterpret_cast<void*(__cdecl*)(const char*, uint32_t)>(0x537A10)("2P_PressStart.fng", 0x2EBB974D);
        if (obj) reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF)(
            obj, g_lastKeyboardSeat >= 0 ? "Player One Press START\nPress Y / SPACE for Rematch"
                                         : "Player One Press START\nPress Y for Rematch");
    }
    if (!g_inCarSelect) g_quickStart = false, g_autoPickSeat = g_autoBackSeat = -1;
    static int s_waited = 0;                       // X that didn't open the shop: forget it after 30 frames
    if (g_customizeSeat >= 0 && !g_customizeLeft && g_inCarSelect && ++s_waited > 30) g_customizeSeat = -1;
    if (g_customizeSeat < 0 || g_customizeLeft) s_waited = 0;
    if (g_customizeSeat >= 0 && !g_inCarSelect) g_customizeLeft = true;
    if (!SplitMode()) g_customizeSeat = g_returnSeat = g_autoJoinSeat = -1, g_nextRace = false;
    if (g_seat >= 2 && !g_inCarSelect && g_customizeSeat < 0) {
        RestoreSeat(0);
        RestoreSeat(1);
        g_seat = 0;
        Log("car select left: seats reset");
    }
}

// "Players" row on the split-screen options screen (UI_QRModeOptions): rows are 0x54-byte widgets (base ctor
// 0x51E230(screen+0xFC, +0x10C, +0x114, +0x11C, 1), added with 0x54F170(row, 1)); vtable slot 1 = Act(fng, msg)
// (left 0x9120409E / right 0xB5971BF1), slot 3 = Draw (label [row+0x2C], value [row+0x30]). The options screen
// builds its rows per race type (switch at 0x4E2C9A: 0x4CCF60 circuit, ...; each with its own split branch), then
// finishes with 0x539650(0) at 10 call sites: the row is appended right before that.
#define FN_QR_FINISH_ROWS  0x539650      // void __thiscall (screen; int)
#define FN_ROW_CTOR        0x51E230
#define FN_SCREEN_ADD_ROW  0x54F170
#define MSG_LEFT           0x9120409E
#define MSG_RIGHT          0xB5971BF1
#define ROW_VTABLE_LAPS    0x7910B8
static const uintptr_t kQrFinishRowsCalls[] = {0x4E2CAC, 0x4E2CBE, 0x4E2CD0, 0x4E2CE2, 0x4E2CF4, 0x4E2D06, 0x4E2D18,
                                               0x4E2D2A, 0x4E2D4D, 0x4E2D5F};
static uint32_t g_playersRowVt[18];

static void __fastcall PlayersRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Players");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%d", SplitPlayers());
}

static void __fastcall PlayersRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    int n = SplitPlayers(), max = MAX_LOCAL_PLAYERS;
    if (msg == MSG_LEFT) g_splitPlayers = n > 2 ? n - 1 : max;
    if (msg == MSG_RIGHT) g_splitPlayers = n < max ? n + 1 : 2;
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}

// "Opponents" and "Difficulty" rows: AI racers in split races. Quick race types (0x83AAB4; starters 0x53FCF8): 0/1/7
// circuit/sprint/lap KO, 2 drag, 3 drift, 4 URL, 5/10 Street X, 8 outrun, 9 / 11 others. AI only where single player
// offers opponents (the per-type option builders that add 0x4B3750). Drift: circuit drift only - the drift starter
// zeroes the AI on sprint drift tracks (track info 0x5D3E40(track) +0x80 == 1, at 0x53F8B0; NFSU2 Extra Options marks
// sprint drift AI as broken). Up to 6 racers in all (single player URL; Extra Options allows 5 opponents everywhere).
static bool SprintDriftTrack() {
    int type = at<int>(QR_RACE_TYPE);
    if (type == 0xC) return false;
    uint16_t track = at<uint16_t>(QR_SETTINGS + type * 0x14 + 8);
    auto* info = reinterpret_cast<const uint8_t*(__cdecl*)(int)>(0x5D3E40)(track);
    return info && *reinterpret_cast<const int*>(info + 0x80) == 1;
}
static bool AiRaceType() {
    switch (at<int>(QR_RACE_TYPE)) {
    case 0: case 1: case 2: case 4: case 5: case 7: case 10: return true;
    case 3: return !SprintDriftTrack();
    default: return false;
    }
}
static uint32_t g_opponentsRowVt[18];
static int SplitOpponents() {
    int max = MAX_RACERS - SplitPlayers();
    if (!AiRaceType() || max < 0) return 0;
    return g_splitOpponents < 0 ? 0 : g_splitOpponents > max ? max : g_splitOpponents;
}
static void __fastcall OpponentsRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Opponents");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%d", SplitOpponents());
}
static void __fastcall OpponentsRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    int n = SplitOpponents(), max = MAX_RACERS - SplitPlayers();
    if (max < 0) max = 0;
    if (msg == MSG_LEFT) g_splitOpponents = n > 0 ? n - 1 : max;
    if (msg == MSG_RIGHT) g_splitOpponents = n < max ? n + 1 : 0;
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}
static void AddRow(uint8_t* screen, uint32_t* vt) {
    auto* row = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x54));
    if (!row) return;
    reinterpret_cast<void(__thiscall*)(void*, void*, void*, void*, void*, int)>(FN_ROW_CTOR)(
        row, screen + 0xFC, screen + 0x10C, screen + 0x114, screen + 0x11C, 1);
    *reinterpret_cast<uint32_t**>(row) = vt;
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(FN_SCREEN_ADD_ROW)(screen, row, 1);
}
static void AddOpponentsRow(uint8_t* screen) {
    if (!g_opponentsRowVt[0]) {
        memcpy(g_opponentsRowVt, reinterpret_cast<void*>(ROW_VTABLE_LAPS), sizeof(g_opponentsRowVt));
        g_opponentsRowVt[1] = reinterpret_cast<uint32_t>(&OpponentsRowAct);
        g_opponentsRowVt[3] = reinterpret_cast<uint32_t>(&OpponentsRowDraw);
    }
    AddRow(screen, g_opponentsRowVt);
}
#define FN_DIFFICULTY_ROW_CTOR 0x4B3930  // the game's quick race Difficulty row (same args as 0x51E230)
static void AddDifficultyRow(uint8_t* screen) {
    auto* row = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x54));
    if (!row) return;
    reinterpret_cast<void(__thiscall*)(void*, void*, void*, void*, void*, int)>(FN_DIFFICULTY_ROW_CTOR)(
        row, screen + 0xFC, screen + 0x10C, screen + 0x114, screen + 0x11C, 1);
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(FN_SCREEN_ADD_ROW)(screen, row, 1);
}
static void AddPlayersRow(uint8_t* screen) {
    if (!g_playersRowVt[0]) {
        memcpy(g_playersRowVt, reinterpret_cast<void*>(ROW_VTABLE_LAPS), sizeof(g_playersRowVt));
        g_playersRowVt[1] = reinterpret_cast<uint32_t>(&PlayersRowAct);
        g_playersRowVt[3] = reinterpret_cast<uint32_t>(&PlayersRowDraw);
    }
    AddRow(screen, g_playersRowVt);
}

static uint32_t g_carsRowVt[18];
static void __fastcall CarsRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Cars");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%s", kCarsModeText[g_carsMode]);
}
static void __fastcall CarsRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    if (msg == MSG_LEFT) g_carsMode = (g_carsMode + CARS_MODES - 1) % CARS_MODES;
    if (msg == MSG_RIGHT) g_carsMode = (g_carsMode + 1) % CARS_MODES;
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}
static void AddCarsRow(uint8_t* screen) {
    if (!g_carsRowVt[0]) {
        memcpy(g_carsRowVt, reinterpret_cast<void*>(ROW_VTABLE_LAPS), sizeof(g_carsRowVt));
        g_carsRowVt[1] = reinterpret_cast<uint32_t>(&CarsRowAct);
        g_carsRowVt[3] = reinterpret_cast<uint32_t>(&CarsRowDraw);
    }
    AddRow(screen, g_carsRowVt);
}

static void __fastcall QrFinishRows(uint8_t* screen, void*, int arg) {
    if (SplitMode()) AddPlayersRow(screen);
    if (SplitMode()) AddCarsRow(screen);
    if (SplitMode() && AiRaceType()) AddOpponentsRow(screen), AddDifficultyRow(screen);
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_QR_FINISH_ROWS)(screen, arg);
}
