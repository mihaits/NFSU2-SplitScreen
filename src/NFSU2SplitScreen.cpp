// NFSU2SplitScreen.asi - native 2-4 player split screen for Need for Speed Underground 2 (PC, speed2.exe v1.2).
//
// The PC build still contains the console's two-player split-screen code (game mode 4, the 2P HUDs, two-player race
// setup, a two-position streamer) but the frontend entry, the renderer's split path and the input separation were
// cut or never finished. This plugin restores them and extends the game from 2 to 4 local players: player arrays,
// input port tables, sound listeners and per-view render state are relocated or generalized, players 3/4 get their
// own views (5/6), HUD package copies, minimaps, cameras, streaming, reflections, weather and effects, and every
// player gets their car's single-player gauges.
//
// Everything is patched at load time (WriteCall / WriteJump / trampolines on fixed addresses). Each subsystem below
// starts with a comment block explaining the game code involved; docs/TECHNICAL.md is the overview and index.
//
// Addresses are for speed2.exe v1.2 (image base 0x400000, no ASLR). IsSupportedExe() checks the patched bytes first.

#include <windows.h>
#include <intrin.h>
#include <shlwapi.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <cstring>

#include "XInputDevices.h"

// ---------------------------------------------------------------- game addresses
// (#defines so the inline-asm hooks can use them as immediates)
#define GAME_MODE          0x850078    // 2 quick race, 4 split screen
#define QR_SETTINGS        0x83AAC4    // per race type, stride 0x14, byte 0 = local player count
#define QR_RACE_TYPES      13
#define FE_PLAYER          0x85AD28    // player the frontend currently serves
#define FE_PORTS           0x7F4454    // [2] controller bitmask bound to each FE player
#define GARAGE             0x83A9D0    // profile/garage object (player car list at +0x3C0, car id at +0xE8)
#define GARAGE_LIST        0x3C0
#define GARAGE_LIST_BACKUP 0x1677C     // console split screen backs the list up here; the QR flow's back step
#define GARAGE_LIST_SIZE   (0x26F6 * 4) // to the main menu restores it in mode 4 (0x515D70)
#define GARAGE_CAR         0xE8
#define GARAGE_CAR_BACKUP  0x20354
#define NUM_PLAYERS        0x89E7C0    // local players in the active race
#define LAYOUT             0x8707E0    // applied view layout (3 = two player cameras)
#define SCREEN_W           0x870980
#define SCREEN_H           0x870984
#define D3D_DEVICE         0x870974
#define VIEWS              0x832DE0    // eView[], stride 0x70
#define RT_BACKBUFFER      0x86E768    // render target struct shared by view 1 and the HUD view
#define INPUT_DEVICES      0x874C40    // input device records, stride 0x370
#define INPUT_DEVICE_MAX   6
#define INPUT_FOCUS        0x870988    // non-zero while devices are polled

#define IMPORT_DIRECTINPUT8CREATE 0x783024  // IAT slot used by the game's DirectInput8Create thunk
#define FN_LAYOUT_APPLY    0x5C21E0    // per-frame view layout (cdecl, no args)
#define FN_POLL_PORT       0x5CFD60    // void* __cdecl (int port, int* type, void** prev)
#define AT_QR_SET_MODE     0x4FC061    // Quick Race react: mov [GAME_MODE],2           (10 bytes)
#define AT_QR_SET_MODE_RET 0x4FC06B
#define AT_MAINMENU_QR     0x4AEED8    // main menu builder: `call 0x570A60` after adding Quick Race (5 bytes)
#define AT_MAINMENU_QR_RET 0x4AEEDD
#define FN_MAINMENU_CHECK  0x570A60    // the call we displace
#define FN_MALLOC          0x575620    // void* __cdecl (size)
#define FN_MENU_OPTION_INIT 0x51F670   // __thiscall (option, iconHash, textHash, 0)
#define FN_QR_REACT        0x4FC050    // Quick Race option react (__thiscall, 5 stack args)
#define FN_MENU_OPTION_DTOR 0x4AEB70
#define MMSPLIT_ICON       0x883E25D6   // console "MMSplitScreen" icon/text, still in the PC data files
#define MMSPLIT_TEXT       0x6A2B93EB   // "2 Player Split-Screen" -> shown as "Split-Screen" (RenameSplitMenuText)
#define AT_JOYPAD_TITLE    0x4DCA18    // Controls joypad page: `call 0x50C900` setting the page title (5 bytes)
#define FN_SET_TEXT_HASH   0x50C900    // void __cdecl (FEObject*, languageHash)
#define FN_SET_TEXT_FMT    0x537BE0    // int __cdecl (FEObject*, const char* fmt, ...)
#define FN_LOCALIZE        0x4FFA80    // const char* __cdecl (languageHash)
#define CONTROLS_PAGE      0x8364EC    // current Controls page
#define CONTROLS_PAGE_DEV  0x835938    // [page * 3] = input device record shown on that page
#define AT_FE_OBJ_XFORM    0x51B0B0    // FE object transform (thiscall: data, out mtx, out color, level, scale) entry
#define AT_FE_OBJ_XFORM_RET 0x51B0B6   // (6 bytes: push ebp; mov ebp,esp; and esp,-16)
#define FN_FE_PACKAGE_ACTIVE 0x52CF60  // bool __cdecl (const char* fng)
#define FN_FE_SEND_MSG     0x55DDA0    // void __cdecl (msg, const char* fng)
#define FN_FE_SWITCH       0x537980    // void __cdecl (const char* fromFng, const char* toFng, flags)
#define AT_PRESSSTART      0x498070    // 2P_PressStart handler entry                   (9 bytes)
#define AT_PRESSSTART_RET  0x498079
#define AT_CARSELECT       0x4FC1F0    // split-screen car select message handler entry (5 bytes)
#define AT_CARSELECT_RET   0x4FC1F5
#define FE_INPUT_CUR       0x86EAF0    // merged device state the frontend reads (16 bytes)
#define FE_INPUT_PREV      0x86B7E0
#define AT_VIEWLOOP_TOP    0x5CB000    // player-view render loop body start            (5 bytes)
#define AT_VIEWLOOP_TOP_RET 0x5CB005
#define AT_VIEWLOOP_EXIT   0x5CB72D    // after the loop                                (6 bytes)
#define AT_VIEWLOOP_EXIT_RET 0x5CB733
#define AT_VIEWLOOP_MAX    0x5CB722    // imm8 of `cmp eax,1` (last view index)
#define FN_GET_VIEW        0x48B1E0    // View* __cdecl (id)
#define AT_WORLD_VIEWS     0x5B9CAA    // world renderer per-frame view registration, after views 1..3 (5 bytes)
#define AT_WORLD_VIEWS_RET 0x5B9CAF
#define FN_WORLD_ADD_VIEW  0x4888A0    // void __cdecl (View*, entry, flags)
#define AT_SKY_CALL        0x5CB227    // view loop: `call 0x60F880` (sky dome, cdecl View*) (5 bytes)
#define FN_DRAW_SKY        0x60F880    // draws only for view ids 1..3 and the env map views
#define WORLD_VIEW_COUNT   0x6C0       // entries of 0x90 bytes, 12 max
#define FN_VIEW_CULL_CTOR  0x5E0260    // per-view object at +0x60 (0x3B0 bytes, thiscall, no args)
#define FN_VIEW_RENDER_CTOR 0x61E150   // per-view object at +0x64 (0x51C0 bytes, thiscall (view, 0))
#define AT_PROJECTION      0x5C80B0    // projection built, before view-proj/frustum    (10 bytes)
#define AT_PROJECTION_RET  0x5C80BA
#define AT_SECTION_REMAP   0x5DACDB    // `jnz` skipping the 2P section remap when not 2 players -> jmp

#define MSG_START          0xB5AF2461
#define MSG_ACCEPT         0x0C407210
#define MSG_PC_ACTIVATE    0x9803F6E2
#define MSG_BACK           0x911AB364
#define MSG_FRAME_TICK     0xC98356BA   // sent to the active FE screen every frame

template <class T> static T& at(uintptr_t a) { return *reinterpret_cast<T*>(a); }

struct RenderTarget {  // 0x1C bytes
    int type, ownerView;
    void* surface;
    void* depth;
    int enabled, width, height;
};
static_assert(sizeof(RenderTarget) == 0x1C, "render target layout");

struct View {  // 0x70 bytes
    uint8_t* platInfo;  // +0x00
    int id;             // +0x04
    uint8_t active;     // +0x08
    uint8_t pad09[0x40 - 0x09];
    uint8_t* camera;    // +0x40
    uint8_t pad44[0x58 - 0x44];
    RenderTarget* rt;   // +0x58
    uint8_t pad5c[0x70 - 0x5C];
};
static_assert(sizeof(View) == 0x70, "eView layout");

struct Viewport9 { DWORD X, Y, Width, Height; float MinZ, MaxZ; };  // D3DVIEWPORT9

static View& GetView(int n) { return reinterpret_cast<View*>(VIEWS)[n]; }

// ---------------------------------------------------------------- state
static bool g_xinputPads = true;          // XInput pads appear to the game as DirectInput gamepads
static bool g_hideDirectInputPads = true; // ...and the real DirectInput game controllers are hidden
static bool g_menuSplit = false;         // set by the main menu "Split Screen" option, consumed by Quick Race
static int  g_prevMode = 0;
#define MAX_LOCAL_PLAYERS 4
static const int kPlayerViewId[MAX_LOCAL_PLAYERS] = {1, 2, 5, 6};   // views 5..8 are unused by the game
static RenderTarget g_rt[MAX_LOCAL_PLAYERS]; // private targets for the player views (one per screen region)
static int g_screenViews = 2;            // player views drawn this frame (2, or 4 for quadrants)
static bool g_viewTintInit[2];           // views 5/6 have their own screen tint entries (see TintUpdateWrapper)
static bool g_quadTest = false;          // debug: 2P race drawn in quadrants, views 5/6 reuse P1/P2's cameras
static int  g_extraPlayers = 0;          // debug: players 3.. added to 2P races (cloning P1/P2's car and profile)
static int g_vpLogFrames = 0;            // debug: log SetViewport calls for N frames (mailbox command 6)
static int  g_currentView = 0;           // view being drawn by the player-view loop (read by asm)
static void* g_origSetViewport = nullptr;
static uint8_t g_portCur[4][16], g_portPrev[4][16];
static int g_port0Type = 0;

static void ResetAssignments();
static void* ExtraCamera(int k);
static void InitPlayerView(int id);

static bool SplitMode()   { return at<int>(GAME_MODE) == 4; }
static bool SplitRacing() { return SplitMode() && at<int>(NUM_PLAYERS) == 2 && at<int>(LAYOUT) == 3; }

// ---------------------------------------------------------------- patch helpers
static void Write(uintptr_t addr, const void* data, size_t n) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(addr), data, n);
    VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
}

static void WriteJump(uintptr_t where, const void* to, size_t len) {
    uint8_t buf[64];
    if (len < 5 || len > sizeof(buf)) return;
    buf[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(to) - (where + 5));
    memcpy(buf + 1, &rel, 4);
    memset(buf + 5, 0x90, len - 5);
    Write(where, buf, len);
}

// Callable copy of the first `len` bytes at `where` (must be whole, position-independent instructions).
static void* MakeTrampoline(uintptr_t where, size_t len) {
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, reinterpret_cast<void*>(where), len);
    tramp[len] = 0xE9;
    int32_t rel = static_cast<int32_t>((where + len) - (reinterpret_cast<uintptr_t>(tramp) + len + 5));
    memcpy(tramp + len + 1, &rel, 4);
    return tramp;
}

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

// ---------------------------------------------------------------- debug log (Debug=1 in the ini)
static char g_logPath[MAX_PATH];
static char g_iniPath[MAX_PATH];
static bool g_debug = false;

static void Log(const char* fmt, ...) {
    if (!g_debug) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (FILE* f = fopen(g_logPath, "a")) {
        fprintf(f, "%8lu %s\n", GetTickCount(), line);
        fclose(f);
    }
}

// Debug: values left on the x87 register stack (a game function returning a float in ST0 called as void leaks one
// value per call; after 8 every x87 calculation returns garbage).
static int X87Depth() {
    uint16_t env[14];
    __asm fnstenv env
    __asm fldcw word ptr env              // fnstenv masks exceptions; restore the control word
    int used = 0;
    for (int i = 0; i < 8; ++i) used += ((env[4] >> (2 * i)) & 3) != 3;
    return used;
}
static void CheckX87(const char* where) {
    static char last[64];
    int d = X87Depth();
    if (d && strcmp(last, where)) { Log("x87 stack: %d values at %s", d, where); strcpy_s(last, where); }
}


// ---------------------------------------------------------------- split-screen HUD
// WidescreenFix moves the 1P HUD objects to the screen edges by matching their exact FE positions; the 2P HUDs
// have other positions and stay in the 4:3 area. While split racing, top-level FE objects that sit on a side of
// the screen (FE space is 640x480 around the centre) are pushed out by the extra width of the real aspect ratio.
// The game reads the object data through the pointer we hand it, so a shifted copy is used (never the original).
#define HUD_SIDE_MIN_X 100.0f            // closer to the centre = centred element (lap counter, countdown, ...)
static float g_hudCopies[16][0x14];
static int g_hudCopyNext = 0;
static int g_hudLogged = 0;
static bool g_inCarSelect = false;       // per frame: split car select is the active screen
static bool g_editingSlot2 = false;      // per frame: Controls screen shows player 2's joypad page
static bool g_hudWiden = false;          // per frame: split racing and no menu over the HUD

static void UpdateEditingSlot2() {
    using ActiveFn = bool(__cdecl*)(const char*);
    auto active = reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE);
    g_inCarSelect = SplitMode() && active("UI_QRCarSelect.fng");
    bool editing = false;
    int device = at<int>(CONTROLS_PAGE_DEV + at<int>(CONTROLS_PAGE) * 12);
    if (device >= 0 && device < INPUT_DEVICE_MAX && at<int>(INPUT_DEVICES + device * 0x370 + 0x230) == 1)
        editing = active("UI_Options_PC_Controller.fng");
    if (editing != g_editingSlot2) Log("editing joypad slot 2: %d", editing);
    g_editingSlot2 = editing;
}

// Quadrant HUDs (3-4 players): every object of a player's HUD package is mapped to that player; while it renders
// (0x537262 wrapper) its top-level transform is moved into the player's quadrant: the 2P layouts (top/bottom half,
// full width) are compressed horizontally into the quadrant and scaled uniformly by the same factor.
static std::unordered_map<const void*, int> g_hudObjSlot;
static const void* g_hudPackages[MAX_LOCAL_PLAYERS];
static int g_renderSlot = -1;
static bool g_quadHud = false;           // per frame: 4 views and HUD packages mapped

// Per-player minimaps (3-4 players). The minimap belongs to P1's HUD (2PHudTop: map, mask, north arrow, target
// ring, car indicators, all around (-230, 5)) and is driven by one manager (0x8364D4): its update 0x4CA840 centres
// the map on car entity (type 4, id 0) and places every entity's marker; a type-4 entity's id indexes the local
// players (0x4AC374). Each frame the update runs once per player centred on that player's car (the id pushed at
// 0x4CA900), the minimap objects' data is snapshotted, and the last run (P1) leaves the game's own state. The render
// wrapper then draws every minimap object once per player from its snapshot, in that player's quadrant.
#define MINIMAP_X       -230.0f
#define MINIMAP_Y       5.0f
#define MINIMAP_RADIUS  100.0f
#define MM_MAX_OBJS     192             // map pieces + every marker object of the package (17 kinds x 8)
#define MM_DATA_SIZE    0x54             // FE object data: transform, colour, rotation, UV rect
static uint8_t* g_mmObj[MM_MAX_OBJS];    // P1's minimap objects (groups and their children)
static uint8_t* g_mmData[MM_MAX_OBJS];   // data of the top-level minimap objects, groups and their children
static int g_mmObjCount = 0, g_mmDataCount = 0;
// Group transforms are computed ahead of rendering (0x51B7F0) and cached, so a group (the target ring) can't be
// moved per player: while per-player minimaps are on, its transform is the origin and its children (level 1) are
// placed per player instead.
static const void* g_mmGroupData[4];
static const void* g_mmChildData[MM_MAX_OBJS];
static int g_mmGroupCount = 0, g_mmChildCount = 0;
static uint8_t g_mmSnap[MAX_LOCAL_PLAYERS][MM_MAX_OBJS][MM_DATA_SIZE];
// Snapshots are keyed by the data they were taken from: the object lists are rebuilt every 30 frames (markers join
// and leave depending on where they are), and restoring by list index after a rebuild wrote one object's data into
// another (the map disc got a marker's size and colour -> broken minimaps, user report).
static uint8_t* g_mmSnapData[MM_MAX_OBJS];
static int g_mmSnapCount = 0;

static void TakeMinimapSnapshot(int k) {
    g_mmSnapCount = g_mmDataCount;      // same list for every player of a frame (no remap inside MinimapUpdate)
    memcpy(g_mmSnapData, g_mmData, sizeof(g_mmSnapData[0]) * g_mmDataCount);
    for (int i = 0; i < g_mmSnapCount; ++i) memcpy(g_mmSnap[k][i], g_mmSnapData[i], MM_DATA_SIZE);
}

static void ApplyMinimapSnapshot(int k) {
    for (int i = 0; i < g_mmSnapCount; ++i) memcpy(g_mmSnapData[i], g_mmSnap[k][i], MM_DATA_SIZE);
}
static int g_mmPlayers = 0;              // per frame: players with a minimap snapshot
static int g_mmTarget = -1;              // while rendering a minimap object: whose copy

static bool NearMinimap(const float* data) {
    return fabsf(data[7] - MINIMAP_X) < MINIMAP_RADIUS && fabsf(data[8] - MINIMAP_Y) < MINIMAP_RADIUS;
}

static void AddMinimapObject(uint8_t* obj, bool child) {
    auto* data = *reinterpret_cast<uint8_t**>(obj + 0x2C);
    if (g_mmObjCount < MM_MAX_OBJS) g_mmObj[g_mmObjCount++] = obj;
    if (child && data && g_mmChildCount < MM_MAX_OBJS) g_mmChildData[g_mmChildCount++] = data;
    if (*reinterpret_cast<int*>(obj + 0x18) == 5) {
        if (data && g_mmGroupCount < 4) g_mmGroupData[g_mmGroupCount++] = data;
        for (auto* c = *reinterpret_cast<uint8_t**>(obj + 0x64); c; c = *reinterpret_cast<uint8_t**>(c + 4))
            AddMinimapObject(c, true);
    }
    if (data && g_mmDataCount < MM_MAX_OBJS) g_mmData[g_mmDataCount++] = data;
}

// Car markers of players 3/4: PlayerCarIndicator_1/_2 of P3's copy of the package (registered as markers of the
// minimap manager, see ExtendMinimapManager); drawn per player like P1's minimap objects, with their own colours.
static uint8_t* g_mmExtraIcon[2];
static uint8_t* g_mmMarker[MAX_LOCAL_PLAYERS];   // car markers: P1's package _1/_2 (P1, P2), P3's copy _1/_2 (P3, P4)
static unsigned g_mmFrame = 0, g_mmMarkersFrame = 0;
static const uint8_t kMarkerColour[2][4] = {{230, 90, 40, 255}, {40, 210, 230, 255}};  // P3 blue, P4 yellow (B, G, R, A)

static uint8_t* FindFeObject(const uint8_t* pkg, uint32_t hash) {
    for (auto* obj = *reinterpret_cast<uint8_t* const*>(pkg + 0x4C); obj; obj = *reinterpret_cast<uint8_t**>(obj + 4))
        if (*reinterpret_cast<uint32_t*>(obj + 0x10) == hash) return obj;
    return nullptr;
}

// Minimap objects by name: the map, its mask, north arrow and ring, and markers (the name patterns 0x4DB900 uses,
// table 0x7F6828: 17 x {format, type}, indices 0..7) that are in use (near the map; spare ones are parked far off).
// Other HUD parts near the map (drift score rows) are not minimap objects.
#define MARKER_NAME_TABLE  0x7F6828
static uint32_t g_mmFixedHash[4];
static uint32_t g_mmMarkerHash[17 * 8];
static const void* g_mmHideData[MM_MAX_OBJS];   // P3's copy's own minimap pieces (hidden)
static int g_mmHideCount = 0;

static void InitMinimapNames() {
    if (g_mmFixedHash[0]) return;
    using HashFn = uint32_t(__cdecl*)(const char*);
    auto hash = reinterpret_cast<HashFn>(0x505450);
    const char* fixed[4] = {"Track2000_map", "Minimap_Mask", "Minimap_North_indicator", "TrackMapTargetRing"};
    for (int i = 0; i < 4; ++i) g_mmFixedHash[i] = hash(fixed[i]);
    for (int t = 0; t < 17; ++t)
        for (int i = 0; i < 8; ++i) {
            char name[160];
            sprintf_s(name, at<const char*>(MARKER_NAME_TABLE + t * 8), i);
            g_mmMarkerHash[t * 8 + i] = hash(name);
        }
}

// Every marker object belongs to the minimap whether or not it is on the map right now: the lists stay the same while
// players move (filtering by position made markers drop out and flicker), and each player's minimap draws only the
// markers that are on its map (DrawsOnMinimap).
static bool IsMinimapObject(const uint8_t* obj) {
    uint32_t h = *reinterpret_cast<const uint32_t*>(obj + 0x10);
    for (uint32_t f : g_mmFixedHash)
        if (h == f) return true;
    if (!*reinterpret_cast<float* const*>(obj + 0x2C)) return false;
    for (uint32_t m : g_mmMarkerHash)
        if (h == m) return true;
    return false;
}

static bool IsMarkerHash(uint32_t h) {
    for (uint32_t m : g_mmMarkerHash)
        if (h == m) return true;
    return false;
}

// with the current player's snapshot applied: markers away from the map centre are spare/parked ones
static bool DrawsOnMinimap(const uint8_t* obj) {
    if (!IsMarkerHash(*reinterpret_cast<const uint32_t*>(obj + 0x10))) return true;
    auto* data = *reinterpret_cast<const float* const*>(obj + 0x2C);
    return data && NearMinimap(data);
}

static void HideCopyMinimap(const uint8_t* copy) {
    if (!copy) return;
    for (auto* obj = *reinterpret_cast<uint8_t* const*>(copy + 0x4C); obj && g_mmHideCount < MM_MAX_OBJS;
         obj = *reinterpret_cast<uint8_t**>(obj + 4))
        if (obj != g_mmExtraIcon[0] && obj != g_mmExtraIcon[1] && IsMinimapObject(obj))
            g_mmHideData[g_mmHideCount++] = *reinterpret_cast<void**>(obj + 0x2C);
}

static void MapMinimapObjects(const uint8_t* pkg, const uint8_t* p3pkg, const uint8_t* p2pkg, const uint8_t* p4pkg) {
    InitMinimapNames();
    if (!pkg) g_mmSnapCount = 0;
    g_mmHideCount = 0;
    g_mmObjCount = g_mmDataCount = g_mmGroupCount = g_mmChildCount = 0;
    g_mmExtraIcon[0] = g_mmExtraIcon[1] = nullptr;
    memset(g_mmMarker, 0, sizeof(g_mmMarker));
    if (!pkg) return;
    using HashFn = uint32_t(__cdecl*)(const char*);
    auto hash = reinterpret_cast<HashFn>(0x505450);       // FE name hash
    g_mmMarker[0] = FindFeObject(pkg, hash("PlayerCarIndicator_1"));
    g_mmMarker[1] = FindFeObject(pkg, hash("PlayerCarIndicator_2"));
    if (p3pkg) {
        g_mmExtraIcon[0] = FindFeObject(p3pkg, hash("PlayerCarIndicator_1"));
        g_mmExtraIcon[1] = g_extraPlayers >= 2 ? FindFeObject(p3pkg, hash("PlayerCarIndicator_2")) : nullptr;   // P4
        for (uint8_t* icon : g_mmExtraIcon)
            if (icon) AddMinimapObject(icon, false);
        g_mmMarker[2] = g_mmExtraIcon[0];
        g_mmMarker[3] = g_mmExtraIcon[1];
        for (auto* obj = *reinterpret_cast<uint8_t* const*>(p3pkg + 0x4C); obj && g_mmHideCount < MM_MAX_OBJS;
             obj = *reinterpret_cast<uint8_t**>(obj + 4))
            if (obj != g_mmExtraIcon[0] && obj != g_mmExtraIcon[1] && IsMinimapObject(obj))
                g_mmHideData[g_mmHideCount++] = *reinterpret_cast<void**>(obj + 0x2C);
    }
    HideCopyMinimap(p2pkg);
    HideCopyMinimap(p4pkg);
    for (auto* obj = *reinterpret_cast<uint8_t* const*>(pkg + 0x4C); obj; obj = *reinterpret_cast<uint8_t**>(obj + 4))
        if (IsMinimapObject(obj)) AddMinimapObject(obj, false);
    // snapshot entries of objects no longer mapped (left the map, or freed with a rebuilt HUD) are dropped
    int kept = 0;
    for (int i = 0; i < g_mmSnapCount; ++i) {
        bool mapped = false;
        for (int j = 0; j < g_mmDataCount; ++j) mapped |= g_mmData[j] == g_mmSnapData[i];
        if (!mapped) continue;
        if (kept != i) {
            g_mmSnapData[kept] = g_mmSnapData[i];
            for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) memcpy(g_mmSnap[k][kept], g_mmSnap[k][i], MM_DATA_SIZE);
        }
        ++kept;
    }
    g_mmSnapCount = kept;
}

// FE objects keep their transform until marked changed (obj+0x1C bit 0x400000); objects that were laid out before
// they were mapped (a restarted race rebuilds the HUDs) would keep the unmapped placement, so every remap (30 frames)
// marks them.
static void MapHudObjects(const uint8_t* obj, int slot) {
    for (; obj; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4)) {
        *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C) |= 0x400000;
        g_hudObjSlot[obj] = slot;
        if (const void* data = *reinterpret_cast<const void* const*>(obj + 0x2C)) g_hudObjSlot[data] = slot;
        if (*reinterpret_cast<const int*>(obj + 0x18) == 5)        // group: children at +0x64
            MapHudObjects(*reinterpret_cast<const uint8_t* const*>(obj + 0x64), slot);
    }
}

static const void* HudPackageOf(int k) {
    auto* player = static_cast<uint8_t*>(nullptr);
    extern void* GetLocalPlayer(int);
    player = static_cast<uint8_t*>(GetLocalPlayer(k));
    if (!player) return nullptr;
    auto* hud = *reinterpret_cast<uint8_t**>(player + 0x9A4);
    if (!hud) return nullptr;
    const char* name = *reinterpret_cast<const char**>(hud + 0x10);
    if (!name) return nullptr;
    // the cache's own lookup (0x51BB70) handles stored and compressed chunks (the copies are stored)
    auto* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, const char*)>(0x51BB70)(reinterpret_cast<void*>(0x8637B0), name);
    if (e) return *reinterpret_cast<void**>(e + 0x18);
    return nullptr;
}

static void UpdateQuadHud() {
    g_quadHud = false;
    if (g_mmExtraIcon[0] && !HudPackageOf(2)) {   // P3's HUD gone (back in the menus): the frontend reuses the memory
        g_mmExtraIcon[0] = g_mmExtraIcon[1] = nullptr;   // (help bar buttons matched these and were skipped)
        memset(g_mmMarker, 0, sizeof(g_mmMarker));
    }
    if (!SplitRacing() || g_screenViews < 3) return;
    bool changed = false;
    const void* pk[MAX_LOCAL_PLAYERS] = {};
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        pk[k] = HudPackageOf(k);
        changed |= pk[k] != g_hudPackages[k];
    }
    static int frames = 0;
    if (++frames >= 30) { frames = 0; changed = true; }   // HUD elements add objects at runtime
    if (changed) {
        g_hudObjSlot.clear();
        for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
            g_hudPackages[k] = pk[k];
            if (pk[k]) MapHudObjects(*reinterpret_cast<const uint8_t* const*>(
                static_cast<const uint8_t*>(pk[k]) + 0x4C), k);
        }
        MapMinimapObjects(static_cast<const uint8_t*>(pk[0]), static_cast<const uint8_t*>(pk[2]),
                          static_cast<const uint8_t*>(pk[1]), static_cast<const uint8_t*>(pk[3]));
        static size_t lastCount = 0;
        if (g_hudObjSlot.size() != lastCount)
            Log("quad HUD: mapped %d objects", static_cast<int>(g_hudObjSlot.size()));
        lastCount = g_hudObjSlot.size();
    }

    g_quadHud = !g_hudObjSlot.empty();
    if (!g_quadHud) g_mmPlayers = 0;
}

static int g_pauseOwner = -1;            // player 3/4 who opened the pause menu (-1: none / player 1-2)
static int g_pausePort = -1;             // any player who opened the pause menu (-1: none)
static bool g_pauseSeen = false;         // the pause menus were seen open since g_pauseOwner was set

static void UpdateHudWiden() {
    using ActiveFn = bool(__cdecl*)(const char*);
    auto active = reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE);
    g_hudWiden = SplitRacing() && !active("UI_Pause.fng") && !active("UI_PauseOptions.fng") &&
                 !active("UI_PauseOptionsMain.fng") && !active("UI_PC_Customize_Options.fng");
    if (!SplitRacing()) g_pauseOwner = g_pausePort = -1;
    else if (!g_hudWiden) g_pauseSeen = true;
    else if (g_pauseSeen) g_pauseOwner = g_pausePort = -1, g_pauseSeen = false;   // pause menus closed: slots back
}

// Player k's minimap sits in the free left corner of the quadrant: bottom-left with P1's top-half layout (P1, P3),
// top-left with P2's bottom-half layout (P2, P4; its drift score panel takes the lower left). (x, y) relative to the
// minimap centre.
static void MinimapPlace(float* copy, int k, float x, float y) {
    float aspect = static_cast<float>(at<int>(SCREEN_W)) / static_cast<float>(at<int>(SCREEN_H));
    float halfW = 240.0f * aspect;
    const float ms = 0.55f;
    float left = (k & 1) ? 0.0f : -halfW, top = (k & 2) ? 0.0f : -240.0f;
    if (k == 2 && g_screenViews == 3) left = -halfW / 2;  // 3 players: P3 centred
    bool topLayout = true;               // all players use the top-half layout in quadrants
    copy[7] = left + 52.0f + x * ms;
    copy[8] = (topLayout ? top + 240.0f - 48.0f : top + 52.0f) + y * ms;
    copy[0xE] *= ms;
    copy[0xF] *= ms;
    copy[4] *= ms;                       // rotation pivot (north indicator: 72 below it, at the map centre)
    copy[5] *= ms;
    copy[6] *= ms;
}

static bool Contains(const void* const* list, int n, const void* p) {
    for (int i = 0; i < n; ++i)
        if (list[i] == p) return true;
    return false;
}

static bool QuadTransform(float* copy, int slot) {
    float aspect = static_cast<float>(at<int>(SCREEN_W)) / static_cast<float>(at<int>(SCREEN_H));
    float halfW = 240.0f * aspect;                       // half the visible width in FE units (320 at 4:3)
    float s = halfW / 640.0f;                            // full-width layout -> quadrant width
    if (g_mmTarget >= 0) {                                   // a player's minimap (icons may be off-centre)
        MinimapPlace(copy, g_mmTarget, copy[7] - MINIMAP_X, copy[8] - MINIMAP_Y);
        return true;
    }
    bool right = slot & 1, bottom = (slot >> 1) & 1;
    bool srcBottom = false;                              // every player uses a copy of the top-half package
    // the 2P layout (a full-width half, centre (0, -120) / (0, 120)) scaled uniformly about its centre into the
    // quadrant: elements made of several objects (the drag tach's segments over its arc) stay together
    float srcCy = srcBottom ? 120.0f : -120.0f, dstCy = bottom ? 120.0f : -120.0f;
    float cx = right ? halfW / 2 : -halfW / 2;
    if (slot == 2 && g_screenViews == 3) cx = 0.0f;      // 3 players: P3 centred in the bottom half
    copy[7] = cx + copy[7] * s;
    copy[8] = dstCy + (copy[8] - srcCy) * s;
    if (fabsf(copy[8] - dstCy) > 160.0f) copy[8] += copy[8] > dstCy ? 2000.0f : -2000.0f;   // parked off-screen
                                                                                             // (icon pools)
    copy[0xE] *= s;
    copy[0xF] *= s;
    copy[4] *= s;                        // rotation pivot
    copy[5] *= s;
    copy[6] *= s;
    return true;
}

// Single-player gauges per player (see CreateGauges): objects of the players' copies of the 1P HUD package, placed
// into their view: the 1P layout's bottom-right corner (bottom centre for the drag tach) on the view's, scaled.
static std::unordered_map<const void*, int> g_gaugeShow;   // gauge objects and their data -> player
static std::unordered_map<const void*, int> g_gaugeHide;   // not drawn: the rest of the 1P copies, the 2P gauges
static int g_gaugeSlot = -1;             // while rendering a gauge object: whose
static bool g_gaugeDrag = false;
static float g_gaugeScale = 0.6f;

static void GaugeTransform(float* copy, int k) {
    float aspect = static_cast<float>(at<int>(SCREEN_W)) / static_cast<float>(at<int>(SCREEN_H));
    float halfW = 240.0f * aspect;
    float left = -halfW, right = halfW, bottom = (k & 1) ? 240.0f : 0.0f;
    if (g_screenViews >= 3) {
        left = (k & 1) ? 0.0f : -halfW;
        right = left + halfW;
        bottom = (k & 2) ? 240.0f : 0.0f;
        if (k == 2 && g_screenViews == 3) left = -halfW / 2, right = halfW / 2;
    }
    float s = g_gaugeScale;
    if (g_gaugeDrag) {
        // the 1P drag HUD spans the screen (tach arc along the left edge, speed top right): the whole layout fits
        // the view's height, side elements kept at the view's sides (as on a wide screen)
        s = 0.5f;
        float x = copy[7];
        copy[7] = x < -HUD_SIDE_MIN_X ? left + (x + 320.0f) * s : x > HUD_SIDE_MIN_X ? right + (x - 320.0f) * s
                                                                              : (left + right) / 2 + x * s;
        copy[8] = bottom - 120.0f + copy[8] * s;
        if (x > HUD_SIDE_MIN_X && copy[8] < bottom - 180.0f) copy[7] -= 10.0f, copy[8] += 40.0f;   // the speed, below the 2P HUD's position box
    } else {
        copy[7] = right + (copy[7] - 320.0f) * s;
        copy[8] = bottom + (copy[8] - 240.0f) * s;
    }
    copy[0xE] *= s;
    copy[0xF] *= s;
    copy[4] *= s;
    copy[5] *= s;
    copy[6] *= s;
}

static float* __stdcall HudObjectFilter(float* data, int level) {
    if (data && level == 0 && !g_gaugeShow.empty()) {
        int k = g_gaugeSlot;
        if (k < 0) {
            auto it = g_gaugeShow.find(data);
            if (it != g_gaugeShow.end()) k = it->second;
        }
        if (k >= 0) {
            float* copy = g_hudCopies[g_hudCopyNext];
            g_hudCopyNext = (g_hudCopyNext + 1) & 15;
            memcpy(copy, data, sizeof(g_hudCopies[0]));
            GaugeTransform(copy, k);
            return copy;
        }
    }
    if (data && g_mmPlayers > 0) {
        bool group = level == 0 && Contains(g_mmGroupData, g_mmGroupCount, data);
        bool child = level != 0 && g_mmTarget >= 0 && Contains(g_mmChildData, g_mmChildCount, data);
        if (group || child) {
            float* copy = g_hudCopies[g_hudCopyNext];
            g_hudCopyNext = (g_hudCopyNext + 1) & 15;
            memcpy(copy, data, sizeof(g_hudCopies[0]));
            if (group) copy[7] = copy[8] = 0.0f;
            else MinimapPlace(copy, g_mmTarget, copy[7], copy[8]);
            return copy;
        }
    }
    if (data && level == 0 && g_quadHud) {
        int slot = g_renderSlot;
        if (slot < 0) {                                    // groups: the transform carries the group's data
            auto it = g_hudObjSlot.find(data);
            if (it != g_hudObjSlot.end()) slot = it->second;
        }
        if (slot >= 0) {
            float* copy = g_hudCopies[g_hudCopyNext];
            g_hudCopyNext = (g_hudCopyNext + 1) & 15;
            memcpy(copy, data, sizeof(g_hudCopies[0]));
            QuadTransform(copy, slot);
            if (slot >= 1 && g_mmPlayers > 0 && Contains(g_mmHideData, g_mmHideCount, data))
                copy[0xE] = copy[0xF] = 0.0f;                 // the copies of P1's package: their own minimaps
            return copy;
        }
    }
    if (!data || level != 0 || !g_hudWiden) return data;
    float x = data[7], y = data[8];
    if (fabsf(x) < HUD_SIDE_MIN_X || fabsf(y) > 240.0f) return data;
    float aspect = static_cast<float>(at<int>(SCREEN_W)) / static_cast<float>(at<int>(SCREEN_H));
    float extra = 240.0f * aspect - 320.0f;
    if (extra <= 0.0f) return data;
    if (g_debug && g_quadHud && g_hudLogged < 64) {
        ++g_hudLogged;
        Log("hud x=%.1f y=%.1f -> %+.1f (quad %d, slot %d, data %p)", x, y, x < 0 ? -extra : extra, g_quadHud,
            g_renderSlot, data);
    }
    float* copy = g_hudCopies[g_hudCopyNext];
    g_hudCopyNext = (g_hudCopyNext + 1) & 15;
    memcpy(copy, data, sizeof(g_hudCopies[0]));
    copy[7] = x < 0 ? x - extra : x + extra;
    return copy;
}

static __declspec(naked) void FeObjXformHook() {
    __asm {
        pushad
        push dword ptr [esp + 0x30]      // level (arg 4)
        push dword ptr [esp + 0x28]      // data (arg 1; +4 for the previous push)
        call HudObjectFilter             // stdcall
        mov dword ptr [esp + 0x24], eax  // data
        popad
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        push AT_FE_OBJ_XFORM_RET
        ret
    }
}

// ---------------------------------------------------------------- debug write watch (hardware breakpoints)
// Logs the code address of every write to up to 4 watched dwords on the game's main thread.
static DWORD g_mainThreadId = 0;
static uintptr_t g_watch[4];

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD hit = e->ContextRecord->Dr6 & 0xF;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i) {
        if (!(hit & (1u << i))) continue;
        // return addresses on the stack: dwords in the game's code right after a call instruction
        uint32_t rets[10] = {};
        int n = 0;
        auto* sp = reinterpret_cast<const uint32_t*>(e->ContextRecord->Esp);
        for (int k = 0; k < 2048 && n < 10; ++k) {
            if (k == 4 && n == 1) k = 0x204;   // skip the 0x800-byte text buffer of 0x532670 (FE text setter)
            uint32_t v = sp[k];
            if (v < 0x401005 || v >= 0x780000) continue;
            auto* c = reinterpret_cast<const uint8_t*>(v);
            if (c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10))
                rets[n++] = v;
        }
        Log("watch %08X written by eip %08X value %08X (calls %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X)",
            static_cast<unsigned>(g_watch[i]), static_cast<unsigned>(e->ContextRecord->Eip),
            *reinterpret_cast<uint32_t*>(g_watch[i]), rets[0], rets[1], rets[2], rets[3], rets[4], rets[5], rets[6],
            rets[7], rets[8], rets[9]);
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI ApplyWatchThread(LPVOID) {
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_mainThreadId);
    if (!t) return 0;
    SuspendThread(t);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(t, &c);
    c.Dr0 = g_watch[0]; c.Dr1 = g_watch[1]; c.Dr2 = g_watch[2]; c.Dr3 = g_watch[3];
    c.Dr7 = 0;
    for (int i = 0; i < 4; ++i)
        if (g_watch[i]) c.Dr7 |= (1u << (i * 2)) | (0xDu << (16 + i * 4));   // local enable, write, 4 bytes
    SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

static void SetWatch(int slot, uintptr_t addr) {
    static bool handler = false;
    if (!handler) { AddVectoredExceptionHandler(1, WatchHandler); handler = true; }
    g_watch[slot & 3] = addr;
    if (HANDLE h = CreateThread(nullptr, 0, ApplyWatchThread, nullptr, 0, nullptr)) {
        if (GetCurrentThreadId() != g_mainThreadId) WaitForSingleObject(h, 1000);
        CloseHandle(h);
    }
    Log("watch slot %d = %08X", slot & 3, static_cast<unsigned>(addr));
}

// ---------------------------------------------------------------- debug command mailbox (Debug=1)
// Test tools write a command here; it runs on the game thread at the next frame. Address is in the log.
struct Mailbox {
    uint32_t magic;          // 'NFMB'
    volatile uint32_t seq;   // writer bumps after filling the fields
    volatile uint32_t done;  // = seq when executed
    uint32_t cmd;            // 1 send FE message, 2 switch screen, 3 write dword
    uint32_t arg, arg2;
    char s1[64], s2[64];
};
static Mailbox g_mailbox = {0x424D464E};

// debug (mailbox 7, arg = address of an IDirect3DSurface9* global): dump that render target as raw BGRA
void DumpSurface(uintptr_t at_) {
    void* dev = at<void*>(D3D_DEVICE);
    void* surf = at_ ? *reinterpret_cast<void**>(at_) : nullptr;
    if (!dev || !surf) return;
    struct { DWORD Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality; UINT Width, Height; } desc = {};
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(surf))[12])(surf, &desc);
    auto vt = *reinterpret_cast<void***>(dev);
    void* sys = nullptr;
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, DWORD, void**, HANDLE*)>(vt[36])(
        dev, desc.Width, desc.Height, desc.Format, 2, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, void*)>(vt[32])(dev, surf, sys);
    if (SUCCEEDED(hr)) {
        struct { INT Pitch; void* pBits; } lr;
        auto svt = *reinterpret_cast<void***>(sys);
        if (SUCCEEDED(reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, DWORD)>(svt[13])(sys, &lr, nullptr, 0))) {
            if (FILE* f = fopen("surfdump.raw", "wb")) {
                for (UINT y = 0; y < desc.Height; ++y) fwrite(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, 4, desc.Width, f);
                fclose(f);
            }
            reinterpret_cast<HRESULT(__stdcall*)(void*)>(svt[14])(sys);
        }
    }
    if (sys) reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(sys))[2])(sys);
    Log("surface dump %08X: %ux%u fmt %u hr %08X", static_cast<unsigned>(at_), desc.Width, desc.Height, desc.Format, hr);
}

static void RunMailbox() {
    if (!g_debug || g_mailbox.seq == g_mailbox.done) return;
    uint32_t seq = g_mailbox.seq;
    switch (g_mailbox.cmd) {
    case 1: reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(g_mailbox.arg, g_mailbox.s1); break;
    case 2:   // switching from a package that isn't the active screen crashes the game
        if (reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)(g_mailbox.s1))
            reinterpret_cast<void(__cdecl*)(const char*, const char*, uint32_t)>(FN_FE_SWITCH)(g_mailbox.s1, g_mailbox.s2,
                                                                                             g_mailbox.arg);
        else
            Log("mailbox: %s is not active, switch skipped", g_mailbox.s1);
        break;
    case 3: at<uint32_t>(g_mailbox.arg) = g_mailbox.arg2; break;
    case 5: SetWatch(g_mailbox.arg2, g_mailbox.arg); break;   // arg = address (0 = clear), arg2 = slot
    case 6: g_vpLogFrames = g_mailbox.arg; break;            // debug: log SetViewport calls for arg frames
    case 7: DumpSurface(g_mailbox.arg); break;
    case 4: {  // virtual pad: arg = index | connected << 8, arg2 = buttons | lt << 16 | rt << 24, s1 = 4 x int16 sticks
        const int16_t* st = reinterpret_cast<const int16_t*>(g_mailbox.s1);
        XPadSetVirtual(g_mailbox.arg & 0xFF, (g_mailbox.arg >> 8) & 1, g_mailbox.arg2 & 0xFFFF, (g_mailbox.arg2 >> 16) & 0xFF,
                       g_mailbox.arg2 >> 24, st[0], st[1], st[2], st[3]);
        break;
    }
    }
    if (g_mailbox.cmd != 4) Log("mailbox cmd %u arg %08X %s %s", g_mailbox.cmd, g_mailbox.arg, g_mailbox.s1, g_mailbox.s2);
    g_mailbox.done = seq;
}

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
        int fe = at<int>(FE_PLAYER);
        int seat = (g_seat & 1) == fe ? g_seat : fe;
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
    if (msg != MSG_BACK || g_seat != 2) return false;
    using ActiveFn = bool(__cdecl*)(const char*);
    if (reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng"))
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

// ---------------------------------------------------------------- rendering
// Player view k renders into its screen region (top/bottom halves, or quadrants); the game always passes (0,0).
static int g_regionX[MAX_LOCAL_PLAYERS], g_regionY[MAX_LOCAL_PLAYERS];

static void* CurrentRenderTarget(void* dev) {
    void* rt0 = nullptr;           // IDirect3DDevice9::GetRenderTarget (slot 38); only the pointer is needed
    auto getRt = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void**)>((*reinterpret_cast<void***>(dev))[38]);
    if (SUCCEEDED(getRt(dev, 0, &rt0)) && rt0)
        reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(rt0))[2])(rt0);   // Release
    return rt0;
}

#define STREAK_SURFACE_ADDR 0x86FF70
#define STREAK_AA_SURFACE   0x86F2E0     // with anti-aliasing ([0x870D08] > 0) the streak pass draws here, then copies
static bool g_streakFullCopy = false;    // the AA path's copy of the whole previous streak image (0x5C7120)
static bool IsStreakSurface(void* surf) {
    return surf && !g_streakFullCopy && (surf == at<void*>(STREAK_SURFACE_ADDR) || surf == at<void*>(STREAK_AA_SURFACE));
}
// With AA the streak pass first copies the whole previous streak image (texture 0x86F2E4) into 0x86F2E0
// (0x5C7120 at 0x5CBBB0, a full-screen quad) - that copy must stay full-screen, only what the view draws is confined.
#define AT_STREAK_AA_COPY  0x5CBBB0
static void __cdecl StreakAACopy(void* surf, void* tex) {
    g_streakFullCopy = true;
    reinterpret_cast<void(__cdecl*)(void*, void*)>(0x5C7120)(surf, tex);
    g_streakFullCopy = false;
}
static void StreakRegionViewport(int k, Viewport9* vp) {    // streak buffer = screen size (0x86FF70)
    int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
    if (vp->X != 0 || vp->Y != 0 || vp->Width != static_cast<DWORD>(w) || vp->Height != static_cast<DWORD>(h)) return;
    vp->X = g_regionX[k];
    vp->Y = g_regionY[k];
    vp->Width = g_rt[k].width;
    vp->Height = g_rt[k].height;
}

static HRESULT __stdcall SetViewportHook(void* dev, Viewport9* vp) {
    // Only viewports on the backbuffer are moved: passes inside a view also render into their own (often
    // half-screen sized) textures, e.g. the speed blur, which must keep their (0,0) viewport.
    int k = -1;
    for (int i = 0; i < g_screenViews; ++i)
        if (SplitRacing() && g_currentView == kPlayerViewId[i] && GetView(kPlayerViewId[i]).rt == &g_rt[i]) k = i;
    if (vp && k >= 0 && vp->X == 0 && vp->Y == 0 &&
        CurrentRenderTarget(dev) == reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface) {
        if (vp->Width == static_cast<DWORD>(g_rt[k].width) && vp->Height == static_cast<DWORD>(g_rt[k].height)) {
            vp->X = g_regionX[k];
            vp->Y = g_regionY[k];
        }
        // Post passes that target the whole backbuffer (light streaks composite at 0x5CBD5C / 0x5C7108) would stretch
        // this view's result over every other view: keep them inside the view's region.
        else if (vp->Width == static_cast<DWORD>(at<int>(SCREEN_W)) && vp->Height == static_cast<DWORD>(at<int>(SCREEN_H))) {
            vp->X = g_regionX[k];
            vp->Y = g_regionY[k];
            vp->Width = g_rt[k].width;
            vp->Height = g_rt[k].height;
        }
    }
    // the streak/overbright buffer (half resolution, whole screen): a player view only touches its own part
    if (vp && k >= 0 && IsStreakSurface(CurrentRenderTarget(dev))) StreakRegionViewport(k, vp);
    if (g_vpLogFrames > 0 && vp)
        Log("vp view %d: %u,%u %ux%u rt %p (caller %p)", g_currentView, vp->X, vp->Y, vp->Width, vp->Height,
            CurrentRenderTarget(dev), _ReturnAddress());
    return reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, vp);
}

// SetRenderTarget resets the viewport to the whole surface without a SetViewport call. Inside a player view, the
// backbuffer's "whole surface" is the view's region: post passes that render elsewhere and come back to the
// backbuffer (speed blur composite 0x5C6BE0) would otherwise draw over the whole screen.
static void* g_origSetRenderTarget = nullptr;
extern int g_obScissorSlotExt;
void ReapplyOverbrightScissor(void* dev);
static HRESULT __stdcall SetRenderTargetHook(void* dev, DWORD index, void* surface) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(g_origSetRenderTarget)(dev, index, surface);
    if (SUCCEEDED(hr) && index == 0 && g_obScissorSlotExt >= 0) ReapplyOverbrightScissor(dev);
    if (SUCCEEDED(hr) && index == 0 && surface && SplitRacing() && IsStreakSurface(surface))
        for (int k = 0; k < g_screenViews; ++k)
            if (g_currentView == kPlayerViewId[k] && GetView(kPlayerViewId[k]).rt == &g_rt[k]) {
                Viewport9 vp = {0, 0, static_cast<DWORD>(at<int>(SCREEN_W)), static_cast<DWORD>(at<int>(SCREEN_H)), 0.0f,
                                1.0f};
                StreakRegionViewport(k, &vp);
                reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
            }
    if (SUCCEEDED(hr) && index == 0 && surface && SplitRacing() &&
        surface == reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface)
        for (int k = 0; k < g_screenViews; ++k)
            if (g_currentView == kPlayerViewId[k] && GetView(kPlayerViewId[k]).rt == &g_rt[k]) {
                Viewport9 vp = {static_cast<DWORD>(g_regionX[k]), static_cast<DWORD>(g_regionY[k]),
                                static_cast<DWORD>(g_rt[k].width), static_cast<DWORD>(g_rt[k].height), 0.0f, 1.0f};
                reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
            }
    return hr;
}

static void HookDeviceMethod(void* dev, int slotIndex, void* hook, void** orig) {
    void** slot = *reinterpret_cast<void***>(dev) + slotIndex;
    if (*slot == hook) return;
    *orig = *slot;
    Write(reinterpret_cast<uintptr_t>(slot), &hook, sizeof(hook));
}

// Debug: device lost / reset tracing (the game resets on TestCooperativeLevel == D3DERR_DEVICENOTRESET, 0x5D2AFF /
// 0x5D2BE2, or after a video option change, 0x5D288B).
static void* g_origPresent = nullptr;
static void* g_origReset = nullptr;
static void* g_origTestCoop = nullptr;
static HRESULT __stdcall PresentHook(void* dev, const void* a, const void* b, HWND c, const void* d) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, const void*, const void*, HWND, const void*)>(g_origPresent)(dev, a, b, c, d);
    if (FAILED(hr)) Log("d3d: Present %08X (fg %p)", hr, GetForegroundWindow());
    return hr;
}
static void AddressSpace(unsigned* freeMb, unsigned* largestMb) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t total = 0, largest = 0;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));
         a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize)
        if (mbi.State == MEM_FREE) { total += mbi.RegionSize; if (mbi.RegionSize > largest) largest = mbi.RegionSize; }
    *freeMb = static_cast<unsigned>(total >> 20);
    *largestMb = static_cast<unsigned>(largest >> 20);
}
static HRESULT __stdcall TestCoopHook(void* dev) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*)>(g_origTestCoop)(dev);
    static HRESULT last = 0;
    if (hr != last) {
        unsigned freeMb, largestMb;
        AddressSpace(&freeMb, &largestMb);
        Log("d3d: TestCooperativeLevel %08X (caller %p) address space free %u MB, largest %u MB", hr, _ReturnAddress(),
            freeMb, largestMb);
    }
    last = hr;
    return hr;
}
static HRESULT __stdcall ResetHook(void* dev, void* params) {
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_origReset)(dev, params);
    void* stack[12];
    USHORT n = RtlCaptureStackBackTrace(1, 12, stack, nullptr);
    char trace[200] = "";
    for (USHORT i = 0; i < n && strlen(trace) < 180; ++i) sprintf_s(trace + strlen(trace), sizeof(trace) - strlen(trace), " %p", stack[i]);
    Log("d3d: Reset %08X (fg %p) stack%s", hr, GetForegroundWindow(), trace);
    return hr;
}

static WNDPROC g_origWndProc = nullptr;
static LRESULT CALLBACK DebugWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ACTIVATEAPP: Log("wnd: WM_ACTIVATEAPP %d (other thread %u)", static_cast<int>(wp), static_cast<unsigned>(lp)); break;
    case WM_ACTIVATE: Log("wnd: WM_ACTIVATE %d (other %p)", LOWORD(wp), reinterpret_cast<void*>(lp)); break;
    case WM_SETFOCUS: Log("wnd: WM_SETFOCUS (from %p)", reinterpret_cast<void*>(wp)); break;
    case WM_KILLFOCUS: Log("wnd: WM_KILLFOCUS (to %p)", reinterpret_cast<void*>(wp)); break;
    case WM_DISPLAYCHANGE: Log("wnd: WM_DISPLAYCHANGE %dx%d %d bpp", LOWORD(lp), HIWORD(lp), static_cast<int>(wp)); break;
    case WM_SIZE: Log("wnd: WM_SIZE %d %dx%d", static_cast<int>(wp), LOWORD(lp), HIWORD(lp)); break;
    case WM_WINDOWPOSCHANGED: {
        auto* wpos = reinterpret_cast<const WINDOWPOS*>(lp);
        Log("wnd: WM_WINDOWPOSCHANGED %d,%d %dx%d flags %X after %p", wpos->x, wpos->y, wpos->cx, wpos->cy, wpos->flags,
            reinterpret_cast<void*>(wpos->hwndInsertAfter));
        break;
    }
    case WM_SYSCOMMAND: Log("wnd: WM_SYSCOMMAND %X", static_cast<unsigned>(wp)); break;
    case WM_NCACTIVATE: Log("wnd: WM_NCACTIVATE %d", static_cast<int>(wp)); break;
    case WM_DEVICECHANGE: Log("wnd: WM_DEVICECHANGE %X", static_cast<unsigned>(wp)); break;
    case WM_SETTINGCHANGE: Log("wnd: WM_SETTINGCHANGE %X", static_cast<unsigned>(wp)); break;
    }
    return CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void HookDeviceDebug() {
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev || !g_debug) return;
    if (!g_origWndProc)
        if (HWND w = FindWindowA("GameFrame", nullptr))
            g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DebugWndProc)));
    HookDeviceMethod(dev, 3, reinterpret_cast<void*>(&TestCoopHook), &g_origTestCoop);
    HookDeviceMethod(dev, 16, reinterpret_cast<void*>(&ResetHook), &g_origReset);
    HookDeviceMethod(dev, 17, reinterpret_cast<void*>(&PresentHook), &g_origPresent);
}

static void* g_origSetRenderState;
static void* g_origDrawIndexed;
static HRESULT __stdcall SetRenderStateHook(void* dev, DWORD state, DWORD value);
static HRESULT __stdcall DrawIndexedHook(void* dev, int type, int base, UINT minIdx, UINT n, UINT start, UINT count);
static void HookDeviceSetViewport() {
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return;
    HookDeviceMethod(dev, 0xBC / 4, reinterpret_cast<void*>(&SetViewportHook), &g_origSetViewport);
    HookDeviceMethod(dev, 0x94 / 4, reinterpret_cast<void*>(&SetRenderTargetHook), &g_origSetRenderTarget);
    HookDeviceMethod(dev, 57, reinterpret_cast<void*>(&SetRenderStateHook), &g_origSetRenderState);
    HookDeviceMethod(dev, 82, reinterpret_cast<void*>(&DrawIndexedHook), &g_origDrawIndexed);
}

// The world renderer collects the views to draw every frame (0x5B9C20: views 1..3, env map 0x10..0x15, 4, 9; 12
// entries). Add the extra player views after 1..3 (the mirror view 3 is off in split screen, so they fit).
static uint8_t* g_worldViews;            // this frame's world view table (on the frame render's stack)
static void __cdecl RegisterExtraWorldViews(uint8_t* world) {
    g_worldViews = world;
    int& count = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    for (int k = 2; k < g_screenViews; ++k) {
        View& v = GetView(kPlayerViewId[k]);
        if (!v.active || !v.rt || !v.rt->surface || count >= 12 - 8) continue;   // keep room for 0x10..0x15, 4, 9
        uint8_t* entry = world + count * 0x90;
        ++count;
        reinterpret_cast<void(__cdecl*)(View*, void*, uint32_t)>(FN_WORLD_ADD_VIEW)(&v, entry,
                                                                                at<int>(0x8707A4) ? 0x1000 : 0x6000);
    }
}

static __declspec(naked) void WorldViewsHook() {
    __asm {
        pushad
        push esi                         // world renderer
        call RegisterExtraWorldViews
        add esp, 4
        popad
        mov eax, dword ptr ds:[0x00870CB8]   // stolen
        push AT_WORLD_VIEWS_RET
        ret
    }
}

// The sky dome is only drawn for view ids 1..3 (and env map faces); the extra player views pose as view 1.
static void __cdecl DrawSky(View* v) {
    int id = v->id;
    if (id >= 5 && id <= 8) v->id = 1;
    reinterpret_cast<void(__cdecl*)(View*)>(FN_DRAW_SKY)(v);
    v->id = id;
}

// Views 5..8 exist but the renderer only sets up 1..4 (0x5C2F90); give a spare view what view 2 gets there.
static void InitPlayerView(int id) {
    View& v = GetView(id);
    auto* raw = reinterpret_cast<uint8_t*>(&v);
    if (*reinterpret_cast<void**>(raw + 0x60)) return;
    using MallocFn = void*(__cdecl*)(size_t);
    void* cull = reinterpret_cast<MallocFn>(FN_MALLOC)(0x3B0);
    void* rend = reinterpret_cast<MallocFn>(FN_MALLOC)(0x51C0);
    if (!cull || !rend) return;
    memset(cull, 0, 0x3B0);              // the game's views got fresh zeroed memory at boot; the ctors rely on it
    memset(rend, 0, 0x51C0);
    reinterpret_cast<void*(__thiscall*)(void*)>(FN_VIEW_CULL_CTOR)(cull);
    *static_cast<View**>(cull) = &v;
    reinterpret_cast<void*(__thiscall*)(void*, View*, int)>(FN_VIEW_RENDER_CTOR)(rend, &v, 0);
    // as 0x61E0F0 does for views 1 and 2
    *reinterpret_cast<float*>(static_cast<uint8_t*>(rend) + 0x509C) = 1.0f;
    reinterpret_cast<void(__thiscall*)(void*, int, float)>(0x613050)(rend, 5, 1.0f);
    *reinterpret_cast<void**>(raw + 0x60) = cull;
    *reinterpret_cast<void**>(raw + 0x64) = rend;
    Log("view %d initialised", id);
}

// Light streaks (g_LightStreaksEnable 0x870CE0): 0x5CBB00 keeps ONE streak buffer that persists across frames and is
// fed by every view, so in split screen each view shows the others' streaks. Until there is a buffer per view, split
// views take the game's no-streaks branch at 0x5CB577 (`cmp [0x870CE0], ebx; je 0x5CB5AC`). The option itself is not
// touched: its buffers are created from it at race load (toggling it mid-race left them null -> crash on quit).
#define AT_STREAKS_BRANCH  0x5CB577      // 8 bytes: cmp dword [0x870CE0], ebx (6) + je rel8 (2)
#define AT_STREAKS_ON      0x5CB57F
#define AT_STREAKS_OFF     0x5CB5AC
// Overbright (g_OverBrightEnable 0x870CFC, needs streaks + glow) uses that buffer (0x86FF70, screen size) as its
// persistent accumulation: the streak function fades it and adds each view's light glows, and the full-screen overbright
// post (0x5CCA00) blurs the scene into it and shows it (the streak composite is skipped then). Without the streak pass
// nothing faded it in split -> bright spots burned in. With overbright on, split views run the streak pass again, with
// everything it draws into the buffer confined to the view's part of it (StreakRegionViewport).
#define STREAK_SURFACE     0x86FF70
#define OPT_OVERBRIGHT     0x870CFC
static bool OverbrightActive() { return at<int>(OPT_OVERBRIGHT) && at<int>(0x870CE4) && at<int>(0x870CE0); }
#define AT_STREAK_VIEW_CALL 0x5CBC02    // `push 1; call 0x48B1E0`: the glows drawn into the buffer are view 1's
static void* __cdecl StreakView(int id) {
    for (int k = 0; k < g_screenViews; ++k)
        if (SplitRacing() && g_currentView == kPlayerViewId[k]) return &GetView(g_currentView);
    return &GetView(id);
}
// Without overbright the streak function ends with its own composite of the buffer onto the view (0x5C6BE0 at
// 0x5CBDA6: a full-clip-space quad, UVs 0..1 = the whole buffer). In a split view it would squeeze every player's part
// of the buffer into the view, so while that composite runs for a player view its UVs are remapped to the view's part:
// the blit builds the quad on its stack (4 vertices of 0x30 bytes at esp+0x30: position, then 4 UV pairs) and calls
// 0x5BCBC0 at 0x5C6EC6 before drawing it.
#define AT_STREAK_COMPOSITE_CALL 0x5CBDA6
#define FN_SCREEN_BLIT     0x5C6BE0      // cdecl, 8 args
#define AT_BLIT_QUAD_READY 0x5C6EC6      // call 0x5BCBC0 (vertices at the caller's esp+0x30)
#define FN_BLIT_QUAD_READY 0x5BCBC0
static int g_streakCompositeView = -1;   // player view whose streak composite is running (-1: none)
static void __cdecl StreakComposite(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g,
                                    uint32_t h) {
    for (int k = 0; k < g_screenViews; ++k)
        if (SplitRacing() && g_currentView == kPlayerViewId[k]) g_streakCompositeView = k;
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(
        FN_SCREEN_BLIT)(a, b, c, d, e, f, g, h);
    g_streakCompositeView = -1;
}
static void __cdecl RemapStreakQuad(float* vertices) {
    int k = g_streakCompositeView;
    float w = static_cast<float>(at<int>(SCREEN_W)), h = static_cast<float>(at<int>(SCREEN_H));
    if (k < 0 || w <= 0 || h <= 0) return;
    float x0 = g_regionX[k] / w, y0 = g_regionY[k] / h, sx = g_rt[k].width / w, sy = g_rt[k].height / h;
    for (int v = 0; v < 4; ++v)
        for (int t = 0; t < 4; ++t) {
            float* uv = vertices + v * 12 + 4 + t * 2;
            uv[0] = x0 + uv[0] * sx;
            uv[1] = y0 + uv[1] * sy;
        }
}
static __declspec(naked) void BlitQuadReadyHook() {
    __asm {
        cmp g_streakCompositeView, 0
        jl done
        pushad
        lea eax, [esp + 0x20 + 0x40]     // caller's esp+0x30: past pushad, our return address and its 3 arguments
        push eax
        call RemapStreakQuad
        add esp, 4
        popad
    done:
        push FN_BLIT_QUAD_READY
        ret
    }
}
static int __cdecl StreaksAllowed() { return at<int>(0x870CE0) != 0; }
static __declspec(naked) void StreaksBranchHook() {
    __asm {
        pushad
        call StreaksAllowed
        test eax, eax
        popad
        jz off
        push AT_STREAKS_ON
        ret
    off:
        push AT_STREAKS_OFF
        ret
    }
}

static void ApplySplitLayout() {
    auto* bb = reinterpret_cast<RenderTarget*>(RT_BACKBUFFER);
    if (SplitRacing()) {
        HookDeviceSetViewport();
        int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
        bool quad = g_quadTest || g_extraPlayers > 0;
        g_screenViews = !quad ? 2 : g_extraPlayers == 1 ? 3 : 4;      // 3 players: bottom-right quadrant empty
        for (int k = 0; k < g_screenViews; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (k >= 2) {
                InitPlayerView(kPlayerViewId[k]);
                if (g_extraPlayers > 0) { if (void* c = ExtraCamera(k)) v.camera = static_cast<decltype(v.camera)>(c); }
                else v.camera = GetView(kPlayerViewId[k - 2]).camera;   // QuadTest: reuse P1/P2's cameras
            }
            g_rt[k] = *bb;               // refresh every frame: surfaces change after a device reset
            g_rt[k].ownerView = kPlayerViewId[k];
            g_rt[k].enabled = 1;
            g_rt[k].width = quad ? w / 2 : w;
            g_rt[k].height = h / 2;
            g_regionX[k] = quad ? (k & 1) * (w / 2) : 0;
            if (g_screenViews == 3 && k == 2) g_regionX[k] = w / 4;   // 3 players: P3 centred in the bottom half
            g_regionY[k] = quad ? (k >> 1) * (h / 2) : k * (h / 2);
            v.rt = &g_rt[k];
            v.active = 1;
        }
        for (int k = g_screenViews; k < MAX_LOCAL_PLAYERS; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (v.rt == &g_rt[k]) { v.rt = nullptr; v.active = 0; }
        }
        // Layouts 1/2 enable the road reflection view 4 (its 320x240 target) and the headlight view 9 (0x5C233D); layout
        // 3 has no case, so a split race inherited them from the intro (layout 1). Without an intro (the street drift
        // tracks 4174-4179 have no usable scene) they stayed off, and with road reflections on the main render returns
        // at once when view 4 is inactive (0x5CAB35) -> black world, and quitting crashed in the flare render.
        View& v4 = GetView(4);
        if (v4.rt) { v4.rt->enabled = 1; v4.rt->width = 320; v4.rt->height = 240; }
        v4.active = 1;
        GetView(9).active = 1;
    } else {
        g_screenViews = 2;
        g_viewTintInit[0] = g_viewTintInit[1] = false;      // per-race copies (they point into track data)
        View& v1 = GetView(1);
        if (v1.rt == &g_rt[0]) v1.rt = bb;
        for (int k = 1; k < MAX_LOCAL_PLAYERS; ++k) {
            View& v = GetView(kPlayerViewId[k]);
            if (v.rt == &g_rt[k]) { v.rt = nullptr; v.active = 0; }
        }
    }
}

// Speed blur (g_MotionBlurEnable, 0x5CC460, called per player view at 0x5CB31F): reads view 1's camera for the speed
// and blur centre (0x48B1E0(1) at 0x5CC474 / 0x5CC490), smooths its strength in one global (0x870900) and copies the
// whole backbuffer into its blur texture (StretchRect at 0x5CC760) -> only P1 got it, centred on the screen centre.
// Per view: the view being drawn supplies the camera, keeps its own smoothed strength, and only its region is copied.
#define AT_BLUR_CALL       0x5CB31F
#define FN_MOTION_BLUR     0x5CC460
#define AT_BLUR_VIEW_CALL1 0x5CC474
#define AT_BLUR_VIEW_CALL2 0x5CC490
#define AT_BLUR_COPY       0x5CC760      // `call [ecx+0x88]` IDirect3DDevice9::StretchRect (6 bytes)
#define BLUR_STRENGTH      0x870900
static float g_blurStrength[MAX_LOCAL_PLAYERS];
struct BlurCopyArgs { void* dev; void* src; void* dst; int filter; };
static BlurCopyArgs g_blurCopy;                 // the view-region copy made during the current blur pass
using StretchRectFn = HRESULT(__stdcall*)(void*, void*, const RECT*, void*, const RECT*, int);
static StretchRectFn StretchRectOf(void* dev) {
    return reinterpret_cast<StretchRectFn>((*reinterpret_cast<void***>(dev))[0x88 / 4]);
}

static int CurrentPlayerSlot() {
    if (!SplitRacing()) return -1;
    for (int k = 0; k < g_screenViews; ++k)
        if (g_currentView == kPlayerViewId[k]) return k;
    return -1;
}

static View* __cdecl BlurView(int n) {
    int k = CurrentPlayerSlot();
    return &GetView(k >= 0 ? kPlayerViewId[k] : n);
}

static void __cdecl MotionBlurForView() {
    int k = CurrentPlayerSlot();
    float& strength = *reinterpret_cast<float*>(BLUR_STRENGTH);
    if (g_debug) CheckX87("motion blur in");
    if (k < 0) { reinterpret_cast<void(__cdecl*)()>(FN_MOTION_BLUR)(); return; }
    float shared = strength;
    strength = g_blurStrength[k];
    g_blurCopy = {};
    reinterpret_cast<void(__cdecl*)()>(FN_MOTION_BLUR)();
    g_blurStrength[k] = strength;
    strength = shared;
    if (g_debug) {                       // per view: camera speed and how often the blur actually drew
        static unsigned s_frames[MAX_LOCAL_PLAYERS], s_drawn[MAX_LOCAL_PLAYERS];
        static float s_maxSpeed[MAX_LOCAL_PLAYERS];
        ++s_frames[k];
        if (g_blurCopy.dev) ++s_drawn[k];
        if (uint8_t* cam = GetView(kPlayerViewId[k]).camera) {
            const float* v = reinterpret_cast<const float*>(cam + 0x1E0);
            float speed = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (speed > s_maxSpeed[k]) s_maxSpeed[k] = speed;
        }
        if (s_frames[k] >= 180) {
            Log("blur view %d: max camera speed %.1f, drawn %u/%u frames", k + 1, s_maxSpeed[k], s_drawn[k], s_frames[k]);
            s_frames[k] = s_drawn[k] = 0;
            s_maxSpeed[k] = 0;
        }
    }
    // The blur texture is also the frame-end post chain's screen copy (0x86EB0C, e.g. depth of field reads it after
    // the views): leave it holding the whole screen, as the unpatched pass does.
    if (g_blurCopy.dev)
        StretchRectOf(g_blurCopy.dev)(g_blurCopy.dev, g_blurCopy.src, nullptr, g_blurCopy.dst, nullptr, g_blurCopy.filter);
}

static HRESULT __stdcall BlurCopy(void* dev, void* src, const RECT* srcRect, void* dst, const RECT* dstRect, int filter) {
    RECT region;
    int k = CurrentPlayerSlot();
    if (k >= 0 && !srcRect) {
        region = {g_regionX[k], g_regionY[k], g_regionX[k] + g_rt[k].width, g_regionY[k] + g_rt[k].height};
        srcRect = &region;
        if (!dstRect) g_blurCopy = {dev, src, dst, filter};
    }
    return StretchRectOf(dev)(dev, src, srcRect, dst, dstRect, filter);
}

typedef void(__cdecl* VoidFn)();
static VoidFn g_origLayoutApply;

static void EndResultsCount();
static void UpdateGaugeMaps();
static void SyncPlayerSettings(bool newRace);
static void UpdateFreeCamHook();
static void UpdateStartZoom();
static void UpdateIntroHiding();

static void __cdecl LayoutApplyHook() {
    if (g_debug) CheckX87("frame start");
    g_origLayoutApply();
    if (g_debug) CheckX87("after layout apply");
    HookDeviceDebug();
    XPadUpdate();
    UpdateFreeCamHook();
    UpdateStartZoom();
    UpdateIntroHiding();
    UpdateHudWiden();
    EndResultsCount();
    SyncPlayerSettings(false);
    UpdateQuadHud();
    UpdateGaugeMaps();
    ++g_mmFrame;
    if (g_vpLogFrames > 0 && --g_vpLogFrames == 0) Log("vp log end");
    UpdateEditingSlot2();
    UpdateBindSwap();
    UpdateSeats();
    RunMailbox();
    UpdateModeTransitions();
    ApplySplitLayout();
}

// The split views draw into copies of the backbuffer target (g_rt). On alt-tab / device reset the game releases and
// recreates the backbuffer surfaces in the middle of a frame, so copies taken at frame start can point at released
// surfaces (crash in d3d9 under SetViewport, 0x5CB1EA). Re-copy right before each player view renders.
// Car light glare slots (0x48AB20: per light type, counters 0x82DAE8[2], capacity 0x7F3D44 {200, 50}) are reset once
// per frame (0x48AC00 at 0x5CBAE1) but made and drawn per player view: with 3-4 views the per-frame budget ran out as
// cars came into view and the later views lost their glares (user: P3's and then P1's nitrous glare vanished). Each
// player view starts with an empty budget.
#define GLARE_SLOT_COUNTS  0x82DAE8

// World headlights: one projector, view 9 (camera 0x870A20), placed once per frame by 0x5C2AE0(view 9, human index 0)
// (called at 0x5D2955; human index -> racer id via the race's human slots 0x89E808[], car found by that id in the race
// manager's list) and its matrices built by 0x5C7EC0. Every player view projected P1's headlights. Each player view
// now re-aims the projector at its own car right before it renders.
#define FN_HEADLIGHT_CAMERA 0x5C2AE0     // void __cdecl (View* view9, int humanIndex)
#define FN_VIEW_UPDATE      0x5C7EC0     // void __thiscall (platInfo; View*, 0, 0, 0)
#define OPT_CAR_HEADLIGHTS  0x870CC4
#define HUMAN_RACER_SLOTS   0x89E808     // racer ids of humans 0, 1
// The headlight pass draws view 9's entry of the frame's world view table (0x5D6D20 at 0x5CB218), culled once per
// frame from the projector on P1 (0x5E9A30: pass 1 stores the camera position / direction and the scenery section,
// pass 2 culls each entry into the shared item buffer 0x876DE0 via 0x5E99C0) -> other players' lights only reached
// geometry near P1. Re-cull view 9's entry from the re-aimed projector into the free buffer space after the frame's
// lists (the buffer cursor is left alone, so each player view reuses that space).
#define FN_WORLD_ENTRY_FRUSTUM 0x5C26E0  // void __cdecl (View*, entry)
#define FN_WORLD_CULL_ENTRY    0x5E99C0  // void __thiscall (world; entry)
#define WORLD_SECTION_MODE     0x800DF8
#define WORLD_SECTION_MASK     0x8832C0
static void RecullHeadlightView(View& v9) {
    uint8_t* world = g_worldViews;
    if (!world || !v9.camera) return;
    int n = *reinterpret_cast<int*>(world + WORLD_VIEW_COUNT);
    uint8_t* e = nullptr;
    for (int i = 0; i < n && i < 12; ++i)
        if (*reinterpret_cast<View**>(world + i * 0x90 + 0x40) == &v9) e = world + i * 0x90;
    if (!e) return;
    reinterpret_cast<void(__cdecl*)(View*, void*)>(FN_WORLD_ENTRY_FRUSTUM)(&v9, e);
    const float* cam = reinterpret_cast<const float*>(v9.camera + 0x40);
    float* pos = reinterpret_cast<float*>(e + 0x60);
    pos[0] = cam[0]; pos[1] = cam[1]; pos[2] = cam[2];          // +0x60 position, +0x6C direction (camera +0x50)
    pos[3] = cam[4]; pos[4] = cam[5]; pos[5] = cam[6];
    *reinterpret_cast<int*>(e + 0x80) = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(&v9) + 0xC);
    int mode = at<int>(WORLD_SECTION_MODE), section = -1;
    if (mode == 1 || (mode == 2 && at<int>(0x7FB708) % 0x3D >= 0xF)) {   // as pass 1 (mode 3 left unrestricted)
        int x = static_cast<int>(pos[0]), y = static_cast<int>(pos[1]);
        int cell = (((y >> 5) & 0x7F) << 7) + ((x >> 5) & 0x7F);
        if (!(reinterpret_cast<const uint8_t*>(WORLD_SECTION_MASK)[cell >> 3] & (1 << (cell & 7))))
            section = (((y >> 5) & 0x1F) << 5) + ((x >> 5) & 0x1F);
    }
    *reinterpret_cast<int*>(e + 0x84) = section;
    uint32_t cursor = *reinterpret_cast<uint32_t*>(world + 0x6C8);
    *reinterpret_cast<uint32_t*>(e + 0x48) = cursor;
    *reinterpret_cast<uint32_t*>(e + 0x4C) = cursor;
    *reinterpret_cast<uint32_t*>(e + 0x50) = *reinterpret_cast<uint32_t*>(world + 0x6CC);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_WORLD_CULL_ENTRY)(world, e);
}

// Per-car headlight pools on the road ("2PLAYERHEADLIGHT1" decal, 0x615490(view): every car with lights on that is
// not hidden in the view) are drawn by the 1-player render path only (0x5CAC07); the split per-view path skips them,
// so only the projector car lit the road. Draw them in the split path's matching pass, after 0x566400 (0x5CB2E3).
#define AT_SPLIT_PASS_CALL   0x5CB2E3
#define FN_SPLIT_PASS        0x566400    // void __cdecl (View*)
#define FN_HEADLIGHT_POOLS   0x615490    // void __cdecl (View*)
// As in single player, the view's own car gets no pool (its projector lights the road; the pool is a flat quad that
// tilts with the car and clipped into the road under braking): its lights flag (+0x550) is off during the call.
// Every player's car has a projector (WorldDrawWithBeams, also in the full screen race intro), so no player car gets
// a pool: their lights flags are off during the call. Traffic keeps its pools, as in single player.
static uint8_t* PlayerCar(int k) {
    int id = k < 2 ? reinterpret_cast<const int*>(HUMAN_RACER_SLOTS)[k] : k;
    uint8_t* mgr = at<uint8_t*>(0x890080);
    if (!mgr) return nullptr;
    for (auto* c = *reinterpret_cast<uint8_t**>(mgr); c && c != mgr; c = *reinterpret_cast<uint8_t**>(c))
        if (*reinterpret_cast<int16_t*>(c + 0xA) == id) return c;
    return nullptr;
}
static void __cdecl PoolsForTraffic(View* v) {
    if (!SplitMode() || at<int>(NUM_PLAYERS) != 2) {
        reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v);
        return;
    }
    uint8_t* car[MAX_LOCAL_PLAYERS] = {};
    uint8_t lights[MAX_LOCAL_PLAYERS] = {};
    for (int k = 0; k < 2 + (g_extraPlayers > 0 ? g_extraPlayers : 0) && k < MAX_LOCAL_PLAYERS; ++k)
        if ((car[k] = PlayerCar(k)) != nullptr) { lights[k] = car[k][0x550]; car[k][0x550] = 0; }
    reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) if (car[k]) car[k][0x550] = lights[k];
}
static void __cdecl SplitPassWithHeadlightPools(View* v) {
    reinterpret_cast<void(__cdecl*)(View*)>(FN_SPLIT_PASS)(v);
    if (SplitRacing() && at<int>(OPT_CAR_HEADLIGHTS)) PoolsForTraffic(v);
}
// The race intro is drawn full screen by the 1-player path, which draws the pools itself (0x5CAC07).
#define AT_1P_POOLS_CALL     0x5CAC07

// Rain: every view has its own rain object (view +0x64: falling rain + the drops on the lens, updated from that
// view's camera by 0x6140F0), but the frame draws only view 1's, once over the whole screen: falling rain at
// 0x5CB46B (0x61F120, also updates the drops) and the lens drops at 0x5CB9A6 (0x5C7AB0: copies the screen, then a
// quad per drop at x/y 0..1 that also samples the copy there). Split: each player view gets its own pass - falling
// rain in its region (viewport moved by SetViewportHook via g_currentView), drops remapped into its region.
#define AT_RAIN_CALL       0x5CB46B      // `call 0x61F120` (thiscall rain object)
#define FN_RAIN_UPDATE     0x61F120
#define RAIN_TIME_ACCUM    0x8A1D50      // global accumulator advanced by 0x61F120 (once per frame)
#define AT_RAIN_DROPS_CALL 0x5CB9A6      // `call 0x5C7AB0` (cdecl, byte arg)
#define FN_RAIN_DROPS      0x5C7AB0
#define AT_RAIN_DROP_GET   0x5C7C52      // `call 0x6142C0` (thiscall (i, &x, &y, &alpha, &size), ret 0x14)
#define FN_RAIN_DROP_GET   0x6142C0
static const uintptr_t kRainDropsViewCalls[] = {0x5C7AB8, 0x5C7C04, 0x5C7C47, 0x5C7DD4};   // `call 0x48B1E0` (push 1)
static int g_rainSlot = -1;              // player slot whose drops are being drawn

// Weather region caches ("Wind" 0x8A3180, "Normal Fog" 0x8A1E90, "Night Normal" 0x8A31F0, 0x8A3250) are 2-entry
// static arrays indexed by view id - 1. Each entry is a query cache (region the given position was last found in),
// recomputed from the position passed in, so views 5/6 (players 3/4) share players 1/2's entries instead of reading
// past the arrays (0x6136B0: falling rain update; 0x614300: per-view splashes, already run for views 5/6).
#define AT_RAIN_REGION_SLOT   0x6136C3   // mov eax, [ecx+4]; dec eax; push edi        (ecx = view)
#define AT_SPLASH_REGION_SLOT 0x61437E   // mov ebp, [eax+4]; dec ebp; mov [esp+0x38], ebp   (eax = view)
// The falling rain is emitted around the view's car (0x61E700: frame +0x5010 from the car's matrix): view id 1 ->
// human slot 0, anything else -> slot 1, so P3/P4's rain fell around P2's car (not in their view).
#define AT_RAIN_CAR        0x61E724      // cmp [eax+4], 1 ... push 0/1; call 0x5ED980  (eax = view)
#define AT_RAIN_CAR_DONE   0x61E73A      // eax = racer id
static int RacerIdOfPlayer(int k);
static int __fastcall RainCarRacer(int id) {
    int k = id == 1 ? 0 : 1;
    for (int i = 0; i < MAX_LOCAL_PLAYERS; ++i)
        if (id == kPlayerViewId[i]) k = i;
    return RacerIdOfPlayer(k);
}
static __declspec(naked) void RainCarHook() {
    __asm {
        mov ecx, dword ptr [eax + 4]
        call RainCarRacer
        push AT_RAIN_CAR_DONE
        ret
    }
}
static int __fastcall WeatherSlotOfView(int id) {
    for (int k = 2; k < MAX_LOCAL_PLAYERS; ++k)
        if (id == kPlayerViewId[k]) return k - 2;
    return id - 1;
}
static __declspec(naked) void RainRegionSlot() {
    __asm {
        push edx
        push ecx
        mov ecx, dword ptr [ecx + 4]
        call WeatherSlotOfView
        pop ecx
        pop edx
        xchg edi, dword ptr [esp]        // return address <-> edi: push edi, then return
        push edi
        mov edi, dword ptr [esp + 4]
        ret
    }
}
static __declspec(naked) void SplashRegionSlot() {
    __asm {
        push ecx
        push edx
        push eax
        mov ecx, dword ptr [eax + 4]
        call WeatherSlotOfView
        mov ebp, eax
        pop eax
        pop edx
        pop ecx
        mov dword ptr [esp + 4 + 0x38], ebp
        ret
    }
}

static void __fastcall RainForViews(void* rain1, void*) {
    if (!SplitRacing()) { reinterpret_cast<void(__thiscall*)(void*)>(FN_RAIN_UPDATE)(rain1); return; }
    void* dev = at<void*>(D3D_DEVICE);
    float accum = at<float>(RAIN_TIME_ACCUM);
    int saved = g_currentView;
    Viewport9 before = {};                // later passes (flares, particles) rely on the viewport left by the views
    bool haveBefore = dev && SUCCEEDED(reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(
                                           (*reinterpret_cast<void***>(dev))[48])(dev, &before));   // GetViewport
    for (int k = 0; k < g_screenViews; ++k) {
        View& v = GetView(kPlayerViewId[k]);
        auto* rain = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(&v) + 0x64);
        if (!v.active || !rain) continue;
        if (k > 0) at<float>(RAIN_TIME_ACCUM) = accum;
        g_currentView = kPlayerViewId[k];
        Viewport9 vp = {static_cast<DWORD>(g_regionX[k]), static_cast<DWORD>(g_regionY[k]),
                        static_cast<DWORD>(g_rt[k].width), static_cast<DWORD>(g_rt[k].height), 0.0f, 1.0f};
        if (dev && g_origSetViewport) reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &vp);
        reinterpret_cast<void(__thiscall*)(void*)>(FN_RAIN_UPDATE)(rain);
    }
    g_currentView = saved;
    if (haveBefore && g_origSetViewport) reinterpret_cast<HRESULT(__stdcall*)(void*, Viewport9*)>(g_origSetViewport)(dev, &before);
}

static void* __cdecl RainDropsView(int id) {
    return &GetView(g_rainSlot >= 0 ? kPlayerViewId[g_rainSlot] : id);
}

static void __fastcall RainDropGet(void* rain, void*, int i, float* x, float* y, float* alpha, float* size) {
    reinterpret_cast<void(__thiscall*)(void*, int, float*, float*, float*, float*)>(FN_RAIN_DROP_GET)(rain, i, x, y,
                                                                                                    alpha, size);
    if (g_rainSlot < 0) return;
    float w = static_cast<float>(at<int>(SCREEN_W)), h = static_cast<float>(at<int>(SCREEN_H));
    if (w <= 0 || h <= 0) return;
    int k = g_rainSlot;
    float rh = g_rt[k].height / h;
    *x = g_regionX[k] / w + *x * (g_rt[k].width / w);
    *y = g_regionY[k] / h + *y * rh;
    *size *= rh;                          // same pixel shape, scaled with the view
}

static void __cdecl RainDropsForViews(int arg) {
    if (!SplitRacing()) { reinterpret_cast<void(__cdecl*)(int)>(FN_RAIN_DROPS)(arg); return; }
    for (int k = 0; k < g_screenViews; ++k) {
        if (!GetView(kPlayerViewId[k]).active) continue;
        g_rainSlot = k;
        reinterpret_cast<void(__cdecl*)(int)>(FN_RAIN_DROPS)(arg);
    }
    g_rainSlot = -1;
}

static void HeadlightsForPlayer(int k) {
    View& v9 = GetView(9);
    if (!at<int>(OPT_CAR_HEADLIGHTS) || !v9.active || !v9.camera || !v9.platInfo) return;
    int* slot0 = reinterpret_cast<int*>(HUMAN_RACER_SLOTS);
    int saved = *slot0;
    *slot0 = k < 2 ? slot0[k] : k;       // extra players' racers follow the two 2P humans (ids 2, 3)
    reinterpret_cast<void(__cdecl*)(View*, int)>(FN_HEADLIGHT_CAMERA)(&v9, 0);
    *slot0 = saved;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v9.platInfo, &v9, 0, 0, 0);
    RecullHeadlightView(v9);
}

// Road reflections: view 4 renders the scene from player 1's camera (it shares view 1's camera object) into a 320x240
// texture (0x870894, surface 0x870890 = view 4's target) before the player views (0x5CAB15..0x5CACCC, blurred by
// 0x5C7E40 through the second texture 0x87089C/0x870898); road materials bind [0x870894] when they draw (0x5BC7D1...)
// and sample it at each pixel's projected position, so every view showed player 1's reflection. Now players 2..4 get
// their own texture, rendered right after player 1's (same place in the frame: rendering it inside the view loop left
// the view's state broken), with their camera and their view's projection (AdjustProjection), and [0x870894] is
// switched to the player's texture before each player view. The textures are D3DPOOL_DEFAULT like the game's and are
// released with them on device loss (0x5BA610), recreated on demand.
#define OPT_ROAD_REFLECTION 0x870CD4     // 0 off, 1 .., 2/3 full
#define GAME_STATE          0x8654A4     // 6 = racing
#define CURRENT_PASS        0x870910
#define RENDER_PASS_MAIN    0x86EB28
#define FN_RT_BEGIN         0x5BA3C0     // void __cdecl (RenderTarget*, int clear)
#define FN_WORLD_DRAW       0x5D6AD0     // void __thiscall (world views; View*, flags)
#define FN_REFLECT_CARS     0x631250     // void __cdecl (View*)
#define FN_DRAW_CARS        0x630DE0     // void __cdecl (View*, int)
#define FN_FLUSH_DRAWS      0x5CF420
#define FN_REFLECT_POST     0x5C7E40     // racing and option 3: blur
#define REFLECT_SURFACE     0x870890
#define REFLECT_TEXTURE     0x870894
#define REFLECT_FORMAT      0x7FEE78
#define AT_REFLECTIONS_DONE 0x5CACCC     // cmp [0x870CB8], ebx (6 bytes) - after view 4, before the env map
#define FN_RELEASE_TARGETS  0x5BA610     // device lost: releases the render target textures
static int g_reflectionPlayer = 0;       // player whose camera/projection view 4 has
#define RENDER_PASS_GLOW    0x86EB5C
#define OPT_GLOW            0x870CE4
static void* g_reflTex[MAX_LOCAL_PLAYERS];   // [0] unused (player 1 uses the game's)
static void* g_reflSurf[MAX_LOCAL_PLAYERS];
static void* g_gameReflTex;              // the game's texture/surface while ours are swapped in
static void* g_gameReflSurf;

template <class F> static void InRenderPass(uintptr_t passAddr, F draw) {
    auto* pass = at<uint8_t*>(passAddr);
    void* dev = at<void*>(D3D_DEVICE);
    if (!pass || !dev) return;
    at<void*>(CURRENT_PASS) = pass;
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(pass))[1])(pass);
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(dev))[0x15C / 4])(
        dev, *reinterpret_cast<void**>(pass + 0x24));
    auto* effect = *reinterpret_cast<void**>(pass + 0x20);
    unsigned passes = 0;
    reinterpret_cast<HRESULT(__stdcall*)(void*, unsigned*, DWORD)>((*reinterpret_cast<void***>(effect))[0x104 / 4])(
        effect, &passes, 0);
    draw();
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(pass))[2])(pass);
    reinterpret_cast<HRESULT(__stdcall*)(void*)>((*reinterpret_cast<void***>(effect))[0x114 / 4])(effect);
    at<void*>(CURRENT_PASS) = nullptr;
}

static void ComRelease(void*& p) {
    if (p) reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(p))[2])(p);
    p = nullptr;
}

static void ReleaseReflectionTextures() {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) { ComRelease(g_reflSurf[k]); ComRelease(g_reflTex[k]); }
}

static bool EnsureReflectionTexture(int k) {
    if (g_reflTex[k] && g_reflSurf[k]) return true;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return false;
    auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*)>(
        (*reinterpret_cast<void***>(dev))[23]);                      // CreateTexture
    if (FAILED(create(dev, 320, 240, 1, 1 /* D3DUSAGE_RENDERTARGET */, at<DWORD>(REFLECT_FORMAT), 0 /* DEFAULT */,
                      &g_reflTex[k], nullptr)))
        return false;
    auto level = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, void**)>(
        (*reinterpret_cast<void***>(g_reflTex[k]))[18]);            // GetSurfaceLevel
    if (FAILED(level(g_reflTex[k], 0, &g_reflSurf[k]))) { ComRelease(g_reflTex[k]); return false; }
    Log("road reflection texture for player %d created", k + 1);
    return true;
}

static void RenderRoadReflection(int k) {
    View& v4 = GetView(4);
    int opt = at<int>(OPT_ROAD_REFLECTION);
    View* v = &v4;
    v4.camera = GetView(kPlayerViewId[k]).camera;
    g_reflectionPlayer = k;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v4.platInfo, v, 0, 0, 0);
    RecullHeadlightView(v4);
    reinterpret_cast<void(__cdecl*)(RenderTarget*, int)>(FN_RT_BEGIN)(v4.rt, 1);
    if (opt > 1) {
        reinterpret_cast<void(__thiscall*)(void*, View*, int)>(FN_WORLD_DRAW)(g_worldViews, v, 8);
        reinterpret_cast<void(__cdecl*)(View*)>(FN_SPLIT_PASS)(v);
        reinterpret_cast<void(__cdecl*)(View*)>(FN_REFLECT_CARS)(v);
        InRenderPass(RENDER_PASS_MAIN, [v] { reinterpret_cast<void(__cdecl*)(View*, int)>(FN_DRAW_CARS)(v, 1); });
    }
    InRenderPass(RENDER_PASS_MAIN, [v] { reinterpret_cast<void(__cdecl*)(View*)>(FN_HEADLIGHT_POOLS)(v); });
    reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
    if (at<int>(GAME_STATE) == 6 && opt == 3) reinterpret_cast<void(__cdecl*)()>(FN_REFLECT_POST)();
    InRenderPass(RENDER_PASS_GLOW, [v] {        // glow sprites: tail/head light reflections
        if (!at<int>(OPT_GLOW)) return;
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x491860)(v, 2);
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x61AE90)(v, 1);
        reinterpret_cast<void(__cdecl*)(View*, int)>(0x61ACE0)(v, 1);
        reinterpret_cast<void(__cdecl*)(View*, int, int)>(0x494270)(v, 0, 1);
    });
}

// The car's own reflection on the road (0x631250(view): local player 0 / human slot 0 for view 4, player 1 / slot 1 for
// anything else) drew player 1's car into every player's road reflection -> the player view 4 is rendered for.
#define AT_REFLECT_CAR_PICK  0x631289    // mov eax,[edi+4] ... call 0x5ED980 (edi = view)
#define AT_REFLECT_CAR_DONE  0x6312B0    // esi = local player, eax = racer id
void* GetLocalPlayer(int k);
static void* __fastcall ReflectedCarPlayer(int viewId, int* racer) {
    int k = viewId == 4 ? g_reflectionPlayer : 1;
    *racer = RacerIdOfPlayer(k);
    return GetLocalPlayer(k);
}
static __declspec(naked) void ReflectCarPickHook() {
    __asm {
        sub esp, 4
        mov edx, esp
        mov ecx, dword ptr [edi + 4]
        call ReflectedCarPlayer
        mov esi, eax
        pop eax
        push AT_REFLECT_CAR_DONE
        ret
    }
}

static void __cdecl RoadReflectionsForPlayers() {
    View& v4 = GetView(4);
    if (!SplitRacing() || !at<int>(OPT_ROAD_REFLECTION) || !v4.active || !v4.rt || !v4.rt->enabled || !v4.platInfo ||
        !g_worldViews || !at<void*>(REFLECT_TEXTURE))
        return;
    g_gameReflTex = at<void*>(REFLECT_TEXTURE);
    g_gameReflSurf = at<void*>(REFLECT_SURFACE);
    uint8_t* cam1 = v4.camera;
    for (int k = 1; k < g_screenViews; ++k) {
        if (!GetView(kPlayerViewId[k]).camera || !EnsureReflectionTexture(k)) continue;
        at<void*>(REFLECT_TEXTURE) = g_reflTex[k];
        at<void*>(REFLECT_SURFACE) = g_reflSurf[k];
        v4.rt->surface = g_reflSurf[k];
        RenderRoadReflection(k);
    }
    at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    at<void*>(REFLECT_SURFACE) = g_gameReflSurf;
    v4.rt->surface = g_gameReflSurf;
    v4.camera = cam1;
    g_reflectionPlayer = 0;
    reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v4.platInfo, &v4, 0, 0, 0);
}

static __declspec(naked) void ReflectionsDoneHook() {
    __asm {
        pushad
        call RoadReflectionsForPlayers
        popad
        cmp dword ptr ds:[0x00870CB8], ebx  // stolen
        push AT_REFLECTIONS_DONE + 6
        ret
    }
}

// player view k samples its own reflection texture (player 1: the game's)
static void UseReflectionOf(int k) {
    if (!g_gameReflTex || at<void*>(REFLECT_SURFACE) != g_gameReflSurf) return;   // not this frame / device reset
    at<void*>(REFLECT_TEXTURE) = k > 0 && g_reflTex[k] ? g_reflTex[k] : g_gameReflTex;
}

// The nitrous screen effect's strength (effect channel handler 0x5D7770, writes the view effect's channel 4 from the
// car's nitrous bar via 0x5925B0) picks the car by view: view 1 -> local player 1 (in creation order), any other view
// -> player 2, so P3's view showed P2's bar (frozen) and P3's own nitrous never registered (user). The view's own player.
#define AT_NOS_FX_PLAYER      0x5D778D   // cmp edx,1; setne cl; mov edx,[ecx*4+inOrder]  (13 bytes, edx = view id)
#define AT_NOS_FX_PLAYER_RET  0x5D779A
void* LocalPlayerInOrder(int k);
static void* __fastcall NosFxPlayer(int viewId) {
    int k = viewId == 1 ? 0 : 1;
    for (int i = 0; i < MAX_LOCAL_PLAYERS; ++i)
        if (viewId == kPlayerViewId[i]) k = i;
    return LocalPlayerInOrder(k);
}
static __declspec(naked) void NosFxPlayerHook() {
    __asm {
        mov ecx, edx
        call NosFxPlayer
        mov edx, eax
        push AT_NOS_FX_PLAYER_RET
        ret
    }
}

// Overbright strength follows the nitrous of player 1 only: once per frame 0x5BC180 reads view 1's effect state
// (+0x60 object: +0x3C nitrous on, +0x1E0 its intensity), smooths it (0x876848, rate 0x800B90) and sets the amount the
// full-screen overbright pass adds into its accumulation (0x7FF114, blend factor at 0x5CCB7E) -> P1's nitrous
// brightened every view. Now each player view has its own smoothed amount, and the accumulation blit (0x5C6BE0 at
// 0x5CCBA5) runs once per view with that view's blend factor, limited to its region by a scissor rectangle (the blit
// itself stays full-screen so its sampling is unchanged; SetRenderTargetHook re-applies the scissor).
#define AT_OVERBRIGHT_AMOUNT_CALL 0x5D2B5A   // call 0x5BC180 (once per frame)
#define FN_OVERBRIGHT_AMOUNT      0x5BC180
#define AT_OVERBRIGHT_ACCUM_CALL  0x5CCBA5   // call 0x5C6BE0 (cdecl, 8 args) into the streak surface
static void SetRenderState(DWORD state, DWORD value);
static float g_obSmoothed[MAX_LOCAL_PLAYERS];
static int g_obAmount[MAX_LOCAL_PLAYERS];
static int g_obThreshold[MAX_LOCAL_PLAYERS];   // bright-pass threshold 0x7FF10C (216 -> 62 with nitrous)
int g_obScissorSlotExt = -1;
#define g_obScissorSlot g_obScissorSlotExt
static void __cdecl OverbrightAmounts() {
    reinterpret_cast<void(__cdecl*)()>(FN_OVERBRIGHT_AMOUNT)();
    g_obAmount[0] = at<int>(0x7FF114);
    g_obThreshold[0] = at<int>(0x7FF10C);
    g_obSmoothed[0] = at<float>(0x876848);
    if (!SplitRacing()) return;
    for (int k = 1; k < g_screenViews; ++k) {
        auto* fx = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(&GetView(kPlayerViewId[k])) + 0x60);
        float t = at<float>(0x7D7D3C);
        if (at<uint8_t>(0x870904)) t = at<float>(0x7FF7C8);
        else if (at<int>(GAME_STATE) != 6) t = at<float>(0x7A20A8);
        else if (fx && *reinterpret_cast<int*>(fx + 0x3C)) t = *reinterpret_cast<float*>(fx + 0x1E0);
        float lo = at<float>(0x7FF0FC), x = lo + (at<float>(0x7FF100) - lo) * t;
        float rate = at<float>(0x800B90);
        g_obSmoothed[k] = (1.0f - rate) * g_obSmoothed[k] + rate * x;
        int a = at<int>(0x7FF118), b = at<int>(0x7FF11C);
        g_obAmount[k] = static_cast<int>(a + (b - a) * g_obSmoothed[k]);
        int c = at<int>(0x7FF120), d = at<int>(0x7FF124);
        g_obThreshold[k] = static_cast<int>(c + (d - c) * g_obSmoothed[k]);
    }
}
static RECT g_obScissorRect;
static void SetScissor(void* dev, int k, float scale = 1.0f) {   // region of player k, scaled to the target's size
    g_obScissorRect = {static_cast<LONG>(g_regionX[k] * scale), static_cast<LONG>(g_regionY[k] * scale),
                       static_cast<LONG>((g_regionX[k] + g_rt[k].width) * scale),
                       static_cast<LONG>((g_regionY[k] + g_rt[k].height) * scale)};
    reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*)>((*reinterpret_cast<void***>(dev))[75])(dev, &g_obScissorRect);
}
void ReapplyOverbrightScissor(void* dev) {
    reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*)>((*reinterpret_cast<void***>(dev))[75])(dev, &g_obScissorRect);
}
// The bright pass of the overbright post (0x5C5F70 with its last argument 1: half-size scene 0x86FF7C -> 0x86FFA8,
// calls at 0x5CCA40 / 0x5CCA61) uses the threshold 0x7FF10C, which 0x5BC180 lowers with player 1's nitrous -> every
// view got brighter. Run once per view with its own threshold, scissored to its region (scaled to the target).
#define AT_BRIGHT_PASS_CALL1 0x5CCA40
#define AT_BRIGHT_PASS_CALL2 0x5CCA61
#define AT_BRIGHT_PASS_CALL0 0x5CCA25    // the downsample before it (last argument 0), same treatment in case it is the one
static void __cdecl BrightPassPerView(void* dst, void* src, int mode, void* params, int flag) {
    auto blit = reinterpret_cast<void(__cdecl*)(void*, void*, int, void*, int)>(0x5C5F70);
    void* dev = at<void*>(D3D_DEVICE);
    if (!SplitRacing() || g_screenViews < 2 || !dev || !dst || at<int>(GAME_STATE) != 6) { blit(dst, src, mode, params, flag); return; }
    struct { DWORD Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality; UINT Width, Height; } desc = {};
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(dst))[12])(dst, &desc);
    float scale = at<int>(SCREEN_W) > 0 ? static_cast<float>(desc.Width) / at<int>(SCREEN_W) : 1.0f;
    int saved = at<int>(0x7FF10C);
    SetRenderState(174, 1);
    for (int k = 0; k < g_screenViews; ++k) {
        at<int>(0x7FF10C) = g_obThreshold[k];
        g_obScissorSlot = k;
        SetScissor(dev, k, scale);
        blit(dst, src, mode, params, flag);
    }
    g_obScissorSlot = -1;
    SetRenderState(174, 0);
    at<int>(0x7FF10C) = saved;
}
static void __cdecl OverbrightAccumulate(void* a1, int a2, void* a3, void* a4, int a5, int a6, int a7, int a8) {
    auto blit = reinterpret_cast<void(__cdecl*)(void*, int, void*, void*, int, int, int, int)>(0x5C6BE0);
    void* dev = at<void*>(D3D_DEVICE);
    if (!SplitRacing() || g_screenViews < 2 || !dev || at<int>(GAME_STATE) != 6) { blit(a1, a2, a3, a4, a5, a6, a7, a8); return; }
    SetRenderState(174, 1);                                         // D3DRS_SCISSORTESTENABLE
    for (int k = 0; k < g_screenViews; ++k) {
        uint32_t v = static_cast<uint32_t>(g_obAmount[k]) & 0xFF;
        SetRenderState(0xC1, v | v << 8 | v << 16 | v << 24);      // D3DRS_BLENDFACTOR
        g_obScissorSlot = k;
        SetScissor(dev, k);
        blit(a1, a2, a3, a4, a5, a6, a7, a8);
    }
    g_obScissorSlot = -1;
    SetRenderState(174, 0);
}

// 3 players: P3 is centred in the bottom half, leaving a bar on each side. Nothing draws there, so both bars are
// filled black every frame after the views - in the picture and in the streak/overbright buffer (whose full-screen
// pass would otherwise show leftovers).
static void ClearUnusedAreas() {
    if (!SplitRacing() || g_screenViews != 3) return;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return;
    int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
    RECT bars[2] = {{0, h / 2, w / 4, h}, {w / 4 + w / 2, h / 2, w, h}};
    auto fill = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, DWORD)>((*reinterpret_cast<void***>(dev))[35]);
    void* targets[2] = {reinterpret_cast<RenderTarget*>(RT_BACKBUFFER)->surface,
                        at<int>(0x870CFC) ? at<void*>(0x86FF70) : nullptr};
    for (void* t : targets)
        if (t) for (const RECT& r : bars) fill(dev, t, &r, 0);
}

// after the player views: the game's texture back
static void RestoreRoadReflectionView() {
    ClearUnusedAreas();
    if (g_gameReflTex && at<void*>(REFLECT_SURFACE) == g_gameReflSurf) at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    g_gameReflTex = nullptr;
    g_gameReflSurf = nullptr;
}

// Car reflections: the cars sample a 128x128 cube render target (0x870858, faces 0x87085C[6], depth 0x870874[6])
// rendered every frame by views 0x10..0x15 (0x5CACF0 loop) from six cameras of the env camera object (0x871BC0) that
// 0x61AB50 places at player 1's car. Players 2..4 get their own cube, one player per frame in turn, rendered right
// after player 1's (same face block, cameras placed at their car); before each player view their faces are copied
// into the game's cube (StretchRect), so player 1's view sees its own and the others theirs.
#define OPT_CAR_ENV_MAP     0x870CB8
#define OPT_ENV_MAP_WORLD   0x870CBC
#define ENV_CUBE_FACES      0x87085C     // IDirect3DSurface9*[6]
#define ENV_CUBE_SIZE       0x7FEE6C
#define ENV_CUBE_FORMAT     0x7FEE70
#define ENV_CAMERAS         0x871BC0
#define ENV_CAR_OFFSET      0x803E50     // float[3] added to the car position
#define FN_PLACE_ENV_CAMERAS 0x5C3450    // void __thiscall (cameras; const float* eye, const float* target)
#define FN_PLACE_ENV_P1     0x61AB50     // the game's placement (player 1)
#define FN_ENV_PREPARE      0x5B9D90
#define AT_ENV_DONE         0x5CAE12     // call 0x5CF420 after the face loop
static void* g_envCube[MAX_LOCAL_PLAYERS];
static void* g_envFace[MAX_LOCAL_PLAYERS][6];
static bool g_envValid[MAX_LOCAL_PLAYERS];
static int g_envTurn = 1;

static void ReleaseEnvCubes() {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        for (void*& f : g_envFace[k]) ComRelease(f);
        ComRelease(g_envCube[k]);
        g_envValid[k] = false;
    }
}

static bool EnsureEnvCube(int k) {
    if (g_envCube[k]) return true;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return false;
    auto create = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, DWORD, DWORD, void**, HANDLE*)>(
        (*reinterpret_cast<void***>(dev))[25]);                      // CreateCubeTexture
    if (FAILED(create(dev, at<UINT>(ENV_CUBE_SIZE), 1, 1 /* D3DUSAGE_RENDERTARGET */, at<DWORD>(ENV_CUBE_FORMAT),
                      0 /* DEFAULT */, &g_envCube[k], nullptr)))
        return false;
    auto face = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, UINT, void**)>(
        (*reinterpret_cast<void***>(g_envCube[k]))[18]);            // GetCubeMapSurface
    for (int i = 0; i < 6; ++i)
        if (FAILED(face(g_envCube[k], i, 0, &g_envFace[k][i]))) { ReleaseEnvCubes(); return false; }
    Log("car env map for player %d created", k + 1);
    return true;
}

static void SetRenderState(DWORD state, DWORD value) {
    void* dev = at<void*>(D3D_DEVICE);
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>((*reinterpret_cast<void***>(dev))[57])(dev, state, value);
}

static void RenderEnvCube(int k) {
    // cameras at player k's car, looking from their camera (as 0x61AB50 does for player 1)
    auto* car = static_cast<uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, int)>(0x5F0E30)(
        at<void*>(0x890080), RacerIdOfPlayer(k)));
    uint8_t* cam = GetView(kPlayerViewId[k]).camera;
    if (!car || !cam) return;
    const float* pos = reinterpret_cast<const float*>(car + 0x50);
    const float* off = reinterpret_cast<const float*>(ENV_CAR_OFFSET);
    float target[3] = {pos[0] + off[0], pos[1] + off[1], pos[2] + off[2]};
    float eye[3];
    memcpy(eye, cam + 0x40, sizeof(eye));
    // the cube is in camera space (the cars sample it with a camera-space reflection vector): 0x5C3450 orients the
    // faces by view 1's camera -> player k's camera for the call
    View& v1 = GetView(1);
    uint8_t* cam1 = v1.camera;
    v1.camera = cam;
    reinterpret_cast<void(__thiscall*)(void*, const float*, const float*)>(FN_PLACE_ENV_CAMERAS)(
        reinterpret_cast<void*>(ENV_CAMERAS), eye, target);
    v1.camera = cam1;
    void** gameFaces = reinterpret_cast<void**>(ENV_CUBE_FACES);
    for (int id = 0x10; id <= 0x15; ++id) {
        View& v = GetView(id);
        if (!v.active || !v.rt || !v.rt->enabled || !v.platInfo) continue;
        int f = -1;
        for (int i = 0; i < 6; ++i) if (v.rt->surface == gameFaces[i]) f = i;
        if (f < 0) continue;
        void* saved = v.rt->surface;
        v.rt->surface = g_envFace[k][f];
        reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v.platInfo, &v, 0, 0, 0);
        RecullHeadlightView(v);
        reinterpret_cast<void(__cdecl*)(RenderTarget*, int)>(FN_RT_BEGIN)(v.rt, 1);
        SetRenderState(7, 0);            // D3DRS_ZENABLE: sky without depth
        SetRenderState(14, 0);           // D3DRS_ZWRITEENABLE
        reinterpret_cast<void(__cdecl*)(View*)>(FN_DRAW_SKY)(&v);
        SetRenderState(14, 1);
        SetRenderState(7, 1);
        reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
        reinterpret_cast<void(__cdecl*)()>(FN_ENV_PREPARE)();
        if (at<int>(OPT_ENV_MAP_WORLD))
            reinterpret_cast<void(__thiscall*)(void*, View*, int)>(FN_WORLD_DRAW)(g_worldViews, &v, 0);
        reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();
        v.rt->surface = saved;
    }
    g_envValid[k] = true;
}

static void __cdecl EnvMapsForPlayers() {
    reinterpret_cast<void(__cdecl*)()>(FN_FLUSH_DRAWS)();   // replaced call
    if (!SplitRacing() || !at<int>(OPT_CAR_ENV_MAP) || at<int>(GAME_STATE) != 6 || !g_worldViews ||
        !at<void*>(ENV_CUBE_FACES) || g_screenViews < 2)
        return;
    if (g_envTurn < 1 || g_envTurn >= g_screenViews) g_envTurn = 1;
    int k = g_envTurn++;
    if (!GetView(kPlayerViewId[k]).camera || !EnsureEnvCube(k)) return;
    RenderEnvCube(k);
    for (int id = 0x10; id <= 0x15; ++id) {     // the faces' views back on player 1's cameras
        View& v = GetView(id);
        if (v.active && v.platInfo)
            reinterpret_cast<void(__thiscall*)(void*, View*, int, int, int)>(FN_VIEW_UPDATE)(v.platInfo, &v, 0, 0, 0);
    }
    reinterpret_cast<void(__cdecl*)()>(FN_PLACE_ENV_P1)();
}

// before player view k: its cube's faces into the game's cube (player 1: rendered there this frame, first view)
static void UseEnvMapOf(int k) {
    if (k < 1 || !g_envValid[k] || !g_envCube[k] || !at<int>(OPT_CAR_ENV_MAP)) return;
    void* dev = at<void*>(D3D_DEVICE);
    void** gameFaces = reinterpret_cast<void**>(ENV_CUBE_FACES);
    auto stretch = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, void*, const RECT*, DWORD)>(
        (*reinterpret_cast<void***>(dev))[34]);                      // StretchRect
    for (int i = 0; i < 6; ++i)
        if (gameFaces[i] && g_envFace[k][i]) stretch(dev, g_envFace[k][i], nullptr, gameFaces[i], nullptr, 0);
}

using ReleaseTargetsFn = void(__cdecl*)();
static ReleaseTargetsFn g_origReleaseTargets;
static void __cdecl ReleaseTargetsHook() {
    if (g_gameReflTex && at<void*>(REFLECT_TEXTURE) != g_gameReflTex) at<void*>(REFLECT_TEXTURE) = g_gameReflTex;
    g_gameReflTex = g_gameReflSurf = nullptr;
    ReleaseReflectionTextures();
    ReleaseEnvCubes();
    g_origReleaseTargets();
}

// Headlight projectors for every player. The game has one projector: the headlight pass (0x5D6D20(world; view 9,
// 0x488B90) at 0x5CB218) only flags the meshes in view 9's culled list (+8 -> +0x1B |= 0x80); the world draw turns the
// flag into render flag 0x200 (0x5D6B85), which picks the material's headlight shader variant (0x5C5953:
// worldheadlight.efx etc.) lit by view 9's projector, and clears the flag. The world draw only queues the meshes;
// 0x5CF420 draws the queue (normally at 0x5CB2A0), setting the projector params per draw (0x5BDAA0, colour from
// 0x60F510). One projector per draw, so the other players' beams are added in extra passes
// after the view's world draw (0x5CB23B), per other player whose lights are on, with view 9 aimed at that car:
//   1. its flagged meshes again with the headlight colour 0, subtracted from the frame (blend op REVSUBTRACT)
//   2. the same meshes with the beam, added (blend op ADD)
// = + mesh * beam (the base lighting and the fog colour cancel out). Only the flagged meshes are drawn: the world
// draw's include mask (0x2000 for flags & 0x10, imm at 0x5D6B0F) is 0x200 meanwhile. No depth writes, depth test
// with a small bias (both passes use the same headlight shader, the view's own pass may not have).
#define AT_MAIN_WORLD_DRAW    0x5CB23B   // call 0x5D6AD0 (world views; view, 0x20)
#define FN_FLAG_BEAM_MESHES   0x5D6D20   // void __thiscall (world views; View*, void(__cdecl*)(item*))
#define FN_FLAG_BEAM_MESH     0x488B90
#define AT_WORLD_INCLUDE_MASK 0x5D6B0F   // imm32 of `mov [ebp-0Ch], 2000h` (world draw flags & 0x10)
#define FN_DRAW_QUEUED        0x5CF420   // void __cdecl (): draws the queued meshes (per effect; headlight params per draw)
#define AT_BEAM_COLOR_CALL    0x5BDC20   // call 0x60F510: headlight colour (0x00BBGGRR)
#define FN_CAR_LIGHT_COLOR    0x60F510
static int g_beamPass;                   // 1: subtract (colour 0), 2: add
static bool g_beamSkipBlended;           // the material asked for alpha blending: not drawn in the beam passes
static HRESULT __stdcall SetRenderStateHook(void* dev, DWORD state, DWORD value) {
    if (g_beamPass && state == 27) g_beamSkipBlended = value != 0;
    if (g_beamPass) switch (state) {
        case 14: value = 0; break;                              // ZWRITEENABLE
        case 23: value = 4; break;                              // ZFUNC: LESSEQUAL
        case 27: value = 1; break;                              // ALPHABLENDENABLE
        case 19: case 20: value = 2; break;                     // SRCBLEND, DESTBLEND: ONE
        case 171: value = g_beamPass == 1 ? 3 : 1; break;       // BLENDOP: REVSUBTRACT / ADD
        case 206: value = 0; break;                             // SEPARATEALPHABLENDENABLE
        case 195: { float b = -0.00002f; memcpy(&value, &b, 4); break; }   // DEPTHBIAS
        case 175: { float b = -1.0f; memcpy(&value, &b, 4); break; }       // SLOPESCALEDEPTHBIAS
    }
    return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>(g_origSetRenderState)(dev, state, value);
}
// Semi-transparent materials don't cancel out (the subtracted copy clamps at black where it covers less than the
// whole pixel): their draws are skipped in the beam passes.
static HRESULT __stdcall DrawIndexedHook(void* dev, int type, int base, UINT minIdx, UINT n, UINT start, UINT count) {
    if (g_beamPass && g_beamSkipBlended) return 0;
    return reinterpret_cast<HRESULT(__stdcall*)(void*, int, int, UINT, UINT, UINT, UINT)>(g_origDrawIndexed)(
        dev, type, base, minIdx, n, start, count);
}
static uint32_t __cdecl BeamColor(void* car) {
    if (g_beamPass == 1) return 0;
    return reinterpret_cast<uint32_t(__cdecl*)(void*)>(FN_CAR_LIGHT_COLOR)(car);
}
static void __cdecl ClearBeamMesh(uint8_t* item) { (*reinterpret_cast<uint8_t**>(item + 8))[0x1B] &= 0x7F; }
static const DWORD kBeamStates[] = {14, 23, 27, 19, 20, 171, 206, 195, 175};

static void __fastcall WorldDrawWithBeams(void* world, void*, View* v, int flags) {
    using DrawFn = void(__thiscall*)(void*, View*, int);
    reinterpret_cast<DrawFn>(FN_WORLD_DRAW)(world, v, flags);
    // split racing, or the full screen race intro (view 1, player 1's projector)
    if (!SplitMode() || at<int>(NUM_PLAYERS) != 2 || !at<int>(OPT_CAR_HEADLIGHTS) || world != g_worldViews) return;
    int k = -1;
    for (int i = 0; i < g_screenViews && i < MAX_LOCAL_PLAYERS; ++i) if (&GetView(kPlayerViewId[i]) == v) k = i;
    View& v9 = GetView(9);
    void* dev = at<void*>(D3D_DEVICE);
    if (dev && (!g_origSetRenderState || !g_origDrawIndexed)) {   // the first race's intro comes before any split frame
        HookDeviceMethod(dev, 57, reinterpret_cast<void*>(&SetRenderStateHook), &g_origSetRenderState);
        HookDeviceMethod(dev, 82, reinterpret_cast<void*>(&DrawIndexedHook), &g_origDrawIndexed);
    }
    if (k < 0 || !v9.active || !v9.camera || !v9.platInfo || !v->camera || !dev || !g_origSetRenderState ||
        !g_origDrawIndexed)
        return;
    DWORD blending = 0;
    const float* eye = reinterpret_cast<const float*>(v->camera + 0x40);
    using FlagFn = void(__thiscall*)(void*, View*, void*);
    auto flag = reinterpret_cast<FlagFn>(FN_FLAG_BEAM_MESHES);
    auto drawQueued = reinterpret_cast<void(__cdecl*)()>(FN_DRAW_QUEUED);
    DWORD saved[sizeof(kBeamStates) / sizeof(kBeamStates[0])];
    auto get = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD*)>((*reinterpret_cast<void***>(dev))[58]);
    bool any = false;
    int players = 2 + (g_extraPlayers > 0 ? g_extraPlayers : 0);   // the intro has 1 view for all of them
    for (int j = 0; j < players && j < MAX_LOCAL_PLAYERS; ++j) {
        const uint8_t* car = j == k ? nullptr : PlayerCar(j);
        if (!car || !car[0x550]) continue;
        const float* pos = reinterpret_cast<const float*>(car + 0x50);
        float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
        if (dx * dx + dy * dy + dz * dz > 250.0f * 250.0f) continue;   // beam out of sight
        if (!any) {
            for (size_t i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) get(dev, kBeamStates[i], &saved[i]);
            blending = saved[2];
            static const uint32_t headlightOnly = 0x200;
            Write(AT_WORLD_INCLUDE_MASK, &headlightOnly, 4);
            drawQueued();                // the view's own world draw first, as the game would
            any = true;
        }
        HeadlightsForPlayer(j);
        for (int pass = 1; pass <= 2; ++pass) {
            flag(world, &v9, reinterpret_cast<void*>(FN_FLAG_BEAM_MESH));
            reinterpret_cast<DrawFn>(FN_WORLD_DRAW)(world, v, flags | 0x10);   // queues the flagged meshes
            g_beamPass = pass;
            for (DWORD st : kBeamStates) SetRenderState(st, 0);   // forced to the pass's values by the hook
            g_beamSkipBlended = blending != 0;   // the game's own blend state (it may not set it again)
            drawQueued();
            g_beamPass = 0;
        }
        flag(world, &v9, reinterpret_cast<void*>(&ClearBeamMesh));   // meshes outside the player view's list
    }
    if (!any) return;
    static const uint32_t original = 0x2000;
    Write(AT_WORLD_INCLUDE_MASK, &original, 4);
    for (size_t i = 0; i < sizeof(saved) / sizeof(saved[0]); ++i) SetRenderState(kBeamStates[i], saved[i]);
    HeadlightsForPlayer(k);              // the projector back on the view's own car for the rest of the view
}

static void __cdecl RefreshViewTarget(int viewId) {
    if (!SplitRacing()) return;
    if (g_screenViews > 2) memset(reinterpret_cast<void*>(GLARE_SLOT_COUNTS), 0, 8);
    auto* bb = reinterpret_cast<RenderTarget*>(RT_BACKBUFFER);
    for (int k = 0; k < g_screenViews; ++k)
        if (kPlayerViewId[k] == viewId && GetView(viewId).rt == &g_rt[k]) {
            g_rt[k].surface = bb->surface;
            g_rt[k].depth = bb->depth;
            HeadlightsForPlayer(k);
            UseReflectionOf(k);
            UseEnvMapOf(k);
        }
}

static __declspec(naked) void ViewLoopTopHook() {
    __asm {
        mov edx, dword ptr [esp + 0x10]  // loop counter 1..4 -> player view id (stolen: mov edx,[esp+10]; push edx)
        mov edx, dword ptr [kPlayerViewId + edx * 4 - 4]
        mov g_currentView, edx
        pushad
        push edx
        call RefreshViewTarget
        add esp, 4
        popad
        push edx
        push AT_VIEWLOOP_TOP_RET
        ret
    }
}

static __declspec(naked) void ViewLoopExitHook() {
    __asm {
        mov g_currentView, 0
        pushad
        call RestoreRoadReflectionView
        popad
        push AT_VIEWLOOP_EXIT_RET
        cmp byte ptr ds:[0x008709D0], bl // stolen; flags are consumed by the jz after the return point
        ret
    }
}

// Projection for the half-height split views: keep the full-screen horizontal FOV (hor+ for the screen aspect) and
// derive the vertical scale from the viewport aspect. Runs right before the game builds view-proj and frustum.
static void __cdecl AdjustProjection(View* v, float* plat) {
    bool player = false;
    for (int k = 0; k < g_screenViews; ++k) player |= v->id == kPlayerViewId[k];
    // view 4 (road reflection texture) must project exactly like the player view that samples it
    const RenderTarget* shape = v->rt;
    if (v->id == 4 && g_reflectionPlayer >= 0 && g_reflectionPlayer < g_screenViews) {
        player = true;
        shape = &g_rt[g_reflectionPlayer];
    }
    if (!SplitRacing() || !player || !shape || !v->camera || shape->height <= 0) return;
    unsigned fov = *reinterpret_cast<uint16_t*>(v->camera + 0xC4);
    if (!fov) return;
    const float kPi = 3.14159265f;
    float half = fov * (kPi / 65536.0f);                          // half of the camera's (4:3) horizontal FOV
    float screenAspect = static_cast<float>(at<int>(SCREEN_W)) / at<int>(SCREEN_H);
    float p00 = (1.0f / tanf(half)) * (4.0f / 3.0f) / screenAspect;
    float viewAspect = static_cast<float>(shape->width) / shape->height;
    plat[0x40 / 4] = p00;
    plat[0x54 / 4] = -p00 * viewAspect;
}

static __declspec(naked) void ProjectionHook() {
    __asm {
        pushad
        push esi                         // platInfo
        push ebp                         // view
        call AdjustProjection
        add esp, 8
        popad
        push ebx                         // stolen: push ebx ; xor edi,edi ; push esi ; lea ebp,[esi+80h]
        xor edi, edi
        push esi
        lea ebp, [esi + 0x80]
        push AT_PROJECTION_RET
        ret
    }
}

// ---------------------------------------------------------------- local player arrays (4 players)
// The game keeps its local players in two 2-slot arrays followed by the count: 0x8900AC p[2] (by index),
// 0x8900B4 q[2] (creation order), 0x8900BC count. Every reference is an absolute operand in .text (295, no data
// pointers); move them to a 4-slot block with the same shape, so "end of p" == q and "end of q" == count still hold.
#define OLD_PLAYERS_P      0x8900AC
#define OLD_PLAYERS_END    0x8900C0
struct LocalPlayers { void* byIndex[MAX_LOCAL_PLAYERS]; void* inOrder[MAX_LOCAL_PLAYERS]; int count; };
static LocalPlayers g_localPlayers;
void* GetLocalPlayer(int k) { return k >= 0 && k < g_localPlayers.count && k < MAX_LOCAL_PLAYERS ? g_localPlayers.byIndex[k] : nullptr; }
void* LocalPlayerInOrder(int k) { return k >= 0 && k < MAX_LOCAL_PLAYERS ? g_localPlayers.inOrder[k] : nullptr; }

static uintptr_t RelocatePlayerAddr(uint32_t a) {
    uint32_t k = a - OLD_PLAYERS_P;
    auto base = reinterpret_cast<uintptr_t>(&g_localPlayers);
    if (k < 8) return base + k;                                      // p[0..1] (+ bytes)
    if (k < 16) return base + offsetof(LocalPlayers, inOrder) + (k - 8);
    return base + offsetof(LocalPlayers, count) + (k - 16);
}

#define AT_PLAYERS_TEARDOWN_DISP 0x60AA76   // disp32 of `mov esi, [eax*4 + 0x8900B0]` at 0x60AA73
static int RelocateLocalPlayers() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(0x400000);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(0x400000 + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    int patched = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        auto* p = reinterpret_cast<uint8_t*>(0x400000 + sec->VirtualAddress);
        size_t n = sec->Misc.VirtualSize;
        DWORD old;
        VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
        for (size_t o = 0; o + 4 <= n; ++o) {
            uint32_t v;
            memcpy(&v, p + o, 4);
            if (v < OLD_PLAYERS_P || v >= OLD_PLAYERS_END || (v - OLD_PLAYERS_P) % 4) continue;
            uint32_t nv = static_cast<uint32_t>(RelocatePlayerAddr(v));
            memcpy(p + o, &nv, 4);
            ++patched;
            o += 3;
        }
        VirtualProtect(p, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, n);
    }
    return patched;
}

// Car light glares (model render 0x5D6570): a car's light glare slots (0x48AB20(type, view mask 1/2)) are only made
// when the view being rendered is view 1 or 2 (GetView(2) check at 0x5D683C), with mask (view != view 1) + 1 (GetView(1)
// at 0x5D686F); the player view loop draws them with mask 1 (0x494270). Views 5/6 (P3/P4) got none: no nitrous
// taillight glare for P3 (user). While view 5/6 renders, both lookups return it: its glares are made like view 1's.
#define AT_GLARE_VIEW2_CALL 0x5D683C
#define AT_GLARE_VIEW1_CALL 0x5D686F
// Glares are made while models render for a view (outside the player view loop): the view comes from the model
// renderer's context ([ebp+0xC] + 0x40 in 0x5D6570).
static View* __cdecl GlareViewOf(View* rendering, int n) {
    for (int k = 2; k < g_screenViews; ++k)
        if (rendering == &GetView(kPlayerViewId[k])) return rendering;
    return &GetView(n);
}
static __declspec(naked) void GlareView() {
    __asm {
        mov eax, dword ptr [esp + 4]     // n
        mov ecx, dword ptr [ebp + 0xC]   // render context of 0x5D6570
        mov ecx, dword ptr [ecx + 0x40]  // its view
        push eax
        push ecx
        call GlareViewOf
        add esp, 8
        ret
    }
}


// Per-view world update 0x5DBE80 (called at 0x5CBADC and 0x5D2C21): for n = 1..2 it takes GetView(n) and the local
// player inOrder[n != 1] and updates that view's visibility/effect state (0x5D7580, emitter lists 0x5D7720, nitrous
// blur flag via 0x5DBD70 -> view+0x60 +0x18). Views 5/6 never got it: no nitrous blur for P3 (watch: only view 1's
// flag ever written). Loop over player views kPlayerViewId[n - 1] with inOrder[n - 1] for n = 1..4 (inactive views are
// skipped by the loop itself: `test [view+8]`).
#define AT_VIEWUPD_VIEW_CALL 0x5DBE91    // `call 0x48B1E0` after `push ebx`
#define AT_VIEWUPD_PLAYER  0x5DBEE0      // xor eax,eax; cmp ebx,1; setne al; mov eax,[eax*4+players] (15 bytes)
#define AT_VIEWUPD_LAST    0x5DBF48      // imm8 of `cmp ebx, 2`
// Inside it, the screen tint update (0x5D77F0) indexes two 2-entry tables by view id - 1 ("Sunrise Screen Tint"
// 0x88EA08, "Night Screen Tint" 0x88F210, 0x30 bytes each, objects registered by name with the data system): views
// 5/6 read past them (crash 0x5FA4E3 when the split view started). The tint is computed from the view's position on
// every call, so views 5/6 use views 1/2's entries.
#define AT_TINT_INDEX      0x5D7831      // lea edi,[esi+esi*2]; shl edi,4 (6 bytes), esi = view id - 1
#define AT_TINT_INDEX_RET  0x5D7837
static __declspec(naked) void TintIndexHook() {
    __asm {
        cmp esi, 2
        jb ok
        and esi, 1
    ok:
        lea edi, [esi + esi * 2]
        shl edi, 4
        push AT_TINT_INDEX_RET
        ret
    }
}

// The tint entries keep state between frames, so views 5/6 sharing views 1/2's entries made P3's surroundings leak
// into P1's view (P1's glares stopped being made as P3 drove elsewhere: glare probe, mask-1 allocations fell to 0).
// The tint update 0x5D77F0 (cdecl (index, ctx), called through a handler table; view id at [[ctx]] + 4) runs for views
// 5/6 with their own copies of the two entries swapped in.
#define FN_TINT_UPDATE     0x5D77F0      // 9 bytes stolen: sub esp,38h; push ebx; mov ebx,[esp+40h]; push ebp
#define TINT_TABLE_SUNRISE 0x88EA08
#define TINT_TABLE_NIGHT   0x88F210
#define TINT_ENTRY_SIZE    0x30
static void* g_origTintUpdate;
static uint8_t g_viewTint[2][2][TINT_ENTRY_SIZE];  // [view 5/6][sunrise, night]
static void __cdecl TintUpdateWrapper(int index, uint8_t* ctx) {
    auto call = reinterpret_cast<void(__cdecl*)(int, uint8_t*)>(g_origTintUpdate);
    uint8_t* view = ctx ? *reinterpret_cast<uint8_t**>(ctx) : nullptr;
    int id = view ? *reinterpret_cast<int*>(view + 4) : 0;
    if (id != 5 && id != 6) { call(index, ctx); return; }
    int v = id - 5;
    uint8_t* entry[2] = {reinterpret_cast<uint8_t*>(TINT_TABLE_SUNRISE) + v * TINT_ENTRY_SIZE,
                         reinterpret_cast<uint8_t*>(TINT_TABLE_NIGHT) + v * TINT_ENTRY_SIZE};
    uint8_t saved[2][TINT_ENTRY_SIZE];
    for (int t = 0; t < 2; ++t) {
        memcpy(saved[t], entry[t], TINT_ENTRY_SIZE);
        if (!g_viewTintInit[v]) memcpy(g_viewTint[v][t], entry[t], TINT_ENTRY_SIZE);
        memcpy(entry[t], g_viewTint[v][t], TINT_ENTRY_SIZE);
    }
    g_viewTintInit[v] = true;
    if (g_debug) CheckX87("tint in");
    call(index, ctx);
    if (g_debug) CheckX87("tint out");
    for (int t = 0; t < 2; ++t) {
        memcpy(g_viewTint[v][t], entry[t], TINT_ENTRY_SIZE);
        memcpy(entry[t], saved[t], TINT_ENTRY_SIZE);
    }
}

static View* __cdecl PlayerViewByNumber(int n) {
    return &GetView(n >= 1 && n <= MAX_LOCAL_PLAYERS ? kPlayerViewId[n - 1] : n);
}
static void PatchViewUpdateLoop() {
    WriteCall(AT_VIEWUPD_VIEW_CALL, &PlayerViewByNumber);
    uint8_t code[15] = {0x8D, 0x43, 0xFF,                        // lea eax, [ebx - 1]
                        0x8B, 0x04, 0x85, 0, 0, 0, 0,            // mov eax, [eax*4 + inOrder]
                        0x90, 0x90, 0x90, 0x90, 0x90};
    uint32_t inOrder = reinterpret_cast<uint32_t>(&g_localPlayers.inOrder[0]);
    memcpy(code + 6, &inOrder, 4);
    Write(AT_VIEWUPD_PLAYER, code, sizeof(code));
    const uint8_t last = MAX_LOCAL_PLAYERS;
    Write(AT_VIEWUPD_LAST, &last, 1);
    WriteJump(AT_TINT_INDEX, &TintIndexHook, 6);
    g_origTintUpdate = MakeTrampoline(FN_TINT_UPDATE, 9);
    WriteJump(FN_TINT_UPDATE, &TintUpdateWrapper, 9);
    WriteCall(AT_GLARE_VIEW2_CALL, &GlareView);
    WriteCall(AT_GLARE_VIEW1_CALL, &GlareView);
}

// Sound: the audio code picks, for a sound's position, the nearest local player (0x459AC0) or that player's car
// (0x459A50) - distance attenuation, doppler, which player "hears" it. Both compared players 1 and 2 only, so sounds
// near P3/P4 were judged from P1/P2's distance (user: P3 only heard when near P1/P2, like traffic). All local players.
#define FN_NEAREST_PLAYER_CAR 0x459A50   // void* __cdecl (const float* pos) -> player->car (player+4)
#define FN_NEAREST_PLAYER  0x459AC0      // void* __cdecl (const float* pos) -> player
static uint8_t* NearestLocalPlayer(const float* pos) {
    auto* first = static_cast<uint8_t*>(g_localPlayers.inOrder[0]);
    int n = g_localPlayers.count < MAX_LOCAL_PLAYERS ? g_localPlayers.count : MAX_LOCAL_PLAYERS;
    if (!SplitRacing() && n > 2) n = 2;  // single view (results, Street X layout 1): the game's 2 like its listeners
    if (n < 2 || !pos) return first;
    uint8_t* best = nullptr;
    float bestD = 0;
    for (int k = 0; k < n; ++k) {
        auto* player = static_cast<uint8_t*>(g_localPlayers.inOrder[k]);
        auto* car = player ? *reinterpret_cast<uint8_t**>(player + 4) : nullptr;
        if (!car) continue;
        const float* c = reinterpret_cast<const float*>(car + 0x60);
        float dx = c[0] - pos[0], dy = c[1] - pos[1], dz = c[2] - pos[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (!best || d < bestD) { best = player; bestD = d; }
    }
    return best ? best : first;
}
static void* __cdecl NearestPlayer(const float* pos) { return NearestLocalPlayer(pos); }
static void* __cdecl NearestPlayerCar(const float* pos) {
    uint8_t* player = NearestLocalPlayer(pos);
    return player ? *reinterpret_cast<void**>(player + 4) : nullptr;
}

// ---------------------------------------------------------------- sound listeners (4)
// The audio mixes each sound against one of 2 listeners (split screen: P1's and P2's cameras). Listener state is 14
// 2-entry arrays (old base, stride below) written by the listener update 0x46D7E0 (hand-unrolled for listener 0 and
// 1: audio player list [[0x82B884]+0xAC] nodes with index +0x10 0/1 -> node+0x30 -> +0x18 -> +0x238 -> +0x1C camera)
// and read per sound by its listener index (sound+0xD0, chosen as the nearer of 2 at 0x45E147). Arrays move to
// 4-entry storage (all .text references rewritten); listeners 2/3 are filled by running the update a second time
// with players 3/4's list nodes posing as 0/1 and their slots swapped in; each sound picks the nearest listener.
struct ListenerArray { uint32_t oldBase, stride; uint8_t* newBase; };
static ListenerArray g_listenerArrays[] = {
    {0x82BE70, 8, nullptr},    {0x82BE80, 0x10, nullptr}, {0x82BECC, 8, nullptr},    {0x82BEDC, 8, nullptr},
    {0x82BEF0, 0x10, nullptr}, {0x82BF10, 8, nullptr},    {0x82BF20, 8, nullptr},    {0x82BF34, 8, nullptr},
    {0x82C610, 0x10, nullptr}, {0x82C670, 8, nullptr},    {0x82C6C4, 8, nullptr},    {0x82BB40, 4, nullptr},
    {0x82BB48, 4, nullptr},    {0x82BB50, 4, nullptr},
};
#define LISTENER_POS_ARRAY 3             // 0x82BEDC: listener (x, y) used for the nearest-listener choice
static uint8_t g_listenerMem[4096];
static bool g_listenerParked[MAX_LOCAL_PLAYERS];      // stale since the race load (see ListenerUpdate)
static float g_listenerParkPos[MAX_LOCAL_PLAYERS][2];

static int RelocateListenerArrays() {
    size_t used = 0;
    for (ListenerArray& a : g_listenerArrays) {
        a.newBase = g_listenerMem + used;
        memcpy(a.newBase, reinterpret_cast<void*>(a.oldBase), a.stride * 2);   // current values for 0/1
        used += a.stride * MAX_LOCAL_PLAYERS;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(0x400000);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(0x400000 + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    int patched = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        auto* p = reinterpret_cast<uint8_t*>(0x400000 + sec->VirtualAddress);
        size_t n = sec->Misc.VirtualSize;
        DWORD old;
        VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
        for (size_t o = 0; o + 4 <= n; ++o) {
            uint32_t v;
            memcpy(&v, p + o, 4);
            for (const ListenerArray& a : g_listenerArrays)
                if (v >= a.oldBase && v < a.oldBase + a.stride * 2) {
                    uint32_t nv = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a.newBase)) + (v - a.oldBase);
                    memcpy(p + o, &nv, 4);
                    ++patched;
                    o += 3;
                    break;
                }
        }
        VirtualProtect(p, n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, n);
    }
    return patched;
}

static uint8_t* ListenerSlot(const ListenerArray& a, int k) { return a.newBase + a.stride * k; }

// audio player list nodes {.., +4 next, .., +0x10 index}
static void RemapAudioPlayerIndices(int pairBase, int count) {
    auto* root = at<uint8_t*>(0x82B884);
    auto* list = root ? *reinterpret_cast<uint8_t**>(root + 0xAC) : nullptr;
    for (auto* node = list ? *reinterpret_cast<uint8_t**>(list + 0x10) : nullptr; node;
         node = *reinterpret_cast<uint8_t**>(node + 4)) {
        int& idx = *reinterpret_cast<int*>(node + 0x10);
        for (int j = 0; j < count; ++j) {
            if (idx == pairBase + j) { idx = j; break; }
            if (idx == j) { idx = pairBase + j; break; }
        }
    }
}

#define AT_LISTENER_UPDATE_CALL 0x483CB3
#define GAME_STATE_ADDR 0x8654A4
#define FN_LISTENER_UPDATE 0x46D7E0
static int ActiveListeners() {
    if (!SplitRacing()) return 2;
    int n = g_localPlayers.count;
    return n < 2 ? 2 : n > MAX_LOCAL_PLAYERS ? MAX_LOCAL_PLAYERS : n;
}
static void __fastcall ListenerUpdate(void* ecx, void*) {
    auto update = [ecx]() { reinterpret_cast<void(__fastcall*)(void*)>(FN_LISTENER_UPDATE)(ecx); };
    update();
    int n = ActiveListeners();
    for (int pair = 2; pair < n; pair += 2) {
        int count = n - pair >= 2 ? 2 : 1;           // 3 players: only player 3 poses as 0, player 2 stays 1
        uint8_t saved[sizeof(g_listenerArrays) / sizeof(g_listenerArrays[0])][2 * 0x10];
        int i = 0;
        for (ListenerArray& a : g_listenerArrays) {
            memcpy(saved[i++], ListenerSlot(a, 0), a.stride * 2);
            memcpy(ListenerSlot(a, 0), ListenerSlot(a, pair), a.stride * count);
        }
        RemapAudioPlayerIndices(pair, count);
        update();
        RemapAudioPlayerIndices(pair, count);         // the swap is its own inverse
        i = 0;
        for (ListenerArray& a : g_listenerArrays) {
            memcpy(ListenerSlot(a, pair), ListenerSlot(a, 0), a.stride * count);
            memcpy(ListenerSlot(a, 0), saved[i++], a.stride * 2);
        }
    }
    // A race load starts with every listener where the previous race left it; listener 0 then follows the intro
    // camera, but listeners 1-3 (players 2-4's cameras) stay put until the race view runs. Sounds bound to them -
    // the cars' own engine/turbo sounds go to their player's listener, others to the nearest - were mixed against the
    // old track's positions, so after a track change the intro was silent apart from ambience (user, 2P and 3P; the
    // first race and repeated tracks worked: listeners near / at 0,0). From a race load until a player's camera moves,
    // that player's listener copies listener 0 (the intro camera) and is left out of the nearest choice.
    static int s_lastState = -1;
    int state = at<int>(GAME_STATE_ADDR);
    auto camPos = [](int k) -> const float* {
        uint8_t* c = GetView(kPlayerViewId[k]).camera;
        return c ? reinterpret_cast<const float*>(c + 0x40) : nullptr;
    };
    if (state != s_lastState && state >= 3 && state <= 5)
        for (int k = 1; k < MAX_LOCAL_PLAYERS; ++k) {
            const float* c = camPos(k);
            g_listenerParked[k] = true;
            g_listenerParkPos[k][0] = c ? c[0] : 0.0f;
            g_listenerParkPos[k][1] = c ? c[1] : 0.0f;
        }
    s_lastState = state;
    for (int k = 1; k < n; ++k) {
        if (!g_listenerParked[k]) continue;
        const float* c = camPos(k);
        if (c && (c[0] != g_listenerParkPos[k][0] || c[1] != g_listenerParkPos[k][1])) { g_listenerParked[k] = false; continue; }
        for (ListenerArray& la : g_listenerArrays) memcpy(ListenerSlot(la, k), ListenerSlot(la, 0), la.stride);
    }
    static unsigned s_tick;
    if (g_debug && n > 2 && ++s_tick % 600 == 0) {
        const ListenerArray& e = g_listenerArrays[4];          // 0x82BEF0: listener position (x, y, z)
        auto pos = [&](int k) { return reinterpret_cast<const float*>(ListenerSlot(e, k)); };
        Log("listeners: %.1f,%.1f | %.1f,%.1f | %.1f,%.1f | %.1f,%.1f", pos(0)[0], pos(0)[1], pos(1)[0], pos(1)[1],
            pos(2)[0], pos(2)[1], pos(3)[0], pos(3)[1]);
    }
}

// Nearest listener for a sound (replaces the 2-way choice at 0x45E147; the 2-listener mode was already established).
#define AT_LISTENER_CHOICE 0x45E147      // mov edx, 1 (5 bytes) ... up to the consumer call at 0x45E1C1
#define AT_LISTENER_CHOICE_DONE 0x45E1C1
static int __cdecl NearestListener(const uint8_t* sound) {
    const float* pos = *reinterpret_cast<const float* const*>(sound + 0x70);
    const ListenerArray& a = g_listenerArrays[LISTENER_POS_ARRAY];
    int best = 0;
    float bestD = 0;
    for (int k = 0; k < ActiveListeners(); ++k) {
        const float* l = reinterpret_cast<const float*>(ListenerSlot(a, k));
        if (k > 0 && g_listenerParked[k]) continue;     // still the intro camera's copy (ListenerUpdate)
        float dx = pos[0] - l[0], dy = pos[1] - l[1], d = dx * dx + dy * dy;
        if (k == 0 || d < bestD) { best = k; bestD = d; }
    }
    return best;
}
static __declspec(naked) void ListenerChoiceHook() {
    __asm {
        mov dword ptr ds:[0x82BC14], 1
        pushad
        push ecx
        call NearestListener
        add esp, 4
        mov dword ptr [esp + 0x1C], eax  // popad restores it into eax
        popad
        mov dword ptr [ecx + 0xD0], eax
        push AT_LISTENER_CHOICE_DONE
        ret
    }
}

// ---------------------------------------------------------------- particles near any player
// The particle emitter update 0x491E50 only spawns when the emitter (+0x70 pos) is within its range (effect +0x80)
// of one of the 3 camera positions the particle system snapshots each frame (0x48BD90: views 1, 2, 0 at system
// +0x10/+0x20/+0x30). P3/P4's cameras (views 5/6) were missing -> their tyre smoke and nitrous flames stopped
// when far from P1/P2. The range test now also checks the extra players' cameras.
#define AT_EMITTER_RANGE      0x491E8F   // mov eax,[esi+0x80] (6 bytes); the 3-camera loop follows
#define AT_EMITTER_RANGE_DONE 0x491EE4   // edi = in range
static int __cdecl EmitterNearCamera(const uint8_t* system, const float* pos, float range) {
    if (!(range > 0)) return 0;
    auto inRange = [&](const float* c) {
        float dx = pos[0] - c[0], dy = pos[1] - c[1], dz = pos[2] - c[2];
        return dx * dx + dy * dy + dz * dz < range * range;
    };
    for (int i = 0; i < 3; ++i)
        if (inRange(reinterpret_cast<const float*>(system + 0x10 + i * 0x10))) return 1;
    if (SplitRacing())
        for (int k = 2; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
            const View& v = GetView(kPlayerViewId[k]);
            if (v.active && v.camera && inRange(reinterpret_cast<const float*>(v.camera + 0x40))) return 1;
        }
    return 0;
}
// "Distance to the nearest racer" 0x5F1160 (race manager [0x890080], racer positions at +0x80 + i*0x10, count +0x14)
// measures racers 0/1 only unless some mode flags (0x89CF69, 0x8669F4, 0x866A14) are set. Callers use it for
// "is this near a player": car effects (0x60A1AB: no tyre smoke / nitrous flames beyond 150 m), the per-car
// update 0x60A0EB, sound 0x46A262, ... -> P3/P4 counted as far from everyone. With extra players: all racers.
#define FN_NEAREST_RACER_DIST 0x5F1160   // float __thiscall (manager; const float* pos)
typedef float(__thiscall* NearestRacerDistFn)(void*, const float*);
static NearestRacerDistFn g_origNearestRacerDist;
static float __fastcall NearestRacerDistance(uint8_t* mgr, void*, const float* pos) {
    if (!SplitRacing() || g_extraPlayers <= 0) return g_origNearestRacerDist(mgr, pos);
    int n = *reinterpret_cast<int*>(mgr + 0x14);
    if (n > 6) n = 6;
    float best = 3.0e38f;
    for (int i = 0; i < n || i < 2; ++i) {
        const float* p = reinterpret_cast<const float*>(mgr + 0x80 + i * 0x10);
        float dx = pos[0] - p[0], dy = pos[1] - p[1], dz = pos[2] - p[2], d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d < best) best = d;
    }
    return best;
}

static __declspec(naked) void EmitterRangeHook() {
    __asm {
        mov eax, dword ptr [esi + 0x80]
        mov dword ptr [esp + 0x20], eax
        pushad
        push eax                         // range
        lea edx, [ebx + 0x70]
        push edx                         // emitter position
        push ecx                         // particle system
        call EmitterNearCamera
        add esp, 12
        mov dword ptr [esp], eax         // popad restores it into edi
        popad
        push AT_EMITTER_RANGE_DONE
        ret
    }
}

// ---------------------------------------------------------------- extra local players (3, 4)
// The race runs as a 2-player race (all the game's "== 2 players" logic stays on its 2P paths); players 3 and 4 are
// added next to it: human racers in race setup, player objects at race start, views 5/6 with their own cameras.
#define FN_CREATE_HUMAN_RACERS 0x525ED0  // void __cdecl (count, p2, p3): human racer specs for players 0..count-1
#define FN_RACER_SPEC_CTOR 0x6373E0      // __thiscall (spec; playerIndex, human, carRecord, profile, -2, 0)
#define AT_RACER_SPEC_CALL 0x52602A      // the ctor call inside the per-player loop
#define FN_CREATE_PLAYERS  0x5F4250      // race start: local player objects for the human racers
#define AT_CREATE_PLAYERS_CALL 0x60A798
#define FN_FIND_RACER      0x5ED920      // racer* __thiscall (raceParams; playerIndex)
#define RACE_PARAMS        0x89E7A0
#define FN_PLAYER_CTOR     0x5F1670      // __thiscall (player(0x9C0); port, index)
#define FN_PLAYER_ATTACH   0x5F3E10      // __thiscall (player; car)
#define RACE_CARS          0x890080      // list of race cars; car+0xA (short) = player index of its racer
#define FN_LIST_END        0x502FD0
#define FN_CAMERA_CTOR     0x44F5A0      // __thiscall (camera), static cameras are 0x320 apart
#define AT_HUMAN_MANAGER_CAP 0x4856C8    // imm8 of `push 2`: capacity of the per-human-racer component manager
static const uintptr_t kCreateHumanRacersCalls[] = {0x53F2B5, 0x53F800, 0x53FA03, 0x52694E, 0x53F4E3, 0x53FB8B,
                                                    0x526814, 0x53F656};
static bool g_extraRacerPass = false;
static void* g_extraCamera[MAX_LOCAL_PLAYERS];

static void PrepareExtraNames();
#define AI_COUNT           0x89E7C4      // AI racers; the race starters set it to 0 with 2 local players and create
                                         // the AI (0x53EE90(count)) right after the human racers
static void __cdecl CreateHumanRacers(int count, int p2, int p3) {
    using Fn = void(__cdecl*)(int, int, int);
    if (count == 2 && SplitMode()) {
        at<int>(AI_COUNT) = SplitOpponents();
        if (SplitOpponents()) Log("race: %d AI opponents", SplitOpponents());
    }
    reinterpret_cast<Fn>(FN_CREATE_HUMAN_RACERS)(count, p2, p3);
    int extra = g_extraPlayers < 2 ? g_extraPlayers : 2;
    if (count != 2 || extra <= 0) return;
    // second pass over players 0..extra-1 with the spec's player index shifted by 2 (RacerSpecIndexShift):
    // same profile, car and port as players 1/2
    int ownCar[2] = {at<int>(CAR_IDS), at<int>(CAR_IDS + 4)};
    for (int k = 0; k < extra; ++k)
        if (g_seatCar[2 + k]) at<int>(CAR_IDS + k * 4) = g_seatCar[2 + k];   // picked on the car select
    Log("extra racers: cars %08X %08X (players 1/2: %08X %08X)", at<int>(CAR_IDS), at<int>(CAR_IDS + 4), ownCar[0],
        ownCar[1]);
    Log("race: type %d (0x83AAB4), track %d (0x89E7A0), qr type %d (0x850088), racers %d", at<int>(0x83AAB4),
        at<int>(RACE_PARAMS), at<int>(0x850088), at<int>(RACE_PARAMS + 0x74));
    PrepareExtraNames();
    g_extraRacerPass = true;
    reinterpret_cast<Fn>(FN_CREATE_HUMAN_RACERS)(extra, p2, p3);
    g_extraRacerPass = false;
    at<int>(CAR_IDS) = ownCar[0];
    at<int>(CAR_IDS + 4) = ownCar[1];
    Log("extra human racers added: %d (racers now %d)", extra, at<int>(RACE_PARAMS + 0x74));
}

// Race standings (0x56D850, rebuilt during the race; drift scoring at 0x56DDD0 looks every racer up in it and
// crashes on a missing one) are built for [0x89E7C0] + [0x89E7C8] racers: the extra human racers are counted in for
// the call.
#define FN_BUILD_STANDINGS 0x56D850      // void __cdecl (int)
#define RACE_COUNT_B       0x89E7C8      // racers counted besides the local players
static const uintptr_t kBuildStandingsCalls[] = {0x5F7B3F, 0x5F94C7, 0x6052F8};
static void __cdecl BuildStandings(int arg) {
    int counted = at<int>(NUM_PLAYERS) + at<int>(RACE_COUNT_B);
    int missing = at<int>(RACE_PARAMS + 0x74) - counted;
    if (missing < 0 || !SplitMode() || g_extraPlayers <= 0) missing = 0;
    // only the extra human players: the street drift (touge) tracks 4174-4179 add non-human racers (ids 50+) to the
    // race count, and counting them overran the standings' static entry array 0x864640 (broken list -> crash in the
    // drift scoring sort 0x56DDD0 / later on the way back to the menus)
    if (missing > g_extraPlayers) missing = g_extraPlayers;
    at<int>(RACE_COUNT_B) += missing;
    reinterpret_cast<void(__cdecl*)(int)>(FN_BUILD_STANDINGS)(arg);
    at<int>(RACE_COUNT_B) -= missing;
}

// Race results (0x54CCA0 at 0x551447, thiscall (results; race)): one entry per racer, but the pointer list the results
// screens read (results +0xCD8, 6 slots; e.g. 0x430960 loops the racer count) is filled for [0x89E7C0..CC] racers
// only -> P3/P4's slots stayed null and the results screen crashed (0x430987). Extra racers counted in for the call.
#define AT_RACE_RESULTS_CALL 0x551447
#define FN_RACE_RESULTS      0x54CCA0
// The post-race screens (results rows 0x4BF0A0 and others, stats, back to results) size their lists with the same sum
// -> the extra racers stay counted from the results until the race is left: split race over, a restart (CreatePlayers),
// or right before the race setup paths that read the counters (0x5260B0 at 0x5324F2, 0x526970 at 0x530A91 / 0x581902; the quick race
// menus rewrite them anyway, 0x526720).
static int g_resultsExtra = 0;
static void UncountResultsRacers() {
    if (!g_resultsExtra) return;
    at<int>(RACE_COUNT_B) -= g_resultsExtra;
    Log("results: extra racers uncounted (%d)", g_resultsExtra);
    g_resultsExtra = 0;
}
// The results list holds one entry per race car, but its size comes from the counters. A split race once ended with
// an AI racer that never got a car (3 AI specs, 5 cars; cause unknown, not reproduced) and the results rows read past
// the list (crash 0x4BF2D2). The AI counter is capped at the AI cars that exist (race cars: count [[0x890118]+0x24],
// car i at [[0x890080]+0x1C+4i], spec car+0x14, spec+4 == 2 for AI).
static void CapAiCountToCars() {
    auto* list = at<uint8_t*>(0x890118);
    auto* cars = at<uint8_t*>(0x890080);
    if (!list || !cars) return;
    int n = *reinterpret_cast<int*>(list + 0x24), ai = 0;
    for (int i = 0; i < n; ++i) {
        auto* car = reinterpret_cast<uint8_t**>(cars + 0x1C)[i];
        auto* spec = car ? *reinterpret_cast<uint8_t**>(car + 0x14) : nullptr;
        if (spec && *reinterpret_cast<int*>(spec + 4) == 2) ++ai;
    }
    if (at<int>(AI_COUNT) > ai) {
        Log("results: %d AI counted but %d AI cars (of %d cars) - counter capped", at<int>(AI_COUNT), ai, n);
        at<int>(AI_COUNT) = ai;
    }
}
static void __fastcall RaceResults(void* results, void*, void* race) {
    UncountResultsRacers();
    if (SplitMode()) CapAiCountToCars();
    int counted = at<int>(0x89E7C0) + at<int>(0x89E7C4) + at<int>(0x89E7C8) + at<int>(0x89E7CC);
    int missing = at<int>(RACE_PARAMS + 0x74) - counted;
    if (missing < 0 || !SplitMode() || g_extraPlayers <= 0) missing = 0;    // only P3/P4 are missing from the counts
    if (missing > g_extraPlayers) missing = g_extraPlayers;                 // (touge drift: non-human racers too)
    Log("results: racers %d, counted %d (%d %d %d %d), adding %d", at<int>(RACE_PARAMS + 0x74), counted,
        at<int>(0x89E7C0), at<int>(0x89E7C4), at<int>(0x89E7C8), at<int>(0x89E7CC), missing);
    at<int>(RACE_COUNT_B) += missing;
    g_resultsExtra = missing;
    reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_RACE_RESULTS)(results, race);
}
static void EndResultsCount() {
    if (g_resultsExtra && !SplitMode()) UncountResultsRacers();   // not SplitRacing: Street X results use layout 1
}

// Post-race scene (NIS): its car tracks are bound to the race cars by car array index (table 0x827B9C, 0x14 a track,
// +0xC car index; 0x4309D0 sets mover mode 5 = NISMvr) and the cars it doesn't feature are held at the world origin.
// With 3-4 players + AI, array entries 2/3 (P3, the first AI) are such cars: harmless for AI, but a human car gets
// the fall-through check (0x5ECBA0: below the ground -> +0x96C) and is reset onto the road every ~0.5 s (0x6009A0 via
// 0x601324), each reset switching its mover 5 -> 0 -> 5 = a screen flash and an audio duck (user: rhythmic sound gate
// with flashing on the post-race menu). A car driven by the NIS is not reset.
#define AT_FALL_RESET_CALL 0x601324      // call 0x6009A0 (thiscall [car+0x18]); esi = car, mover mode at car+0x28
#define FN_FALL_RESET      0x6009A0
#define MOVER_NIS          5
static __declspec(naked) void FallResetHook() {
    __asm {
        cmp dword ptr [esi + 0x28], MOVER_NIS
        je skip
        mov eax, FN_FALL_RESET
        jmp eax
    skip:
        ret
    }
}
static const uintptr_t kRaceSetupCalls[][2] = {{0x5324F2, 0x5260B0}, {0x530A91, 0x526970}, {0x581902, 0x526970}};
static int __cdecl RaceSetup5260B0(int a) { UncountResultsRacers(); return reinterpret_cast<int(__cdecl*)(int)>(0x5260B0)(a); }
static int __cdecl RaceSetup526970(int a) { UncountResultsRacers(); return reinterpret_cast<int(__cdecl*)(int)>(0x526970)(a); }

// Racer names come from the per-player block (0x83A9E0 + index*0xAB4C, name at +0): players 3/4 reused P1's / P2's
// (the same name twice on the results). They get P2's name with its digit swapped ("Player 2" -> "Player 3").
#define PLAYER_BLOCKS      0x83A9E0
#define PLAYER_BLOCK_SIZE  0xAB4C
static void PrepareExtraNames() {
    const char* p2 = reinterpret_cast<const char*>(PLAYER_BLOCKS + PLAYER_BLOCK_SIZE);
    for (int k = 0; k < 2; ++k) {
        char* n = g_extraNames[k];
        strncpy_s(n, 16, p2, _TRUNCATE);
        size_t len = strlen(n);
        if (len && n[len - 1] == '2') n[len - 1] = static_cast<char>('3' + k);
        else sprintf_s(n, 16, "Player %d", 3 + k);
    }
}
// Car skins: a car's paint and vinyls are composited into the texture DUMMY_SKIN<n> named by its ride info (+0x584,
// set by 0x61C280(ride; n)); the garage entries pick n from the player index (0x503A80: index + 1, 0x503880: 1 or 2),
// so players 3/4 got P1's / P2's skin texture and rendered with mismatched skin textures (user: P3's
// stock 3000GT dark with stray textures, e.g. a door card texture on the hood). Their ride infos get DUMMY_SKIN3/4 (the single-player opponents' slots; split races have no
// AI) before the racer spec copies them. Race type 9 sets skin 1 for every human (0x526007) and is left as is.
#define FN_RIDE_SET_SKIN   0x61C280      // void __thiscall (RideInfo*; int n)
static void __cdecl ExtraRacerSkin(void* ride, int playerIndex) {
    if (at<int>(0x83AAB4) == 9) return;
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_RIDE_SET_SKIN)(ride, playerIndex + 1);
}
// AI cars are picked to match a reference car (0x4FEAF0: the first car type in a shuffled list whose stock performance
// 0x5B6990(ride + 0x10; 1, -1.0) is within 3% (doubling) of the reference's): racer 0's, player 1's (0x53EE90 at
// 0x53EEE9: 0x5ED900(race; 0) + 0x10). In split races it is the fastest human car.
#define AT_AI_REFERENCE_CALL 0x53EEE9    // call 0x5ED900 (thiscall race; racer index) -> spec (+0x10 ride)
#define FN_RACER_SPEC        0x5ED900
#define FN_CAR_PERFORMANCE   0x5B6990    // float __thiscall (ride + 0x10; 1, -1.0)
static uint8_t* __fastcall AiReferenceRacer(void* race, void*, int index) {
    using SpecFn = uint8_t*(__thiscall*)(void*, int);
    uint8_t* best = reinterpret_cast<SpecFn>(FN_RACER_SPEC)(race, index);
    if (index != 0 || !SplitMode() || at<int>(NUM_PLAYERS) != 2) return best;
    float bestRating = -1e9f;
    for (int k = 0; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
        uint8_t* spec = reinterpret_cast<SpecFn>(FN_RACER_SPEC)(race, k);
        if (!spec || *reinterpret_cast<int*>(spec + 4) != 1) continue;          // humans only
        float rating = reinterpret_cast<float(__thiscall*)(void*, int, float)>(FN_CAR_PERFORMANCE)(spec + 0x20, 1, -1.0f);
        Log("AI reference: racer %d car rating %.3f", k, rating);
        if (rating > bestRating) bestRating = rating, best = spec;
    }
    return best;
}

// AI racers' cars (0x4FEAF0(out, 100 + i, 2 + i, ...), from 0x53EE90) get DUMMY_SKIN<2 + i> (call at 0x4FEB51): after
// the one human of single player. In split races that is player 2's (and 3/4's) skin: they start after the humans.
#define AT_AI_SET_SKIN     0x4FEB51
static void __fastcall AiSetSkin(void* ride, void*, int n) {
    if (SplitMode() && at<int>(NUM_PLAYERS) == 2) {
        n += 2 + g_extraPlayers - 1;     // 2 + i -> humans + 1 + i
        Log("AI car: DUMMY_SKIN%d", n);
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_RIDE_SET_SKIN)(ride, n);
}
static __declspec(naked) void RacerSpecIndexShift() {
    __asm {
        cmp g_extraRacerPass, 0
        je ctor
        add dword ptr [esp + 4], 2       // playerIndex (first stack argument)
        mov eax, dword ptr [esp + 4]
        and eax, 1
        shl eax, 4
        add eax, offset g_extraNames
        mov dword ptr [esp + 0x10], eax  // name (4th argument)
        push ecx
        push dword ptr [esp + 8]         // playerIndex
        push dword ptr [esp + 0x14]      // ride info (3rd argument)
        call ExtraRacerSkin
        add esp, 8
        pop ecx
    ctor:
        mov eax, FN_RACER_SPEC_CTOR
        jmp eax
    }
}

// ---- HUD packages for players 3/4
// HUD elements bind to objects of a loaded FE package by package name (0x537A60(name, hash)); the game has one
// package per 2P HUD (2PHudTop/Bot[_DRAG/_DRIFT/_OUTRUN]), all kept in the package cache (0x8637B0, entries:
// +4/+8 list node, +0xC FNG data, +0x10 resource header (+8 = name hash), +0x18 active package).
#define FE_PACKAGE_CACHE   0x8637B0
#define FN_FIND_PACKAGE    0x51BB70      // entry* __thiscall (cache; name)
#define FN_FE_HASH         0x505450      // uint __cdecl (const char*)
#define FN_FE_CACHE_ENTRY_CTOR 0x51B8E0  // entry* __thiscall (resource chunk); the loader (0x51BCA0) allocates 0x54
#define FE_CACHE_ENTRY_SIZE 0x54
#define FE_CHUNK_COMPRESSED 0x30210      // chunk id: JDLZ-compressed package
#define FE_CHUNK_STORED    0x30203       // uncompressed package (the 1P HUDs)
#define FN_JDLZ_DECOMPRESS 0x574670      // void __cdecl (const JDLZ block*, void* out)
#define AT_PLAYER_HUD_CALL 0x5F1A0C      // player ctor: `call 0x4F8BF0` (HUD ctor, thiscall (fng, index))
#define FN_HUD_CTOR        0x4F8BF0
static int g_hudCloneFor = -1;           // player index whose HUD is being built (CreatePlayers)
static char g_hudCloneName[MAX_LOCAL_PLAYERS][64];

// Players 3/4 get a private copy of P1's / P2's HUD package (and every player one of the 1P HUD, CreateGauges),
// registered in the cache under a name of the same length ("2PHudTop.fng" -> "2PHudTo3.fng"): the package is
// decompressed (JDLZ) into a stored chunk, its own name (FNG +0x28, then its path; lookups find the package by it)
// gets the new last character, and the chunk gets an entry from the game's own constructor.
static char g_hudCloneSource[MAX_LOCAL_PLAYERS][64];

static bool CloneFePackage(const char* src, const char* dst) {
    using FindFn = uint8_t*(__thiscall*)(void*, const char*);
    using MallocFn = void*(__cdecl*)(size_t);
    using HashFn = uint32_t(__cdecl*)(const char*);
    auto* cache = reinterpret_cast<uint8_t*>(FE_PACKAGE_CACHE);
    if (reinterpret_cast<FindFn>(FN_FIND_PACKAGE)(cache, dst)) return true;   // already cloned
    uint8_t* e = reinterpret_cast<FindFn>(FN_FIND_PACKAGE)(cache, src);
    if (!e) return false;
    auto* hdr = *reinterpret_cast<uint8_t**>(e + 0x10);
    uint32_t type = hdr ? *reinterpret_cast<uint32_t*>(hdr) : 0;
    if (type != FE_CHUNK_COMPRESSED && type != FE_CHUNK_STORED) return false;
    auto malloc_ = reinterpret_cast<MallocFn>(FN_MALLOC);
    // the copy is always a stored (uncompressed) chunk: patching the name inside the JDLZ stream also changed every
    // later string copied from it by back-references (the drag package's "Drag_*.tga" textures became "Dra1_*.tga")
    uint8_t* nh;
    size_t size;
    if (type == FE_CHUNK_COMPRESSED) {
        uint8_t* jdlz = hdr + 0xC;
        size = *reinterpret_cast<uint32_t*>(jdlz + 8);           // uncompressed size
        nh = static_cast<uint8_t*>(malloc_(size + 8));
        if (!nh) return false;
        reinterpret_cast<void(__cdecl*)(const void*, void*)>(FN_JDLZ_DECOMPRESS)(jdlz, nh + 8);
    } else {
        size = *reinterpret_cast<uint32_t*>(hdr + 4);
        nh = static_cast<uint8_t*>(malloc_(size + 8));
        if (!nh) return false;
        memcpy(nh + 8, hdr + 8, size);
    }
    *reinterpret_cast<uint32_t*>(nh) = FE_CHUNK_STORED;
    *reinterpret_cast<uint32_t*>(nh + 4) = static_cast<uint32_t>(size);
    size_t hdrSize = size + 8;
    char base[64];
    strncpy_s(base, src, _TRUNCATE);
    if (char* dot = strrchr(base, '.')) *dot = 0;
    char ch = dst[strlen(base) - 1];
    {
        // the FNG's name at +0x28 (the cache finds the package by it) and its path repeat the name
        size_t len = strlen(base);
        int patched = 0;
        for (size_t i = 8; i + len < hdrSize && i < 0x200; ++i)
            if (_strnicmp(reinterpret_cast<char*>(nh + i), base, len) == 0) nh[i + len - 1] = static_cast<uint8_t>(ch), ++patched;
        if (!patched) { Log("FE package %s: name not found", src); return false; }
    }
    // the game's own entry (0x54 bytes, ctor 0x51B8E0: a stored chunk's FNG data is the chunk itself)
    auto* ne = static_cast<uint8_t*>(malloc_(FE_CACHE_ENTRY_SIZE));
    if (!ne) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*)>(FN_FE_CACHE_ENTRY_CTOR)(ne, nh);
    auto** head = reinterpret_cast<uint8_t**>(cache + 4);         // list nodes at entry+4: next, prev
    auto* node = ne + 4;
    uint8_t* first = head[0];
    *reinterpret_cast<uint8_t**>(node) = first;
    *reinterpret_cast<uint8_t**>(node + 4) = cache + 4;
    *reinterpret_cast<uint8_t**>(first + 4) = node;
    head[0] = node;
    Log("FE package %s cloned as %s (%u bytes)", src, dst, static_cast<unsigned>(size));
    return true;
}

static void ForwardToGauges(uint32_t msg, const char* fng);
// Messages the game sends to a HUD package by name (show/hide/animations) also go to its clones.
using SendMsgFn = void(__cdecl*)(uint32_t, const char*);
static SendMsgFn g_origSendMsg;
static void __cdecl SendFeMessage(uint32_t msg, const char* fng) {
    g_origSendMsg(msg, fng);
    if (!fng) return;
    for (int k = 1; k < MAX_LOCAL_PLAYERS; ++k)
        if (g_hudCloneName[k][0] && _stricmp(fng, g_hudCloneSource[k]) == 0) g_origSendMsg(msg, g_hudCloneName[k]);
    ForwardToGauges(msg, fng);
}

static void CreateGauges(int k, void* hud);
static void* __fastcall PlayerHudCtor(void* hud, void* edx, const char* fng, int index) {
    using CtorFn = void*(__fastcall*)(void*, void*, const char*, int);
    int who = g_hudCloneFor >= 2 ? g_hudCloneFor : index;
    // quadrants (3-4 players): every player gets the top-half layout, so P2/P4 build theirs from a copy of P1's
    // "2PHudTop..." instead of the bottom-half "2PHudBot..." (same variant); P3 from a copy of P1's as before
    bool quad = g_extraPlayers > 0 && (who == 1 || who == 3);
    if (((who >= 2 && who == g_hudCloneFor) || quad) && who < MAX_LOCAL_PLAYERS && fng) {
        char src[64];
        strncpy_s(src, fng, _TRUNCATE);
        if (quad)
            if (char* bot = strstr(src, "Bot")) memcpy(bot, "Top", 3);
        char base[48];
        strncpy_s(base, src, _TRUNCATE);
        if (char* dot = strrchr(base, '.')) *dot = 0;
        size_t len = strlen(base);
        if (len >= 2) {
            base[len - 1] = static_cast<char>('0' + who + 1);   // same length: "2PHudTop" -> "2PHudTo3"
            char name[160];
            sprintf_s(name, "%s.fng", base);
            if (CloneFePackage(src, name)) {
                strncpy_s(g_hudCloneSource[who], fng, _TRUNCATE);   // messages for the original go to the copy
                strncpy_s(g_hudCloneName[who], name, _TRUNCATE);
                fng = g_hudCloneName[who];
            }
        }
        if (who >= 2) index = who;       // elements read the player's own data (p[index]); no 2nd minimap
    }
    void* made = reinterpret_cast<CtorFn>(FN_HUD_CTOR)(hud, edx, fng, index);
    CreateGauges(who >= 2 ? who : index, made);
    return made;
}

// Racer of a local player: 0x5ED960 reads the race's human slots (+0x68, 3 ints, then the racer count); players
// 3/4 have no slot, their racers carry the player index directly (0x5ED920 matches spec byte +1).
#define FN_RACER_OF_PLAYER 0x5ED960
static __declspec(naked) void RacerOfPlayer() {
    __asm {
        mov eax, dword ptr [esp + 4]     // player index
        cmp eax, 2
        jge direct
        mov eax, dword ptr [ecx + eax * 4 + 0x68]
        cmp eax, -1
        jle none
        mov dword ptr [esp + 4], eax
    direct:
        mov eax, FN_FIND_RACER
        jmp eax
    none:
        xor eax, eax
        ret 4
    }
}

// ---- Single-player gauges in split screen (user: the car-specific tach / boost / nitrous gauges and the 1P drag
// tach instead of the simplified 2P ones). The HUD ctor 0x4F8BF0 builds the same element classes for every package,
// binding objects by name, so the look comes from the package and the classes' NUM_PLAYERS == 1 branches. Each player
// additionally gets a Tachometer (or DragRaceTachometer), TurboMeter and NitrousGauge bound to a private copy of the 1P
// package (HUD_SingleRace / HUD_Drag / HUD_Drift -> "HUD_SingleRac1.fng"...), built, state-switched and updated with
// NUM_PLAYERS = 1. They read their player's car (index -> racer -> CUSTOM_HUD part 0xA3, "HUDINDEX"), so customized
// gauges work per player; each player's custom texture pack is loaded with P1's (LoadHudTextures). The 2P package's
// gauge objects and the copies' other objects are not drawn; the gauges are placed per view (GaugeTransform).
#define FN_FE_PUSH_PACKAGE 0x555F10      // void __cdecl (fng, 0, 0, 0)
#define FN_FE_POP_PACKAGE  0x5379A0      // void __cdecl (fng): what the HUD dtor does with its package
#define FN_GAME_FREE       0x575630
#define FN_FE_HIDE_OBJECT  0x50CA00      // void __cdecl (obj)
#define FN_ELEMENT_LIST_FREE 0x4835C0    // element dtor: frees its object list (head at +0, node +8 = object)
#define FN_ELEMENT_SET_STATE 0x4C6690    // thiscall (state lo, hi): shows/hides its objects by its masks
#define AT_HUD_UPDATE_CALL 0x6005DB      // player update: `call 0x4EC5A0` (HUD update, thiscall (player, arg))
#define FN_HUD_UPDATE      0x4EC5A0
#define AT_HUD_DTOR_CALL   0x4FD963      // HUD deleting dtor: `call 0x4FD720`
#define FN_HUD_DTOR_BODY   0x4FD720
#define AT_HUD_TEX_LOAD_CALL 0x57F3D8    // race load step: `call 0x577F60` (P1's custom HUD textures, thiscall (cb, ctx))
#define FN_HUD_TEX_LOAD    0x577F60
#define AT_HUD_TEX_UNLOAD_CALL 0x5816D4  // `call 0x580FC0` (thiscall: releases them)
#define FN_HUD_TEX_UNLOAD  0x580FC0
struct GaugeKind { uint32_t hudOffset; uintptr_t ctor; size_t size; uintptr_t update; };
static const GaugeKind kGauges[] = {
    {0x18, 0x4F5100, 0xC8, 0x4C68D0},    // Tachometer (not drag)
    {0x34, 0x4F6500, 0xA8, 0x4C7090},    // DragRaceTachometer (drag)
    {0x1C, 0x4F5A70, 0x38, 0x4C6B80},    // TurboMeter
    {0x28, 0x4F6A90, 0x38, 0x4C7B20},    // NitrousGauge
};
struct PlayerGauges { void* hud; void* elem[4]; char fng[32]; uint32_t state[2]; bool stateSet; bool dead; };
static PlayerGauges g_gauges[MAX_LOCAL_PLAYERS];
static bool g_spGauges = true;
static int g_gaugeMapFrames = 0;

static bool IsDragRace() { return at<uint8_t>(0x89E7D8) || at<int>(0x890108); }
static bool IsDriftRace() { return at<uint8_t>(0x89E7D9) || at<int>(0x890104); }

struct NumPlayersOne {
    int saved;
    NumPlayersOne() : saved(at<int>(NUM_PLAYERS)) { at<int>(NUM_PLAYERS) = 1; }
    ~NumPlayersOne() { at<int>(NUM_PLAYERS) = saved; }
};

static const uint8_t* ActivePackage(const char* name) {
    auto* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, const char*)>(FN_FIND_PACKAGE)(
        reinterpret_cast<void*>(FE_PACKAGE_CACHE), name);
    return e ? *reinterpret_cast<uint8_t**>(e + 0x18) : nullptr;
}

template <class F> static void ForElementObjects(const void* elem, F f) {
    if (!elem) return;
    auto* head = static_cast<const uint8_t*>(elem);
    int guard = 0;
    for (auto* node = *reinterpret_cast<uint8_t* const*>(head); node && node != head && ++guard < 512;
         node = *reinterpret_cast<uint8_t**>(node))
        if (auto* obj = *reinterpret_cast<uint8_t**>(node + 8)) f(obj);
}

static void AddTree(std::unordered_map<const void*, int>& m, const uint8_t* obj, int k, int depth = 0) {
    if (!obj || depth > 8) return;
    m[obj] = k;
    if (const void* data = *reinterpret_cast<const void* const*>(obj + 0x2C)) m[data] = k;
    if (*reinterpret_cast<const int*>(obj + 0x18) == 5)
        for (auto* c = *reinterpret_cast<const uint8_t* const*>(obj + 0x64); c; c = *reinterpret_cast<const uint8_t* const*>(c + 4))
            AddTree(m, c, k, depth + 1);
}

// objects of a 1P copy: groups holding a gauge object are drawn (placed as the player's), everything else is hidden
static bool HasOwned(const uint8_t* obj, int depth = 0) {
    if (!obj || depth > 8) return false;
    if (g_gaugeShow.count(obj)) return true;
    if (*reinterpret_cast<const int*>(obj + 0x18) == 5)
        for (auto* c = *reinterpret_cast<const uint8_t* const*>(obj + 0x64); c; c = *reinterpret_cast<const uint8_t* const*>(c + 4))
            if (HasOwned(c, depth + 1)) return true;
    return false;
}

static bool HasShown(const uint8_t* obj, int depth = 0) { return HasOwned(obj, depth); }

static void SplitCopyObjects(const uint8_t* obj, int k, int depth = 0) {
    for (; obj && depth <= 8; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4)) {
        if (g_gaugeShow.count(obj)) {
            *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C) |= 0x400000;   // cached transform
            continue;
        }
        if (HasShown(obj, depth)) {
            *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C) |= 0x400000;
            g_gaugeShow[obj] = k;
            if (const void* data = *reinterpret_cast<const void* const*>(obj + 0x2C)) g_gaugeShow[data] = k;
            if (*reinterpret_cast<const int*>(obj + 0x18) == 5) {
                // groups start hidden in the drag package (shown by the game for the 1P HUD); the elements only
                // switch their own objects
                // (only the group itself: 0x50CA50 also shows every child, which re-showed the elements' hidden
                // objects every remap - shift light blinking, N2O gamebreaker overlay stuck on)
                if (HasOwned(obj, depth)) {
                    auto& flags = *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(obj) + 0x1C);
                    if (flags & 1) flags = (flags & ~1u) | 0x400000;
                }
                SplitCopyObjects(*reinterpret_cast<const uint8_t* const*>(obj + 0x64), k, depth + 1);
            }
        } else {
            AddTree(g_gaugeHide, obj, k);   // children are queued for rendering on their own
            if (depth > 0) reinterpret_cast<void(__cdecl*)(const void*)>(FN_FE_HIDE_OBJECT)(obj);   // drawn by its group
        }
    }
}

static void BuildGaugeMaps() {
    g_gaugeShow.clear();
    g_gaugeHide.clear();
    // copies whose HUD is gone stay hidden until the frontend has dropped them (their custom textures get unloaded)
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.dead) continue;
        const uint8_t* pkg = ActivePackage(g.fng);
        if (!pkg) { g.dead = false; continue; }
        for (auto* obj = *reinterpret_cast<const uint8_t* const*>(pkg + 0x4C); obj; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4))
            AddTree(g_gaugeHide, obj, k);
    }
    g_gaugeDrag = IsDragRace();             // (also during the intro: the copies' other objects must stay hidden)
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        for (void* e : g.elem) ForElementObjects(e, [k](const uint8_t* obj) { AddTree(g_gaugeShow, obj, k); });
    }
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        for (const GaugeKind& kind : kGauges)
            ForElementObjects(*reinterpret_cast<void**>(static_cast<uint8_t*>(g.hud) + kind.hudOffset),
                              [k](const uint8_t* obj) { if (!g_gaugeShow.count(obj)) AddTree(g_gaugeHide, obj, k); });
        const uint8_t* pkg = ActivePackage(g.fng);
        if (pkg) SplitCopyObjects(*reinterpret_cast<const uint8_t* const*>(pkg + 0x4C), k);
        if (g_debug && g_gaugeMapFrames == 1) {
            static int s_logs = 0;
            if (s_logs++ < 8) Log("1P gauges %d: package %p (%s)", k + 1, pkg, g.fng);
        }
    }
    static size_t s_last = 0;
    if (g_gaugeShow.size() != s_last) {
        s_last = g_gaugeShow.size();
        Log("1P gauges: %d shown, %d hidden", static_cast<int>(g_gaugeShow.size()), static_cast<int>(g_gaugeHide.size()));
    }
}

static void UpdateGaugeMaps() {
    if (++g_gaugeMapFrames < 30 && g_gaugeMapFrames > 1) return;   // elements add objects at runtime
    g_gaugeMapFrames = 1;
    BuildGaugeMaps();
}

static void DestroyGauges(int k) {
    for (void*& e : g_gauges[k].elem) {
        if (!e) continue;
        reinterpret_cast<void(__thiscall*)(void*)>(FN_ELEMENT_LIST_FREE)(e);
        reinterpret_cast<void(__cdecl*)(void*)>(FN_GAME_FREE)(e);
        e = nullptr;
    }
    g_gauges[k].hud = nullptr;
    g_gauges[k].dead = true;
    reinterpret_cast<void(__cdecl*)(const char*)>(FN_FE_POP_PACKAGE)(g_gauges[k].fng);
    BuildGaugeMaps();
    Log("1P gauges for player %d removed (%s %s)", k + 1, g_gauges[k].fng, ActivePackage(g_gauges[k].fng) ? "still active" : "popped");
}

static void CreateGauges(int k, void* hud) {
    if (!g_spGauges || !SplitMode() || at<int>(NUM_PLAYERS) != 2 || k < 0 || k >= MAX_LOCAL_PLAYERS || !hud) return;
    if (g_gauges[k].hud) DestroyGauges(k);
    bool drag = IsDragRace();
    const char* srcs[2] = {drag ? "HUD_Drag.fng" : IsDriftRace() ? "HUD_Drift.fng" : "HUD_SingleRace.fng",
                           "HUD_SingleRace.fng"};
    PlayerGauges& g = g_gauges[k];
    bool ok = false;
    for (const char* src : srcs) {
        strcpy_s(g.fng, src);
        char* dot = strrchr(g.fng, '.');
        dot[-1] = static_cast<char>('1' + k);
        if ((ok = CloneFePackage(src, g.fng))) break;
        Log("1P gauges: no %s", src);
    }
    if (!ok) return;
    reinterpret_cast<void(__cdecl*)(const char*, int, int, int)>(FN_FE_PUSH_PACKAGE)(g.fng, 0, 0, 0);
    NumPlayersOne one;
    for (int i = 0; i < 4; ++i) {
        g.elem[i] = nullptr;
        if ((i == 0 && drag) || (i == 1 && !drag)) continue;
        void* mem = reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(kGauges[i].size);
        if (mem) g.elem[i] = reinterpret_cast<void*(__thiscall*)(void*, const char*, int)>(kGauges[i].ctor)(mem, g.fng, k);
    }
    g.hud = hud;
    g.dead = false;
    g.stateSet = false;
    g_gaugeMapFrames = 0;
    Log("1P gauges for player %d from %s", k + 1, g.fng);
}

// the HUD's messages (show / intro animations: the 1P drag package starts with its tach faded out) reach the copies
static void ForwardToGauges(uint32_t msg, const char* fng) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        PlayerGauges& g = g_gauges[k];
        if (!g.hud) continue;
        const char* hn = *reinterpret_cast<const char**>(static_cast<uint8_t*>(g.hud) + 0x10);
        bool match = hn && _stricmp(fng, hn) == 0;
        if (!match && k > 0 && g_hudCloneName[k][0] && _stricmp(fng, g_hudCloneSource[k]) == 0) match = true;
        if (match) g_origSendMsg(msg, g.fng);
    }
}

static void __fastcall HudUpdateHook(void* hud, void*, void* player, void* arg) {
    // the 2P package's own gauges (hidden) are not updated: both sets updating per frame for the same player
    // fought over shared state (the N2O gamebreaker overlay stayed on, the shift light blinked)
    void* saved[4] = {};
    bool mine = false;
    for (PlayerGauges& g : g_gauges) mine |= g.hud == hud;
    auto* h = static_cast<uint8_t*>(hud);
    if (mine)
        for (int i = 0; i < 4; ++i) std::swap(saved[i], *reinterpret_cast<void**>(h + kGauges[i].hudOffset));
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(FN_HUD_UPDATE)(hud, player, arg);
    if (mine)
        for (int i = 0; i < 4; ++i) std::swap(saved[i], *reinterpret_cast<void**>(h + kGauges[i].hudOffset));
    for (PlayerGauges& g : g_gauges) {
        if (g.hud != hud) continue;
        uint32_t lo = *reinterpret_cast<uint32_t*>(h + 8), hi = *reinterpret_cast<uint32_t*>(h + 0xC);
        NumPlayersOne one;
        if (!g.stateSet || lo != g.state[0] || hi != g.state[1]) {
            g.state[0] = lo, g.state[1] = hi, g.stateSet = true;
            for (void* e : g.elem)
                if (e) reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(FN_ELEMENT_SET_STATE)(e, lo & 0xFDFBFFFF, hi);
        }
        for (int i = 0; i < 4; ++i)
            if (g.elem[i]) reinterpret_cast<void(__thiscall*)(void*, void*)>(kGauges[i].update)(g.elem[i], player);
    }
}

static void __fastcall HudDtorHook(void* hud) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k)
        if (g_gauges[k].hud == hud) DestroyGauges(k);
    reinterpret_cast<void(__thiscall*)(void*)>(FN_HUD_DTOR_BODY)(hud);
}

// Custom gauge textures: the race load (0x577F60) loads GLOBAL\HUD_CustomTextures_<HUDINDEX>.bin for P1's car only
// and makes its 8 textures resident (names from 0x5ED9B0); the other players' packs are loaded the same way first.
static uint32_t g_hudTexList[MAX_LOCAL_PLAYERS][8];
static int g_hudTexIndex[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1};
static bool g_hudTexFile[MAX_LOCAL_PLAYERS];   // this player's entry loaded the pack file (unloads it)
static void __cdecl HudTexLoaded(void*) {}

static int CustomHudIndex(int k, uint8_t** spec, uint8_t** car) {
    auto* racer = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(FN_RACER_OF_PLAYER)(reinterpret_cast<void*>(0x89E7A0), k);
    if (!racer || *reinterpret_cast<int8_t*>(racer) > 0) return -1;
    uint8_t* s = racer + 0x10;
    auto* c = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(0x5B5350)(s + 0x10, 1);
    if (!c) return -1;
    void* part = reinterpret_cast<void*(__thiscall*)(void*, int)>(0x6102F0)(s, 0xA3);
    int idx = 0;
    if (part) {
        uint32_t key = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(0x43DB50)("HUDINDEX");
        idx = reinterpret_cast<int(__thiscall*)(void*, uint32_t, int)>(0x61B500)(part, key, 0);   // (key, 0)
    }
    *spec = s;
    *car = c + 0x1F0;
    return idx;
}

static void HudTexPath(char* out, size_t n, int idx) { sprintf_s(out, n, "GLOBAL\\HUD_CustomTextures_%02d.bin", idx); }

static void __fastcall HudTexLoadHook(void* self, void*, void* cb, void* ctx) {
    // the 1P drag tach uses the drag set of the custom textures (0x5ED9B0 picks it for drag with NUM_PLAYERS == 1)
    bool asOne = g_spGauges && SplitMode() && at<int>(NUM_PLAYERS) == 2 && IsDragRace();
    int savedPlayers = at<int>(NUM_PLAYERS);
    if (asOne) at<int>(NUM_PLAYERS) = 1;
    if (g_spGauges && SplitMode() && savedPlayers == 2) {
        int loaded[MAX_LOCAL_PLAYERS];
        int n = 0;
        uint8_t *spec, *car;
        loaded[n++] = CustomHudIndex(0, &spec, &car);    // the game's (P1)
        for (int k = 1; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k) {
            int idx = CustomHudIndex(k, &spec, &car);
            Log("custom HUD index: player %d %d (player 1 %d)", k + 1, idx, loaded[0]);
            if (idx < 0) continue;
            // the pack file once per index; the texture list per player: which of the pack's textures a car uses
            // depends on the car too (user: P4's stock Celica tach had no numbers, P3's stock IS300 shared its index)
            bool fileLoaded = false;
            for (int i = 0; i < n; ++i) fileLoaded |= loaded[i] == idx;
            if (!fileLoaded) {
                char path[64];
                HudTexPath(path, sizeof(path), idx);
                reinterpret_cast<int(__cdecl*)(const char*, int, int, int)>(0x4944D0)(path, 0, 0, 0);
                reinterpret_cast<void(__cdecl*)(int)>(0x490170)(0);
            }
            g_hudTexFile[k] = !fileLoaded;
            memset(g_hudTexList[k], 0, sizeof(g_hudTexList[k]));
            reinterpret_cast<void(__cdecl*)(int, void*, int, uint32_t*, int)>(0x5ED9B0)(   // (spec, car parts, units, out, index)
                *reinterpret_cast<int*>(spec), car, at<uint8_t>(0x83AA1A), g_hudTexList[k], idx);
            reinterpret_cast<void(__cdecl*)(uint32_t*, int, void(__cdecl*)(void*), void*, int)>(0x494D60)(
                g_hudTexList[k], 8, &HudTexLoaded, nullptr, 0);
            g_hudTexIndex[k] = idx;
            loaded[n++] = idx;
            Log("custom HUD textures %02d %s for player %d", idx, fileLoaded ? "(pack already loaded)" : "loaded", k + 1);
        }
    }
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(FN_HUD_TEX_LOAD)(self, cb, ctx);
    if (asOne) at<int>(NUM_PLAYERS) = savedPlayers;
}

static void __fastcall HudTexUnloadHook(void* self) {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {     // every list first: a pack file can be shared (also P1's)
        if (g_hudTexIndex[k] < 0) continue;
        reinterpret_cast<void(__cdecl*)(uint32_t*, int)>(0x490150)(g_hudTexList[k], 8);
        memset(g_hudTexList[k], 0, sizeof(g_hudTexList[k]));
    }
    reinterpret_cast<void(__thiscall*)(void*)>(FN_HUD_TEX_UNLOAD)(self);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        if (g_hudTexIndex[k] < 0) continue;
        if (g_hudTexFile[k]) {
            char path[64];
            HudTexPath(path, sizeof(path), g_hudTexIndex[k]);
            reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x494550)(path, 0, 0);
        }
        g_hudTexIndex[k] = -1;
        g_hudTexFile[k] = false;
    }
}

#define AT_FE_RENDER_OBJ_CALL 0x537262   // `call 0x5370A0` (render one FE object, stdcall (obj, arg))
#define FN_FE_RENDER_OBJ   0x5370A0
// 0x5370A0 also takes the render context in ECX (set by the caller), so the wrapper must preserve it.
static void __cdecl SetRenderSlot(void* obj) {
    int slot = -1;
    if (g_quadHud) {
        auto it = g_hudObjSlot.find(obj);
        if (it != g_hudObjSlot.end()) slot = it->second;
    }
    g_renderSlot = slot;
}

// Never inlined: the compiler can't see that this asm calls a function, and inlined it kept SSE values (the minimap
// centre and radius constants) in registers across the game's render call, which clobbers them -> after the first
// marker every NearMinimap check compared garbage (own car markers missing / markers flickering, user reports).
static __declspec(noinline) void __stdcall CallFeRender(void* obj, int arg, void* ctx) {
    __asm {
        mov ecx, ctx
        push arg
        push obj
        mov eax, FN_FE_RENDER_OBJ
        call eax
    }
}

static bool IsMarkerName(const uint8_t* obj) {
    uint32_t h = *reinterpret_cast<const uint32_t*>(obj + 0x10);
    for (uint32_t m : g_mmMarkerHash)
        if (h == m) return true;
    return false;
}

static void __stdcall RenderFeObjectWith(void* obj, int arg, void* ctx) {
    if (g_debug) { static int s_d = -1; int d = X87Depth(); if (d != s_d) { Log("x87 depth %d entering FE object render", d); s_d = d; } }
    // P3/P4's car markers (recoloured and unhidden for the per-player minimaps) are only drawn by the quad HUD; after the
    // race (Street X results switch to layout 1) they showed up at their raw positions (user: blue marker on results)
    if (!g_quadHud && obj && (obj == g_mmExtraIcon[0] || obj == g_mmExtraIcon[1])) return;
    if ((!g_gaugeShow.empty() || !g_gaugeHide.empty()) && obj) {
        static unsigned s_n, s_show, s_hide;
        if (g_debug && ++s_n % 20000 == 0) Log("gauge render: %u objs, %u shown, %u hidden", s_n, s_show, s_hide);
        if (g_gaugeHide.count(obj)) { ++s_hide; return; }
        auto it = g_gaugeShow.find(obj);
        if (it != g_gaugeShow.end()) ++s_show;
        if (it != g_gaugeShow.end()) {
            if (!SplitRacing() || !g_hudWiden) return;      // intro cameras, pause menus: no gauges
            g_gaugeSlot = it->second;
            g_renderSlot = -1;
            CallFeRender(obj, arg, ctx);
            g_gaugeSlot = -1;
            return;
        }
    }
    SetRenderSlot(obj);
    int mm = -1;
    if (g_renderSlot >= 0 && g_mmPlayers > 0)
        for (int i = 0; i < g_mmObjCount; ++i)
            if (g_mmObj[i] == obj) mm = i;
    // minimap markers that are not on a minimap right now (spare ones, the game parks them away from the map) are not
    // drawn: placed as ordinary HUD objects they landed at random spots in the quadrants (user report)
    if (mm < 0 && g_mmTarget < 0 && g_renderSlot >= 0 && g_mmPlayers > 0 && IsMarkerName(static_cast<uint8_t*>(obj))) {
        g_renderSlot = -1;
        return;
    }
    int markers = 2 + (g_extraPlayers < 2 ? g_extraPlayers : 2);
    bool marker = false;
    for (int j = 0; j < markers; ++j) marker |= obj == g_mmMarker[j];
    if (mm >= 0 && marker && g_mmTarget < 0) {
        // the car markers are drawn together, once per frame, when P1's package reaches one of them (P3's package is
        // drawn before P1's map, which would cover its markers); in each minimap its own player's marker goes last
        if (g_mmMarkersFrame != g_mmFrame && (obj == g_mmMarker[0] || obj == g_mmMarker[1])) {
            g_mmMarkersFrame = g_mmFrame;
            for (int k = 0; k < g_mmPlayers; ++k) {
                ApplyMinimapSnapshot(k);
                g_mmTarget = k;
                for (int n = 1; n <= markers; ++n) {
                    uint8_t* m = g_mmMarker[(k + n) % markers];     // k last
                    if (!m) continue;
                    // off this player's map (too far away): the game parks it far from the map centre
                    auto* md = *reinterpret_cast<const float**>(m + 0x2C);
                    static unsigned s_mmLog;
                    if (g_debug && (s_mmLog++ % 600) < markers * g_mmPlayers)
                        Log("minimap %d: marker %d at %.3f,%.3f (%08X %08X) d %.3f %.3f flags %08X %s", k + 1,
                            (k + n) % markers + 1, md ? md[7] : 0.0f, md ? md[8] : 0.0f,
                            md ? reinterpret_cast<const uint32_t*>(md)[7] : 0, md ? reinterpret_cast<const uint32_t*>(md)[8] : 0,
                            md ? fabsf(md[7] - MINIMAP_X) : -1.0f, md ? fabsf(md[8] - MINIMAP_Y) : -1.0f,
                            *reinterpret_cast<uint32_t*>(m + 0x1C), md && NearMinimap(md) ? "drawn" : "skipped");
                    if (!md || !NearMinimap(md)) continue;
                    *reinterpret_cast<uint32_t*>(m + 0x1C) |= 0x400000;
                    g_renderSlot = 0;
                    CallFeRender(m, arg, ctx);
                    if (g_debug) CheckX87("after marker render");
                }
            }
            ApplyMinimapSnapshot(0);
            g_mmTarget = -1;
        }
    } else if (mm < 0 || g_mmTarget >= 0) {     // not a minimap object, or nested in one being drawn per player
        CallFeRender(obj, arg, ctx);
    } else {
        for (int k = 0; k < g_mmPlayers; ++k) {
            ApplyMinimapSnapshot(k);
            for (int i = 0; i < g_mmObjCount; ++i)          // "changed": recompute the cached transform
                *reinterpret_cast<uint32_t*>(g_mmObj[i] + 0x1C) |= 0x400000;
            if (!DrawsOnMinimap(static_cast<uint8_t*>(obj))) continue;
            g_mmTarget = k;
            g_renderSlot = 0;
            CallFeRender(obj, arg, ctx);
        }
        ApplyMinimapSnapshot(0);
        g_mmTarget = -1;
    }
    g_renderSlot = -1;
}

static __declspec(naked) void RenderFeObject() {
    __asm {
        push ecx                         // render context
        push dword ptr [esp + 0xC]       // arg
        push dword ptr [esp + 0xC]       // obj
        call RenderFeObjectWith
        ret 8
    }
}

#define AT_MINIMAP_UPDATE_CALL 0x4DA650  // `call 0x4CA840` (minimap manager update, thiscall, no args)
#define FN_MINIMAP_UPDATE  0x4CA840
#define AT_MINIMAP_CENTRE_ID 0x4CA901    // imm8 of `push 0`: id of the car entity the map centres on
#define MARKER_POOL        0x836560      // list of free marker descriptors {obj, type} (filled by 0x4DB900(fng))
#define MARKER_ALLOCATOR   0x8364B4      // fixed allocator of the descriptors (0x66 x 8 bytes, made at 0x4AC6D0)
#define AT_MARKER_POOL_SIZE 0x4AC6D8     // imm8 of `push 0x66`
#define FN_POOL_ALLOC      0x43F8F0      // void* __cdecl (allocator)
#define FN_LIST_NODE       0x440EC0      // node* (): list node, payload at +8
#define FN_MINIMAP_ADD      0x4EC790     // entity* __thiscall (manager; type, id): takes a pool marker of that type
static void* g_mmExtendedMgr = nullptr;

// A free car marker (type 4) for an FE object, as 0x4DB900 registers them. P1's package nearly fills the
// descriptor allocator, so its size is raised (and a failed allocation adds nothing: a null descriptor in the pool
// crashes its cleanup).
static bool AddCarMarker(uint8_t* obj) {
    auto* desc = reinterpret_cast<uint32_t*(__cdecl*)(void*)>(FN_POOL_ALLOC)(at<void*>(MARKER_ALLOCATOR));
    if (!desc) return false;
    desc[0] = reinterpret_cast<uint32_t>(obj);
    desc[1] = 4;
    auto* node = reinterpret_cast<uint32_t*(__cdecl*)()>(FN_LIST_NODE)();
    if (!node) return false;
    node[2] = reinterpret_cast<uint32_t>(desc);
    auto* tail = at<uint32_t*>(MARKER_POOL + 4);
    tail[0] = reinterpret_cast<uint32_t>(node);
    at<uint32_t*>(MARKER_POOL + 4) = node;
    node[1] = reinterpret_cast<uint32_t>(tail);
    node[0] = MARKER_POOL;
    return true;
}

// Players 3/4 get car entities (type 4, id 2/3) in the minimap manager; their markers are P3's package copy's.
// Redone whenever the manager is rebuilt (0x4F92A0 recreates it with a fresh pool).
static void ExtendMinimapManager(uint8_t* mgr) {
    if (mgr == g_mmExtendedMgr || !g_mmExtraIcon[0]) return;
    g_mmExtendedMgr = mgr;
    for (int idx = 2; idx < 2 + g_extraPlayers && idx < MAX_LOCAL_PLAYERS; ++idx) {
        if (!g_mmExtraIcon[idx - 2] || !AddCarMarker(g_mmExtraIcon[idx - 2])) {
            Log("minimap: no marker for player %d", idx + 1);
            continue;
        }
        auto* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, int, int)>(FN_MINIMAP_ADD)(mgr, 4, idx);
        Log("minimap: player %d car entity %p (marker %p)", idx + 1, e, e ? *reinterpret_cast<void**>(e + 0x1C) : 0);
    }
}

// The manager is destroyed with player 1's HUD (0x4FBFD0 -> 0x4ECB80, call at 0x4FBFF2), the last one to go: the
// destructor parks every entity's marker (0x51D4D0), but players 3/4's markers belong to player 3's HUD package,
// gone by then. Their entities are taken out first (entity and descriptor back to their allocators, 0x43F920).
#define AT_MINIMAP_DESTROY_CALL 0x4FBFF2
#define FN_MINIMAP_DESTROY 0x4ECB80
#define ENTITY_ALLOCATOR   0x8364B8
#define FN_POOL_FREE       0x43F920      // void __cdecl (allocator, item)
static void __fastcall DestroyMinimapManager(uint8_t* mgr) {
    auto free_ = reinterpret_cast<void(__cdecl*)(void*, void*)>(FN_POOL_FREE);
    for (auto* e = *reinterpret_cast<uint8_t**>(mgr); e && e != mgr;) {
        auto* next = *reinterpret_cast<uint8_t**>(e);
        auto* desc = *reinterpret_cast<uint8_t**>(e + 0x1C);
        if (desc && *reinterpret_cast<int*>(desc + 4) == 4 && *reinterpret_cast<int*>(e + 8) >= 2) {
            auto* prev = *reinterpret_cast<uint8_t**>(e + 4);
            *reinterpret_cast<uint8_t**>(prev) = next;
            *reinterpret_cast<uint8_t**>(next + 4) = prev;
            free_(at<void*>(MARKER_ALLOCATOR), desc);
            free_(at<void*>(ENTITY_ALLOCATOR), e);
            Log("minimap: player %d car entity removed", *reinterpret_cast<int*>(e + 8) + 1);
        }
        e = next;
    }
    g_mmExtendedMgr = nullptr;
    reinterpret_cast<void(__fastcall*)(void*)>(FN_MINIMAP_DESTROY)(mgr);
}

static void __fastcall MinimapUpdate(uint8_t* mgr, void*) {
    auto update = reinterpret_cast<void(__fastcall*)(void*, void*)>(FN_MINIMAP_UPDATE);
    int n = g_quadHud && g_mmObjCount > 0 ? g_screenViews : 0;
    g_mmPlayers = 0;
    if (!n) { update(mgr, nullptr); return; }
    ExtendMinimapManager(mgr);
    for (int i = 0; i < 2 && i < g_extraPlayers; ++i)
        if (uint8_t* icon = g_mmExtraIcon[i]) {
            auto* data = *reinterpret_cast<uint32_t**>(icon + 0x2C);
            for (int c = 0; c < 4; ++c) data[c] = kMarkerColour[i][c];
            *reinterpret_cast<uint32_t*>(icon + 0x1C) &= ~1u;            // shown (P3's copy starts hidden)
        }
    for (int k = n - 1; k >= 0; --k) {                                    // ends with the game's own (centre P1)
        uint8_t id = static_cast<uint8_t>(k);
        Write(AT_MINIMAP_CENTRE_ID, &id, 1);
        update(mgr, nullptr);
        TakeMinimapSnapshot(k);
    }
    g_mmPlayers = n;
}

void* GetLocalPlayer(int k);

// HUD element at hud+0x70 (0x4F89D0, 0x48 bytes): its data bindings (small objects, vtables 0x795Bxx, list node at
// +4) are deleted by its dtor through the generic list clear 0x4358E0 (call at 0x4F8BAE). With 4 players one of player
// 4's bindings sometimes loses its link back to the head (last node, next == 0: quit to menu crashed at 0x4358F7).
// Cause not found yet; a binding with next == 0 is taken as the last one, and the clear stops at anything else that
// isn't a linked binding (the rest is leaked, nothing is touched).
#define AT_BINDINGS_CLEAR_CALL 0x4F8BAE
static void __fastcall ClearHudBindings(uint32_t* head) {
    for (int guard = 0; guard < 256; ++guard) {
        auto* node = reinterpret_cast<uint32_t*>(head[0]);
        if (node == head) return;
        auto* next = reinterpret_cast<uint32_t*>(node[0]);
        auto* prev = reinterpret_cast<uint32_t*>(node[1]);
        uint32_t vt = node[-1];
        if (!next && prev && prev[0] == reinterpret_cast<uint32_t>(node) && vt >= 0x795000 && vt < 0x79C000) {
            Log("HUD bindings %p: last node %p lost its link to the head", head, node);
            next = head;
        }
        bool linked = next && prev && (next == head || next[1] == reinterpret_cast<uint32_t>(node)) &&
                      prev[0] == reinterpret_cast<uint32_t>(node);
        if (!linked || vt < 0x795000 || vt >= 0x79C000) {
            Log("HUD bindings %p: broken node %p (next %p, vt %08X), rest leaked", head, node, next, vt);
            head[0] = head[1] = reinterpret_cast<uint32_t>(head);
            return;
        }
        prev[0] = reinterpret_cast<uint32_t>(next);
        next[1] = reinterpret_cast<uint32_t>(prev);
        void* obj = node - 1;
        reinterpret_cast<void(__thiscall*)(void*, int)>(*reinterpret_cast<uintptr_t*>(vt))(obj, 1);
    }
}

// Audio file system ("bf:", EA async big-file layer): 0x486510 creates it with 0x6F6750(bigFiles 1, handles 12, mem)
// (size from 0x6F6628(1, 12)). Every racer's engine/turbo/skid banks keep files open; if the
// 12 handles run out the open path pops an empty free list (crash 0x6F6AF9, seen once on a 2nd 3P race). Measured
// (debug log below): 3P race load peaks at 4 handles, no leak across races -> not exhaustion by design; kept as a
// probe. Handle counts are the imm8s at 0x486531 / 0x486541 (`push 0xc`).
#define AT_SOUND_FS_CREATE_CALL 0x486547
#define FN_SOUND_FS_CREATE 0x6F6750      // void* __cdecl (bigFiles, handles, mem); free handle list at +0x3C (count +8)
static uint8_t* g_soundFs;
static void* __cdecl CreateSoundFs(int bigFiles, int handles, void* mem) {
    g_soundFs = reinterpret_cast<uint8_t*(__cdecl*)(int, int, void*)>(FN_SOUND_FS_CREATE)(bigFiles, handles, mem);
    return g_soundFs;
}
static int SoundFsFreeHandles() { return g_soundFs ? *reinterpret_cast<int*>(g_soundFs + 0x3C + 8) : -1; }
// debug: lowest free count seen after an open (vtable slot 3 = open(name, mode, out) __thiscall, ret 0xC)
#define SOUND_FS_VT_OPEN   (0x7C2E2C + 0xC)
static int g_soundFsMinFree = 1 << 30;
static uintptr_t g_soundFsOpen;
#define SOUND_FS_VT_CLOSE  (0x7C2E2C + 0x10)
static uintptr_t g_soundFsClose;
static char g_soundFsName[64][48];      // debug: name per open handle ("" = not open)
static void LogCallers(const char* what) {
    void* frames[12] = {};
    USHORT n = RtlCaptureStackBackTrace(2, 12, frames, nullptr);
    char buf[256]; int len = 0;
    for (USHORT i = 0; i < n && len < 220; ++i)
        len += sprintf_s(buf + len, sizeof(buf) - len, " %p", frames[i]);
    Log("%s; callers:%s", what, buf);
}
// The handle free list (+0x3C) is popped by open and pushed by close without a lock; if the audio thread closes while
// the main thread opens, the list can break (crash 0x6F6AF9: open got a null node; seen after many races, in the
// menus after a 2P drift race, 2026-10-18; handles never ran low). Open and close are serialized.
static CRITICAL_SECTION g_soundFsLock;
static DWORD g_soundFsThreadsSeen[8];
static void NoteSoundFsThread(const char* what) {   // logs each thread once (opens/closes do come from 2 other threads)
    DWORD tid = GetCurrentThreadId();
    if (tid == g_mainThreadId) return;
    for (DWORD& t : g_soundFsThreadsSeen) {
        if (t == tid) return;
        if (!t) { t = tid; Log("sound fs: %s from thread %lu (main %lu)", what, tid, g_mainThreadId); return; }
    }
}
static void __fastcall SoundFsClose(void* fs, void*, int handle) {
    NoteSoundFsThread("close");
    EnterCriticalSection(&g_soundFsLock);
    if (handle >= 0 && handle < 64) {
        if (!g_soundFsName[handle][0]) {
            char what[96];
            sprintf_s(what, "sound fs: close of handle %d that is not open", handle);
            LogCallers(what);
        }
        g_soundFsName[handle][0] = 0;
    }
    reinterpret_cast<void(__thiscall*)(void*, int)>(g_soundFsClose)(fs, handle);
    LeaveCriticalSection(&g_soundFsLock);
}
static int __fastcall SoundFsOpen(void* fs, void*, const char* name, int mode, int* out) {
    NoteSoundFsThread("open");
    EnterCriticalSection(&g_soundFsLock);
    int r = reinterpret_cast<int(__thiscall*)(void*, const char*, int, int*)>(g_soundFsOpen)(fs, name, mode, out);
    LeaveCriticalSection(&g_soundFsLock);
    if (r >= 0 && r < 64) {
        if (g_soundFsName[r][0]) Log("sound fs: handle %d reopened while open (%s)", r, g_soundFsName[r]);
        strncpy_s(g_soundFsName[r], name ? name : "?", _TRUNCATE);
    }
    int free = SoundFsFreeHandles();
    if (free < g_soundFsMinFree) { g_soundFsMinFree = free; Log("sound fs: %d free handles (opened %s)", free, name); }
    return r;
}

// Game memory pools (0x440390 alloc / 0x440540 free) link each block into the pool's allocated list (pool+0xC)
// OUTSIDE the pool's own lock, but the sound engine's thread also allocates/frees (seen at boot and during bank
// loads). Concurrent calls corrupt the list -> random crashes in the audio code (0x4405CF, 0x6F6AF9, 0x6F80B6), far
// more likely with 3-4 player cars loading sound banks at race start. Serialize both entry points.
#define FN_GAME_POOL_ALLOC 0x440390      // void* __cdecl (pool/flags, name, ?, size)  (8 bytes stolen)
#define FN_GAME_POOL_FREE  0x440540      // void __cdecl (ptr)                         (7 bytes stolen)
static void* g_origPoolAlloc;
static void* g_origPoolFree;
static CRITICAL_SECTION g_poolLock;
static DWORD g_mainThread, g_poolThreadsSeen[8];
static void NotePoolThread(const char* what) {
    DWORD tid = GetCurrentThreadId();
    if (!g_debug || tid == g_mainThread) return;
    for (DWORD& t : g_poolThreadsSeen) {
        if (t == tid) return;
        if (!t) { t = tid; Log("pool: %s from thread %lu (main %lu), serialized", what, tid, g_mainThread); return; }
    }
}
static void* __cdecl PoolAllocLocked(int a, const char* b, int c, int d) {
    EnterCriticalSection(&g_poolLock);
    NotePoolThread("alloc");
    void* r = reinterpret_cast<void*(__cdecl*)(int, const char*, int, int)>(g_origPoolAlloc)(a, b, c, d);
    LeaveCriticalSection(&g_poolLock);
    return r;
}
static void __cdecl PoolFreeLocked(void* ptr) {
    EnterCriticalSection(&g_poolLock);
    NotePoolThread("free");
    reinterpret_cast<void(__cdecl*)(void*)>(g_origPoolFree)(ptr);
    LeaveCriticalSection(&g_poolLock);
}

// Track streaming memory: race load (0x57A5E3) sizes the "Track Streaming" pool from a table by race kind
// (0x79DC54 KB: 60000 normal, 43000 with 2 local players, 36000 special) -> 0x5E4E70(streamer; bytes), and a second
// pool (0x7FA9D4 KB: 11000 / 9000 / 16000) -> 0x610750(0x8A3288; bytes). The 2-player sizes were made for the reduced
// split-screen sections; split races stream the full single-player sections (section remap skipped) for 2-4 far-apart
// areas, so with 43 MB the sections around a player far from the others went missing (user: Palm Highway, 2P; earlier
// 3-4P black world). Sized from the single-player budget: track pool 60 MB x (players + 1) / 2, second pool x players / 2.
#define AT_TRACK_POOL_CALL 0x57A63A
#define FN_TRACK_POOL_CREATE 0x5E4E70    // void __thiscall (streamer; bytes)
#define AT_SECOND_POOL_CALL 0x57A62C
#define FN_SECOND_POOL_CREATE 0x610750   // void __thiscall (0x8A3288; bytes)
#define TRACK_POOL_KB_1P   0x79DC54      // int[3] KB: normal, 2 players, special
#define SECOND_POOL_KB_1P  0x7FA9D4
static bool SplitPools() { return SplitMode() && at<int>(NUM_PLAYERS) == 2; }
static void __fastcall CreateTrackStreamingPool(void* streamer, void*, int bytes) {
    if (SplitPools()) {
        int players = 2 + g_extraPlayers;
        int split = at<int>(TRACK_POOL_KB_1P) * 1024 / 2 * (players + 1);
        if (split > bytes) bytes = split;
    }
    Log("track streaming pool: %d KB", bytes / 1024);
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_TRACK_POOL_CREATE)(streamer, bytes);
}
// The second pool is the car loader's (0x8A3288): car models and textures. When it runs short, the loader evicts the
// textures of car types no racer holds (0x63A1B0 -> 0x639F20; the AI's types are not held) and those cars draw
// untextured (user: a transparent AI car with 2P + 4 AI). AI opponents get 3000 KB each on top (the game's 6-car size
// 16000 KB vs 11000 KB for 4 cars: ~2500 KB a car).
static int SplitOpponents();
static void __fastcall CreateSecondStreamingPool(void* pool, void*, int bytes) {
    if (SplitPools()) {
        int players = 2 + g_extraPlayers;
        int split = at<int>(SECOND_POOL_KB_1P) * 1024 / 2 * players + SplitOpponents() * 3000 * 1024;
        if (split > bytes) bytes = split;
    }
    Log("second streaming pool: %d KB", bytes / 1024);
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_SECOND_POOL_CREATE)(pool, bytes);
}

// Race intro (NIS) selection 0x436840: context 0x431F10 (+0 racers, +4 humans, +8 humans' grid-slot mask, +0xC AI
// mask, +0x14 track byte) filtered against each scene (0x42FE00: slot mask, racer range +6..+7, ...); with exactly 2
// local players the 2P list (0x42FDA0) is used, otherwise the single-player one. Scenes exist for 2 and 4 cars, so
// 3 players got none (and the race started without an intro), nor did 5-6 cars with AI opponents (no intro, no grid
// girls); both are offered the 4-car scenes.
#define AT_INTRO_CONTEXT_CALL 0x4368F4
#define FN_INTRO_CONTEXT   0x431F10      // void __thiscall (ctx; trackId)
// With exactly 2 local players only IntroNis12 / IntroNis13 pass the 2P filter 0x42FDA0 (at 0x43695F); tracks whose
// eligible scenes include neither got no intro, and a 2P race that starts without an intro never switches to the
// world render (black screen with the HUD; user: Freeway West, reproduced on track 4021 with the filter forced off).
// When the 2P list comes out empty (0x4369FE) the scene loop runs again with the filter passing every scene, i.e. the
// scenes single player would pick from.
#define AT_INTRO_2P_FILTER   0x43695F    // call 0x42FDA0 (thiscall ctx; scene) -> bool
#define FN_INTRO_2P_FILTER   0x42FDA0
#define AT_INTRO_EMPTY       0x4369FE    // mov ecx,[ebp+18h]; xor edi,edi  (no scene)
#define AT_INTRO_EMPTY_RET   0x436A03
#define AT_INTRO_LOOP        0x4368F9    // list init + scene loop (edi = 0)
static bool g_intro2PFallback = false;
static bool g_introHideExtra = false;    // 5-6 cars got a 4-car scene: the extra cars are hidden during it
static bool __fastcall Intro2PFilter(void* ctx, void*, void* scene) {
    return g_intro2PFallback || reinterpret_cast<bool(__thiscall*)(void*, void*)>(FN_INTRO_2P_FILTER)(ctx, scene);
}
static int __cdecl IntroRetryWanted() {
    if (g_intro2PFallback || GetLocalPlayer(1) == nullptr || GetLocalPlayer(2) != nullptr) return 0;   // 2 players
    g_intro2PFallback = true;
    Log("intro: no 2-player scene for this track, using the single-player scenes");
    return 1;
}
static __declspec(naked) void IntroEmptyHook() {
    __asm {
        pushad
        call IntroRetryWanted
        test eax, eax
        popad
        jz none
        xor edi, edi
        push AT_INTRO_LOOP
        ret
    none:
        mov ecx, dword ptr [ebp + 0x18]
        xor edi, edi
        push AT_INTRO_EMPTY_RET
        ret
    }
}
static void __fastcall IntroContext(int* ctx, void*, int track) {
    g_intro2PFallback = false;
    g_introHideExtra = false;            // (also when the previous race's scene never started)
    reinterpret_cast<void(__thiscall*)(int*, int)>(FN_INTRO_CONTEXT)(ctx, track);
    Log("intro context: racers %d humans %d slots %X ai %X track byte %d (sound fs: %d free handles)", ctx[0], ctx[1],
        ctx[2], ctx[3], ctx[5], SoundFsFreeHandles());
    // 3 or 5-6 cars (AI opponents): the 4-car scenes (they animate their own 4 grid slots; cars in slots 5/6 stay on
    // the grid; a human in slot 5/6 fails the scenes' slot mask -> no intro, as before)
    if (SplitMode() && (ctx[0] == 3 || ctx[0] > 4)) {
        if (ctx[0] > 4) Log("intro: %d cars, offered the 4-car scenes", ctx[0]), g_introHideExtra = true;
        ctx[0] = 4;
    }
}

// Grid order: 0x5264A0(order) numbers the racer specs' grid slots (+3), order 1 = humans (spec +4 == 1) then AI (2),
// order 2 = AI first (the URL, drift and Street X starters). With 5-6 cars the humans then stood in slots 4-6, outside
// every intro scene's slots (4-car scenes: slots 1-4) -> no intro (user). Split races with more than 4 cars put the
// humans in front.
#define FN_ASSIGN_GRID     0x5264A0      // void __cdecl (int order): 6 bytes stolen
using AssignGridFn = void(__cdecl*)(int);
static AssignGridFn g_origAssignGrid;
static void __cdecl AssignGrid(int order) {
    if (order == 2 && SplitMode() && at<int>(NUM_PLAYERS) == 2) {
        auto* specs = at<uint8_t*>(0x89E810);
        int n = at<int>(0x89E814), cars = 0;
        for (int i = 0; specs && i < n; ++i) {
            int type = *reinterpret_cast<int*>(specs + i * 0x940 + 4);
            if (type == 1 || type == 2) ++cars;
        }
        if (cars > 4) {
            Log("grid: %d cars, humans in front", cars);
            order = 1;
        }
    }
    g_origAssignGrid(order);
}

// A 4-car scene drives its 4 cars onto the grid through the extra cars already standing in grid slots 5/6 (user). Those
// are hidden while the scene runs: a car the scene doesn't drive (mover car+0x28 != 5, NISMvr) reports itself not
// visible to the per-view car check 0x60D4F0 (car body 0x6317A2, lights 0x60D594, headlight pools 0x6154D3), until
// the scene has released its cars; they appear on their grid spots when the race view starts.
#define FN_CAR_NOT_VISIBLE 0x60D4F0      // int __cdecl (car, view): nonzero = skip the car in this view (5 bytes stolen)
using CarNotVisibleFn = int(__cdecl*)(uint8_t*, void*);
static CarNotVisibleFn g_origCarNotVisible;
static bool g_introSceneSeen = false;    // a car was driven by the intro scene (g_introHideExtra set)
static int __cdecl CarNotVisible(uint8_t* car, void* view) {
    if (g_introHideExtra && g_introSceneSeen && car && *reinterpret_cast<int*>(car + 0x28) != MOVER_NIS) return 1;
    return g_origCarNotVisible(car, view);
}
static void UpdateIntroHiding() {        // per frame
    if (!g_introHideExtra) return;
    bool scene = false;
    if (uint8_t* mgr = at<uint8_t*>(RACE_CARS))
        for (auto* c = *reinterpret_cast<uint8_t**>(mgr); c && c != mgr; c = *reinterpret_cast<uint8_t**>(c))
            if (*reinterpret_cast<int*>(c + 0x28) == MOVER_NIS) scene = true;
    if (scene && !g_introSceneSeen) Log("intro: extra cars hidden during the scene");
    if (scene) g_introSceneSeen = true;
    else if (g_introSceneSeen || !SplitMode()) {
        if (g_introSceneSeen) Log("intro: scene over, extra cars shown");
        g_introHideExtra = g_introSceneSeen = false;
    }
}

// Per-player settings (camera, HUD options, feedback...: 0x83AA64 + index*0x28, 2 entries; the race data follows at
// 0x83AAB4) -> players 3/4 read race data as settings (e.g. camera 0 instead of P1's drift camera). The in-race readers
// use a 4-entry mirror instead: entries 0/1 synced both ways with the game's table every frame (options/pause menus
// write the table, the in-race camera change 0x5F2FA0 writes the mirror), entries 2/3 are players 3/4's own (seat
// store; the first race without any copies P1's / P2's).
// The pause menu's player options edit the pausing player's entry (0x4A7AE0: index of the pausing player among
// players 1/2, 0 for anyone else). While player 3/4 owns the pause, their entry is swapped into the game's table
// (entry owner & 1, which 0x4A7AE0 then returns), so the menus show and change their settings.
#define PLAYER_SETTINGS    0x83AA64
#define PLAYER_FEEDBACK    4             // settings +4: Feedback (rumble) option
#define FN_PAUSING_PLAYER  0x4A7AE0      // int __cdecl (): 5 bytes stolen (mov eax, [0x8900D8])
static uint8_t g_settings[MAX_LOCAL_PLAYERS][PLAYER_SETTING_SIZE];
static uint8_t g_settingsSeen[MAX_LOCAL_PLAYERS][PLAYER_SETTING_SIZE];   // the mirror as last synced
static int g_settingsSwap = -1;          // player 3/4 whose entry sits in the game's table (entry & 1)
static const uintptr_t kSettingsReaders[] = {
    0x41E23E, 0x41E245, 0x41E28D, 0x497D59, 0x4DA0F1, 0x4DA8DA, 0x5EA7DD, 0x5EA8C7, 0x5EB406, 0x5EB426, 0x5F17E9,
    0x5F17F2, 0x5F2A93, 0x5F2FB6, 0x5F2FC8, 0x5F325F, 0x5F32AA, 0x5F33CE, 0x5F33DD, 0x5F3402, 0x5F345E, 0x5F3A33,
    0x5F6388, 0x5FFE31, 0x6001FB, 0x600497, 0x60353D};
static int SettingsSeat(int entry) { return g_settingsSwap >= 0 && entry == (g_settingsSwap & 1) ? g_settingsSwap : entry; }

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
        int entries[2] = {g_settingsSwap & 1, swap & 1};
        g_settingsSwap = swap;
        for (int e : entries) {
            if (e < 0) continue;
            int seat = SettingsSeat(e);
            memcpy(game + e * PLAYER_SETTING_SIZE, g_settings[seat], PLAYER_SETTING_SIZE);
            memcpy(g_settingsSeen[seat], g_settings[seat], PLAYER_SETTING_SIZE);
        }
        Log("player settings: %s", swap >= 0 ? "pause menu edits player 3/4" : "game table back to players 1/2");
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
        g_kbFeedbackEntry = g_keyboardSeat & 1;
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
    if (g_settingsSwap >= 0) return g_settingsSwap & 1;
    return reinterpret_cast<int(__cdecl*)()>(g_origPausingPlayer)();
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

// Drift HUD gap ("+N" / "-N" under the position, 0x4DA414): the 2-player path scores the racers of human slots 0 and 1
// (0x56DD50(racer) +0x10) and shows slot0 - slot1 for player 0, the reverse for player 1 and 0 for anyone else.
// Now for every local player: leading -> own score - the next best, otherwise own score - the leader's (same numbers
// as the game's with 2 players).
#define AT_DRIFT_GAP       0x4DA445      // `push 0` (2-player branch; ebx = player)
#define AT_DRIFT_GAP_DONE  0x4DA49A      // edx = gap
#define HUMAN_RACER_SLOTS_EARLY 0x89E808
#define FN_DRIFT_SCORE     0x56DD50      // entry* __cdecl (racer); +0x10 float score
static int RacerIdOfPlayer(int k) { return k < 2 ? reinterpret_cast<const int*>(HUMAN_RACER_SLOTS_EARLY)[k] : k; }
static int __cdecl DriftGap(const uint8_t* player) {
    int me = *reinterpret_cast<const int*>(player + 0x10), n = 2 + g_extraPlayers;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    auto score = [](int k) {
        auto* e = reinterpret_cast<const uint8_t*(__cdecl*)(int)>(FN_DRIFT_SCORE)(RacerIdOfPlayer(k));
        return e ? static_cast<int>(*reinterpret_cast<const float*>(e + 0x10)) : 0;
    };
    int own = score(me), best = INT_MIN;
    bool any = false;
    for (int k = 0; k < n; ++k)
        if (k != me) { int s = score(k); if (!any || s > best) best = s; any = true; }
    if (!any) return 0;
    return own - best;                    // leading: positive gap to the next best; else negative gap to the leader
}
static __declspec(naked) void DriftGapHook() {
    __asm {
        pushad
        push ebx
        call DriftGap
        add esp, 4
        mov dword ptr [esp + 0x14], eax  // popad restores it into edx
        popad
        push AT_DRIFT_GAP_DONE
        ret
    }
}

// Race HUD gap (circuit / sprint / Street X / URL, same element, branch at 0x4DA4DC): the game gets the time gap
// between local players 0 and 1 (0x5F82D0(status; &out, other, self), positive = self ahead, players' +4 -> +0x1C) and
// shows it for player 0 / 1, 0 for anyone else. Now for every local player the smallest gap over the others: leading
// -> + gap to the next, otherwise - gap to the leader (with 2 players the game's own numbers).
#define AT_RACE_GAP        0x4DA4DC
#define AT_RACE_GAP_DONE   0x4DA526      // `test eax, eax` with eax = [esp + 0x34] = gap
#define FN_RACE_TIME_GAP   0x5F82D0      // void __thiscall (status; int* out, info other, info self)
#define RACE_STATUS        0x890118
static int __cdecl RaceGap(const uint8_t* element) {
    int me = *reinterpret_cast<const int*>(element + 0x10), n = 2 + g_extraPlayers;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    auto info = [](int k) -> void* {
        auto* p = static_cast<uint8_t*>(GetLocalPlayer(k));
        auto* q = p ? *reinterpret_cast<uint8_t**>(p + 4) : nullptr;
        return q ? *reinterpret_cast<void**>(q + 0x1C) : nullptr;
    };
    void* self = info(me);
    auto* status = *reinterpret_cast<void**>(RACE_STATUS);
    if (!self || !status) return 0;
    int best = 0;
    bool any = false;
    for (int k = 0; k < n; ++k) {
        void* other = k != me ? info(k) : nullptr;
        if (!other) continue;
        int gap = 0;
        reinterpret_cast<void(__thiscall*)(void*, int*, void*, void*)>(FN_RACE_TIME_GAP)(status, &gap, other, self);
        if (!any || gap < best) best = gap;
        any = true;
    }
    return best;
}
static __declspec(naked) void RaceGapHook() {
    __asm {
        pushad
        push ebx
        call RaceGap
        add esp, 4
        mov dword ptr [esp + 0x20 + 0x34], eax
        popad
        mov eax, dword ptr [esp + 0x34]
        push AT_RACE_GAP_DONE
        ret
    }
}

// Shared "My Cars": the quick race garage is part of each player's block (+0x3B0; P2's block is the 2P "Player 2"
// with an empty garage -> P2 (and P4) only got stock cars). Every garage lookup (`imul reg, index, 0xAB4C` followed by
// block +0x3B0) now resolves to P1's (the loaded profile's) garage: car select lists, category checks, customize,
// race setup (0x525F92 used the loop's block pointer). Single player always uses index 0.
static const uintptr_t kGarageIndexSites[] = {0x49524D, 0x495F83, 0x495FE3, 0x49605F, 0x497EF3, 0x4DF9F1, 0x4E0192,
    0x4E230C, 0x4EEBA4, 0x4EECB9, 0x4EF0A1, 0x511E1F, 0x515D15, 0x521140, 0x5158BB, 0x529CC9, 0x530A4B, 0x53477F,
    0x5347EE, 0x552D38};
#define AT_RACER_GARAGE    0x525F92      // mov edx,[esp+18h]; add edx,3B0h (10 bytes)
static int ShareGarage() {
    int n = 0;
    for (uintptr_t at : kGarageIndexSites) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(at);
        if (b[0] != 0x69 || *reinterpret_cast<const uint32_t*>(b + 2) != PLAYER_BLOCK_SIZE) continue;
        uint32_t zero = 0;
        Write(at + 2, &zero, 4);
        ++n;
    }
    uint8_t movGarage[10] = {0xBA, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};   // mov edx, P1's garage
    uint32_t garage = PLAYER_BLOCKS + 0x3B0;
    memcpy(movGarage + 1, &garage, 4);
    if (*reinterpret_cast<const uint32_t*>(AT_RACER_GARAGE) == 0x1824548B) { Write(AT_RACER_GARAGE, movGarage, 10); ++n; }
    return n;
}

static void* FindRaceCar(int playerIndex) {
    using EndFn = void*(__cdecl*)();
    void* end = reinterpret_cast<EndFn>(FN_LIST_END)();
    void** list = *reinterpret_cast<void***>(RACE_CARS);
    for (void** node = static_cast<void**>(*list); node && node != end; node = static_cast<void**>(*node))
        if (*reinterpret_cast<short*>(reinterpret_cast<uint8_t*>(node) + 0xA) == playerIndex) return node;
    return nullptr;
}

// ---------------------------------------------------------------- catch-up with 3-4 players
// Catch-up (Quick Race option, 0x5028F0 -> race params 0x89E7F8) between the human players: 0x5F6740 (per human driver:
// +0x18 player, +0x1C racer, factor at +0x468) makes the factor a curve (0x81CB60 + 0xA6C) of (own progress - the
// other human's progress) * 0x88F2B0, progress = racer +0x14 (other: player +4 -> car +0x1C). It only ran with exactly 2
// local players (0x5F676A) -> no catch-up at all with 3-4. Now it runs with 2 or more, and each human measures against
// the furthest ahead other human (with 2 players: the other one, as before).
#define AT_HUMAN_CATCHUP_COUNT 0x5F6771  // jne (count != 2) -> jl
#define AT_HUMAN_CATCHUP_OTHER 0x5F6773  // other human -> edx, ecx = [esi+0x1C]                      (25 bytes)
#define HUMAN_CATCHUP_OTHER_RET 0x5F678C

static const uint8_t* RacerOfLocalPlayer(void* player) {
    auto* car = player ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4) : nullptr;
    return car ? *reinterpret_cast<uint8_t**>(car + 0x1C) : nullptr;
}

static void* __cdecl CatchupOtherHuman(const uint8_t* driver) {
    void* self = *reinterpret_cast<void* const*>(driver + 0x18);
    void* other = nullptr;
    float best = -1e30f;
    for (int k = 0; k < g_localPlayers.count && k < MAX_LOCAL_PLAYERS; ++k) {
        void* p = GetLocalPlayer(k);
        const uint8_t* racer = RacerOfLocalPlayer(p);
        if (!p || p == self || !racer) continue;
        float progress = *reinterpret_cast<const float*>(racer + 0x14);
        if (!other || progress > best) { best = progress; other = p; }
    }
    static DWORD logged[MAX_LOCAL_PLAYERS];
    int me = self ? *reinterpret_cast<int*>(static_cast<uint8_t*>(self) + 0x10) & 3 : 0;
    if (g_debug && other && GetTickCount() - logged[me] > 3000) {
        logged[me] = GetTickCount();
        const uint8_t* mine = *reinterpret_cast<uint8_t* const*>(driver + 0x1C);
        Log("catch-up: P%d (progress %.3f) vs P%d (progress %.3f), factor %.3f", me + 1,
            mine ? *reinterpret_cast<const float*>(mine + 0x14) : 0.0f,
            *reinterpret_cast<int*>(static_cast<uint8_t*>(other) + 0x10) + 1, best,
            *reinterpret_cast<const float*>(driver + 0x468));
    }
    return other ? other : GetLocalPlayer(0);
}

static __declspec(naked) void HumanCatchupOtherHook() {
    __asm {
        push esi
        call CatchupOtherHuman
        add esp, 4
        mov edx, eax
        mov ecx, dword ptr [esi + 0x1C]
        push HUMAN_CATCHUP_OTHER_RET
        ret
    }
}

// AI rubber-banding (AI opponents in split races): 0x4173ED.. picks the human each AI car measures against (AI +0x748,
// a local player): 1 player -> that one; 2 -> the leader, or the trailer when the AI is behind a threshold (AI count /
// 2, +1 per human within it; racer +0xA = position). With 3-4 the target stayed 0 and was read at 0x4174E2 (crash).
// The same rule over every human: `AiTargetHook` at 0x4174C5 (reached with the target set, or unset for 3-4).
#define AT_AI_TARGET_DONE  0x4174C5      // mov ecx, 0x89E7A0 (5 bytes); ebx = AI, [esp+0x14] = its racer
#define AI_TARGET_RET      0x4174CA
static void __cdecl AiRubberTarget(uint8_t* ai, const uint8_t* aiRacer) {
    void*& target = *reinterpret_cast<void**>(ai + 0x748);
    int n = at<int>(NUM_PLAYERS) == 2 ? 2 + g_extraPlayers : 0;
    if (target || n < 3 || !aiRacer) return;
    if (n > MAX_LOCAL_PLAYERS) n = MAX_LOCAL_PLAYERS;
    int threshold = at<int>(AI_COUNT) / 2;
    void *leader = nullptr, *trailer = nullptr;
    int best = 127, worst = -128;
    for (int k = 0; k < n; ++k) {
        void* p = GetLocalPlayer(k);
        const uint8_t* r = RacerOfLocalPlayer(p);
        if (!r) continue;
        int pos = static_cast<int8_t>(r[0xA]);
        if (pos <= threshold) ++threshold;
        if (pos < best) best = pos, leader = p;
        if (pos > worst) worst = pos, trailer = p;
    }
    target = static_cast<int8_t>(aiRacer[0xA]) > threshold ? trailer : leader;
}
static __declspec(naked) void AiTargetHook() {
    __asm {
        pushad
        push dword ptr [esp + 0x20 + 0x14]   // the AI's racer
        push ebx                         // the AI
        call AiRubberTarget
        add esp, 8
        popad
        mov ecx, 0x89E7A0
        push AI_TARGET_RET
        ret
    }
}

static void InstallCatchup() {
    const uint8_t jl = 0x7C;
    Write(AT_HUMAN_CATCHUP_COUNT, &jl, 1);
    WriteJump(AT_HUMAN_CATCHUP_OTHER, &HumanCatchupOtherHook, 25);
    WriteJump(AT_AI_TARGET_DONE, &AiTargetHook, 5);
}

static void __cdecl CreatePlayers() {
    // a restart from the post-race menu skips the race setup paths that uncount the results' extra racers: the
    // restarted race counted one racer too many (drift: a racer message without a name, crash 0x43E99A)
    UncountResultsRacers();
    SyncPlayerSettings(true);
    reinterpret_cast<void(__cdecl*)()>(FN_CREATE_PLAYERS)();
    if (g_debug && GetPrivateProfileIntA("DEBUG", "WatchHud", 0, g_iniPath) && g_localPlayers.byIndex[0]) {
        auto* hud = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(g_localPlayers.byIndex[0]) + 0x9A4);
        if (hud && *reinterpret_cast<uint8_t**>(hud + 0x18))
            SetWatch(0, reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(hud + 0x18) + 0x18));
    }
    if (at<int>(NUM_PLAYERS) != 2 || !SplitMode()) return;
    for (int idx = 2; idx < 2 + g_extraPlayers && idx < MAX_LOCAL_PLAYERS; ++idx) {
        auto* racer = reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(FN_FIND_RACER)(
            reinterpret_cast<void*>(RACE_PARAMS), idx);
        void* car = FindRaceCar(idx);
        if (!racer || *reinterpret_cast<int*>(racer + 4) != 1 || !car) {
            Log("player %d: no racer/car (racer %p car %p)", idx + 1, racer, car);
            continue;
        }
        auto* player = static_cast<uint8_t*>(reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x9C0));
        if (!player) continue;
        int port = idx;                                          // input port 2/3 (PollExtraPort)
        // constructed as player 1/2 so the ctor picks P1's / P2's 2P HUD package (PlayerHudCtor clones it for this
        // player), then re-indexed
        void* twin = g_localPlayers.byIndex[idx & 1];
        g_hudCloneFor = idx;                                     // its HUD gets a private package copy
        reinterpret_cast<void*(__thiscall*)(void*, int, int)>(FN_PLAYER_CTOR)(player, port, idx & 1);
        g_hudCloneFor = -1;
        g_localPlayers.byIndex[idx & 1] = twin;
        g_localPlayers.byIndex[idx] = player;
        *reinterpret_cast<int*>(player + 0x10) = idx;
        PrepareExtraNames();                                     // its name (+0x30, 15 chars; results, labels)
        strncpy_s(reinterpret_cast<char*>(player + 0x30), 16, g_extraNames[idx - 2], _TRUNCATE);
        InitPlayerView(kPlayerViewId[idx]);                     // camera movers need the view's camera
        if (void* cam = ExtraCamera(idx))
            GetView(kPlayerViewId[idx]).camera = static_cast<uint8_t*>(cam);
        *reinterpret_cast<int*>(player + 0x18) = kPlayerViewId[idx];   // its view (the ctor picks 1/2 or 1)
        *reinterpret_cast<int*>(player + 0x1C) = 0;                      // no mirror view
        reinterpret_cast<void(__thiscall*)(void*, void*)>(FN_PLAYER_ATTACH)(player, car);
        Log("player %d created (car %p, view %d)", idx + 1, car, kPlayerViewId[idx]);
    }
}

static void* ExtraCamera(int k) {
    if (!g_extraCamera[k]) {
        void* c = reinterpret_cast<void*(__cdecl*)(size_t)>(FN_MALLOC)(0x320);
        if (!c) return nullptr;
        memset(c, 0, 0x320);
        reinterpret_cast<void*(__thiscall*)(void*)>(FN_CAMERA_CTOR)(c);
        g_extraCamera[k] = c;
    }
    return g_extraCamera[k];
}

// ---------------------------------------------------------------- input port tables (4 ports)
// 0x8655A0: 2 x 0x1C8 port tables (binding nodes, port id +0x1B4, device type +0x1B8, ...), built by a static
// vector ctor. 32 code refs (all inside .text); 0x865930 is both "end of tables" (two loop bounds) and the next
// global (mov ecx, 0x865930) -> only the two cmp operands move. Done before the game's static constructors run.
#define OLD_PORT_TABLES    0x8655A0
#define PORT_TABLE_SIZE    0x1C8
#define OLD_PORT_TABLES_END 0x865930
#define AT_PORT_TABLES_CTOR_COUNT 0x77D9CB  // imm8 of the static vector ctor's count
#define AT_PORT_TABLES_DTOR_COUNT 0x782296  // imm8 of the atexit vector dtor's count
#define AT_PORT_BOUND_1    0x57B3AC          // cmp ebp, 0x865930 (operand)
#define AT_PORT_BOUND_2    0x580AD4          // cmp esi, 0x865930 (operand)
#define AT_INPUT_NODE_POOL 0x579559          // imm32 0xA0: binding node pool size
#define AT_INPUT_INIT_CALL 0x57ED80          // call 0x579550 (input init: port ids, pool, action 1 per port)
#define FN_INPUT_INIT      0x579550
#define FN_ADD_BINDING     0x579410          // void __cdecl (port, action, handler, arg)
alignas(16) static uint8_t g_portTables[MAX_LOCAL_PLAYERS * PORT_TABLE_SIZE];

static int RelocatePortTables() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(0x400000);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(0x400000 + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    auto base = reinterpret_cast<uint32_t>(g_portTables);
    int patched = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".text", 6) != 0) continue;
        auto* p = reinterpret_cast<uint8_t*>(0x400000 + sec->VirtualAddress);
        size_t n = sec->Misc.VirtualSize;
        DWORD old;
        VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
        for (size_t o = 0; o + 4 <= n; ++o) {
            uint32_t v;
            memcpy(&v, p + o, 4);
            if (v < OLD_PORT_TABLES || v >= OLD_PORT_TABLES_END) continue;
            uint32_t nv = base + (v - OLD_PORT_TABLES);
            memcpy(p + o, &nv, 4);
            ++patched;
            o += 3;
        }
        VirtualProtect(p, n, old, &old);
    }
    uint32_t end = base + MAX_LOCAL_PLAYERS * PORT_TABLE_SIZE;
    Write(AT_PORT_BOUND_1, &end, 4);
    Write(AT_PORT_BOUND_2, &end, 4);
    const uint8_t count = MAX_LOCAL_PLAYERS;
    Write(AT_PORT_TABLES_CTOR_COUNT, &count, 1);
    Write(AT_PORT_TABLES_DTOR_COUNT, &count, 1);
    const uint32_t pool = 0x140;
    Write(AT_INPUT_NODE_POOL, &pool, 4);
    return patched;
}

// Rumble, as on the console. The game's rumble engine is shared with the PS2 version (SLUS_210.65): events
// 0x5727B0(type, port, amount) pick an effect from the table 0x7FB7D8 (PS2 0x523070; same parameter blocks
// 0x79DC6C.. = PS2 0x487148..) and queue its curve on that port's channel (0x5726D0 -> 0x572390; PS2 0x2E89D8 ->
// 0x2E8888 -> 0x2E8388). Channels: 0x8653B0 + port * 0x60 = 2 sub-channels of 0x30 (PS2 0x5231C0), each summing up to
// 3 running curves into an int 0..255 at +0x28 every frame (0x572470; PS2 0x2E8510). The per-frame update 0x572610
// (PS2 0x2E8748) then hands the port's values to the pad: on PS2 0x333530(port, sub0, sub1) -> DualShock actuators
// (0x333530(port, v, v), v = sub-channel 0's value); on PC that call is an empty stub (0x4022C0 at 0x57266E).
// Both actuators get sub-channel 0's value v (large motor = v, small motor on when v > 128); restored here with that
// conversion on the player's own XInput pad (large -> left motor, small -> right motor at full speed). Collisions (0x572E50 -> 0x865360) only drive the wheels' constant force, as on PS2.
// The channel state exists for 2 ports (channels 0x8653B0 [0xC0], effect strengths 0x864728 [0x60, indexed type * 2 +
// port]); players 3/4 (ports 2/3) get a shadow copy that is swapped in for their events and for a second run of the
// per-frame update.
#define FN_RUMBLE          0x5727B0          // void __cdecl (type, port, amount); entry 5 bytes: mov eax,[esp+4]; push esi
#define FN_RUMBLE_RET      0x5727B5
#define AT_FF_UPDATE_CALL  0x5814AD          // `call 0x572610` (per frame)
#define FN_FF_UPDATE       0x572610
#define AT_FF_PAD_OUTPUT   0x57266E          // `call 0x4022C0` (stub; cdecl (port, value, value))
#define FF_CHANNELS        0x8653B0
#define FF_CHANNELS_SIZE   0xC0
#define FF_STRENGTHS       0x864728
#define FF_STRENGTHS_SIZE  0x60
// Player options "Feedback" = per-player settings +4 (byte; toggled by 0x49B260). The menu greys it unless the last
// DirectInput device enumerated reported DIDC_FORCEFEEDBACK (0x5B8240 returns 0x87078C, only read by the 2 option
// screens); the plugin's virtual pads don't, so it is forced selectable while rumble is on.
#define FN_FF_CAPABLE      0x5B8240
#define AT_MAP_MODE_MAX    0x49AFF9      // Map mode option: `setne cl` (split mode -> max Static)
// Car detail: with the split layout (0x5B98A0 == 3) the car renderers 0x620AE0 and 0x6162E0 scale the car's screen size
// by 0.7 (0x7A6700) and force the LOD range to 1..1 (the low model), and the car parts renderer 0x615630 scales the size
// too. Each `jne` past that block becomes `jmp`: split cars use the Car Geometry Detail option like single player.
static const uintptr_t kSplitCarLodJumps[] = {0x620C8C, 0x616316, 0x6156CC};
static uint8_t g_ffShadowChannels[FF_CHANNELS_SIZE], g_ffShadowStrengths[FF_STRENGTHS_SIZE];
static int g_ffPortBase = 0;                 // 2 while the players 3/4 state is swapped in
static int g_ffMotor[4];                     // per port: motor value 0..255
static bool g_rumble = true;
static float g_rumbleStrength = 1.0f;
static int g_rumblePad[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1};   // pad last sent to (to silence it)
static void* g_origRumble;

static void SwapFfShadow() {
    auto swap = [](uint8_t* a, uint8_t* b, size_t n) { for (size_t i = 0; i < n; ++i) { uint8_t t = a[i]; a[i] = b[i]; b[i] = t; } };
    swap(reinterpret_cast<uint8_t*>(FF_CHANNELS), g_ffShadowChannels, FF_CHANNELS_SIZE);
    swap(reinterpret_cast<uint8_t*>(FF_STRENGTHS), g_ffShadowStrengths, FF_STRENGTHS_SIZE);
}

static void __cdecl RumbleExtraPort(int type, int port, float amount) {
    if (g_extraPlayers <= 0 || port > 3) return;
    SwapFfShadow();
    g_ffPortBase = 2;
    reinterpret_cast<void(__cdecl*)(int, int, float)>(g_origRumble)(type, port - 2, amount);
    g_ffPortBase = 0;
    SwapFfShadow();
}

// the stub's arguments: (port, value, value) - sub-channel 0's motor value twice (the PS2 passes the same byte for
// both actuators too: 0x2E87E4 / 0x2E87EC load +0x28 of the same sub-channel)
static void __cdecl FfPadOutput(int port, int value, int) {
    g_ffMotor[((port & 1) + g_ffPortBase) & 3] = value;
}

static bool PauseMenuOpen() {
    using ActiveFn = bool(__cdecl*)(const char*);
    auto active = reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE);
    return active("UI_Pause.fng") || active("UI_PauseOptions.fng") || active("UI_PauseOptionsMain.fng") ||
           active("UI_PC_Customize_Options.fng");
}

// ---------------------------------------------------------------- start zoom for every player
// After the race intro the chase camera eases out from close to the car (its offset spring, CubicCameraMover +0x38:
// +0 distance (negative = behind), +8 its target, +0x60 height, +0x68 its target, +4/+0x10/+0x14/+0x18/+0x64 rates).
// Only player 1's camera is new then (view 1 showed the intro); the other players' cameras exist from the race load
// and finished the same ease-out unseen. On the first split frame, once player 1's spring starts, the other players'
// springs are set to the same fractions of their targets, so every view zooms out together.
#define CUBIC_MOVER_VTABLE_UPDATE 0x787F2C   // CubicCameraMover vtable 0x787F28, slot 1: Update 0x453570
#define FN_CUBIC_MOVER_UPDATE     0x453570
static uint8_t* g_chaseMover[MAX_LOCAL_PLAYERS];
static int g_chaseMoverFrame[MAX_LOCAL_PLAYERS];
static int g_startZoomFrames;            // frames left to wait for player 1's ease-out
static int g_lastLayout;
static void __fastcall NoteChaseMover(uint8_t* mover) {
    const uint8_t* cam = *reinterpret_cast<uint8_t* const*>(mover + 0x1C);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k)
        if (cam && GetView(kPlayerViewId[k]).camera == cam) { g_chaseMover[k] = mover; g_chaseMoverFrame[k] = g_mmFrame; }
}
static __declspec(naked) void ChaseMoverUpdateHook() {
    __asm {
        push ecx
        call NoteChaseMover              // fastcall: ecx = mover
        pop ecx
        mov eax, FN_CUBIC_MOVER_UPDATE
        jmp eax
    }
}
static float* ChaseSpring(int k) {       // only a mover updated in the last frames (it is freed with its camera)
    if (!g_chaseMover[k] || g_mmFrame - g_chaseMoverFrame[k] > 2) return nullptr;
    return *reinterpret_cast<float**>(g_chaseMover[k] + 0x38);
}
static void UpdateStartZoom() {
    int layout = at<int>(LAYOUT);
    if (layout == 3 && g_lastLayout != 3 && SplitMode()) g_startZoomFrames = 30;
    g_lastLayout = layout;
    if (g_startZoomFrames <= 0) return;
    --g_startZoomFrames;
    float* p1 = ChaseSpring(0);
    if (!p1 || fabsf(p1[2]) < 1.0f || fabsf(p1[26]) < 0.1f || fabsf(p1[0] - p1[2]) < 0.5f) return;   // not started
    float dist = p1[0] / p1[2], height = p1[24] / p1[26];
    for (int k = 1; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k)
        if (float* s = ChaseSpring(k)) {
            s[0] = s[7] = s[2] * dist;   // +0x1C follows +0 at the start (player 1's dump)
            s[24] = s[26] * height;
            s[1] = s[4] = s[5] = s[6] = s[25] = 0.0f;
        }
    Log("start zoom: other players' cameras from %.2f of their distance", dist);
    g_startZoomFrames = 0;
}

// ---------------------------------------------------------------- WidescreenFix free camera per player
// NFSU2 WidescreenFix ([CAMERA] Enable) adds a free look camera: it replaces the eCreateLookAtMatrix call in
// CubicCameraMover::Update (0x453B44, call 0x5BA9A0(out, from, to, up); esi = mover: +0x1C camera, +0x48 mode, +0x51
// look behind) and turns the camera around the car by one global yaw/pitch offset, from the right stick (the game's
// last polled joystick slot) and the mouse, returning after a timeout once the car moves. In split screen every
// player's camera went through it: one offset (and one stick) for everyone.
// Once WidescreenFix has patched the call, the plugin chains in front of it: outside split racing everything goes to
// WidescreenFix as before; while split racing each player's camera (mover camera == that player's view camera) gets
// WidescreenFix's behaviour with its own state, that player's right stick, and the mouse for player 1. Settings come
// from WidescreenFix's ini ([CAMERA], [MISC] RightStickDeadzone).
#define AT_LOOKAT_CALL     0x453B44
#define FN_LOOKAT          0x5BA9A0      // void __cdecl eCreateLookAtMatrix(bMatrix4*, bVector3* from, to, up)
typedef void(__cdecl* LookAtFn)(void*, float*, float*, float*);
static LookAtFn g_lookAtNext = nullptr;  // WidescreenFix's (or the game's) function at the call site
struct FreeCam { float yaw, pitch, idle, prevX, prevY, speed; bool prevValid; LARGE_INTEGER last; };
static FreeCam g_freeCam[MAX_LOCAL_PLAYERS];
static struct { bool loaded, enabled, invert, horizontalOnly; float timeout, returnSpeed, stickSens, mouseSens, deadzone; } g_freeCamCfg;
static int g_frameMouseDx, g_frameMouseDy;   // mouse movement of this frame (player 1's camera)

static void LoadFreeCamConfig() {
    g_freeCamCfg.loaded = true;
    char path[MAX_PATH];
    strcpy_s(path, g_iniPath);
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "NFSUnderground2.WidescreenFix.ini");
    auto readF = [&](const char* sec, const char* key, float def) {
        char buf[32];
        GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
        return buf[0] ? static_cast<float>(atof(buf)) : def;
    };
    g_freeCamCfg.enabled = GetPrivateProfileIntA("CAMERA", "Enable", 1, path) != 0;
    g_freeCamCfg.timeout = readF("CAMERA", "CameraReturnTimeout", 3.0f);
    g_freeCamCfg.returnSpeed = readF("CAMERA", "CameraReturnSpeed", 2.0f);
    g_freeCamCfg.stickSens = readF("CAMERA", "StickLookSensitivity", 1.0f);
    g_freeCamCfg.mouseSens = readF("CAMERA", "MouseLookSensitivity", 1.0f) * 0.15f;
    g_freeCamCfg.invert = GetPrivateProfileIntA("CAMERA", "InvertLook", 0, path) != 0;
    g_freeCamCfg.horizontalOnly = GetPrivateProfileIntA("CAMERA", "OnlyAllowHorizontalStickMovement", 0, path) != 0;
    float dz = readF("MISC", "RightStickDeadzone", 10.0f);
    g_freeCamCfg.deadzone = (dz < 0 ? 0 : dz > 100 ? 100 : dz) / 100.0f;
    Log("free camera: WidescreenFix camera %s", g_freeCamCfg.enabled ? "on" : "off");
}

static float LerpAngleDeg(float from, float to, float t) {
    float diff = to - from;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    return from + diff * t;
}

static void __cdecl SplitLookAt(const uint8_t* mover, void* out, float* from, float* to, float* up) {
    int k = -1;
    if (SplitRacing() && g_freeCamCfg.enabled)
        for (int i = 0; i < 2 + g_extraPlayers && i < MAX_LOCAL_PLAYERS; ++i)
            if (GetView(kPlayerViewId[i]).camera == *reinterpret_cast<uint8_t* const*>(mover + 0x1C)) k = i;
    if (k < 0) { g_lookAtNext(out, from, to, up); return; }
    if (k == 0) {   // WidescreenFix's own function on a throwaway copy: it also hides the mouse cursor while racing
        float scratchOut[16], f[4] = {from[0], from[1], from[2], 0}, t[4] = {to[0], to[1], to[2], 0}, u[4] = {up[0], up[1], up[2], 0};
        g_lookAtNext(scratchOut, f, t, u);
    }
    auto lookAt = reinterpret_cast<LookAtFn>(FN_LOOKAT);
    FreeCam& c = g_freeCam[k];
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = c.last.QuadPart ? static_cast<float>(now.QuadPart - c.last.QuadPart) / freq.QuadPart : 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    c.last = now;
    int mode = *reinterpret_cast<const int*>(mover + 0x48);
    if (mover[0x51] || (mode != 0 && mode != 1 && mode != 5)) {   // look behind / other cameras: original
        c.yaw = c.pitch = c.idle = 0.0f;
        c.prevValid = false;
        lookAt(out, from, to, up);
        return;
    }
    if (c.prevValid && dt > 0.0f)
        c.speed = sqrtf((to[0] - c.prevX) * (to[0] - c.prevX) + (to[1] - c.prevY) * (to[1] - c.prevY)) / dt;
    c.prevX = to[0]; c.prevY = to[1]; c.prevValid = true;
    float stickX = 0.0f, stickY = 0.0f;
    int16_t rx, ry;
    if (XPadRightStick(g_assignedPad[k], &rx, &ry)) {
        float x = rx / 32768.0f, y = -(ry + 1) / 32768.0f;   // DirectInput Ry points down
        stickX = fabsf(x) <= g_freeCamCfg.deadzone ? 0.0f : -x;
        stickY = fabsf(y) <= g_freeCamCfg.deadzone || g_freeCamCfg.horizontalOnly ? 0.0f : y;
    }
    float mouseX = 0.0f, mouseY = 0.0f;
    if (k == (g_keyboardSeat >= 0 ? g_keyboardSeat : 0)) {     // the mouse goes with the keyboard
        mouseX = static_cast<float>(g_frameMouseDx); mouseY = static_cast<float>(g_frameMouseDy);
        g_frameMouseDx = g_frameMouseDy = 0;
    }
    float deltaX = stickX - mouseX * g_freeCamCfg.mouseSens;
    float deltaY = stickY + mouseY * g_freeCamCfg.mouseSens;
    bool input = fabsf(deltaX) > 0.01f || fabsf(deltaY) > 0.01f;
    if (input && !PauseMenuOpen()) {
        c.yaw += deltaX * g_freeCamCfg.stickSens;
        c.pitch += deltaY * g_freeCamCfg.stickSens * (g_freeCamCfg.invert ? -1.0f : 1.0f);
        c.idle = 0.0f;
    } else {
        c.idle += dt;
        if (c.idle >= g_freeCamCfg.timeout && c.speed > 0.5f) {   // back to the game's camera once the car moves
            float blend = 1.0f - expf(-g_freeCamCfg.returnSpeed * dt);
            c.yaw = LerpAngleDeg(c.yaw, 0.0f, blend);
            c.pitch += (0.0f - c.pitch) * blend;
            if (fabsf(c.yaw) < 0.3f && fabsf(c.pitch) < 0.3f) c.yaw = c.pitch = 0.0f;
        }
    }
    if (c.yaw == 0.0f && c.pitch == 0.0f) { lookAt(out, from, to, up); return; }
    if (c.pitch > 60.0f) c.pitch = 60.0f;
    if (c.pitch < -20.0f) c.pitch = -20.0f;
    const float deg = 0.0174533f;
    float dx = from[0] - to[0], dy = from[1] - to[1], dz = from[2] - to[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz), dist2D = sqrtf(dx * dx + dy * dy);
    float yaw = atan2f(dy, dx) + c.yaw * deg;
    float pitch = atan2f(dz, dist2D) + c.pitch * deg;
    if (pitch > 1.39626f) pitch = 1.39626f;
    if (pitch < -1.39626f) pitch = -1.39626f;
    from[0] = to[0] + dist * cosf(pitch) * cosf(yaw);
    from[1] = to[1] + dist * cosf(pitch) * sinf(yaw);
    from[2] = to[2] + dist * sinf(pitch);
    lookAt(out, from, to, up);
}

static __declspec(naked) void LookAtHook() {
    __asm {
        push dword ptr [esp + 16]        // up
        push dword ptr [esp + 16]        // to
        push dword ptr [esp + 16]        // from
        push dword ptr [esp + 16]        // out
        push esi                         // the camera mover
        call SplitLookAt
        add esp, 20
        ret
    }
}

// Per frame: chain in front of WidescreenFix's camera once it has patched the call (it does so during its own init),
// and take this frame's mouse movement.
static void UpdateFreeCamHook() {
    XMouseTakeDelta(&g_frameMouseDx, &g_frameMouseDy);
    if (!SplitRacing()) for (FreeCam& c : g_freeCam) c = FreeCam{};
    const uint8_t* site = reinterpret_cast<const uint8_t*>(AT_LOOKAT_CALL);
    if (site[0] != 0xE8) return;
    uintptr_t target = AT_LOOKAT_CALL + 5 + *reinterpret_cast<const int32_t*>(site + 1);
    if (target == reinterpret_cast<uintptr_t>(&LookAtHook) || target == FN_LOOKAT) return;   // chained / no WSF camera
    if (!g_freeCamCfg.loaded) LoadFreeCamConfig();
    g_lookAtNext = reinterpret_cast<LookAtFn>(target);
    WriteCall(AT_LOOKAT_CALL, &LookAtHook);
    Log("free camera: chained in front of WidescreenFix's camera (%p)", reinterpret_cast<void*>(target));
}

void* GetLocalPlayer(int k);
static void __cdecl FfUpdate() {
    reinterpret_cast<void(__cdecl*)()>(FN_FF_UPDATE)();
    if (g_extraPlayers > 0 && SplitMode()) {
        SwapFfShadow();
        g_ffPortBase = 2;
        reinterpret_cast<void(__cdecl*)()>(FN_FF_UPDATE)();
        g_ffPortBase = 0;
        SwapFfShadow();
    }
    bool on = g_rumble && at<int>(GAME_STATE) == 6 && !PauseMenuOpen() &&
              (!SplitMode() || SplitRacing());
    int players = SplitMode() ? 2 + g_extraPlayers : 1;
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        int pad = -1;
        auto* player = static_cast<uint8_t*>(k < players ? GetLocalPlayer(k) : nullptr);
        bool feedback = SplitMode() ? g_settings[k][PLAYER_FEEDBACK] != 0
                                    : at<uint8_t>(PLAYER_SETTINGS + PLAYER_FEEDBACK) != 0;
        if (on && player && feedback) pad = SplitMode() ? g_assignedPad[k] : (k == 0 ? XPadLastActive() : -1);
        if (g_rumblePad[k] >= 0 && g_rumblePad[k] != pad) XPadSetRumble(g_rumblePad[k], 0, 0);
        g_rumblePad[k] = pad;
        if (pad < 0) continue;
        int port = *reinterpret_cast<int*>(player + 8) & 3;
        int v = g_ffMotor[port] < 0 ? 0 : g_ffMotor[port] > 255 ? 255 : g_ffMotor[port];
        float largeMotor = v / 255.0f, smallMotor = v > 128 ? 1.0f : 0.0f;   // PS2 0x333530: act[1] = v, act[0] = v > 0x80
        XPadSetRumble(pad, largeMotor * g_rumbleStrength, smallMotor * g_rumbleStrength);
    }
}

static __declspec(naked) void RumbleHook() {
    __asm {
        cmp dword ptr [esp + 8], 2       // port
        jb play
        jmp RumbleExtraPort              // players 3/4: their shadow state (cdecl, same arguments)
    play:
        mov eax, dword ptr [esp + 4]
        push esi
        push FN_RUMBLE_RET
        ret
    }
}

// Collision rumble per player index (0x865370 channels, 2 entries): ignore players 3/4.
#define FN_IMPACT_RUMBLE   0x572E50          // void __cdecl (playerIndex, vector); entry: push ebp; mov ebp,esp; and esp,-16
#define FN_IMPACT_RUMBLE_RET 0x572E56
static __declspec(naked) void ImpactRumbleHook() {
    __asm {
        cmp dword ptr [esp + 4], 2       // player index
        jb play
        ret
    play:
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        push FN_IMPACT_RUMBLE_RET
        ret
    }
}

// "Joystick present on this slot" (0x5BFD70: 0x86FFB4[slot] != 0, 2 slots) gates the driving force feedback
// (0x406460, the tyre/drift effects, slot = input port); ports 2/3 read past the array -> garbage effect pointers.
#define FN_JOY_SLOT_PRESENT 0x5BFD70         // bool __stdcall (slot)
static __declspec(naked) void JoySlotPresent() {
    __asm {
        mov ecx, dword ptr [esp + 4]
        xor eax, eax
        cmp ecx, 2
        jae done
        cmp dword ptr [ecx * 4 + 0x86FFB4], 0
        setne al
    done:
        ret 4
    }
}

static void __cdecl InputInit() {
    reinterpret_cast<void(__cdecl*)()>(FN_INPUT_INIT)();
    for (int port = 2; port < MAX_LOCAL_PLAYERS; ++port) {
        *reinterpret_cast<int*>(g_portTables + port * PORT_TABLE_SIZE + 0x1B4) = port;
        reinterpret_cast<void(__cdecl*)(int, int, uint32_t, int)>(FN_ADD_BINDING)(port, 1, 0x571A20, 0);
    }
}

// ---------------------------------------------------------------- track streaming for players 3/4
// The streamer (0x88EB00) has room for 2 positions (+0xB0 + k*0x5C), fed from the cameras of views 1/2 (0x44EFE0).
// Players 3/4 ride along: each frame their camera gives a zone (0x5E4F20 on a shadow position block); a zone change
// forces the streamer to rebuild its section sets (0x5E5300 reports "changed"), and the rebuild (0x5E5110) also
// marks P3's zone sections under position 0 and P4's under position 1, so they get loaded and kept.
#define STREAMER           0x88EB00
#define FN_STREAM_ZONES    0x5E5300      // bool __thiscall (streamer; short zones[2]) -> changed
#define FN_STREAM_SECTIONS 0x5E5110      // void __thiscall (streamer): per-position section sets
#define FN_ZONE_OF         0x5E4F20      // int __thiscall (streamer; position block)
#define FN_MARK_SECTIONS   0x5DACB0      // void __thiscall (streamer; short* list, n, position)
#define FN_BUILD_ZONE_LIST 0x5DFF50      // int __thiscall (0x883E70; node, short* list, max)
#define ZONE_LISTS_A       0x883E90      // list heads (next pointer at +0)
#define ZONE_LISTS_B       0x883E80
struct StreamPos { int car; float x, y, vx, vy; uint8_t pad[0x48]; };
static StreamPos g_streamPos[2];
static short g_streamZone[2], g_streamZoneUsed[2];
using StreamZonesFn = uint8_t(__fastcall*)(void*, void*, short*);
using StreamSectionsFn = void(__fastcall*)(void*);
static StreamZonesFn g_origStreamZones;
static StreamSectionsFn g_origStreamSections;

static void UpdateExtraStreamZones() {
    for (int k = 0; k < 2; ++k) {
        g_streamZone[k] = 0;
        if (k >= g_extraPlayers || !SplitRacing()) continue;
        View& v = GetView(kPlayerViewId[2 + k]);
        if (!v.active || !v.camera) continue;
        const float* pos = reinterpret_cast<const float*>(v.camera + 0x40);
        const float* vel = reinterpret_cast<const float*>(v.camera + 0x1E0);
        StreamPos& sp = g_streamPos[k];
        sp.car = -1; sp.x = pos[0]; sp.y = pos[1]; sp.vx = vel[0]; sp.vy = vel[1];
        g_streamZone[k] = static_cast<short>(reinterpret_cast<int(__thiscall*)(void*, void*)>(FN_ZONE_OF)(
            reinterpret_cast<void*>(STREAMER), &sp));
    }
}

static uint8_t __fastcall StreamZonesHook(void* streamer, void* edx, short* zones) {
    uint8_t changed = g_origStreamZones(streamer, edx, zones);
    UpdateExtraStreamZones();
    for (int k = 0; k < 2; ++k)
        if (g_streamZone[k] != g_streamZoneUsed[k]) changed = 1;
    return changed;
}

static int ZoneSections(short zone, short* list, int max) {
    for (auto** n = *reinterpret_cast<uint8_t***>(ZONE_LISTS_A); n && n != reinterpret_cast<uint8_t**>(ZONE_LISTS_A);
         n = *reinterpret_cast<uint8_t***>(n)) {
        auto* node = reinterpret_cast<uint8_t*>(n);
        short count = *reinterpret_cast<short*>(node + 0x18);
        for (int i = 0; i < count; ++i)
            if (reinterpret_cast<short*>(node + 0x1A)[i] == zone)
                return reinterpret_cast<int(__thiscall*)(void*, void*, short*, int)>(FN_BUILD_ZONE_LIST)(
                    reinterpret_cast<void*>(0x883E70), node, list, max);
    }
    for (auto** n = *reinterpret_cast<uint8_t***>(ZONE_LISTS_B); n && n != reinterpret_cast<uint8_t**>(ZONE_LISTS_B);
         n = *reinterpret_cast<uint8_t***>(n)) {
        auto* node = reinterpret_cast<uint8_t*>(n);
        if (*reinterpret_cast<short*>(node + 8) != zone) continue;
        int count = *reinterpret_cast<int*>(node + 0x10);
        if (count > max) count = max;
        memcpy(list, node + 0x14, count * sizeof(short));
        return count;
    }
    return 0;
}

static void __fastcall StreamSectionsHook(void* streamer) {
    g_origStreamSections(streamer);
    for (int k = 0; k < 2; ++k) {
        g_streamZoneUsed[k] = g_streamZone[k];
        if (g_streamZone[k] <= 0) continue;
        static short list[0x180];
        int n = ZoneSections(g_streamZone[k], list, 0x180);
        if (n > 0)
            reinterpret_cast<void(__thiscall*)(void*, short*, int, int)>(FN_MARK_SECTIONS)(streamer, list, n, k);
        Log("streaming: player %d zone %d -> %d sections (position %d)", k + 3, g_streamZone[k], n, k);
    }
}

// Load priority (0x5DF580, called at 0x5E7CFC): for each section of the streamer's list (+0x29C, count +0x298) the
// priority is the smallest 0x5DF3A0(section, position block) (distance-based, 0..0x63; uses the block's x/y +4/+8 and
// velocity +0xC/+0x10) over the positions whose bit the section has (+0xB), stored as priority * 100000 + section
// +0x20 at +0x40; it decides what loads first and what is dropped first. P3's / P4's sections ride on bits 0 / 1, so
// they were ranked by the distance to P1 / P2: the sections around a player far from both loaded last and were evicted
// first. Now the distance to P3's / P4's own position counts too (bit 0 / 1); unchanged with 2 players.
#define AT_STREAM_PRIORITY_CALL 0x5E7CFC  // call 0x5DF580 (thiscall streamer)
#define FN_STREAM_PRIORITY      0x5DF580
#define FN_SECTION_DISTANCE     0x5DF3A0  // int __thiscall (streamer; section, position block)
static void __fastcall StreamPriorities(uint8_t* streamer) {
    if (g_extraPlayers <= 0 || !SplitRacing()) {
        reinterpret_cast<void(__thiscall*)(void*)>(FN_STREAM_PRIORITY)(streamer);
        return;
    }
    auto distance = reinterpret_cast<int(__thiscall*)(void*, void*, void*)>(FN_SECTION_DISTANCE);
    int count = *reinterpret_cast<int*>(streamer + 0x298);
    auto** sections = reinterpret_cast<uint8_t**>(streamer + 0x29C);
    int raised = 0;
    for (int i = 0; i < count; ++i) {
        uint8_t* section = sections[i];
        int bits = static_cast<int8_t>(section[0xB]), best = 0x63;
        for (int pos = 0; pos < 2; ++pos)
            if (bits & (1 << pos)) {
                int d = distance(streamer, section, streamer + 0xB0 + pos * 0x5C);
                if (d < best) best = d;
            }
        int own = best;
        for (int k = 0; k < 2 && k < g_extraPlayers; ++k)      // P3 -> bit 0, P4 -> bit 1
            if ((bits & (1 << k)) && g_streamZone[k] > 0) {
                int d = distance(streamer, section, &g_streamPos[k]);
                if (d < best) best = d;
            }
        raised += best < own;
        *reinterpret_cast<int*>(section + 0x40) = best * 100000 + *reinterpret_cast<int*>(section + 0x20);
    }
    static DWORD logged;
    if (g_debug && GetTickCount() - logged > 3000) {
        logged = GetTickCount();
        Log("streaming: %d sections, %d ranked higher by players 3/4's positions", count, raised);
    }
}

// ---------------------------------------------------------------- setup
static bool IsSupportedExe() {
    // original bytes at a few patch sites (v1.2, unpatched)
    const uint8_t loopMax[] = {0x83, 0xF8, 0x01};
    const uint8_t remapJnz[] = {0x75, 0x24};
    const uint8_t qrMode[] = {0xC7, 0x05, 0x78, 0x00, 0x85, 0x00, 0x02, 0x00, 0x00, 0x00};
    const uint8_t pressStart[] = {0x8B, 0x44, 0x24, 0x04, 0x3D, 0x64, 0xB3, 0x1A, 0x91};
    const uint8_t carSelect[] = {0x55, 0x8B, 0x6C, 0x24, 0x10};
    const uint8_t mainMenu[] = {0xE8, 0x83, 0x1B, 0x0C, 0x00};
    const uint8_t joypadTitle[] = {0xE8, 0xE3, 0xFE, 0x02, 0x00};
    return memcmp(reinterpret_cast<void*>(AT_VIEWLOOP_MAX - 2), loopMax, sizeof(loopMax)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_SECTION_REMAP), remapJnz, sizeof(remapJnz)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_QR_SET_MODE), qrMode, sizeof(qrMode)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_PRESSSTART), pressStart, sizeof(pressStart)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_CARSELECT), carSelect, sizeof(carSelect)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_MAINMENU_QR), mainMenu, sizeof(mainMenu)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_JOYPAD_TITLE), joypadTitle, sizeof(joypadTitle)) == 0;
}

static void LoadConfig(HMODULE self) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (!n || !dot) return;
    strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
    g_debug = GetPrivateProfileIntA("MAIN", "Debug", 0, path) != 0;
    g_quadTest = GetPrivateProfileIntA("DEBUG", "QuadTest", 0, path) != 0;
    g_extraPlayers = GetPrivateProfileIntA("DEBUG", "ExtraPlayers", 0, path);
    g_splitPlayers = 2 + (g_extraPlayers < 0 ? 0 : g_extraPlayers > 2 ? 2 : g_extraPlayers);   // debug default
    g_splitOpponents = GetPrivateProfileIntA("DEBUG", "Opponents", 0, path);                   // debug default
    g_spGauges = GetPrivateProfileIntA("HUD", "SinglePlayerGauges", 1, path) != 0;
    g_gaugeScale = GetPrivateProfileIntA("HUD", "GaugeScalePercent", 60, path) / 100.0f;
    g_rumble = GetPrivateProfileIntA("INPUT", "Rumble", 1, path) != 0;
    g_rumbleStrength = GetPrivateProfileIntA("INPUT", "RumbleStrength", 100, path) / 100.0f;
    g_xinputPads = GetPrivateProfileIntA("INPUT", "XInputPads", 1, path) != 0;
    g_hideDirectInputPads = GetPrivateProfileIntA("INPUT", "HideDirectInputControllers", 1, path) != 0;
    strcpy_s(g_iniPath, path);
    strcpy_s(g_playersPath, path);
    strcpy_s(g_playersPath + (dot - path), MAX_PATH - (dot - path), "_Players.ini");
    LoadSeatStore();
    strcpy_s(g_logPath, path);
    strcpy_s(g_logPath + (dot - path), MAX_PATH - (dot - path), ".log");
}

static void Install(HMODULE self) {
    if (!IsSupportedExe()) {
        MessageBoxA(nullptr, "Unsupported speed2.exe (needs v1.2) or another mod patches the same code. Disabled.",
                    "NFSU2SplitScreen", MB_ICONWARNING);
        return;
    }
    LoadConfig(self);
    g_mainThreadId = GetCurrentThreadId();
    if (g_debug) {
        char path[MAX_PATH], key[16], val[32];
        GetModuleFileNameA(self, path, MAX_PATH);
        strcpy_s(strrchr(path, '.'), 5, ".ini");
        for (int i = 0; i < 4; ++i) {
            sprintf_s(key, "Watch%d", i);
            GetPrivateProfileStringA("DEBUG", key, "0", val, sizeof(val), path);
            if (uintptr_t a = strtoul(val, nullptr, 0)) SetWatch(i, a);
        }
    }
    Log("mailbox at %p", &g_mailbox);

    if (g_xinputPads) {
        if (InstallXInputDevices(IMPORT_DIRECTINPUT8CREATE, g_hideDirectInputPads))
            XPadSetResolver(&PadForDevice);
        else
            Log("XInput runtime not found; using DirectInput controllers");
    }

    bool tablesBuilt = *reinterpret_cast<uint32_t*>(OLD_PORT_TABLES) == OLD_PORT_TABLES;
    int portRefs = RelocatePortTables();
    Log("port tables relocated: %d refs%s", portRefs, tablesBuilt ? " (WARNING: already constructed)" : "");
    WriteCall(AT_INPUT_INIT_CALL, &InputInit);
    g_origRumble = MakeTrampoline(FN_RUMBLE, 5);
    WriteJump(FN_RUMBLE, &RumbleHook, 5);
    WriteCall(AT_FF_UPDATE_CALL, &FfUpdate);
    WriteCall(AT_FF_PAD_OUTPUT, &FfPadOutput);
    {   // Gameplay > Map mode (0x83AA1B, handler 0x49AFE0): the console's split mode offers Off/Static only (max =
        // (mode != 4) + 1); with per-player minimaps Rotate works in split too -> `setne cl` becomes `mov cl, 1`
        static const uint8_t setne[3] = {0x0F, 0x95, 0xC1}, movCl1[3] = {0xB1, 0x01, 0x90};
        if (memcmp(reinterpret_cast<void*>(AT_MAP_MODE_MAX), setne, 3) == 0) Write(AT_MAP_MODE_MAX, movCl1, 3);
    }
    for (uintptr_t at : kSplitCarLodJumps) {
        static const uint8_t jmp = 0xEB;
        if (*reinterpret_cast<uint8_t*>(at) == 0x75) Write(at, &jmp, 1);
    }
    if (g_rumble) WriteJump(FN_FF_CAPABLE, &FfCapable, 5);   // Feedback selectable (not for the keyboard player)
    WriteJump(FN_IMPACT_RUMBLE, &ImpactRumbleHook, 6);
    WriteJump(FN_JOY_SLOT_PRESENT, &JoySlotPresent, 5);
    int relocated = RelocateLocalPlayers();
    // the one indexed reference that is "inOrder - 4" (0x8900B0 = old byIndex[1]): the race teardown loop
    // `while (count) delete inOrder[count - 1]` (mov esi,[eax*4+0x8900B0]); relocated as byIndex+4 it read
    // byIndex[count] (3 players: null forever -> hang on quit; 4 players: wrong player freed)
    uint32_t inOrderMinus1 = reinterpret_cast<uint32_t>(&g_localPlayers.inOrder[0]) - 4;
    Write(AT_PLAYERS_TEARDOWN_DISP, &inOrderMinus1, 4);
    Log("local player arrays relocated: %d refs", relocated);
    InstallCatchup();
    PatchViewUpdateLoop();
    Log("sound listener arrays relocated: %d refs", RelocateListenerArrays());
    WriteCall(AT_LISTENER_UPDATE_CALL, &ListenerUpdate);
    WriteJump(AT_LISTENER_CHOICE, &ListenerChoiceHook, 5);
    WriteJump(AT_EMITTER_RANGE, &EmitterRangeHook, 6);
    WriteCall(AT_MAIN_WORLD_DRAW, &WorldDrawWithBeams);
    WriteCall(AT_BEAM_COLOR_CALL, &BeamColor);
    {   // CubicCameraMover::Update through the plugin (start zoom)
        void* hook = reinterpret_cast<void*>(&ChaseMoverUpdateHook);
        if (*reinterpret_cast<uint32_t*>(CUBIC_MOVER_VTABLE_UPDATE) == FN_CUBIC_MOVER_UPDATE)
            Write(CUBIC_MOVER_VTABLE_UPDATE, &hook, 4);
    }
    WriteCall(AT_SPLIT_PASS_CALL, &SplitPassWithHeadlightPools);
    WriteCall(AT_1P_POOLS_CALL, &PoolsForTraffic);
    WriteCall(AT_RAIN_CALL, &RainForViews);
    WriteCall(AT_RAIN_DROPS_CALL, &RainDropsForViews);
    WriteCall(AT_RAIN_DROP_GET, &RainDropGet);
    for (uintptr_t a : kRainDropsViewCalls) WriteCall(a, &RainDropsView);
    WriteJump(AT_REFLECTIONS_DONE, &ReflectionsDoneHook, 6);
    WriteJump(AT_NOS_FX_PLAYER, &NosFxPlayerHook, 13);
    WriteCall(AT_OVERBRIGHT_AMOUNT_CALL, &OverbrightAmounts);
    WriteCall(AT_OVERBRIGHT_ACCUM_CALL, &OverbrightAccumulate);
    WriteCall(AT_BRIGHT_PASS_CALL1, &BrightPassPerView);
    WriteCall(AT_BRIGHT_PASS_CALL2, &BrightPassPerView);
    WriteCall(AT_BRIGHT_PASS_CALL0, &BrightPassPerView);
    WriteJump(AT_REFLECT_CAR_PICK, &ReflectCarPickHook, 5);
    WriteCall(AT_ENV_DONE, &EnvMapsForPlayers);
    g_origReleaseTargets = reinterpret_cast<ReleaseTargetsFn>(MakeTrampoline(FN_RELEASE_TARGETS, 6));
    WriteJump(FN_RELEASE_TARGETS, &ReleaseTargetsHook, 6);
    WriteJump(AT_RAIN_CAR, &RainCarHook, 5);
    WriteCall(AT_RAIN_REGION_SLOT, &RainRegionSlot);
    WriteCall(AT_SPLASH_REGION_SLOT, &SplashRegionSlot);
    { const uint8_t nops[3] = {0x90, 0x90, 0x90}; Write(AT_SPLASH_REGION_SLOT + 5, nops, 3); }
    g_origNearestRacerDist = reinterpret_cast<NearestRacerDistFn>(MakeTrampoline(FN_NEAREST_RACER_DIST, 6));
    WriteJump(FN_NEAREST_RACER_DIST, &NearestRacerDistance, 6);
    WriteJump(FN_NEAREST_PLAYER_CAR, &NearestPlayerCar, 5);
    WriteJump(FN_NEAREST_PLAYER, &NearestPlayer, 5);
    WriteJump(AT_MAINMENU_QR, &MainMenuHook, 5);
    WriteCall(AT_JOYPAD_TITLE, &SetJoypadPageTitle);
    WriteCall(AT_CONTROLS_PAGES_CALL, &BuildControlsPages);
    WriteCall(AT_PROFILE_SAVE_BINDINGS, &ProfileSaveBindings);
    WriteCall(AT_PROFILE_LOAD_BINDINGS, &ProfileLoadBindings);
    WriteJump(AT_FE_OBJ_XFORM, &FeObjXformHook, 6);
    WriteJump(AT_QR_SET_MODE, &QuickRaceModeHook, 10);
    WriteJump(AT_PRESSSTART, &PressStartHook, 9);
    for (uintptr_t a : kCarselToP2Calls) WriteCall(a, &CarselToP2);
    for (uintptr_t a : kCarselStartCalls) WriteCall(a, &CarselStartHook);
    WriteCall(AT_CARSEL_BACK_TO_P1, &CarselBackToP1);
    for (uintptr_t a : kQrFinishRowsCalls) WriteCall(a, &QrFinishRows);
    WriteJump(AT_CARSELECT, &CarSelectHook, 5);
    g_origHelpBarSet = reinterpret_cast<HelpBarSetFn>(MakeTrampoline(FN_HELP_BAR_SET, 5));
    WriteJump(FN_HELP_BAR_SET, &HelpBarSet, 5);
    {   // split-mode Customize straight to the car shop (shared garage)
        const uint8_t* b = reinterpret_cast<const uint8_t*>(AT_CUSTOMIZE_MODE4);
        if (b[0] == 0x83 && b[1] == 0xFF && b[2] == 0x04 && b[3] == 0x75) {
            uint8_t jmp[5] = {0xEB, static_cast<uint8_t>(b[4] + 3), 0x90, 0x90, 0x90};
            Write(AT_CUSTOMIZE_MODE4, jmp, 5);
        }
    }

    g_origPollPort = reinterpret_cast<PollPortFn>(MakeTrampoline(FN_POLL_PORT, 5));
    WriteJump(FN_POLL_PORT, &PollPortHook, 5);

    g_origLayoutApply = reinterpret_cast<VoidFn>(MakeTrampoline(FN_LAYOUT_APPLY, 5));
    WriteJump(FN_LAYOUT_APPLY, &LayoutApplyHook, 5);

    WriteJump(AT_WORLD_VIEWS, &WorldViewsHook, 5);
    g_origStreamZones = reinterpret_cast<StreamZonesFn>(MakeTrampoline(FN_STREAM_ZONES, 7));
    WriteJump(FN_STREAM_ZONES, &StreamZonesHook, 7);
    WriteCall(AT_STREAM_PRIORITY_CALL, &StreamPriorities);
    g_origStreamSections = reinterpret_cast<StreamSectionsFn>(MakeTrampoline(FN_STREAM_SECTIONS, 6));
    WriteJump(FN_STREAM_SECTIONS, &StreamSectionsHook, 6);
    for (uintptr_t site : kCreateHumanRacersCalls) WriteCall(site, &CreateHumanRacers);
    const uint8_t humanCap = MAX_LOCAL_PLAYERS;
    Write(AT_HUMAN_MANAGER_CAP, &humanCap, 1);
    WriteCall(AT_RACER_SPEC_CALL, &RacerSpecIndexShift);
    WriteCall(AT_AI_SET_SKIN, &AiSetSkin);
    WriteCall(AT_AI_REFERENCE_CALL, &AiReferenceRacer);
    WriteCall(AT_PLAYER_HUD_CALL, &PlayerHudCtor);
    WriteCall(AT_HUD_UPDATE_CALL, &HudUpdateHook);
    WriteCall(AT_HUD_DTOR_CALL, &HudDtorHook);
    WriteCall(AT_HUD_TEX_LOAD_CALL, &HudTexLoadHook);
    WriteCall(AT_HUD_TEX_UNLOAD_CALL, &HudTexUnloadHook);
    WriteJump(FN_RACER_OF_PLAYER, &RacerOfPlayer, 5);
    WriteCall(AT_FE_RENDER_OBJ_CALL, &RenderFeObject);
    WriteCall(AT_MINIMAP_UPDATE_CALL, &MinimapUpdate);
    WriteCall(AT_MINIMAP_DESTROY_CALL, &DestroyMinimapManager);
    for (uintptr_t at : kBuildStandingsCalls) WriteCall(at, &BuildStandings);
    WriteCall(AT_RACE_RESULTS_CALL, &RaceResults);
    WriteCall(AT_FALL_RESET_CALL, &FallResetHook);
    g_origCarNotVisible = reinterpret_cast<CarNotVisibleFn>(MakeTrampoline(FN_CAR_NOT_VISIBLE, 5));
    WriteJump(FN_CAR_NOT_VISIBLE, &CarNotVisible, 5);
    g_origAssignGrid = reinterpret_cast<AssignGridFn>(MakeTrampoline(FN_ASSIGN_GRID, 6));
    WriteJump(FN_ASSIGN_GRID, &AssignGrid, 6);
    WriteCall(kRaceSetupCalls[0][0], &RaceSetup5260B0);
    WriteCall(kRaceSetupCalls[1][0], &RaceSetup526970);
    WriteCall(kRaceSetupCalls[2][0], &RaceSetup526970);
    const uint8_t markerPool = 0x7F;                    // 0x66 -> 127 marker descriptors
    Write(AT_MARKER_POOL_SIZE, &markerPool, 1);
    g_mainThread = GetCurrentThreadId();
    InitializeCriticalSection(&g_poolLock);
    g_origPoolAlloc = MakeTrampoline(FN_GAME_POOL_ALLOC, 8);
    WriteJump(FN_GAME_POOL_ALLOC, &PoolAllocLocked, 8);
    g_origPoolFree = MakeTrampoline(FN_GAME_POOL_FREE, 7);
    WriteJump(FN_GAME_POOL_FREE, &PoolFreeLocked, 7);
    WriteCall(AT_SOUND_FS_CREATE_CALL, &CreateSoundFs);
    WriteCall(AT_TRACK_POOL_CALL, &CreateTrackStreamingPool);
    WriteCall(AT_SECOND_POOL_CALL, &CreateSecondStreamingPool);
    WriteCall(AT_PAUSE_MASK_CALL, &PauseInputMask);
    g_origPausingPlayer = MakeTrampoline(FN_PAUSING_PLAYER, 5);
    WriteJump(FN_PAUSING_PLAYER, &PausingPlayer, 5);
    InitializeCriticalSection(&g_soundFsLock);
    {
        g_soundFsOpen = *reinterpret_cast<uintptr_t*>(SOUND_FS_VT_OPEN);
        void* open = reinterpret_cast<void*>(&SoundFsOpen);
        Write(SOUND_FS_VT_OPEN, &open, 4);
        g_soundFsClose = *reinterpret_cast<uintptr_t*>(SOUND_FS_VT_CLOSE);
        void* close = reinterpret_cast<void*>(&SoundFsClose);
        Write(SOUND_FS_VT_CLOSE, &close, 4);
    }
    g_origSendMsg = reinterpret_cast<SendMsgFn>(MakeTrampoline(FN_FE_SEND_MSG, 5));
    WriteJump(FN_FE_SEND_MSG, &SendFeMessage, 5);
    WriteCall(AT_HUD_STATE_CALL, &HudStateOf);
    WriteJump(AT_DRIFT_GAP, &DriftGapHook, 5);
    WriteJump(AT_RACE_GAP, &RaceGapHook, 6);
    {   // in-race name labels (0x4C7006 / 0x4C93CF): human names from the per-player block by index -> the player's own
        // name buffer (+0x30; same text for players 1/2, "Player 3/4" for the extra players)
        static const uint8_t leaEdx[15] = {0x8D, 0x4A, 0x30, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t leaEcx[15] = {0x8D, 0x49, 0x30, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        if (*reinterpret_cast<const uint32_t*>(0x4C7006) == 0x69104A8B) Write(0x4C7006, leaEdx, 15);
        if (*reinterpret_cast<const uint32_t*>(0x4C93CF) == 0x6910498B) Write(0x4C93CF, leaEcx, 15);
    }
    Log("shared garage: %d sites", ShareGarage());
    Log("player settings readers redirected: %d of %d", RedirectPlayerSettings(), static_cast<int>(sizeof(kSettingsReaders) / sizeof(kSettingsReaders[0])));
    WriteCall(AT_INTRO_CONTEXT_CALL, &IntroContext);
    WriteCall(AT_INTRO_2P_FILTER, &Intro2PFilter);
    WriteJump(AT_INTRO_EMPTY, &IntroEmptyHook, 5);
    WriteCall(AT_BINDINGS_CLEAR_CALL, &ClearHudBindings);
    WriteCall(AT_CREATE_PLAYERS_CALL, &CreatePlayers);
    WriteCall(AT_SKY_CALL, &DrawSky);
    WriteJump(AT_STREAKS_BRANCH, &StreaksBranchHook, 8);
    WriteCall(AT_STREAK_VIEW_CALL, &StreakView);
    WriteCall(AT_STREAK_COMPOSITE_CALL, &StreakComposite);
    WriteCall(AT_BLIT_QUAD_READY, &BlitQuadReadyHook);
    WriteCall(AT_STREAK_AA_COPY, &StreakAACopy);
    WriteCall(AT_BLUR_CALL, &MotionBlurForView);
    WriteCall(AT_BLUR_VIEW_CALL1, &BlurView);
    WriteCall(AT_BLUR_VIEW_CALL2, &BlurView);
    {
        uint8_t blurCopy[6] = {0xE8, 0, 0, 0, 0, 0x90};
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&BlurCopy) - (AT_BLUR_COPY + 5));
        memcpy(blurCopy + 1, &rel, 4);
        Write(AT_BLUR_COPY, blurCopy, 6);
    }
    WriteJump(AT_VIEWLOOP_TOP, &ViewLoopTopHook, 5);
    WriteJump(AT_VIEWLOOP_EXIT, &ViewLoopExitHook, 6);
    const uint8_t lastView = MAX_LOCAL_PLAYERS;  // loop counter 1..4 -> views 1, 2, 5, 6 (inactive ones are skipped)
    Write(AT_VIEWLOOP_MAX, &lastView, 1);

    WriteJump(AT_PROJECTION, &ProjectionHook, 10);

    const uint8_t jmp = 0xEB;                    // never remap to the reduced 2P sections
    Write(AT_SECTION_REMAP, &jmp, 1);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Install(module);
    }
    return TRUE;
}
