// Part of NFSU2SplitScreen.cpp: Free Run in split screen.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- Free Run in split screen
// The quick race mode list (UIQRModeSelect::Setup, 0x4B2E..) adds Outrun as the 7th mode in modes 8/0x200/4 (split
// screen = 4) and Free Run (race type 9, icon class 0x790FFC) only in the others: Free Run is added in split screen too.
#define AT_MODE_LIST_FREE_RUN 0x4B2F99   // je (mode 4: skip Free Run), 2 bytes
// The HUD's outrun element (HUD +0x6C, ctor 0x4F87E0: Getaway_Distance_Bar, Distance_Number, ...; outrun challenges
// in Free Run) binds objects only the outrun HUD packages have: with the split race HUDs they are null and its update
// crashed once driving started (0x4C67E9, user). Without its objects the element is not updated.
#define AT_OUTRUN_ELEMENT_UPDATE 0x4EC69A   // call 0x4DABD0 (thiscall element; arg)
#define FN_OUTRUN_ELEMENT_UPDATE 0x4DABD0
static void __fastcall OutrunElementUpdate(uint8_t* element, void*, int arg) {
    if (!*reinterpret_cast<void**>(element + 0x28) || !*reinterpret_cast<void**>(element + 0x2C)) return;
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_OUTRUN_ELEMENT_UPDATE)(element, arg);
}
// When driving starts, Free Run switches the race to its free state (0x89E7B0 = 7, 0x607190) and sets the local player
// count from the human cars ([[0x890080]+0x14], 0x6071D5): 3-4 with players 3/4, and the game's 2-player paths (split
// views) stopped -> dark screen (user). Split screen keeps 2.
#define AT_FREE_STATE_PLAYERS 0x6071D2   // mov ecx, [eax+0x14]; mov [0x89E7C0], ecx (9 bytes)
#define FREE_STATE_PLAYERS_RET 0x6071DB
static __declspec(naked) void FreeStatePlayersHook() {
    __asm {
        mov ecx, dword ptr [eax + 0x14]
        cmp ecx, 2
        jle store
        cmp dword ptr ds:[GAME_MODE], 4
        jne store
        mov ecx, 2
    store:
        mov dword ptr ds:[NUM_PLAYERS], ecx
        push FREE_STATE_PLAYERS_RET
        ret
    }
}
// The screen after the mode select (0x4B2A80): in split screen Outrun goes to the race options (0x7F4460), the rest to
// the track list - except Free Run, which goes to its own screen (0x7F446C: straight to the car select, no Players
// row). Split screen's Free Run goes to the race options like Outrun, on track 4000 (the whole city from the airport;
// user: no track choice).
#define FN_NEXT_SCREEN_AFTER_MODE 0x4B2A80   // const char* __cdecl (): 5 bytes stolen
#define SCREEN_RACE_OPTIONS 0x7F4460
#define FREE_RUN_TRACK     4000
using NextScreenFn = uint32_t(__cdecl*)();
static NextScreenFn g_origNextScreen;
static void SetFreeRunTrack();
static uint32_t __cdecl NextScreenAfterMode() {
    if (SplitMode() && at<int>(QR_RACE_TYPE) == 9) {
        SetFreeRunTrack();   // 4000, or the chosen venue's track ("Start" row below)
        return at<uint32_t>(SCREEN_RACE_OPTIONS);
    }
    return g_origNextScreen();
}
// Back from the race options (0x4B2AF0, the screen's "previous" function): split screen's Outrun goes back to the mode
// select (0x7F445C), the rest to the track list, which Free Run skips. Free Run goes back to the mode select too.
#define FN_PREV_SCREEN_RACE_OPTIONS 0x4B2AF0   // const char* __cdecl (): 5 bytes stolen
#define SCREEN_MODE_SELECT 0x7F445C
static NextScreenFn g_origPrevScreen;
static uint32_t __cdecl PrevScreenRaceOptions() {
    if (SplitMode() && at<int>(QR_RACE_TYPE) == 9) return at<uint32_t>(SCREEN_MODE_SELECT);
    return g_origPrevScreen();
}
// Free Run's option rows (0x4B4A10): the Track Direction row (vtable 0x791608, settings +4) when the track can be
// reversed, then traffic. Split screen has no direction row (forward only); our "Start" row takes its place.
#define AT_FREE_RUN_DIRECTION_ROW 0x4B4A42   // mov ecx, 0x83AAB4 (5 bytes)
#define FREE_RUN_DIRECTION_ROW_RET  0x4B4A47
#define FREE_RUN_DIRECTION_ROW_SKIP 0x4B4AD2
static void __cdecl AddStartRow(uint8_t* screen);
static __declspec(naked) void FreeRunDirectionRowHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        mov ecx, 0x83AAB4
        push FREE_RUN_DIRECTION_ROW_RET
        ret
    skip:
        pushad
        push esi                     // the options screen
        call AddStartRow
        add esp, 4
        popad
        push FREE_RUN_DIRECTION_ROW_SKIP
        ret
    }
}
// Outrun challenges in Free Run (user: "the point is FREE roam"): in the free state (0x89E7B0 == 7) the race update runs
// the challenge logic (0x607CA9..0x608C01: picks an opponent, race +0xBD0, and ends the race when the player pulls
// away), and the player update points a 3D direction arrow at that opponent (0x5F3AE0, MARKER_DIRECTION_AID). Both
// are skipped in split screen.
#define AT_FREE_STATE_RACE  0x607C9C     // cmp dword ptr [0x89E7B0], 7; jne 0x608C01 (13 bytes)
#define FREE_STATE_RACE_RUN 0x607CA9
#define FREE_STATE_RACE_END 0x608C01
static __declspec(naked) void FreeStateRaceHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        cmp dword ptr ds:[0x89E7B0], 7
        jne skip
        push FREE_STATE_RACE_RUN
        ret
    skip:
        push FREE_STATE_RACE_END
        ret
    }
}
#define FN_DIRECTION_ARROW  0x5F3AE0     // void __thiscall (player; int): 6 bytes stolen; ret 4
static void* g_origDirectionArrow;
static __declspec(naked) void DirectionArrowHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        jmp dword ptr [g_origDirectionArrow]
    skip:
        ret 4
    }
}
// Start location ("Start" row, split screen only). City districts keep track 4000 (the whole city, no barriers) and
// move its start spots; closed venues (speedways, drift areas) load their own track id and keep its spots. The race
// start placement (0x6065B0) calls 0x5EB5D0(6, out) at 0x6065C6 during the loading screen: it reads the track's
// Forward/BackwardStartingPosition markers into 6 entries of 0x20 bytes (x, y, z floats at +0, heading word +0x10,
// angle = h * 2pi / 65536, forward = (cos, sin)). The streamer then loads around the placed cars, so moved spots
// come up fully loaded. A start with a spot gets the game's 6-car grid there (kStartGrid) instead of the marker read:
// the venues' base tracks (4700, 4300: the areas without a race layout) have no start markers, and 0x5EB5D0's
// fallback then crashed (null at 0x5DA392). District spots are spot 0
// of a race there (4101, 4121, 4063, 4142).
#define AT_START_SPOTS 0x6065C6          // call 0x5EB5D0 (int n, entry* out), cdecl
#define FN_START_SPOTS 0x5EB5D0
#define CURRENT_TRACK  0x89E7A0          // the loaded race's track id
struct FreeRunStart { const char* name; uint16_t track; float x, y, z; uint16_t heading; };
static const FreeRunStart kFreeRunStarts[] = {
    {"Airport", FREE_RUN_TRACK, 0, 0, 0, 0},                           // 4000's own spots
    {"City Core", FREE_RUN_TRACK, -293.6f, -67.0f, 12.7f, 48352},
    {"Beacon Hill", FREE_RUN_TRACK, -1613.0f, 551.2f, 3.8f, 49152},
    {"Jackson Heights", FREE_RUN_TRACK, -2077.0f, 1928.2f, 190.1f, 57436},
    {"Coal Harbor", FREE_RUN_TRACK, -125.9f, -1412.0f, 13.6f, 1692},
    {"Random", FREE_RUN_TRACK, 0, 0, 0, 0},                            // one of the districts above, per start
    {"Speedway Industrial", 4700, -1646.6f, -1983.0f, -15.0f, 38034},   // base tracks: spots of 4701, 4711,
    {"Speedway Drift Pad", 4700, -1745.2f, -1946.8f, 0.0f, 4915},       // 4708 (SW Drift/StreetX Tune)
    {"Speedway Airport", 4700, 2453.0f, -452.0f, 0.0f, 16384},          // 4311, 4301
    {"Stadium (drift)", 4300, -1183.9f, -674.9f, 6.6f, 0},
    {"Vacant Lot (drift)", 4300, 812.7f, -323.9f, 28.8f, 0},
};
static const int kFreeRunRandom = 5, kFreeRunStartCount = sizeof(kFreeRunStarts) / sizeof(kFreeRunStarts[0]);
// The game's grid (4141's markers): 4 abreast 3.5 m apart, 2 more 5.7 m behind. {metres back, metres to the side}
static const float kStartGrid[6][2] = {{0, 0}, {0, -3.5f}, {0, 3.5f}, {0, -7}, {5.7f, 0}, {5.7f, -3.5f}};
static int g_freeRunStart;               // the row's choice; [FREERUN] Start in the ini is the default
static float g_debugStart[4];            // [DEBUG] FreeRunStart = x y z heading: overrides the district's spot 0
static bool g_debugStartSet;
static void SetFreeRunTrack() {
    at<uint16_t>(QR_SETTINGS + 9 * 0x14 + 8) = kFreeRunStarts[g_freeRunStart].track;
    at<uint32_t>(QR_SETTINGS + 9 * 0x14 + 0xC) = 0;   // track direction forward (its row is hidden)
}
static void __cdecl StartSpots(int n, uint8_t* out) {
    int pick = g_freeRunStart;
    if (pick == kFreeRunRandom) pick = static_cast<int>(GetTickCount() % kFreeRunRandom);
    FreeRunStart to = kFreeRunStarts[pick];
    if (g_debugStartSet)
        to.x = g_debugStart[0], to.y = g_debugStart[1], to.z = g_debugStart[2],
        to.heading = static_cast<uint16_t>(g_debugStart[3]);
    bool move = SplitMode() && at<int>(QR_RACE_TYPE) == 9 && at<int>(CURRENT_TRACK) == to.track &&
                (to.x != 0 || to.y != 0);
    if (!move) reinterpret_cast<void(__cdecl*)(int, uint8_t*)>(FN_START_SPOTS)(n, out);
    float a = to.heading * 6.2831853f / 65536.0f, fx = cosf(a), fy = sinf(a);   // forward; side = (-fy, fx)
    for (int i = 0; i < n; ++i) {
        auto* p = reinterpret_cast<float*>(out + i * 0x20);
        if (move) {
            float back = kStartGrid[i % 6][0], side = kStartGrid[i % 6][1];
            p[0] = to.x - back * fx - side * fy;
            p[1] = to.y - back * fy + side * fx;
            p[2] = to.z;
            *reinterpret_cast<uint16_t*>(out + i * 0x20 + 0x10) = to.heading;
        }
        if (g_debug)
            Log("start spot %d: track %d  %.1f %.1f %.1f  heading %u%s", i, at<int>(CURRENT_TRACK), p[0], p[1], p[2],
                *reinterpret_cast<uint16_t*>(out + i * 0x20 + 0x10), move ? " (moved)" : "");
    }
    if (move) Log("free run start: %s", kFreeRunStarts[pick].name);
}
// Free Run's starter (0x53F580) runs the free state only on track 4000: race state 0 (then 7, the free state) and 3
// in 0x89E7C4, else state 1, a normal race (checkpoints, split times, bonuses on a race track). The venues' base
// tracks are free roam too.
#define AT_FREE_RUN_TRACK_CHECK 0x53F5A0   // cmp esi, 4000; sete bl (9 bytes)
#define FREE_RUN_TRACK_CHECK_RET 0x53F5A9
static __declspec(naked) void FreeRunTrackCheckHook() {
    __asm {
        mov bl, 1
        cmp esi, FREE_RUN_TRACK
        je done
        cmp dword ptr ds:[GAME_MODE], 4
        je done
        xor bl, bl
    done:
        push FREE_RUN_TRACK_CHECK_RET
        ret
    }
}
// Barriers on the venues' base tracks. Each layout's barriers are a scenery group "BARRIERS_<track>" (list at 0x88F760:
// node +0x28 name hash, +0x2C on, +0x30 count, +0x34 object ids) that the race load 0x578060 switches on; the base
// tracks switch none, so every barrier stays hidden (object entry 0x5D6430(id) +4 bit 0x10). Their collision is not
// part of the group: every layout's barriers have vertical wall polygons in the region's collision mesh (identical in
// a race and in free roam, no layout tag), so the hidden barriers were invisible walls (user). Removing the walls
// opened areas without ground collision (cars fell and were reset, user). Per venue (user): the stadium shows the
// barriers on its wall lines (ShowStadiumBarriers: one per stretch of line, gaps filled with angled pieces); the
// speedways show none, and the airport one gets its walls drawn (DrawVenueWalls; they stand off the barriers); the
// vacant lot shows none. Collision packs (0x88F770, per section): records +8 (0x20 each), count +0xC; record +3 flags
// (0x16: the infinitely tall invisible walls, the ones that collide here; 0x12: finite layout walls, they don't), x / y
// int16 / 8 at +8 / +0x10 (4 vertices, a wall is two vertical pairs). Instances: section table 0x883EC0 -> +4 -> +0x20
// array (0x40 each, box +0 / +0xC). Only loaded sections have both, so it runs every 0.5 s from the race load; showing
// = clear 0x10 and apply to the instance (0x5DB0E0), as the group switch 0x5710A0 does.
#define SCENERY_GROUPS         0x88F760
#define FN_STRING_HASH         0x43DB50   // uint32 __cdecl (const char*)
#define FN_SCENERY_OBJECT      0x5D6430   // entry* __cdecl (id)
#define FN_APPLY_SCENERY_FLAGS 0x5DB0E0   // void __thiscall (entry)
#define SCENERY_SECTIONS       0x883EC0
#define COLLISION_PACKS        0x88F770
#define COLLISION_PACK_COUNT   0x208
#define RACE_STATE             0x89E7B0
#define STADIUM_FIRST_LAYOUT   4311
#define STADIUM_LAST_LAYOUT    4315
#define AIRPORT_SPEEDWAY_MIN_X 1000.0f   // 4700: the airport speedway lies east of x 1000, the industrial one west
struct WallSegment { float x0, y0, x1, y1, z0 = 0, z1 = 0; bool ground = false; };
static std::vector<WallSegment> g_venueWalls;            // the loaded walls, from the last pass
static std::unordered_map<long long, float> g_wallGround; // ground height at a wall end, by its int16 x / y
static bool SegmentHitsBox(const WallSegment& w, float bx0, float by0, float bx1, float by1) {
    float t0 = 0, t1 = 1, dx = w.x1 - w.x0, dy = w.y1 - w.y0;
    const float p[4] = {-dx, dx, -dy, dy}, q[4] = {w.x0 - bx0, bx1 - w.x0, w.y0 - by0, by1 - w.y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) { if (q[i] < 0) return false; continue; }
        float t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
        else { if (t < t0) return false; if (t < t1) t1 = t; }
    }
    return true;
}
static float CenterToWall(const WallSegment& w, float x, float y) {
    float dx = w.x1 - w.x0, dy = w.y1 - w.y0, len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((x - w.x0) * dx + (y - w.y0) * dy) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = w.x0 + t * dx - x, ey = w.y0 + t * dy - y;
    return sqrtf(ex * ex + ey * ey);
}
static int CellKey(float x, float y) { return (int(floorf(x / 16)) & 0xFFFF) << 16 | (int(floorf(y / 16)) & 0xFFFF); }
struct ShownBox { float x0, y0, x1, y1; };
static std::vector<ShownBox> g_shownBoxes;   // stadium barriers shown (layouts overlap their own copies of a spot)
static std::unordered_set<int> g_shownIds;   // their object ids, the game's own visible ones included
// The part of a box's extent along a wall (projected on its direction) that shown barriers on the same line (centre
// within 1.5 m of the wall) cover, 0..1. Layouts have their own copies of a spot, some parallel just off the shared line.
// The part of a box's extent along a wall that lies within the wall's own span, 0..1 (a piece across the end of a
// short wall sticks out of it).
static float WallSpan(const WallSegment& w, const ShownBox& b) {
    float dx = w.x1 - w.x0, dy = w.y1 - w.y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return 0;
    float ux = dx / len, uy = dy / len, lo = 1e30f, hi = -1e30f;
    const float xs[4] = {b.x0, b.x1, b.x0, b.x1}, ys[4] = {b.y0, b.y0, b.y1, b.y1};
    for (int i = 0; i < 4; ++i) {
        float t = (xs[i] - w.x0) * ux + (ys[i] - w.y0) * uy;
        lo = min(lo, t), hi = max(hi, t);
    }
    return hi > lo ? max(0.0f, min(hi, len) - max(lo, 0.0f)) / (hi - lo) : 0;
}
static float WallCoverage(const WallSegment& w, const ShownBox& b) {
    float dx = w.x1 - w.x0, dy = w.y1 - w.y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return 1;
    float ux = dx / len, uy = dy / len;
    auto span = [&](const ShownBox& o, float& lo, float& hi) {
        const float xs[4] = {o.x0, o.x1, o.x0, o.x1}, ys[4] = {o.y0, o.y0, o.y1, o.y1};
        lo = 1e30f, hi = -1e30f;
        for (int i = 0; i < 4; ++i) {
            float t = (xs[i] - w.x0) * ux + (ys[i] - w.y0) * uy;
            lo = min(lo, t), hi = max(hi, t);
        }
    };
    float lo, hi;
    span(b, lo, hi);
    if (hi - lo <= 0) return 1;
    std::vector<std::pair<float, float>> covered;
    for (const ShownBox& o : g_shownBoxes) {
        if (CenterToWall(w, (o.x0 + o.x1) / 2, (o.y0 + o.y1) / 2) > 1.5f) continue;
        float olo, ohi;
        span(o, olo, ohi);
        olo = max(olo, lo), ohi = min(ohi, hi);
        if (ohi > olo) covered.push_back({olo, ohi});
    }
    std::sort(covered.begin(), covered.end());
    float sum = 0, end = lo;
    for (const auto& c : covered) {
        if (c.second <= end) continue;
        sum += c.second - max(c.first, end);
        end = c.second;
    }
    return sum / (hi - lo);
}
static void CollectVenueWalls() {
    g_venueWalls.clear();
    for (int i = 0; i < COLLISION_PACK_COUNT; ++i) {
        auto* pack = at<uint8_t*>(COLLISION_PACKS + i * 4);
        if (reinterpret_cast<uintptr_t>(pack) < 0x10000) continue;
        auto* rec = *reinterpret_cast<int16_t**>(pack + 8);
        int count = *reinterpret_cast<int*>(pack + 0xC);
        if (!rec || count <= 0 || count > 100000) continue;
        for (int k = 0; k < count; ++k, rec += 0x10) {
            const int16_t* x = rec + 4; const int16_t* y = rec + 8;
            if ((reinterpret_cast<uint8_t*>(rec)[3] & ~0x20) != 0x16 || x[0] != x[1] || y[0] != y[1] || x[2] != x[3] ||
                y[2] != y[3] ||
                (x[0] == x[2] && y[0] == y[2])) continue;
            g_venueWalls.push_back({x[0] / 8.0f, y[0] / 8.0f, x[2] / 8.0f, y[2] / 8.0f});
        }
    }
}
static int ShowStadiumBarriers() {
    std::unordered_map<int, std::vector<int>> cells;
    for (int id = 0; id < static_cast<int>(g_venueWalls.size()); ++id) {
        const WallSegment& w = g_venueWalls[id];
        for (float cx = floorf(min(w.x0, w.x1) / 16); cx <= floorf(max(w.x0, w.x1) / 16); ++cx)
            for (float cy = floorf(min(w.y0, w.y1) / 16); cy <= floorf(max(w.y0, w.y1) / 16); ++cy)
                cells[CellKey(cx * 16, cy * 16)].push_back(id);
    }
    if (cells.empty()) return 0;
    uint32_t layouts[STADIUM_LAST_LAYOUT - STADIUM_FIRST_LAYOUT + 1];
    for (int t = STADIUM_FIRST_LAYOUT; t <= STADIUM_LAST_LAYOUT; ++t) {
        char name[32];
        sprintf_s(name, "BARRIERS_%d", t);
        layouts[t - STADIUM_FIRST_LAYOUT] = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(FN_STRING_HASH)(name);
    }
    // hidden stadium barriers a wall runs through, with the number of layouts using them
    std::unordered_map<int, int> uses;
    for (auto* node = at<uint8_t*>(SCENERY_GROUPS); node && node != reinterpret_cast<uint8_t*>(SCENERY_GROUPS);
         node = *reinterpret_cast<uint8_t**>(node)) {
        uint32_t hash = *reinterpret_cast<uint32_t*>(node + 0x28);
        bool stadium = false;
        for (uint32_t h : layouts) stadium |= h == hash;
        if (!stadium || *reinterpret_cast<int*>(node + 0x2C)) continue;
        int count = *reinterpret_cast<int*>(node + 0x30);
        auto* ids = reinterpret_cast<uint16_t*>(node + 0x34);
        for (int i = 0; i < count; ++i) ++uses[ids[i]];
    }
    auto object = reinterpret_cast<uint8_t*(__cdecl*)(int)>(FN_SCENERY_OBJECT);
    auto boxOf = [](uint8_t* entry) -> float* {
        auto* section = at<uint8_t*>(SCENERY_SECTIONS + *reinterpret_cast<int16_t*>(entry) * 4);
        auto* pack = section ? *reinterpret_cast<uint8_t**>(section + 4) : nullptr;
        if (!pack) return nullptr;
        return reinterpret_cast<float*>(*reinterpret_cast<uint8_t**>(pack + 0x20) +
                                        *reinterpret_cast<int16_t*>(entry + 2) * 0x40);
    };
    for (const auto& u : uses) {   // barriers the game shows anyway (shared ones): a candidate must not overlap them
        uint8_t* entry = object(u.first);
        if ((entry[4] & 0x10) || g_shownIds.count(u.first)) continue;
        if (float* box = boxOf(entry)) g_shownIds.insert(u.first), g_shownBoxes.push_back({box[0], box[1], box[3], box[4]});
    }
    // candidates: hidden barriers a wall crosses, with the wall whose line passes closest to their centre
    struct Candidate { int id, uses; uint8_t* entry; ShownBox box; int wall; float off; };
    std::vector<Candidate> candidates;
    for (const auto& u : uses) {
        uint8_t* entry = object(u.first);
        if (!(entry[4] & 0x10)) continue;
        float* box = boxOf(entry);
        if (!box || box[3] - box[0] > 15.0f || box[4] - box[1] > 15.0f) continue;   // a layout's road markings (90 m)
        float bx0 = box[0] - 0.5f, by0 = box[1] - 0.5f, bx1 = box[3] + 0.5f, by1 = box[4] + 0.5f;
        float cx0 = (box[0] + box[3]) / 2, cy0 = (box[1] + box[4]) / 2, best = 2.0f;
        int wall = -1;
        for (float cx = floorf(bx0 / 16); cx <= floorf(bx1 / 16); ++cx)
            for (float cy = floorf(by0 / 16); cy <= floorf(by1 / 16); ++cy) {
                auto c = cells.find(CellKey(cx * 16, cy * 16));
                if (c == cells.end()) continue;
                for (int w : c->second) {
                    if (!SegmentHitsBox(g_venueWalls[w], bx0, by0, bx1, by1)) continue;
                    float off = CenterToWall(g_venueWalls[w], cx0, cy0);
                    if (off < best) best = off, wall = w;
                }
            }
        if (wall >= 0) candidates.push_back({u.first, u.second, entry, {box[0], box[1], box[3], box[4]}, wall, best});
    }
    int shown = 0;
    auto show = [&](const Candidate& c) {
        g_shownBoxes.push_back(c.box);
        g_shownIds.insert(c.id);
        c.entry[4] &= ~0x10;
        reinterpret_cast<void(__thiscall*)(void*)>(FN_APPLY_SCENERY_FLAGS)(c.entry);
        ++shown;
    };
    // barriers on a wall line (centre within 0.6 m), the most shared first, unless the line is already covered there
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.uses != b.uses ? a.uses > b.uses : a.id < b.id; });
    for (const Candidate& c : candidates)
        if (c.off < 0.6f && WallCoverage(g_venueWalls[c.wall], c.box) <= 0.3f) show(c);
    // gaps left on a line (only angled pieces there): the closest remaining piece that is mostly over the gap
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.off < b.off; });
    for (const Candidate& c : candidates)
        if (c.off >= 0.6f && (c.entry[4] & 0x10) && WallSpan(g_venueWalls[c.wall], c.box) >= 0.7f &&
            WallCoverage(g_venueWalls[c.wall], c.box) <= 0.5f)
            show(c);
    return shown;
}
// Ground height under a point, as the start grid placement does it (0x5EB7AB..): a collision query object (ctor
// 0x5E0B40, dtor 0x5E0BF0, about 0x318 bytes) is filled around the point by 0x5E68D0 (+8 non-zero: ground found), and
// 0x5E7E70 returns the height. Cached per wall end, within 350 m of a player; at most 600 a pass.
#define FN_QUERY_CTOR   0x5E0B40   // void __thiscall (query)
#define FN_QUERY_DTOR   0x5E0BF0   // void __thiscall (query)
#define FN_QUERY_FILL   0x5E68D0   // void __thiscall (query; float* point, int)
#define FN_QUERY_HEIGHT 0x5E7E70   // float __thiscall (query; float* point, int, int, int)
static long long WallEndKey(float x, float y) {
    return static_cast<long long>(lroundf(x * 8)) << 32 | static_cast<uint32_t>(lroundf(y * 8));
}
static void FindWallGround(const float* cars, int count) {
    alignas(16) static uint8_t query[0x400];
    bool open = false;
    int budget = 600;
    for (WallSegment& w : g_venueWalls) {
        if (w.x0 < AIRPORT_SPEEDWAY_MIN_X) continue;
        bool close = false;
        for (int k = 0; k < count && !close; ++k) {
            float dx = w.x0 - cars[k * 3], dy = w.y0 - cars[k * 3 + 1];
            close = dx * dx + dy * dy < 350.0f * 350.0f;
        }
        if (!close) continue;
        float* z[2] = {&w.z0, &w.z1};
        const float ends[2][2] = {{w.x0, w.y0}, {w.x1, w.y1}};
        int found = 0;
        for (int e = 0; e < 2; ++e) {
            long long key = WallEndKey(ends[e][0], ends[e][1]);
            auto it = g_wallGround.find(key);
            if (it == g_wallGround.end()) {
                if (budget-- <= 0) break;
                if (!open) reinterpret_cast<void(__thiscall*)(void*)>(FN_QUERY_CTOR)(query), open = true;
                float point[3] = {ends[e][0], ends[e][1], cars[2] + 5.0f};
                reinterpret_cast<void(__thiscall*)(void*, float*, int)>(FN_QUERY_FILL)(query, point, 0);
                if (!*reinterpret_cast<int*>(query + 8)) continue;
                float h = reinterpret_cast<float(__thiscall*)(void*, float*, int, int, int)>(FN_QUERY_HEIGHT)(
                    query, point, 0, 0, 0);
                it = g_wallGround.emplace(key, h).first;
            }
            *z[e] = it->second;
            ++found;
        }
        w.ground = found == 2;
    }
    if (open) reinterpret_cast<void(__thiscall*)(void*)>(FN_QUERY_DTOR)(query);
}
static int VenuePass(bool stadium, const float* cars, int count) {
    __try {
        CollectVenueWalls();
        if (!stadium) FindWallGround(cars, count);
        return stadium ? ShowStadiumBarriers() : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static int g_venueTimer;
static bool VenueFreeRun() {
    int track = at<int>(CURRENT_TRACK);
    int state = at<int>(RACE_STATE);   // 0 from the load and through the countdown, 7 once driving
    return SplitRacing() && at<int>(QR_RACE_TYPE) == 9 && (state == 0 || state == 7) && (track == 4700 || track == 4300);
}
static void UpdateVenueBarriers() {
    if (!VenueFreeRun()) {
        if (g_venueTimer) g_venueWalls.clear(), g_shownBoxes.clear(), g_shownIds.clear(), g_wallGround.clear();
        g_venueTimer = 0;
        return;
    }
    if (g_venueTimer-- > 0) return;
    g_venueTimer = 30;
    // the stadium and the vacant lot share track 4300: the vacant lot's layouts (4301-4306) stay hidden
    float cars[MAX_LOCAL_PLAYERS * 3];
    int count = 0;
    for (int k = 0; k < g_screenViews && k < MAX_LOCAL_PLAYERS; ++k)
        if (uint8_t* car = PlayerCar(k)) memcpy(cars + 3 * count++, car + 0x60, 12);
    int shown = VenuePass(at<int>(CURRENT_TRACK) == 4300, cars, count);
    if (shown) Log("free run: track %d: %d barriers with walls shown%s", at<int>(CURRENT_TRACK), shown,
                   shown < 0 ? " (fault)" : "");
}
// The airport speedway's walls drawn in each player view: a band 1.2 m high from the ground at the wall's ends
// (FindWallGround; the walls themselves are infinitely tall planes), in 2 m red and white pieces like barrier tape,
// fading out from 220 to 300 m. Drawn in the split pass after the road's headlight pools, fixed function with the
// view's view-projection matrix (platInfo +0x80).
struct WallVertex { float x, y, z; uint32_t color; };
static void DrawVenueWalls(View* v, const float* carPos) {
    if (!VenueFreeRun() || at<int>(CURRENT_TRACK) != 4700 || g_venueWalls.empty() || !v || !v->platInfo || !carPos) return;
    void* dev = at<void*>(D3D_DEVICE);
    if (!dev) return;
    static std::vector<WallVertex> verts;
    verts.clear();
    for (const WallSegment& w : g_venueWalls) {
        if (w.x0 < AIRPORT_SPEEDWAY_MIN_X || !w.ground) continue;
        float dx = w.x1 - w.x0, dy = w.y1 - w.y0, len = sqrtf(dx * dx + dy * dy);
        int pieces = max(1, static_cast<int>(len / 2.0f + 0.5f));
        for (int i = 0; i < pieces; ++i) {
            float t0 = static_cast<float>(i) / pieces, t1 = static_cast<float>(i + 1) / pieces;
            float ax = w.x0 + dx * t0, ay = w.y0 + dy * t0, bx = w.x0 + dx * t1, by = w.y0 + dy * t1;
            float az = w.z0 + (w.z1 - w.z0) * t0, bz = w.z0 + (w.z1 - w.z0) * t1;
            float mx = (ax + bx) / 2 - carPos[0], my = (ay + by) / 2 - carPos[1], d = sqrtf(mx * mx + my * my);
            if (d > 300.0f) continue;
            float fade = d < 220.0f ? 1.0f : (300.0f - d) / 80.0f;
            uint32_t alpha = static_cast<uint32_t>(0xB0 * fade) << 24;
            uint32_t color = alpha | ((i & 1) ? 0xF0F0F0u : 0xD02020u);
            WallVertex q[6] = {{ax, ay, az, color}, {bx, by, bz, color}, {bx, by, bz + 1.2f, color},
                               {ax, ay, az, color}, {bx, by, bz + 1.2f, color}, {ax, ay, az + 1.2f, color}};
            verts.insert(verts.end(), q, q + 6);
        }
    }
    if (verts.empty()) return;
    void** vt = *reinterpret_cast<void***>(dev);
    using GetPtrFn = HRESULT(__stdcall*)(void*, void**);
    using SetPtrFn = HRESULT(__stdcall*)(void*, void*);
    using GetDwFn = HRESULT(__stdcall*)(void*, DWORD, DWORD*);
    using SetDwFn = HRESULT(__stdcall*)(void*, DWORD, DWORD);
    using GetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD*);
    using SetTssFn = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
    using XformFn = HRESULT(__stdcall*)(void*, DWORD, float*);
    void *vs = nullptr, *ps = nullptr, *decl = nullptr, *tex = nullptr;
    DWORD fvf = 0;
    reinterpret_cast<GetPtrFn>(vt[93])(dev, &vs);                                           // GetVertexShader
    reinterpret_cast<GetPtrFn>(vt[108])(dev, &ps);                                          // GetPixelShader
    reinterpret_cast<GetPtrFn>(vt[88])(dev, &decl);                                         // GetVertexDeclaration
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void**)>(vt[64])(dev, 0, &tex);      // GetTexture
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD*)>(vt[90])(dev, &fvf);                // GetFVF
    // ZENABLE, ZWRITEENABLE, ALPHATESTENABLE, CULLMODE, ALPHABLENDENABLE, SRCBLEND, DESTBLEND, LIGHTING, FOGENABLE
    static const DWORD kStates[] = {7, 14, 15, 22, 27, 19, 20, 137, 28};
    static const DWORD kSet[] = {1, 0, 0, 1, 1, 5, 6, 0, 0};
    const int nStates = static_cast<int>(sizeof(kStates) / sizeof(kStates[0]));
    DWORD rs[sizeof(kStates) / sizeof(kStates[0])];
    for (int i = 0; i < nStates; ++i) reinterpret_cast<GetDwFn>(vt[58])(dev, kStates[i], &rs[i]);
    static const DWORD kTss[4] = {1, 2, 4, 5};                            // COLOROP, COLORARG1, ALPHAOP, ALPHAARG1
    static const DWORD kTssSet[4] = {2, 0, 2, 0};                         // SELECTARG1 DIFFUSE for colour and alpha
    DWORD tss[4];
    for (int i = 0; i < 4; ++i) reinterpret_cast<GetTssFn>(vt[66])(dev, 0, kTss[i], &tss[i]);
    static const DWORD kXforms[3] = {256, 2, 3};                          // WORLD, VIEW, PROJECTION
    float saved[3][16];
    for (int i = 0; i < 3; ++i) reinterpret_cast<XformFn>(vt[45])(dev, kXforms[i], saved[i]);

    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    reinterpret_cast<XformFn>(vt[44])(dev, 256, identity);
    reinterpret_cast<XformFn>(vt[44])(dev, 2, identity);
    reinterpret_cast<XformFn>(vt[44])(dev, 3, reinterpret_cast<float*>(v->platInfo + 0x80));
    reinterpret_cast<SetPtrFn>(vt[92])(dev, nullptr);                                       // SetVertexShader
    reinterpret_cast<SetPtrFn>(vt[107])(dev, nullptr);                                      // SetPixelShader
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(vt[65])(dev, 0, nullptr);    // SetTexture
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD)>(vt[89])(dev, 0x42);                 // SetFVF XYZ | DIFFUSE
    for (int i = 0; i < nStates; ++i) reinterpret_cast<SetDwFn>(vt[57])(dev, kStates[i], kSet[i]);
    for (int i = 0; i < 4; ++i) reinterpret_cast<SetTssFn>(vt[67])(dev, 0, kTss[i], kTssSet[i]);
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, UINT, const void*, UINT)>(vt[83])(
        dev, 4, static_cast<UINT>(verts.size() / 3), verts.data(), sizeof(WallVertex));    // DrawPrimitiveUP, list

    for (int i = 0; i < 3; ++i) reinterpret_cast<XformFn>(vt[44])(dev, kXforms[i], saved[i]);
    for (int i = 0; i < 4; ++i) reinterpret_cast<SetTssFn>(vt[67])(dev, 0, kTss[i], tss[i]);
    for (int i = 0; i < nStates; ++i) reinterpret_cast<SetDwFn>(vt[57])(dev, kStates[i], rs[i]);
    reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(vt[65])(dev, 0, tex);
    if (decl) reinterpret_cast<SetPtrFn>(vt[87])(dev, decl);
    else reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD)>(vt[89])(dev, fvf);
    reinterpret_cast<SetPtrFn>(vt[92])(dev, vs);
    reinterpret_cast<SetPtrFn>(vt[107])(dev, ps);
    for (void* o : {vs, ps, decl, tex})
        if (o) reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(o))[2])(o);   // Release
}
static uint32_t g_startRowVt[18];
static void __fastcall StartRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Start");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%s", kFreeRunStarts[g_freeRunStart].name);
}
static void __fastcall StartRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    if (msg == MSG_LEFT) g_freeRunStart = (g_freeRunStart + kFreeRunStartCount - 1) % kFreeRunStartCount;
    if (msg == MSG_RIGHT) g_freeRunStart = (g_freeRunStart + 1) % kFreeRunStartCount;
    SetFreeRunTrack();
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}
static void __cdecl AddStartRow(uint8_t* screen) {
    if (!g_startRowVt[0]) {
        memcpy(g_startRowVt, reinterpret_cast<void*>(ROW_VTABLE_LAPS), sizeof(g_startRowVt));
        g_startRowVt[1] = reinterpret_cast<uint32_t>(&StartRowAct);
        g_startRowVt[3] = reinterpret_cast<uint32_t>(&StartRowDraw);
    }
    AddRow(screen, g_startRowVt);
}
static void InstallFreeRun() {
    g_freeRunStart = GetPrivateProfileIntA("FREERUN", "Start", 0, g_iniPath);
    if (g_freeRunStart < 0 || g_freeRunStart >= kFreeRunStartCount) g_freeRunStart = 0;
    if (g_debug) {
        char val[64];
        GetPrivateProfileStringA("DEBUG", "FreeRunStart", "", val, sizeof(val), g_iniPath);
        g_debugStartSet = sscanf_s(val, "%f %f %f %f", &g_debugStart[0], &g_debugStart[1], &g_debugStart[2],
                                   &g_debugStart[3]) == 4;
    }
    WriteCall(AT_START_SPOTS, &StartSpots);
    WriteJump(AT_FREE_RUN_TRACK_CHECK, &FreeRunTrackCheckHook, 9);
    const uint8_t nops[2] = {0x90, 0x90};
    Write(AT_MODE_LIST_FREE_RUN, nops, 2);
    g_origNextScreen = reinterpret_cast<NextScreenFn>(MakeTrampoline(FN_NEXT_SCREEN_AFTER_MODE, 5));
    WriteJump(FN_NEXT_SCREEN_AFTER_MODE, &NextScreenAfterMode, 5);
    g_origPrevScreen = reinterpret_cast<NextScreenFn>(MakeTrampoline(FN_PREV_SCREEN_RACE_OPTIONS, 5));
    WriteJump(FN_PREV_SCREEN_RACE_OPTIONS, &PrevScreenRaceOptions, 5);
    WriteJump(AT_FREE_RUN_DIRECTION_ROW, &FreeRunDirectionRowHook, 5);
    WriteCall(AT_OUTRUN_ELEMENT_UPDATE, &OutrunElementUpdate);
    WriteJump(AT_FREE_STATE_PLAYERS, &FreeStatePlayersHook, 9);
    WriteJump(AT_FREE_RUN_SKIN, &FreeRunSkinHook, 7);
    WriteJump(AT_FREE_STATE_RACE, &FreeStateRaceHook, 13);
    g_origDirectionArrow = MakeTrampoline(FN_DIRECTION_ARROW, 6);
    WriteJump(FN_DIRECTION_ARROW, &DirectionArrowHook, 6);
}
