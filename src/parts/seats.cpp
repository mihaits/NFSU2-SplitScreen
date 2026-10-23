// Part of NFSU2SplitScreen.cpp: players 3/4 data, controller assignment, car select seats.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- players 3/4: their own data
// The game keeps per-player data for 2 players only (profile blocks, player options, joypad records, picked car).
// Players 3/4 get their own copy of each, kept in NFSU2SplitScreen_Players.ini next to the plugin:
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
static SeatStore g_seatStore[2];         // players 3, 4
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
    for (int i = 0; i < 2; ++i) {
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
static int g_assignedPad[MAX_LOCAL_PLAYERS] = {PAD_NONE, PAD_NONE, PAD_NONE, PAD_NONE};
static int g_keyboardSeat = -1;          // the player who joined with the keyboard this round (-1: nobody)

static int g_seat = 0;                   // car select: whose turn (0..3); the frontend serves FE player seat & 1
static int g_splitPlayers = 2;           // players chosen on the split-screen options screen (2..4)
static int g_splitOpponents = 0;         // AI opponents chosen there (see SplitOpponents)
#define QR_RACE_TYPE       0x83AAB4      // quick race type (8 = outrun)
// Outrun is a 2-player mode (one car chases the other): no Players row and always 2 players; the choice is kept for
// the other modes.
static bool OutrunRace() { return at<int>(QR_RACE_TYPE) == 8; }
static int SplitPlayers() { return OutrunRace() ? 2 : g_splitPlayers; }

static void ResetAssignments() {
    for (int& pad : g_assignedPad) pad = PAD_NONE;
    g_keyboardSeat = -1;
}

// The pause screen (ctor 0x4EB410) only accepts input from the pausing player: package input mask =
// 0x50D2D0(port of the pausing player [0x8900D8]+0x20) at 0x4EB469. Frontend input only ever comes from joystick
// slots 0/1 (masks 1/2), so a pause by player 3/4 (port 2/3, mask 4/8) locked the menu (user report). Their pad
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
// who joined last may press. The popup's own seat may still hold a stale pad / keyboard from going back.
static int CarselDriverSeat() {
    int fe = at<int>(FE_PLAYER);
    int seat = (g_seat & 1) == fe ? g_seat : fe;
    if (seat >= 1 && reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng"))
        return seat - 1;
    return seat;
}

static int __cdecl PadForDevice(int device) {
    if (device < 0 || device > 1) return PAD_NONE;
    // outside split the last pad used drives; joystick slot 2 only gets it while its bindings are being edited
    // (otherwise both slots' bindings would apply in 1P)
    if (!SplitMode()) return device == 0 || g_editingSlot2 ? XPadLastActive() : PAD_NONE;
    // in the split-screen menus only the player whose turn it is controls the frontend
    if (SplitRacing()) {
        if (g_pauseOwner < 0 || device != (g_pauseOwner & 1)) return g_assignedPad[device];
        int pad = g_assignedPad[g_pauseOwner];
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
        if (seat == g_keyboardSeat) return PAD_NONE;            // the keyboard player's turn: keyboard only
        int pad = g_assignedPad[seat];
        return pad != PAD_NONE ? pad : XPadLastActive();
    }
    if (device != at<int>(FE_PLAYER)) return PAD_NONE;
    return XPadLastActive();
}

static bool KeyboardEnterPressed() {
    static bool wasDown = false;
    bool down = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0 && GetForegroundWindow() == FindWindowA("GameFrame", nullptr);
    bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

// 2P_PressStart handler filter (thiscall: this, msg, p3, port). The PC frontend never sends START itself in a
// usable way (all input is merged and tagged as player 1), so joins are detected here from XInput directly and the
// game's own START/accept messages are ignored. Returns the message the handler should see.
static bool __stdcall CarselFilter(uint8_t* carsel, uint32_t msg);
static uint8_t* g_carsel = nullptr;      // the car select of the current round (set when switching seats)
static int g_autoJoinSeat = -1;           // back from customize: this seat's Press START popup accepts by itself

static uint32_t __stdcall PressStartFilter(uint8_t* popup, uint32_t msg, uint32_t* port) {
    int player = at<int>(FE_PLAYER);
    if (player < 0 || player > 1) return msg;
    int seat = (g_seat & 1) == player ? g_seat : player;      // players 3/4 join on FE players 1/2's popups
    if (msg == MSG_FRAME_TICK) {
        bool keyboard = KeyboardEnterPressed() && (seat == 0 || g_keyboardSeat < 0 || g_keyboardSeat >= seat);
        if (!popup[0x4C]) return msg;                 // first tick sets the popup's ready flag
        if (g_autoJoinSeat == seat) {
            g_autoJoinSeat = -1;
            *port = 1u << player;
            Log("player %d rejoined after customize", seat + 1);
            return MSG_START;
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
            return MSG_START;
        }
        if (keyboard) {
            if (seat == 0) ResetAssignments();
            g_assignedPad[seat] = PAD_NONE;
            g_keyboardSeat = seat;
            *port = 1u << player;
            Log("player %d joined with keyboard", seat + 1);
            return MSG_START;
        }
        return msg;
    }
    if (msg == MSG_START || msg == MSG_ACCEPT || msg == MSG_PC_ACTIVATE) return 0;   // ignored by the handler
    if (msg == MSG_BACK && seat == 2 && g_carsel && CarselFilter(g_carsel, msg)) return 0;   // back to player 2
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

// ---------------------------------------------------------------- 3-4 players: seats in the car select
// The car select serves FE players 0/1 (0x4FA2A0(player) switches: FE_PLAYER, title, car list; 0x4FC3A8/0x4FC9B6
// switch to player 2 after player 1's pick, 0x4FC723 goes back; 0x4FC3BE/0x4FC9C2 start the race (0x53FC20) after
// player 2's pick). With 3-4 players the picks continue: seat 2 (player 3) on FE player 0, seat 3 (player 4) on FE
// player 1, picking from player 1's / player 2's garage. Each pick (car id 0x83AAB8[fe], port 0x83A9D8[fe]) is saved
// per seat and player 1's / player 2's own restored; players 3/4's racers get theirs (CreateHumanRacers).
#define FN_FE_PRINTF       0x537BE0      // void __cdecl (obj, fmt, ...): sets the object's text
#define FN_CARSEL_SWITCH   0x4FA2A0      // void __thiscall (carsel; fePlayer)
#define FN_START_RACE      0x53FC20      // void __cdecl ()
#define CAR_IDS            0x83AAB8      // int [2]: car of each player (in that player's garage)
#define PLAYER_PORTS       0x83A9D8      // int [2]: input port mask of each player
static const uintptr_t kCarselToP2Calls[] = {0x4FC3A8, 0x4FC9B6};
static const uintptr_t kCarselStartCalls[] = {0x4FC3BE, 0x4FC9C2};
#define AT_CARSEL_BACK_TO_P1 0x4FC723
static int g_seatCar[MAX_LOCAL_PLAYERS], g_seatPort[MAX_LOCAL_PLAYERS];

// Players 1/2's last cars are kept by the game (CAR_IDS in their profile blocks); players 3/4's in their seat store.
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

static void SaveSeat(int seat) {
    g_seatCar[seat] = at<int>(CAR_IDS + (seat & 1) * 4);
    g_seatPort[seat] = at<int>(PLAYER_PORTS + (seat & 1) * 4);
    if (seat >= 2 && g_seatStore[seat - 2].car != g_seatCar[seat]) {
        g_seatStore[seat - 2].car = g_seatCar[seat];
        SaveSeatStore(seat - 2);
    }
    Log("player %d picked car %08X (port %d)", seat + 1, g_seatCar[seat], g_seatPort[seat]);
}

static void RestoreSeat(int seat) {            // players 1/2's picks back into the game's slots
    at<int>(CAR_IDS + seat * 4) = g_seatCar[seat];
    at<int>(PLAYER_PORTS + seat * 4) = g_seatPort[seat];
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

static void __fastcall CarselToP2(uint8_t* carsel, void*, int) {      // after FE player 1's pick
    if (SplitPlayers() > 2 && g_seat == 2) {   // player 3 picked
        SaveSeat(2);
        RestoreSeat(0);
        if (SplitPlayers() == 3) {
            g_seat = 0;
            g_extraPlayers = 1;
            reinterpret_cast<void(__cdecl*)()>(FN_START_RACE)();
            return;
        }
        SwitchSeat(carsel, 3);
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
        if (g_seat == 3) {
            SaveSeat(3);
            RestoreSeat(1);
        }
    }
    g_extraPlayers = SplitMode() ? SplitPlayers() - 2 : 0;
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

static void __fastcall CarselBackToP1(uint8_t* carsel, void*, int) {  // Back on FE player 2's pick
    if (g_seat == 3) {
        RestoreSeat(1);
        SwitchSeat(carsel, 2);
        return;
    }
    SwitchSeat(carsel, 0);
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
    if (msg != MSG_BACK || g_seat != 2) return false;   // Back on player 3's popup: to player 2
    // Back on the pick screen itself undoes only this player's join: the game's own Back (0x4FC795) reopens the popup
    using ActiveFn = bool(__cdecl*)(const char*);
    if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng")) return false;
    reinterpret_cast<void(__cdecl*)(const char*)>(0x5379A0)("2P_PressStart.fng");
    RestoreSeat(0);
    SwitchSeat(carsel, 1);
    reinterpret_cast<void(__cdecl*)(int)>(0x50D180)(5);
    return true;
}

// Leaving the car select (Back on player 1 or 3, Customize, ...) ends the round: players 1/2 keep their own cars.
// The Press START popup's text (object 0x2EBB974D) names the FE player; players 3/4 get their own.
// Help bar (bottom buttons, [0x8384D8], visible +0x6C; show 0x54E6A0; set buttons 0x54E6E0 does nothing while hidden):
// leaving a split race rebuilds it hidden (ctor 0x551F80) and returns to the split mode select, whose screens then set
// their buttons on a hidden bar (ignored) -> no buttons until Customize. In the split menus (no race loaded) a hidden
// bar is shown before the buttons are set.
#define FN_HELP_BAR_SHOW   0x54E6A0      // void __thiscall (bar)
#define FN_HELP_BAR_SET    0x54E6E0      // void __thiscall (bar; flags, arg) - 5 bytes stolen
typedef void(__thiscall* HelpBarSetFn)(void*, uint32_t, uint32_t);
static HelpBarSetFn g_origHelpBarSet;
static void __fastcall HelpBarSet(uint8_t* bar, void*, uint32_t flags, uint32_t arg) {
    extern void* GetLocalPlayer(int);
    if (bar && !bar[0x6C] && SplitMode() && !GetLocalPlayer(0)) {
        reinterpret_cast<void(__thiscall*)(void*)>(FN_HELP_BAR_SHOW)(bar);
        Log("help bar shown again (split menus, buttons %X)", flags);
    }
    g_origHelpBarSet(bar, flags, arg);
}

static void PrepareExtraNames();
static char g_extraNames[2][16];
static void UpdateSeats() {
    using ActiveFn = bool(__cdecl*)(const char*);
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
            obj, "Player %s Press START", g_seat == 2 ? "Three" : "Four");
    }
    static int s_waited = 0;                       // X that didn't open the shop: forget it after 30 frames
    if (g_customizeSeat >= 0 && !g_customizeLeft && g_inCarSelect && ++s_waited > 30) g_customizeSeat = -1;
    if (g_customizeSeat < 0 || g_customizeLeft) s_waited = 0;
    if (g_customizeSeat >= 0 && !g_inCarSelect) g_customizeLeft = true;
    if (!SplitMode()) g_customizeSeat = g_returnSeat = g_autoJoinSeat = -1;
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
    printf_(*reinterpret_cast<void**>(row + 0x30), "%d", g_splitPlayers);
}

static void __fastcall PlayersRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    if (msg == MSG_LEFT) g_splitPlayers = g_splitPlayers > 2 ? g_splitPlayers - 1 : MAX_LOCAL_PLAYERS;
    if (msg == MSG_RIGHT) g_splitPlayers = g_splitPlayers < MAX_LOCAL_PLAYERS ? g_splitPlayers + 1 : 2;
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}

// "Opponents" and "Difficulty" rows: AI racers in split races. Quick race types (0x83AAB4; starters 0x53FCF8): 0/1/7
// circuit/sprint/lap KO, 2 drag, 3 drift, 4 URL, 5/10 Street X, 8 outrun, 9 / 11 others. AI only where single player
// offers opponents (the per-type option builders that add 0x4B3750). Drift: circuit drift only - the drift starter
// zeroes the AI on sprint drift tracks (track info 0x5D3E40(track) +0x80 == 1, at 0x53F8B0; NFSU2 Extra Options marks
// sprint drift AI as broken). Up to 6 racers in all (single player URL; Extra Options allows 5 opponents everywhere),
// 4 in drag.
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
static int MaxRacers() { return at<int>(QR_RACE_TYPE) == 2 ? 4 : 6; }
static uint32_t g_opponentsRowVt[18];
static int SplitOpponents() {
    int max = MaxRacers() - SplitPlayers();
    if (!AiRaceType() || max < 0) return 0;
    return g_splitOpponents < 0 ? 0 : g_splitOpponents > max ? max : g_splitOpponents;
}
static void __fastcall OpponentsRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Opponents");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%d", SplitOpponents());
}
static void __fastcall OpponentsRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    int n = SplitOpponents(), max = MaxRacers() - SplitPlayers();
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

static void __fastcall QrFinishRows(uint8_t* screen, void*, int arg) {
    if (SplitMode() && !OutrunRace()) AddPlayersRow(screen);
    if (SplitMode() && AiRaceType()) AddOpponentsRow(screen), AddDifficultyRow(screen);
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_QR_FINISH_ROWS)(screen, arg);
}
