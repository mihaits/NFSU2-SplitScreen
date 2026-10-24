// Part of NFSU2SplitScreen.cpp: traffic around players 3..6.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- "near a racer" distances
// The World ([0x890080]) update 0x609CE0 copies the first +0x14 race cars (racers, at most 6) into slots: position at
// +0x80 + 0x10 * i (car +0x60), road position at +0xE0 + 4 * i (car +0x8FC). Its distance helpers give the distance to
// the nearest slot, but outside online races they only read slots 0 and 1 (P1 and P2; in single player P1 and the first
// AI): 0x5F0FB0 (xy), 0x5F1160 (xyz) and 0x5F12A0 (along the road, 0x5D5CD0 on 0x88F298). Traffic uses them to keep a
// car (0x409800: within 250 of a slot), to refuse spawn points in view (0x409840 / 0x409890 from 0x418110), to remove
// parked cars (0x40BEE0) and to move cars out of sight (TrafficController 0x42A5E0); the World update uses them for
// world triggers (0x5F5580, 100) and 0x5F5730 (150). So traffic around P3..P6 was recycled as out of range and could
// spawn in their view. With more than 2 local players the xy and road helpers also measure to players 3..6's cars
// (0x5F1160 is NearestRacerDistance in racers.cpp: every racer slot).
#define FN_WORLD_DIST_XY   0x5F0FB0      // float __thiscall (World*, const float* pos); 7 bytes stolen
#define FN_WORLD_DIST_ROAD 0x5F12A0      // float __thiscall (World*, float roadPos); 5 bytes stolen
#define FN_ROAD_DELTA      0x5D5CD0      // float __thiscall (route*, float a, float b): a - b along the route
#define ROAD_ROUTE         0x88F298
typedef float(__fastcall* WorldDistPtr)(void* world, void* edx, const float* pos);
typedef float(__fastcall* WorldDistRoad)(void* world, void* edx, float roadPos);
static WorldDistPtr g_origDistXy;
static WorldDistRoad g_origDistRoad;

static uint8_t* ExtraPlayerCar(int k) {
    auto* player = static_cast<uint8_t*>(g_localPlayers.inOrder[k]);
    return player ? *reinterpret_cast<uint8_t**>(player + 4) : nullptr;
}
static int LocalPlayerCount() {
    int n = g_localPlayers.count;
    return n < MAX_LOCAL_PLAYERS ? n : MAX_LOCAL_PLAYERS;
}
static float __fastcall WorldDistXy(void* world, void*, const float* pos) {
    float d = g_origDistXy(world, nullptr, pos);
    for (int k = 2, n = LocalPlayerCount(); k < n; ++k) {
        uint8_t* car = ExtraPlayerCar(k);
        if (!car) continue;
        const float* c = reinterpret_cast<const float*>(car + 0x60);
        float dx = pos[0] - c[0], dy = pos[1] - c[1];
        float e = sqrtf(dx * dx + dy * dy);
        if (e < d) d = e;
    }
    return d;
}
static float __fastcall WorldDistRoadHook(void* world, void*, float roadPos) {
    float d = g_origDistRoad(world, nullptr, roadPos);
    auto delta = reinterpret_cast<float(__thiscall*)(void*, float, float)>(FN_ROAD_DELTA);
    for (int k = 2, n = LocalPlayerCount(); k < n; ++k) {
        uint8_t* car = ExtraPlayerCar(k);
        if (!car) continue;
        float e = fabsf(delta(reinterpret_cast<void*>(ROAD_ROUTE), *reinterpret_cast<float*>(car + 0x8FC), roadPos));
        if (e < d) d = e;
    }
    return d;
}

// ---------------------------------------------------------------- traffic spacing per player
// The traffic update 0x42F910 fills a 3-float stack array (ebp-0x18) with each local player's traffic spacing for their
// area (0x418240(car), thiscall on the traffic manager esi), indexed by player index; index 2 is then taken by the car
// 0x5F0E30(-1). 0x42F800 reads the entry of the random player it spawns around (0x42F88C). P3 shared index 2, P4
// overwrote the saved timer (ebp-0xC), P5 the cars-per-tick limit (ebp-8, a float as int: every car each tick). Now
// players 3..6 go to g_trafficSpacing.
// The same tick moves at most ebp-8 traffic cars (0x42F9F4: 16 in drag, else 1 a second), the respawns shared by every
// player: with players apart, the cars one has passed came back slowly. The limit is now at least one per local player.
#define AT_TRAFFIC_SPACING_FILL     0x42FA19   // mov eax,[count] (5 bytes): the fill loop up to 0x42FA4B
#define AT_TRAFFIC_SPACING_FILL_END 0x42FA4B
#define AT_TRAFFIC_SPACING_READ     0x42F88C   // mov eax,[edx+10h]; fld [ecx+eax*4]; fstp [esp+18h] (10 bytes)
#define AT_TRAFFIC_SPACING_READ_END 0x42F8A3
#define FN_TRAFFIC_SPACING          0x418240
static float g_trafficSpacing[MAX_LOCAL_PLAYERS];
static void __cdecl FillTrafficSpacing(void* traffic, float* stackArr, int* carsPerTick) {
    if (*carsPerTick < LocalPlayerCount()) *carsPerTick = LocalPlayerCount();
    auto spacing = reinterpret_cast<float(__thiscall*)(void*, void*)>(FN_TRAFFIC_SPACING);
    for (int k = 0, n = LocalPlayerCount(); k < n; ++k) {
        auto* player = static_cast<uint8_t*>(g_localPlayers.inOrder[k]);
        if (!player) continue;
        int idx = *reinterpret_cast<int*>(player + 0x10);
        float s = spacing(traffic, *reinterpret_cast<void**>(player + 4));
        if (idx >= 0 && idx < 2) stackArr[idx] = s;
        else if (idx >= 2 && idx < MAX_LOCAL_PLAYERS) g_trafficSpacing[idx] = s;
    }
}
static __declspec(naked) void TrafficSpacingFillHook() {
    __asm {
        pushad
        lea eax, [ebp - 8]
        push eax
        lea eax, [ebp - 0x18]
        push eax
        push esi
        call FillTrafficSpacing
        add esp, 12
        popad
        push AT_TRAFFIC_SPACING_FILL_END
        ret
    }
}
static __declspec(naked) void TrafficSpacingReadHook() {
    __asm {
        mov eax, dword ptr [edx + 0x10]
        cmp eax, 2
        jb game
        fld dword ptr [g_trafficSpacing + eax * 4]
        jmp done
    game:
        fld dword ptr [ecx + eax * 4]
    done:
        fstp dword ptr [esp + 0x18]
        push AT_TRAFFIC_SPACING_READ_END
        ret
    }
}

static void InstallTraffic() {
    g_origDistXy = reinterpret_cast<WorldDistPtr>(MakeTrampoline(FN_WORLD_DIST_XY, 7));
    g_origDistRoad = reinterpret_cast<WorldDistRoad>(MakeTrampoline(FN_WORLD_DIST_ROAD, 5));
    WriteJump(FN_WORLD_DIST_XY, &WorldDistXy, 7);
    WriteJump(FN_WORLD_DIST_ROAD, &WorldDistRoadHook, 5);
    WriteJump(AT_TRAFFIC_SPACING_FILL, &TrafficSpacingFillHook, 5);
    WriteJump(AT_TRAFFIC_SPACING_READ, &TrafficSpacingReadHook, 10);
}
