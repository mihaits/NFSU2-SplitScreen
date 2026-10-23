// Part of NFSU2SplitScreen.cpp: game addresses, state, patch helpers.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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
static bool g_xinputPads = false;         // pad layer installed: XInput pads appear as the game's DirectInput gamepads
static bool g_nativeDualSense = true;    // native DualSense pads (off while DS4Windows runs)
static int  g_dsRumbleMode = -1;         // -1 auto by firmware, 1 old, 2 new
static bool g_menuSplit = false;         // set by the main menu "Split Screen" option, consumed by Quick Race
static int  g_prevMode = 0;
#define MAX_LOCAL_PLAYERS 6
#define MAX_EXTRA_PLAYERS (MAX_LOCAL_PLAYERS - 2)   // players 3.. on top of the game's 2
// Input ports: 4 until players 5/6 get theirs. Giving ports 4/5 an id and action 1 (InputInit -> handler 0x571A20)
// broke the frontend input (splash ignored every pad): the handler writes per-port bytes [port + 0x864788] and calls
// 0x5726A0(port) / 0x50C5D0(port, ..), arrays sized for fewer ports.
#define INPUT_PORTS 4
static const int kPlayerViewId[MAX_LOCAL_PLAYERS] = {1, 2, 5, 6, 7, 8};   // views 5..8 are unused by the game
static RenderTarget g_rt[MAX_LOCAL_PLAYERS]; // private targets for the player views (one per screen region)
static int g_screenViews = 2;            // player views drawn this frame (2, or 4 for quadrants)

// The split layout: player view k's screen rect in pixels with n views on a w x h screen. The only place that knows the
// layout (viewports, HUD, minimaps, gauges and the bars around a short bottom row all read it).
//   2: top / bottom halves   3: quadrants, P3 centred in the bottom half   4: quadrants
//   5: 3 on top, 2 centred below   6: 3 x 2 grid (5/6 not reachable yet)
struct Cell { int x, y, w, h; };
static Cell LayoutCell(int n, int k, int w, int h) {
    if (n <= 2) return {0, k * (h / 2), w, h / 2};
    if (n <= 4) {
        Cell c = {(k & 1) * (w / 2), (k >> 1) * (h / 2), w / 2, h / 2};
        if (n == 3 && k == 2) c.x = w / 4;
        return c;
    }
    int row = k / 3, col = k % 3;
    Cell c = {col * w / 3, row * (h / 2), (col + 1) * w / 3 - col * w / 3, h / 2};
    if (n == 5 && row == 1) c.x += w / 6;
    return c;
}
// The same rect in FE units (x -240*aspect..240*aspect, y -240..240): left, top, right, bottom.
struct FeRect { float left, top, right, bottom; };
static FeRect LayoutFeRect(int n, int k) {
    int w = at<int>(SCREEN_W), h = at<int>(SCREEN_H);
    float halfW = 240.0f * static_cast<float>(w) / static_cast<float>(h);
    Cell c = LayoutCell(n, k, w, h);
    auto fx = [&](int px) { return (2.0f * px / w - 1.0f) * halfW; };
    auto fy = [&](int py) { return (2.0f * py / h - 1.0f) * 240.0f; };
    return {fx(c.x), fy(c.y), fx(c.x + c.w), fy(c.y + c.h)};
}
static bool g_viewTintInit[4];           // views 5..8 have their own screen tint entries (see TintUpdateWrapper)
static bool g_quadTest = false;          // debug: 2P race drawn in quadrants, views 5/6 reuse P1/P2's cameras
static int  g_extraPlayers = 0;          // debug: players 3.. added to 2P races (cloning P1/P2's car and profile)
static int  g_cloneViews = 0;            // debug: views after the players' (5P/6P layouts) showing P1's / P2's camera
// The player whose car and camera player view k shows: its own, or P1/P2 for a debug view (QuadTest, CloneViews).
static int ViewPlayer(int k) { return k >= 2 + g_extraPlayers ? (k & 1) : k; }
static int g_vpLogFrames = 0;            // debug: log SetViewport calls for N frames (mailbox command 6)
static int  g_currentView = 0;           // view being drawn by the player-view loop (read by asm)
static void* g_origSetViewport = nullptr;
static uint8_t g_portCur[MAX_LOCAL_PLAYERS][16], g_portPrev[MAX_LOCAL_PLAYERS][16];
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
