// Part of NFSU2SplitScreen.cpp: track streaming for players 3/4.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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

// Post-race (single view): the streamer's position 0 follows a race car (block +0 = car array index; 0x5E4F20 copies
// its +0x60 position and +0x70 velocity). The post-race scene drives the cars it doesn't feature to the world origin
// (mover 5, NISMvr, in a few frames: thousands of m/s): with 3-4 players P1 can be one (a 2-car scene with P3 and P2
// on top) and the streamer, following it across the map, dropped the finish area; nothing is loaded again on the
// post-race screen -> black world, the shown cars falling through (user). While a followed car is driven by the scene,
// the position follows the view 1 camera (still), from the scene's first frame.
static void FollowCameraForSceneCars(uint8_t* streamer) {
    if (!g_postRace || !SplitMode() || at<int>(NUM_PLAYERS) != 2 || SplitRacing()) return;
    auto* cars = at<uint8_t*>(RACE_CARS);
    auto* list = at<uint8_t*>(0x890118);
    View& v = GetView(1);
    if (!cars || !list || !v.camera) return;
    for (int k = 0; k < 2; ++k) {
        auto* block = streamer + 0xB0 + k * 0x5C;
        int idx = *reinterpret_cast<int*>(block);
        if (idx < 0 || idx >= *reinterpret_cast<int*>(list + 0x24)) continue;
        auto* car = reinterpret_cast<uint8_t**>(cars + 0x1C)[idx];
        if (!car || *reinterpret_cast<int*>(car + 0x28) != MOVER_NIS) continue;
        const float* cam = reinterpret_cast<const float*>(v.camera + 0x40);
        float pos[4] = {cam[0], cam[1], 0.0f, 0.0f};
        *reinterpret_cast<int*>(block) = -1;
        memcpy(block + 4, pos, sizeof(pos));
    }
}

static uint8_t __fastcall StreamZonesHook(void* streamer, void* edx, short* zones) {
    FollowCameraForSceneCars(static_cast<uint8_t*>(streamer));
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
