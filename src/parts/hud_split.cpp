// Part of NFSU2SplitScreen.cpp: split-screen HUD: widening, quad HUD, minimaps.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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
static bool g_mm2p = false;              // per frame: 2P race with P1's minimap objects mapped (one minimap per half)
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

// When P1 finishes, the game hides P1's HUD (hidden bit, flags & 1, on its objects) - the minimap objects every
// player's minimap is drawn from among them (user: the others' maps vanished when P1 finished). The minimap objects
// that were visible right before stay visible for the players still racing; a finished player's own minimap is not
// drawn (P1's HUD hidden; P2-P4's are drawn from P1's objects too, user: P2's stayed after P2 finished).
// The pause menus hide the HUD the same way: only a hide with P1 finished (player +0x50; racer progress +0x14 counts laps) and no menu over
// the HUD counts, and during a pause the kept objects are hidden too (user: P2's minimap showed through P1's pause).
void* GetLocalPlayer(int);
static bool g_mmOwnerHidden = false;     // P1's HUD hidden mid-race
static bool g_mmFinished[MAX_LOCAL_PLAYERS];   // per frame: player k finished -> no minimap of its own (like P1's)
static bool PlayerFinished(int k) {   // player +0x50 (the mode picker 0x5EA650 then picks mode 9, finish race)
    const uint8_t* player = static_cast<const uint8_t*>(GetLocalPlayer(k));
    return player && *reinterpret_cast<const int*>(player + 0x50) != 0;
}
static uint8_t* g_mmVisible[MM_MAX_OBJS];   // minimap objects visible on the last frame before
static int g_mmVisibleCount = 0;
static uint8_t* g_mmOwnerMap = nullptr;  // P1's map object when the kept list was taken
// P1's minimap objects, found in its live package (cached object lists can be stale: a restart rebuilds the HUD's
// objects, also at the same addresses -> crash): top-level minimap objects and every child of the minimap groups.
static void CollectVisibleMinimap(const uint8_t* obj, bool inGroup) {
    for (; obj; obj = *reinterpret_cast<const uint8_t* const*>(obj + 4)) {
        bool mm = inGroup || IsMinimapObject(obj);
        if (mm && !(*reinterpret_cast<const uint32_t*>(obj + 0x1C) & 1) && g_mmVisibleCount < MM_MAX_OBJS)
            g_mmVisible[g_mmVisibleCount++] = const_cast<uint8_t*>(obj);
        if (mm && *reinterpret_cast<const int*>(obj + 0x18) == 5)
            CollectVisibleMinimap(*reinterpret_cast<const uint8_t* const*>(obj + 0x64), true);
    }
}
static void UpdateMinimapOwner() {          // per frame
    // only while racing (game state 6): during a (re)start's load the HUD and players are being rebuilt
    bool racing = SplitRacing() && at<int>(0x8654A4) == 6;
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) g_mmFinished[k] = racing && PlayerFinished(k);
    auto* pkg = at<int>(0x8654A4) == 6 ? static_cast<const uint8_t*>(HudPackageOf(0)) : nullptr;
    uint8_t* map = pkg && g_mmFixedHash[0] ? FindFeObject(pkg, g_mmFixedHash[0]) : nullptr;
    bool same = map && map == g_mmOwnerMap;  // the kept objects still belong to the live HUD
    if (!racing || (g_extraPlayers <= 0 && !g_mm2p) || !map) {
        // hidden again: the post-race view draws P1's HUD as it is (user: a big minimap stayed on screen)
        if (g_mmOwnerHidden && same && !SplitRacing())
            for (int i = 0; i < g_mmVisibleCount; ++i) *reinterpret_cast<uint32_t*>(g_mmVisible[i] + 0x1C) |= 1u;
        g_mmOwnerHidden = false;
        g_mmVisibleCount = 0;
        g_mmOwnerMap = nullptr;
        return;
    }
    if (g_mmOwnerHidden && !same) g_mmOwnerHidden = false, g_mmVisibleCount = 0;   // a rebuilt HUD
    if (!g_hudWiden) {                   // pause menus: the HUD stays as the game has it
        if (g_mmOwnerHidden)
            for (int i = 0; i < g_mmVisibleCount; ++i) *reinterpret_cast<uint32_t*>(g_mmVisible[i] + 0x1C) |= 1u;
        return;
    }
    if (!g_mmOwnerHidden) {
        if (!(*reinterpret_cast<const uint32_t*>(map + 0x1C) & 1) || !g_mmFinished[0]) {
            g_mmVisibleCount = 0;
            CollectVisibleMinimap(*reinterpret_cast<const uint8_t* const*>(pkg + 0x4C), false);
            g_mmOwnerMap = map;
            return;
        }
        if (map != g_mmOwnerMap) return;      // no list taken from this HUD yet
        g_mmOwnerHidden = true;
        Log("minimap: player 1's HUD hidden (finished), %d minimap objects kept for the others", g_mmVisibleCount);
    }
    for (int i = 0; i < g_mmVisibleCount; ++i) *reinterpret_cast<uint32_t*>(g_mmVisible[i] + 0x1C) &= ~1u;
}

// 2P: the HUD packages keep their own layout; only P1's minimap objects are mapped, and drawn once per player, in the
// bottom-left corner of each half (MinimapUpdate, RenderFeObjectWith, MinimapPlace).
static void UpdateMinimaps2P() {
    static const void* s_pkg = nullptr;
    static int s_frames = 0;
    // by the race's players, not g_screenViews: that is still 2 from the loading screens in a 3+ player race's first
    // frame, whose minimap update then drew 3+ players before P3's car entity existed (crash 0x4AC2B3)
    const void* pkg = SplitRacing() && !g_quadTest && g_extraPlayers <= 0 ? HudPackageOf(0) : nullptr;
    if (pkg != s_pkg || ++s_frames >= 30) {   // HUD elements add markers at runtime; a restart rebuilds the HUD
        s_frames = 0;
        if (pkg != s_pkg) g_mmOwnerHidden = false, g_mmVisibleCount = 0;
        if (pkg || g_mm2p) MapMinimapObjects(static_cast<const uint8_t*>(pkg), nullptr, nullptr, nullptr);
        s_pkg = pkg;
    }
    g_mm2p = pkg && g_mmObjCount > 0;
    if (!g_mm2p) g_mmPlayers = 0;
}

static void UpdateQuadHud() {
    g_quadHud = false;
    if (g_mmExtraIcon[0] && !HudPackageOf(2)) {   // P3's HUD gone (back in the menus): the frontend reuses the memory
        g_mmExtraIcon[0] = g_mmExtraIcon[1] = nullptr;   // (help bar buttons matched these and were skipped)
        memset(g_mmMarker, 0, sizeof(g_mmMarker));
    }
    if (!SplitRacing() || g_screenViews < 3) {
        UpdateMinimaps2P();
        UpdateMinimapOwner();
        // the next race's HUD can be rebuilt at the same addresses: map it afresh (stale minimap objects -> crash on
        // restart in UpdateMinimapOwner)
        for (const void*& pkg : g_hudPackages) pkg = nullptr;
        return;
    }
    bool changed = false;
    const void* pk[MAX_LOCAL_PLAYERS] = {};
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        pk[k] = HudPackageOf(k);
        changed |= pk[k] != g_hudPackages[k];
    }
    static int frames = 0;
    if (++frames >= 30) { frames = 0; changed = true; }   // HUD elements add objects at runtime
    if (changed) {
        if (pk[0] != g_hudPackages[0]) g_mmOwnerHidden = false, g_mmVisibleCount = 0;   // a new HUD (restart)
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
    UpdateMinimapOwner();                // after the mapping: the minimap objects of the current HUD (restart: the
}                                        // old HUD's objects were freed)

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
    float ms = g_screenViews == 2 ? 0.8f : 0.55f, f = ms / 0.55f;   // 2P: the halves' HUD is full size
    float left = (k & 1) ? 0.0f : -halfW, top = (k & 2) ? 0.0f : -240.0f;
    if (k == 2 && g_screenViews == 3) left = -halfW / 2;  // 3 players: P3 centred
    if (g_screenViews == 2) left = -halfW, top = k ? 0.0f : -240.0f;   // 2 players: halves
    bool topLayout = true;               // all players use the top-half layout in quadrants
    copy[7] = left + 52.0f * f + x * ms;
    copy[8] = (topLayout ? top + 240.0f - 48.0f * f : top + 52.0f) + y * ms;
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
    if (data && level == 0 && (g_quadHud || (g_mm2p && g_mmTarget >= 0))) {   // 2P: only the minimap objects
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
