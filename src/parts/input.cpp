// Part of NFSU2SplitScreen.cpp: input merge and joypad bindings of players 3/4.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- input
typedef void*(__cdecl* PollPortFn)(int, int*, void**);
static PollPortFn g_origPollPort;

// Same merge rules as the game (0x5CF220): buttons are active-low (AND), triggers take the max, sticks take the
// value furthest from centre. Digital devices (type 0x13) only contribute buttons.
static void MergeDevice(uint8_t* out, const uint8_t* dev) {
    auto* b = reinterpret_cast<uint32_t*>(out);
    b[0] &= *reinterpret_cast<const uint32_t*>(dev + 8);
    b[1] &= *reinterpret_cast<const uint32_t*>(dev + 0xC);
    if (*reinterpret_cast<const int*>(dev + 0x234) == 0x13) return;
    if (dev[0x10] > out[8]) out[8] = dev[0x10];
    if (dev[0x11] > out[9]) out[9] = dev[0x11];
    for (int axis = 0; axis < 2; ++axis)
        if (abs(dev[0x12 + axis] - 0x80) > abs(out[10 + axis] - 0x80)) out[10 + axis] = dev[0x12 + axis];
}

// The keyboard (device record 0, joystick slot 0, type 0x13) is merged by the game into port 0, i.e. player 1. In
// split screen it goes to one port only (KeyboardPort): racing, its player's port (0..3); during a pause the menu's
// port, and only if its player paused (player 3/4: slot port & 1, like their pads); on the car select only on its
// player's turn; elsewhere in the split menus port 0 as before. After a change it waits until every key is up
// (the Escape that paused would otherwise reach the menu as a new press).
static int g_kbPort = -1;
static bool g_kbWait = false;
static const uint8_t* KeyboardRecord() {
    for (int d = 0; d < INPUT_DEVICE_MAX; ++d) {
        auto* rec = reinterpret_cast<const uint8_t*>(INPUT_DEVICES + d * 0x370);
        if (*reinterpret_cast<const int*>(rec + 0x234) == 0x13) return rec;
    }
    return nullptr;
}
static int KeyboardPort() {
    if (SplitRacing()) {
        if (g_keyboardSeat < 0) return -1;
        if (g_pausePort >= 0) return g_pausePort == g_keyboardSeat ? (g_keyboardSeat & 1) : -1;
        return g_keyboardSeat;
    }
    if (g_inCarSelect) {
        int fe = at<int>(FE_PLAYER);
        return ((g_seat & 1) == fe ? g_seat : fe) == g_keyboardSeat ? 0 : -1;
    }
    return 0;
}
static void UpdateKeyboardPort() {       // once per frame (port 0's poll)
    int port = KeyboardPort();
    if (port != g_kbPort) {
        g_kbPort = port;
        g_kbWait = true;
    }
    const uint8_t* kb = KeyboardRecord();
    if (g_kbWait && kb && *reinterpret_cast<const uint32_t*>(kb + 8) == 0xFFFFFFFF &&
        *reinterpret_cast<const uint32_t*>(kb + 0xC) == 0xFFFFFFFF)
        g_kbWait = false;
}
static void MergeKeyboard(uint8_t* out, int port) {
    const uint8_t* kb = KeyboardRecord();
    if (kb && !g_kbWait && g_kbPort == port) MergeDevice(out, kb);
}

// ---------------------------------------------------------------- joypad bindings of players 3/4
// Device records (6 x 0x370 at 0x874C40; +0x230 joystick slot or -1, +0x234 type: 0x13 keyboard, 0x15 our pads) turn
// their device's state into action bits (+8, 8 bytes, active low) and analog values (+0x10..+0x13) every frame:
// 0x5CF220 -> 0x5C1840 (joysticks: DIJOYSTATE2 of the slot at 0x86FFC0 + slot*0x110 for the buttons and at
// [0x870760] + slot*0x110 for axes/POV; action tables +0x18/+0x98/+0x118/+0x198, 32 inputs each; axis setup +0x344..
// +0x368, +0x36C POV count). The profile saves table 0 and the axis setup per device GUID (0x5B9420 save at 0x52A42A,
// 0x5B9570 load at 0x534660); the Controls screen edits the record of the page's device.
// Players 3/4 have no joystick slot. Their bindings live in the seat store and are applied by running 0x5C1840 on a
// private copy of the slot-0 pad record holding their bindings, with their pad's state put into slot 0's state
// buffers for the call (restored right after).
// Controls screen (ctor 0x4ECD10; pages 0x835930, stride 12 {type, ?, record}, 6 max, count screen+0x158, joypad pages
// +0x154, built by 0x4ACE00 at 0x4ECFD5; page switch 0x4ACAA0; joypad page builder 0x4DC9F0): two pages are added for
// players 3/4, both on the slot-0 pad record. While one of them is shown, that record holds the player's bindings (the
// game's own editing, defaults and capture work on it) and player 1's are kept aside; they go back when the page is
// left and before the profile is saved or loaded.
#define FN_RECORD_CONVERT  0x5C1840      // void __thiscall (record)
#define DI_STATE_LIVE      0x86FFC0      // DIJOYSTATE2 [2]
#define DI_STATE_COPY_PTR  0x870760      // -> DIJOYSTATE2 [2] (+ more)
#define DI_STATE_SIZE      0x110
#define DEVICE_RECORD_SIZE 0x370
#define AT_CONTROLS_PAGES_CALL 0x4ECFD5  // call 0x4ACE00 (thiscall screen)
#define FN_CONTROLS_PAGES  0x4ACE00
#define CONTROLS_PAGES     0x835930
#define CONTROLS_PAGE_MAX  6
#define AT_PROFILE_SAVE_BINDINGS 0x52A42A  // call 0x5B9420 (cdecl (out) -> end)
#define FN_PROFILE_SAVE_BINDINGS 0x5B9420
#define AT_PROFILE_LOAD_BINDINGS 0x534660  // call 0x5B9570 (cdecl (in) -> end)
#define FN_PROFILE_LOAD_BINDINGS 0x5B9570
static int g_ctrlPageBase = -1;          // index of player 3's page on the open Controls screen (-1: none)
static int g_bindSwap = -1;              // player 3/4 whose bindings are in the slot-0 pad record
static uint8_t g_p1Tables[REC_BIND_TABLES_SIZE], g_p1Axes[REC_BIND_AXES_SIZE];   // player 1's, kept aside meanwhile

static uint8_t* PadRecord(int slot) {
    for (int d = 0; d < INPUT_DEVICE_MAX; ++d) {
        auto* rec = reinterpret_cast<uint8_t*>(INPUT_DEVICES + d * DEVICE_RECORD_SIZE);
        if (*reinterpret_cast<int*>(rec + 0x230) == slot && *reinterpret_cast<int*>(rec + 0x234) != 0x13) return rec;
    }
    return nullptr;
}

static int ControlsPageSeat() {
    int page = at<int>(CONTROLS_PAGE);
    if (g_ctrlPageBase < 0 || page < g_ctrlPageBase || page >= g_ctrlPageBase + 2) return -1;
    using ActiveFn = bool(__cdecl*)(const char*);
    if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("UI_Options_PC_Controller.fng")) return -1;
    return 2 + page - g_ctrlPageBase;
}

// Player 1's bindings as the game has them (also while a player 3/4 page holds the record).
static void P1Bindings(const uint8_t* rec, const uint8_t** tables, const uint8_t** axes) {
    *tables = g_bindSwap >= 0 ? g_p1Tables : rec + REC_BIND_TABLES;
    *axes = g_bindSwap >= 0 ? g_p1Axes : rec + REC_BIND_AXES;
}

static void EnsureSeatBindings(int seat, const uint8_t* rec) {
    SeatStore& st = g_seatStore[seat - 2];
    if (st.hasBindings || !rec) return;
    const uint8_t *tables, *axes;
    P1Bindings(rec, &tables, &axes);
    memcpy(st.tables, tables, REC_BIND_TABLES_SIZE);
    memcpy(st.axes, axes, REC_BIND_AXES_SIZE);
    st.hasBindings = true;
    SaveSeatStore(seat - 2);
}

static void SetBindSwap(int seat) {
    if (seat == g_bindSwap) return;
    uint8_t* rec = PadRecord(0);
    if (!rec) { g_bindSwap = -1; return; }
    if (g_bindSwap >= 0) {               // the page's edits go to the player, player 1's come back
        SeatStore& st = g_seatStore[g_bindSwap - 2];
        bool changed = memcmp(st.tables, rec + REC_BIND_TABLES, REC_BIND_TABLES_SIZE) != 0 ||
                       memcmp(st.axes, rec + REC_BIND_AXES, REC_BIND_AXES_SIZE) != 0;
        memcpy(st.tables, rec + REC_BIND_TABLES, REC_BIND_TABLES_SIZE);
        memcpy(st.axes, rec + REC_BIND_AXES, REC_BIND_AXES_SIZE);
        if (changed) SaveSeatStore(g_bindSwap - 2);
        memcpy(rec + REC_BIND_TABLES, g_p1Tables, REC_BIND_TABLES_SIZE);
        memcpy(rec + REC_BIND_AXES, g_p1Axes, REC_BIND_AXES_SIZE);
        Log("joypad record: player 1's bindings back (player %d's %s)", g_bindSwap + 1, changed ? "saved" : "unchanged");
        g_bindSwap = -1;
    }
    if (seat >= 0) {
        EnsureSeatBindings(seat, rec);
        memcpy(g_p1Tables, rec + REC_BIND_TABLES, REC_BIND_TABLES_SIZE);
        memcpy(g_p1Axes, rec + REC_BIND_AXES, REC_BIND_AXES_SIZE);
        memcpy(rec + REC_BIND_TABLES, g_seatStore[seat - 2].tables, REC_BIND_TABLES_SIZE);
        memcpy(rec + REC_BIND_AXES, g_seatStore[seat - 2].axes, REC_BIND_AXES_SIZE);
        g_bindSwap = seat;
        Log("joypad record: player %d's bindings in for their Controls page", seat + 1);
    }
}

static void UpdateBindSwap() { SetBindSwap(ControlsPageSeat()); }

static void __fastcall BuildControlsPages(uint8_t* screen) {
    g_ctrlPageBase = -1;
    reinterpret_cast<void(__thiscall*)(void*)>(FN_CONTROLS_PAGES)(screen);
    int& count = *reinterpret_cast<int*>(screen + 0x158);
    int record = -1;
    for (int d = 0; d < INPUT_DEVICE_MAX; ++d)
        if (reinterpret_cast<uint8_t*>(INPUT_DEVICES + d * DEVICE_RECORD_SIZE) == PadRecord(0)) record = d;
    if (!g_xinputPads || record < 0 || count + 2 > CONTROLS_PAGE_MAX) return;
    g_ctrlPageBase = count;
    for (int k = 0; k < 2; ++k) {
        int* entry = reinterpret_cast<int*>(CONTROLS_PAGES + (count + k) * 12);
        entry[0] = 1;                    // joypad page
        entry[1] = 0;
        entry[2] = record;
    }
    count += 2;
    *reinterpret_cast<int*>(screen + 0x154) += 2;
    Log("controls: pages %d/%d for players 3/4 (record %d)", g_ctrlPageBase, g_ctrlPageBase + 1, record);
}

static uint8_t* __cdecl ProfileSaveBindings(uint8_t* out) {
    SetBindSwap(-1);                     // player 1's bindings into the profile (the page puts them back next frame)
    return reinterpret_cast<uint8_t*(__cdecl*)(uint8_t*)>(FN_PROFILE_SAVE_BINDINGS)(out);
}

static uint8_t* __cdecl ProfileLoadBindings(uint8_t* in) {
    SetBindSwap(-1);
    return reinterpret_cast<uint8_t*(__cdecl*)(uint8_t*)>(FN_PROFILE_LOAD_BINDINGS)(in);
}

// Ports 2/3 (players 3/4) have no DirectInput slot: their state is built from their own XInput pad with their own
// bindings (see above), merged like the game merges a port's devices.
static void* PollExtraPort(int port, int* type, void** prev) {
    static const uint8_t idle[12] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0x80, 0x7F};
    static uint8_t record[DEVICE_RECORD_SIZE], state[DI_STATE_SIZE], live[DI_STATE_SIZE], copy[DI_STATE_SIZE];
    uint8_t* cur = g_portCur[port];
    memcpy(g_portPrev[port], cur, 16);
    memcpy(cur, idle, 12);
    memcpy(cur + 12, g_portCur[0] + 12, 4);
    *type = g_port0Type;
    *prev = g_portPrev[port];
    uint8_t* pad0 = PadRecord(0);
    auto* copyState = at<uint8_t*>(DI_STATE_COPY_PTR);
    if (!SplitRacing() || port - 2 >= g_extraPlayers || !at<int>(INPUT_FOCUS)) return cur;
    MergeKeyboard(cur, port);
    if (!pad0 || !copyState || !XPadDiState(g_assignedPad[port], state, sizeof(state))) return cur;
    EnsureSeatBindings(port, pad0);
    const SeatStore& st = g_seatStore[port - 2];
    memcpy(record, pad0, sizeof(record));
    memcpy(record + REC_BIND_TABLES, st.tables, REC_BIND_TABLES_SIZE);
    memcpy(record + REC_BIND_AXES, st.axes, REC_BIND_AXES_SIZE);
    *reinterpret_cast<int*>(record + 0x33C) = 0;     // not capturing a binding
    auto* liveState = reinterpret_cast<uint8_t*>(DI_STATE_LIVE);    // slot 0's
    memcpy(live, liveState, DI_STATE_SIZE);
    memcpy(copy, copyState, DI_STATE_SIZE);
    memcpy(liveState, state, DI_STATE_SIZE);
    memcpy(copyState, state, DI_STATE_SIZE);
    reinterpret_cast<void(__thiscall*)(void*)>(FN_RECORD_CONVERT)(record);
    memcpy(liveState, live, DI_STATE_SIZE);
    memcpy(copyState, copy, DI_STATE_SIZE);
    MergeDevice(cur, record);
    return cur;
}

static void* __cdecl PollPortHook(int port, int* type, void** prev) {
    // The original polls every device (and drives force feedback) on port 0 and leaves each device's state in its
    // record; port 1 just gets the same merged buffer back. In split mode, port N only sees devices in slot N.
    if (port >= 2 && port < MAX_LOCAL_PLAYERS) return PollExtraPort(port, type, prev);
    void* merged = g_origPollPort(port, type, prev);
    if (port == 0) g_port0Type = *type;
    if (!SplitMode() || port < 0 || port > 1) return merged;

    if (port == 0) {
        static const uint8_t idle[12] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0x80, 0x80};
        const uint8_t* all = static_cast<const uint8_t*>(merged);
        UpdateKeyboardPort();
        for (int p = 0; p < 2; ++p) {
            memcpy(g_portPrev[p], g_portCur[p], 16);
            memcpy(g_portCur[p], idle, 12);
            memcpy(g_portCur[p] + 12, all + 12, 4);
            if (!at<int>(INPUT_FOCUS)) continue;
            for (int d = 0; d < INPUT_DEVICE_MAX; ++d) {
                const uint8_t* dev = reinterpret_cast<const uint8_t*>(INPUT_DEVICES + d * 0x370);
                if (*reinterpret_cast<const int*>(dev + 0x230) == p && *reinterpret_cast<const int*>(dev + 0x234) != 0x13)
                    MergeDevice(g_portCur[p], dev);
            }
            MergeKeyboard(g_portCur[p], p);
        }
        // TODO: restrict frontend input to the active player's devices. Overwriting the merged globals
        // (0x86EAF0/0x86B7E0) with the player's state blocks frontend messages entirely, so the frontend's
        // message source is elsewhere; until found, any pad drives the screen of the player whose turn it is.
    }
    *prev = g_portPrev[port];
    return g_portCur[port];
}
