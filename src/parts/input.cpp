// Part of NFSU2SplitScreen.cpp: input merge and joypad bindings of players 3-6.
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
// split screen it goes to one port only (KeyboardPort): racing, its player's port (0..5); during a pause the menu's
// port, and only if its player paused (players 3-6: slot port & 1, like their pads); on the car select only on its
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
        int seat = CarselDriverSeat();
        return seat < 0 || seat == g_keyboardSeat ? 0 : -1;    // -1: player 1's popup, anyone
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

// ---------------------------------------------------------------- joypad bindings of players 3-6
// Device records (6 x 0x370 at 0x874C40; +0x230 joystick slot or -1, +0x234 type: 0x13 keyboard, 0x15 our pads) turn
// their device's state into action bits (+8, 8 bytes, active low) and analog values (+0x10..+0x13) every frame:
// 0x5CF220 -> 0x5C1840 (joysticks: DIJOYSTATE2 of the slot at 0x86FFC0 + slot*0x110 for the buttons and at
// [0x870760] + slot*0x110 for axes/POV; action tables +0x18/+0x98/+0x118/+0x198, 32 inputs each; axis setup +0x344..
// +0x368, +0x36C POV count). The profile saves table 0 and the axis setup per device GUID (0x5B9420 save at 0x52A42A,
// 0x5B9570 load at 0x534660); the Controls screen edits the record of the page's device.
// Players 3-6 have no joystick slot. Their bindings live in the seat store and are applied by running 0x5C1840 on a
// private copy of the slot-0 pad record holding their bindings, with their pad's state put into slot 0's state
// buffers for the call (restored right after).
// Controls screen (ctor 0x4ECD10; pages 0x835930, stride 12 {type, ?, record}, 6 max, relocated to 8; count
// screen+0x158, joypad pages +0x154, built by 0x4ACE00 at 0x4ECFD5; page switch 0x4ACAA0; joypad page builder
// 0x4DC9F0): four pages are added for players 3-6 when they fit, all on the slot-0 pad record. While one of them is
// shown, that record holds the player's bindings (the game's own editing, defaults and capture work on it) and player
// 1's are kept aside; they go back when the page is left and before the profile is saved or loaded.
#define FN_RECORD_CONVERT  0x5C1840      // void __thiscall (record)
#define DI_STATE_LIVE      0x86FFC0      // DIJOYSTATE2 [2]
#define DI_STATE_COPY_PTR  0x870760      // -> DIJOYSTATE2 [2] (+ more)
#define DI_STATE_SIZE      0x110
#define DEVICE_RECORD_SIZE 0x370
#define AT_CONTROLS_PAGES_CALL 0x4ECFD5  // call 0x4ACE00 (thiscall screen)
#define FN_CONTROLS_PAGES  0x4ACE00
#define OLD_CONTROLS_PAGES 0x835930      // 6 x 12 bytes; the next object starts at 0x835978
#define CONTROLS_PAGES     reinterpret_cast<uintptr_t>(g_controlsPages)
// Every .text reference to the table is a disp32/imm32 inside it (77: `mov ebp, 0x835930` at 0x4ACE69 and 76
// `[reg*4 + 0x835938]`; a blind scan finds the same 77).
static void InitControlsPages() { memcpy(g_controlsPages, reinterpret_cast<void*>(OLD_CONTROLS_PAGES), 6 * 12); }
static bool ControlsPagesRef(uint32_t v, uint32_t* nv) {      // for RelocateTextRefs
    if (v < OLD_CONTROLS_PAGES || v >= OLD_CONTROLS_PAGES + 6 * 12) return false;
    *nv = static_cast<uint32_t>(CONTROLS_PAGES) + (v - OLD_CONTROLS_PAGES);
    return true;
}
// The profile packer 0x52A280 (which writes the bindings with 0x5B9420) runs for the snapshot taken when Options opens,
// for the "profile changed?" check behind the save prompt and for the save, all before the player has confirmed both
// prompts. The file is written by the memory-card step 0x6FCF77: state 0xB (+0x64) opens <dir>\<name>\<name> (name at
// +0x9C), writes and closes it, +0x2DC = 0 on success, then state 0xC. Loading (0x5344D0) only runs for a profile read.
#define AT_PROFILE_SAVE_BINDINGS 0x52A42A  // call 0x5B9420 (cdecl (out) -> end)
#define FN_PROFILE_SAVE_BINDINGS 0x5B9420
#define AT_PROFILE_LOAD_BINDINGS 0x534660  // call 0x5B9570 (cdecl (in) -> end)
#define FN_PROFILE_LOAD_BINDINGS 0x5B9570
#define AT_MEMCARD_STEP          0x6FDBFA  // call 0x6FCF77 (thiscall memcard, arg): the save states 9..0xC
#define FN_MEMCARD_STEP          0x6FCF77
static int g_ctrlPageBase = -1;          // index of player 3's page on the open Controls screen (-1: none)
static int g_bindSwap = -1;              // player 3-6 whose bindings are in the slot-0 pad record
static uint8_t g_p1Tables[REC_BIND_TABLES_SIZE], g_p1Axes[REC_BIND_AXES_SIZE];   // player 1's, kept aside meanwhile
static StickBinds g_stickBinds[2];       // players 1/2 (slot 0/1 pad records); saved with the game profile
static const uint8_t* g_extraRecord;     // PollExtraPort's private record while it runs the decoder
static int g_extraSeat;
static void SaveProfileSticks();
static void LoadProfileSticks();

static uint8_t* PadRecord(int slot) {
    for (int d = 0; d < INPUT_DEVICE_MAX; ++d) {
        auto* rec = reinterpret_cast<uint8_t*>(INPUT_DEVICES + d * DEVICE_RECORD_SIZE);
        if (*reinterpret_cast<int*>(rec + 0x230) == slot && *reinterpret_cast<int*>(rec + 0x234) != 0x13) return rec;
    }
    return nullptr;
}

static int ControlsPageSeat() {
    int page = at<int>(CONTROLS_PAGE);
    if (g_ctrlPageBase < 0 || page < g_ctrlPageBase || page >= g_ctrlPageBase + MAX_EXTRA_PLAYERS) return -1;
    using ActiveFn = bool(__cdecl*)(const char*);
    if (!reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("UI_Options_PC_Controller.fng")) return -1;
    return 2 + page - g_ctrlPageBase;
}

// Player 1's bindings as the game has them (also while a player 3-6 page holds the record).
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
    memcpy(st.stick.action, g_stickBinds[0].action, sizeof(st.stick.action));
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
    reinterpret_cast<void(__thiscall*)(void*)>(CallPrev(AT_CONTROLS_PAGES_CALL, FN_CONTROLS_PAGES))(screen);
    int& count = *reinterpret_cast<int*>(screen + 0x158);
    int record = -1;
    for (int d = 0; d < INPUT_DEVICE_MAX; ++d)
        if (reinterpret_cast<uint8_t*>(INPUT_DEVICES + d * DEVICE_RECORD_SIZE) == PadRecord(0)) record = d;
    if (!g_xinputPads || record < 0 || count + MAX_EXTRA_PLAYERS > CONTROLS_PAGE_MAX) return;
    g_ctrlPageBase = count;
    for (int k = 0; k < MAX_EXTRA_PLAYERS; ++k) {
        int* entry = reinterpret_cast<int*>(CONTROLS_PAGES + (count + k) * 12);
        entry[0] = 1;                    // joypad page
        entry[1] = 0;
        entry[2] = record;
    }
    count += MAX_EXTRA_PLAYERS;
    *reinterpret_cast<int*>(screen + 0x154) += MAX_EXTRA_PLAYERS;
    Log("controls: pages %d..%d for players 3..6 (record %d)", g_ctrlPageBase, g_ctrlPageBase + MAX_EXTRA_PLAYERS - 1, record);
}

static uint8_t* __cdecl ProfileSaveBindings(uint8_t* out) {
    SetBindSwap(-1);                     // player 1's bindings into the profile (the page puts them back next frame)
    return reinterpret_cast<uint8_t*(__cdecl*)(uint8_t*)>(CallPrev(AT_PROFILE_SAVE_BINDINGS, FN_PROFILE_SAVE_BINDINGS))(out);
}

// Players 1/2's stick entries are written when the profile file is: a write state that ended without an error.
static void __fastcall MemcardStep(uint8_t* mc, void*, int arg) {
    const int state = *reinterpret_cast<int*>(mc + 0x64);
    reinterpret_cast<void(__thiscall*)(void*, int)>(CallPrev(AT_MEMCARD_STEP, FN_MEMCARD_STEP))(mc, arg);
    if (state == 0xB && *reinterpret_cast<int*>(mc + 0x64) == 0xC && *reinterpret_cast<int*>(mc + 0x2DC) == 0)
        SaveProfileSticks();
}

static uint8_t* __cdecl ProfileLoadBindings(uint8_t* in) {
    SetBindSwap(-1);
    uint8_t* end = reinterpret_cast<uint8_t*(__cdecl*)(uint8_t*)>(CallPrev(AT_PROFILE_LOAD_BINDINGS, FN_PROFILE_LOAD_BINDINGS))(in);
    LoadProfileSticks();
    return end;
}

// Ports 2-5 (players 3-6) have no DirectInput slot: their state is built from their own XInput pad with their own
// bindings (see above), merged like the game merges a port's devices.
// A port with nothing pressed: no action bit (active low), pedals released, steering stick centred (X, Y).
static const uint8_t kPortIdle[12] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0x80, 0x80};
static void* PollExtraPort(int port, int* type, void** prev) {
    static uint8_t record[DEVICE_RECORD_SIZE], state[DI_STATE_SIZE], live[DI_STATE_SIZE], copy[DI_STATE_SIZE];
    uint8_t* cur = g_portCur[port];
    memcpy(g_portPrev[port], cur, 16);
    memcpy(cur, kPortIdle, 12);
    memcpy(cur + 12, g_portCur[0] + 12, 4);
    *type = g_port0Type;
    *prev = g_portPrev[port];
    uint8_t* pad0 = PadRecord(0);
    auto* copyState = at<uint8_t*>(DI_STATE_COPY_PTR);
    if (!SplitRacing() || port - 2 >= g_extraPlayers || !at<int>(INPUT_FOCUS)) return cur;
    MergeKeyboard(cur, port);
    if (!pad0 || !copyState || !XPadDiState(RacePad(port), state, sizeof(state))) return cur;
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
    g_extraRecord = record;
    g_extraSeat = port;
    reinterpret_cast<void(__thiscall*)(void*)>(FN_RECORD_CONVERT)(record);
    g_extraRecord = nullptr;
    memcpy(liveState, live, DI_STATE_SIZE);
    memcpy(copyState, copy, DI_STATE_SIZE);
    MergeDevice(cur, record);
    return cur;
}

// Action tables 1..3 are the frontend's (the Controls screen and the profile only use table 0). The widescreen fix
// (ImproveGamepadSupport) assigns the frontend buttons, START among them, in the slot-0 pad record only, every frame, so
// START did nothing in a menu read through slot 1: a pause by player 2/4/6 only closed with Back (user report; the game's
// own player 2 pause too). All our pads are the same device, so slot 1 gets slot 0's tables.
#define REC_FE_TABLES      (REC_BIND_TABLES + 0x80)
#define REC_FE_TABLES_SIZE (REC_BIND_TABLES_SIZE - 0x80)
static void MirrorFrontendTables() {
    uint8_t *pad0 = PadRecord(0), *pad1 = PadRecord(1);
    if (!pad0 || !pad1 || !memcmp(pad1 + REC_FE_TABLES, pad0 + REC_FE_TABLES, REC_FE_TABLES_SIZE)) return;
    memcpy(pad1 + REC_FE_TABLES, pad0 + REC_FE_TABLES, REC_FE_TABLES_SIZE);
    Log("joypad record: slot 1 frontend actions copied from slot 0");
}

// Old WidescreenFix builds (e.g. the one bundled with underground2.net 1.0.3) put a stick deadzone on the joystick
// poll 0x5C8700 with `call; nop` over `mov esi, eax; test esi, esi; jge` at 0x5C882B. Their hook resumes at 0x5C8869
// by writing it over the saved esi on the stack instead of its return address, so 0x5CFA10's slot loop (esi) ends
// after slot 0: slot 1 is never polled and its zeroed DIJOYSTATE2 reads POV 0 = D-pad up, which the menus get through
// the virtual pad in slot 1 (endless Up). Current builds hook here with a jmp (safetyhook) and are left alone.
// Checked every frame because WidescreenFix may patch after this plugin is loaded.
#define AT_JOY_STATE_RESULT 0x5C882B
static void RepairOldWidescreenFixPoll() {
    static bool repaired = false;
    if (repaired) return;
    const uint8_t* site = reinterpret_cast<const uint8_t*>(AT_JOY_STATE_RESULT);
    if (site[0] != 0xE8 || site[5] != 0x90) return;
    uintptr_t target = AT_JOY_STATE_RESULT + 5 + *reinterpret_cast<const int32_t*>(site + 1);
    HMODULE mod = nullptr;
    char name[MAX_PATH] = {};
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(target), &mod) ||
        !GetModuleFileNameA(mod, name, MAX_PATH) || !StrStrIA(name, "WidescreenFix"))
        return;
    static const uint8_t original[6] = {0x8B, 0xF0, 0x85, 0xF6, 0x7D, 0x38};
    Write(AT_JOY_STATE_RESULT, original, sizeof(original));
    repaired = true;
    Log("input: removed an old WidescreenFix joystick hook at %X (-> %p in %s) that stopped joystick slot 1 from "
        "being polled", AT_JOY_STATE_RESULT, reinterpret_cast<void*>(target), PathFindFileNameA(name));
}

static void* __cdecl PollPortHook(int port, int* type, void** prev) {
    // The original polls every device (and drives force feedback) on port 0 and leaves each device's state in its
    // record; port 1 just gets the same merged buffer back. In split mode, port N only sees devices in slot N.
    if (port >= 2 && port < INPUT_PORTS) return PollExtraPort(port, type, prev);
    if (port == 0) {
        RepairOldWidescreenFixPoll();
        MirrorFrontendTables();
    }
    void* merged = g_origPollPort(port, type, prev);
    if (port == 0) g_port0Type = *type;
    if (!SplitMode() || port < 0 || port > 1) return merged;

    if (port == 0) {
        const uint8_t* all = static_cast<const uint8_t*>(merged);
        UpdateKeyboardPort();
        for (int p = 0; p < 2; ++p) {
            memcpy(g_portPrev[p], g_portCur[p], 16);
            memcpy(g_portCur[p], kPortIdle, 12);
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

// ---------------------------------------------------------------- analog bytes (docs/input.md, "Analog bytes")
// A joypad record's analog bytes: +0x10 throttle, +0x11 brake, +0x12 steering stick X, +0x13 steering stick Y.
// The pedal events all read +0x10/+0x11 (records 0x93/0x96, which read the shared pedal byte +0x13, are retargeted
// at install), and +0x13 carries the Y of the stick steering is bound to, which the menus (0x2E/0x2F) and hydraulics
// (0xA8/0xAD) read, as on console. Both ends of the decoder's axis part (0x5C2044 shared pedals, 0x5C20CD separate
// pedals) jump to AnalogBytes, then to the common epilogue. The separate end's patch stops before 0x5C20D4: the
// decoder's early exits (0x5C189E, 0x5C18CA, 0x5C19DE, 0x5C19F3) jump there.
#define AT_SHARED_PEDALS_END    0x5C2044   // mov byte [esi+0x11], 0; mov byte [esi+0x10], 0 (8 bytes)
#define AT_SEPARATE_PEDALS_END  0x5C20CD   // mov [esi+0x10], cl; mov byte [esi+0x13], 0x80 (7 bytes)
#define AT_AXES_EPILOGUE        0x5C204C   // pop edi; pop esi; pop ebp; pop ebx; mov esp, ebp; pop ebp; ret
#define EVENT_RECORDS           0x7FDD10   // 83 records of 0x20: +8 event, +0x12 bit, +0x14 mode, +0x15 curve
#define EVENT_RECORD_COUNT      83
static const float kAxisScale = 0.0127f;   // the decoder's axis scale (0x7A3C18): range +-10000 -> +-127

static uint8_t* EventRecord(uint32_t event) {
    for (int i = 0; i < EVENT_RECORD_COUNT; ++i) {
        auto* r = reinterpret_cast<uint8_t*>(EVENT_RECORDS + i * 0x20);
        if (*reinterpret_cast<uint32_t*>(r + 8) == event) return r;
    }
    return nullptr;
}

// THROTTLE/BRAKE_ANALOG_ALTERNATE (0x93/0x96) read the shared pedal byte with mode 1/2; they become copies of
// THROTTLE/BRAKE_ANALOG (0x92/0x95): the separate channel, mode 0, same curve.
static bool RetargetPedalEvents() {
    const uint32_t pairs[2][2] = {{0x93, 0x92}, {0x96, 0x95}};
    for (const auto& p : pairs) {
        uint8_t* alt = EventRecord(p[0]);
        const uint8_t* base = EventRecord(p[1]);
        if (!alt || !base || alt[0x12] != 0x58) return false;
        uint8_t fields[4] = {base[0x12], alt[0x13], 0, base[0x15]};   // +0x12 bit, +0x13, +0x14 mode, +0x15 curve
        Write(reinterpret_cast<uintptr_t>(alt + 0x12), fields, sizeof(fields));
    }
    return true;
}

// The game turns floats into ints by truncation (0x75BED4), here too.
// The pedal curve (curve 3: (0,0) (123,128) (133,128) (255,255)) and its inverse, to turn a shared-axis pedal value
// into the separate-channel byte that decodes to the same value.
static int PedalCurve(int x) {
    if (x <= 123) return static_cast<int>(x * 128.0f / 123.0f);
    if (x <= 133) return 128;
    return static_cast<int>(128.0f + (x - 133) * 127.0f / 122.0f);
}
static uint8_t PedalByteFor(int value) {
    // the smallest curve input that reaches the value; the decoder XORs the byte with 1 (the record's polarity) first
    const int x = value <= 128 ? (value * 123 + 127) / 128 : 133 + ((value - 128) * 122 + 126) / 127;
    return static_cast<uint8_t>((x > 255 ? 255 : x) ^ 1);
}

static LONG AxisOf(const uint8_t* record, int offset) {
    const auto* state = at<const uint8_t*>(0x870760) + *reinterpret_cast<const int*>(record + 0x230) * 0x110;
    return *reinterpret_cast<const LONG*>(state + offset);
}

// A trigger value 0..255 as a separate-channel byte: XORed with 1 so that the decoder, which XORs it back, runs the
// curve on the trigger's own value (a full pull gives 255: brake 1.0); a released trigger stays 0 (reads 1, as idle).
static uint8_t TriggerByte(LONG v) { return v > 0 ? static_cast<uint8_t>((v > 255 ? 255 : v) ^ 1) : 0; }

// Shared pedals: +0x10/+0x11 from the bound axis. Z is the triggers (StateOf), whose own values ride in
// rglSlider[0..1]; another axis is split at its centre, each half giving the value the game's shared decoding gives.
static void SharedPedals(uint8_t* record) {
    const int axis = *reinterpret_cast<const int*>(record + 0x350);
    if (axis == 8) {
        record[0x10] = TriggerByte(AxisOf(record, 0x18));
        record[0x11] = TriggerByte(AxisOf(record, 0x1C));
        return;
    }
    const int sign = static_cast<int8_t>(record[0x360]);
    const int shared = 0x7F - static_cast<int>(AxisOf(record, axis) * kAxisScale) * sign;   // the byte 0x5C2029 makes
    const int c = PedalCurve(shared ^ 1);
    const int throttle = c < 0x7F ? (0x7F - c) * 2 : 0;    // the decoder's mode 1 / mode 2 (0x5795C0)
    const int brake = c > 0x80 ? (c - 0x80) * 2 : 0;
    record[0x10] = throttle ? PedalByteFor(throttle) : 0;
    record[0x11] = brake ? PedalByteFor(brake) : 0;
}

// +0x13: Y of the steering stick (offset of a stick's X + 4: lX -> lY, lRx -> lRy), written like +0x12 (up = low).
static void SteeringStickY(uint8_t* record) {
    const int x = *reinterpret_cast<const int*>(record + 0x344);
    int value = 0x80;
    if (record[0x364] && (x == 0 || x == 0x0C)) {
        value = 0x80 + static_cast<int>(AxisOf(record, x + 4) * kAxisScale);
        if (value < 0) value = 0;
        if (value > 255) value = 255;
    }
    record[0x13] = static_cast<uint8_t>(value);
}

// ---------------------------------------------------------------- stick and trigger bindings (docs/input.md)
// The game binds button actions to buttons and POV directions only (table 0, 32 inputs); axes only drive steering and
// the pedals. Stick directions and single triggers get the plugin's table (StickBinds) next to the record's: captured on
// the Controls screen like a button, named on its rows, decoded into the record's action bits.
#define AT_CAPTURE_POLL     0x5CF23D     // call 0x5CA720 (thiscall record): the Controls screen's capture poll
#define FN_CAPTURE_POLL     0x5CA720
#define AT_CAPTURE_BUTTON   0x5CA79D     // call 0x5B8460 (thiscall record, button): store a captured button
#define FN_CAPTURE_BUTTON   0x5B8460
#define AT_CAPTURE_POV      0x5CA7C5     // call 0x5C1720 (thiscall record, pov): store a captured POV direction
#define FN_CAPTURE_POV      0x5C1720
#define AT_BINDING_NAME     0x4ACF67     // call 0x5C20E0 (thiscall record, action, column) -> name, in 0x4ACF40
#define FN_BINDING_NAME     0x5C20E0
#define AT_RESET_BINDINGS   0x4ACDE4     // call 0x5C11A0 (thiscall record): "Reset Keys To Default" on a pad page
#define FN_RESET_BINDINGS   0x5C11A0
#define CAPTURING           0x870768     // byte: a capture is running (the decoder skips every joystick record)
#define PROFILE_NAME        0x83A9E0     // the loaded profile's name (32 bytes; the profile block, 0x52A280 this)
#define REC_CAPTURE_ACTION  0x33C        // action being captured (0 none; 1 gas, 2 brake, 3/4 steer: axis capture)
#define REC_CAPTURE_COLUMN  0x340        // 0 primary, 1 secondary
static const char* const kStickNames[STICK_INPUTS] = {
    "Left Stick X-", "Left Stick X+", "Left Stick Y+", "Left Stick Y-",
    "Right Stick X-", "Right Stick X+", "Right Stick Y+", "Right Stick Y-", "Left Trigger", "Right Trigger"};
static const int kStickPress = 5000, kStickRelease = 4000, kStickRest = 3000;   // of 10000

static StickBinds* StickTableFor(const uint8_t* rec) {
    if (!rec) return nullptr;
    if (rec == g_extraRecord) return &g_seatStore[g_extraSeat - 2].stick;
    if (rec == PadRecord(0)) return g_bindSwap >= 0 ? &g_seatStore[g_bindSwap - 2].stick : &g_stickBinds[0];
    if (rec == PadRecord(1)) return &g_stickBinds[1];
    return nullptr;
}

static int StickOwner(const StickBinds* sb) {      // player number, for the log
    for (int k = 0; k < MAX_EXTRA_PLAYERS; ++k) if (sb == &g_seatStore[k].stick) return k + 3;
    return sb == &g_stickBinds[1] ? 2 : 1;
}

static void StickChanged(const StickBinds* sb) {   // players 3-6 save now, players 1/2 with the profile
    for (int k = 0; k < MAX_EXTRA_PLAYERS; ++k)
        if (sb == &g_seatStore[k].stick) SaveSeatStore(k);
}

// The input's deflection 0..10000 in its own direction (lY / lRy grow downward; Y+ is the stick pushed up).
static int StickValue(const uint8_t* rec, int input) {
    static const int axes[4] = {0x00, 0x04, 0x0C, 0x10};
    if (input >= 8) {                      // rglSlider[0] = RT, [1] = LT, 0..255 (StateOf)
        LONG v = AxisOf(rec, input == 8 ? 0x1C : 0x18);
        return v <= 0 ? 0 : v >= 255 ? 10000 : static_cast<int>(v * 10000 / 255);
    }
    LONG v = AxisOf(rec, axes[input / 2]);
    if (input % 4 == 0 || input % 4 == 2) v = -v;    // X- and Y+ (up) are the negative side
    return v <= 0 ? 0 : v > 10000 ? 10000 : static_cast<int>(v);
}

// The game's rule for one more input on an action (0x5B8460): capturing the secondary keeps the first input already
// holding it (the primary) and clears the others; capturing the primary clears all. The order is the label's: the
// game's 32 inputs, then the plugin's.
static bool ClearStickAction(StickBinds* sb, int action, bool keepFirst) {
    bool changed = false;
    for (int i = 0; i < STICK_INPUTS; ++i) {
        if (sb->action[i] != action) continue;
        if (keepFirst) { keepFirst = false; continue; }
        sb->action[i] = 0;
        changed = true;
    }
    return changed;
}
static bool GameHasAction(const uint8_t* rec, int action) {
    const int* t = reinterpret_cast<const int*>(rec + REC_BIND_TABLES);
    for (int j = 0; j < 32; ++j) if (t[j] == action) return true;
    return false;
}

static const uint8_t* g_capRec;          // the capture being watched (reset when the record decodes again)
static int g_capAction;
static bool g_capArmed[STICK_INPUTS];    // input was back at rest since the capture began

static void StoreStick(uint8_t* rec, StickBinds* sb, int input, int action) {
    bool keep = *reinterpret_cast<int*>(rec + REC_CAPTURE_COLUMN) == 1;
    int* t = reinterpret_cast<int*>(rec + REC_BIND_TABLES);
    for (int j = 0; j < 32; ++j) {
        if (t[j] != action) continue;
        if (keep) { keep = false; continue; }
        t[j] = 0;
    }
    ClearStickAction(sb, action, keep);
    sb->action[input] = action;
    sb->wait[input] = true;              // the push that bound it doesn't fire it
    *reinterpret_cast<int*>(rec + REC_CAPTURE_ACTION) = 0;
    at<uint8_t>(CAPTURING) = 0;
    g_capRec = nullptr;
    StickChanged(sb);
    Log("sticks: %s bound to action %d (column %d)", kStickNames[input], action,
        *reinterpret_cast<int*>(rec + REC_CAPTURE_COLUMN));
}

static void __fastcall CapturePoll(uint8_t* rec) {
    reinterpret_cast<void(__thiscall*)(void*)>(CallPrev(AT_CAPTURE_POLL, FN_CAPTURE_POLL))(rec);
    const int action = *reinterpret_cast<int*>(rec + REC_CAPTURE_ACTION);
    StickBinds* sb = StickTableFor(rec);
    if (action <= 4 || !sb) return;      // none, or an axis role (the game's axis capture)
    if (rec != g_capRec || action != g_capAction) {
        g_capRec = rec;
        g_capAction = action;
        memset(g_capArmed, 0, sizeof(g_capArmed));
    }
    for (int i = 0; i < STICK_INPUTS; ++i) {
        int v = StickValue(rec, i);
        if (v < kStickRest) g_capArmed[i] = true;
        else if (g_capArmed[i] && v > kStickPress) { StoreStick(rec, sb, i, action); return; }
    }
}

// A button or POV direction captured by the game takes the action from the plugin's inputs by the same rule.
static void AfterGameCapture(uint8_t* rec, int action, int column, bool gameHad) {
    StickBinds* sb = StickTableFor(rec);
    if (action <= 4 || !sb) return;
    if (ClearStickAction(sb, action, column == 1 && !gameHad)) StickChanged(sb);
}
static void __fastcall CaptureButton(uint8_t* rec, void*, int button) {
    int action = *reinterpret_cast<int*>(rec + REC_CAPTURE_ACTION), column = *reinterpret_cast<int*>(rec + REC_CAPTURE_COLUMN);
    bool had = GameHasAction(rec, action);
    reinterpret_cast<void(__thiscall*)(void*, int)>(CallPrev(AT_CAPTURE_BUTTON, FN_CAPTURE_BUTTON))(rec, button);
    AfterGameCapture(rec, action, column, had);
}
static void __fastcall CapturePov(uint8_t* rec, void*, int pov) {
    int action = *reinterpret_cast<int*>(rec + REC_CAPTURE_ACTION), column = *reinterpret_cast<int*>(rec + REC_CAPTURE_COLUMN);
    bool had = GameHasAction(rec, action);
    reinterpret_cast<void(__thiscall*)(void*, int)>(CallPrev(AT_CAPTURE_POV, FN_CAPTURE_POV))(rec, pov);
    AfterGameCapture(rec, action, column, had);
}

// A row's label: the game names its own inputs (column 0 = the first input holding the action, 1 = the second);
// past them come the plugin's.
static const char* __fastcall BindingName(uint8_t* rec, void*, int action, int column) {
    const char* name = reinterpret_cast<const char*(__thiscall*)(void*, int, int)>(
        CallPrev(AT_BINDING_NAME, FN_BINDING_NAME))(rec, action, column);
    StickBinds* sb = StickTableFor(rec);
    if ((name && name[0]) || action <= 4 || !sb) return name;
    int k = column;
    const int* t = reinterpret_cast<const int*>(rec + REC_BIND_TABLES);
    for (int j = 0; j < 32; ++j) if (t[j] == action) --k;
    if (k < 0) return name;
    for (int i = 0; i < STICK_INPUTS; ++i) {
        if (sb->action[i] != action || k-- != 0) continue;
        static int8_t shown[64][2];      // input + 1 last logged per action and column
        if (action < 64 && column >= 0 && column < 2 && shown[action][column] != i + 1) {
            shown[action][column] = static_cast<int8_t>(i + 1);
            Log("sticks: label action %d column %d = %s (player %d)", action, column, kStickNames[i], StickOwner(sb));
        }
        return kStickNames[i];
    }
    return name;
}

static void __fastcall ResetBindings(uint8_t* rec) {
    reinterpret_cast<void(__thiscall*)(void*)>(CallPrev(AT_RESET_BINDINGS, FN_RESET_BINDINGS))(rec);
    StickBinds* sb = StickTableFor(rec);
    if (!sb) return;
    memset(sb->action, 0, sizeof(sb->action));
    StickChanged(sb);
    Log("sticks: cleared with Reset Keys To Default");
}

// In the decoder (after the action bits): a bound input past its threshold sets its action (bits are active low).
static void StickDecode(uint8_t* rec) {
    if (rec == g_capRec) g_capRec = nullptr;   // decoding again: the capture is over
    StickBinds* sb = StickTableFor(rec);
    if (!sb) return;
    auto& bits = *reinterpret_cast<uint64_t*>(rec + 8);
    for (int i = 0; i < STICK_INPUTS; ++i) {
        int v = StickValue(rec, i);
        if (sb->wait[i]) { sb->wait[i] = v > kStickRelease; continue; }
        bool held = sb->held[i] ? v > kStickRelease : v > kStickPress;
        if (held != sb->held[i] && sb->action[i])
            Log("sticks: player %d %s %s (action %d)", StickOwner(sb), kStickNames[i], held ? "pressed" : "released",
                sb->action[i]);
        sb->held[i] = held;
        if (held && sb->action[i] > 0 && sb->action[i] < 64) bits &= ~(1ull << sb->action[i]);
    }
}

// Players 1/2's entries follow the game profile, as their button bindings do: saved and loaded with the profile's
// bindings, in the plugin's ini under the profile's name.
static void ProfileSection(char* out, size_t n) {
    char name[32] = {};
    memcpy(name, reinterpret_cast<const void*>(PROFILE_NAME), 31);
    for (char* c = name; *c; ++c)
        if (static_cast<unsigned char>(*c) < 0x20 || *c == '[' || *c == ']' || *c == '=' || *c == ';') *c = '_';
    sprintf_s(out, n, "Profile %s", name);
}
static void SaveProfileSticks() {
    char section[48], buf[64];
    ProfileSection(section, sizeof(section));
    for (int p = 0; p < 2; ++p) {
        StickToText(g_stickBinds[p], buf, sizeof(buf));
        WritePrivateProfileStringA(section, p ? "StickP2" : "StickP1", buf, g_playersPath);
    }
    Log("sticks: saved for [%s]", section);
}
static void LoadProfileSticks() {
    char section[48], buf[64];
    ProfileSection(section, sizeof(section));
    for (int p = 0; p < 2; ++p) {
        GetPrivateProfileStringA(section, p ? "StickP2" : "StickP1", "", buf, sizeof(buf), g_playersPath);
        StickFromText(buf, &g_stickBinds[p]);
    }
    Log("sticks: loaded for [%s]", section);
}

static void InstallStickBindings() {
    WriteCall(AT_CAPTURE_POLL, &CapturePoll);
    WriteCall(AT_CAPTURE_BUTTON, &CaptureButton);
    WriteCall(AT_CAPTURE_POV, &CapturePov);
    WriteCall(AT_BINDING_NAME, &BindingName);
    WriteCall(AT_RESET_BINDINGS, &ResetBindings);
}

static void __fastcall AnalogBytes(uint8_t* record, int shared) {
    if (shared) SharedPedals(record);
    SteeringStickY(record);
    StickDecode(record);
}

static __declspec(naked) void SharedPedalsEndHook() {
    __asm {
        mov ecx, esi
        mov edx, 1
        call AnalogBytes
        mov eax, AT_AXES_EPILOGUE
        jmp eax
    }
}

static __declspec(naked) void SeparatePedalsEndHook() {
    __asm {
        mov byte ptr [esi + 0x10], cl    // the replaced throttle store (+0x13 is AnalogBytes')
        mov ecx, esi
        xor edx, edx
        call AnalogBytes
        mov eax, AT_AXES_EPILOGUE
        jmp eax
    }
}

static void InstallAnalogBytes() {
    if (!RetargetPedalEvents()) { Log("input: pedal event records not as expected, analog bytes left to the game"); return; }
    WriteJump(AT_SHARED_PEDALS_END, &SharedPedalsEndHook, 8);
    WriteJump(AT_SEPARATE_PEDALS_END, &SeparatePedalsEndHook, 7);
}
