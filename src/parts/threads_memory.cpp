// Part of NFSU2SplitScreen.cpp: audio file system and memory pool locks, streaming pools.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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

// Audio memory: the audio pool ([0x82C6D4]: 0x440330 sub-pool, created at boot by 0x4859B0) is 4 MB with sound on
// (0x3FC000 + 0x4000; imm32 at 0x4859BA). Sound banks are read into it (engine banks through 0x457A60, which refuses a
// bank bigger than the largest free block - 1 KB); a refused bank makes the EA bank load (0x7238D2) return -6 and its
// job (0x82B098 + i * 0x2C, status +0x28) never completes. The race's last bank job carries the track loader's callback
// (step 3, 0x486EA0 -> 0x486C10), so the race load stayed in game state 4 forever. Each different player car has its
// own engine bank (~115 KB, sound/Engine/CAR_nn_ENG_MB_SPU.abk); with 6 different cars the 5th engine bank no longer
// fit (user: 6P race load hang, 2026-10-24). 6 MB fits 6 engine banks with room to spare.
#define AT_AUDIO_POOL_SIZE 0x4859BA      // imm32 of `and eax, 3FC000h` (pool = that + 0x4000 with sound on)
static void EnlargeAudioPool() {
    static const uint32_t size = 0x600000 - 0x4000;
    if (at<uint32_t>(AT_AUDIO_POOL_SIZE) == 0x3FC000) Write(AT_AUDIO_POOL_SIZE, &size, 4);
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
// The pool object (streamer +0x6A4) has 192 block records: one per used block and one per free gap, in a fixed array
// at +0x1C, free list at +0xC, block list at +0x14. The allocator 0x5DA4A0 doesn't check for an empty free list: it
// takes the list's sentinel as a record, which puts a block address on the head of the block list and the next walk
// (0x5DEED0) crashes in section data. 5-6 far-apart players want about 150 sections plus about 45 gaps. The records
// are only reached through links, and the pool's dtor 0x5E4D70 empties both lists first, so extra ones can live here.
struct PoolRecord { PoolRecord* next; PoolRecord* prev; int addr, size; uint8_t used; };
#define POOL_RECORDS 192
static PoolRecord g_extraPoolRecords[320];
static void AddPoolRecords(uint8_t* streamer) {
    auto* pool = *reinterpret_cast<uint8_t**>(streamer + 0x6A4);
    if (!pool) return;
    auto* list = reinterpret_cast<PoolRecord*>(pool + 0xC);
    for (PoolRecord& r : g_extraPoolRecords) {
        r.prev = list->prev;
        r.next = list;
        list->prev->next = &r;
        list->prev = &r;
    }
}
static void __fastcall CreateTrackStreamingPool(void* streamer, void*, int bytes) {
    if (SplitPools()) {
        int players = 2 + g_extraPlayers;
        int split = at<int>(TRACK_POOL_KB_1P) * 1024 / 2 * (players + 1);
        if (split > bytes) bytes = split;
    }
    Log("track streaming pool: %d KB", bytes / 1024);
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_TRACK_POOL_CREATE)(streamer, bytes);
    if (SplitPools()) AddPoolRecords(static_cast<uint8_t*>(streamer));
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

// 0x639F20 gives back a texture set's references and unloads the textures that reach 0, then empties the set. If none
// did (another car of the same type holds them all) it returns 0 with the references gone and the list kept, so the
// next unload frees textures this car still draws, and its own unload gives the references back again.
#define AT_PURGE_RELEASE   0x63A21E      // the purge's `call 0x639F20` (thiscall, ret 4)
#define FN_TEXSET_RELEASE  0x639F20
#define TEXSET_COUNT       0x318
#define TEXSET_NAMES       0x31C
#define TEXSET_MAX         0x45
static int __fastcall PurgeReleaseTextures(void* loader, void*, uint8_t* set) {
    int released = reinterpret_cast<int(__thiscall*)(void*, void*)>(FN_TEXSET_RELEASE)(loader, set);
    int n = *reinterpret_cast<int*>(set + TEXSET_COUNT);
    if (released || n <= 0 || n > TEXSET_MAX) { Log("car purge: texture set %p unloaded", set); return released; }
    uint8_t** names = reinterpret_cast<uint8_t**>(set + TEXSET_NAMES);
    for (int i = 0; i < n; ++i)
        if (names[i]) ++*reinterpret_cast<uint16_t*>(names[i] + 0xC);
    Log("car purge: texture set %p unloaded nothing, %d references kept", set, n);
    return 0;
}
