// Part of NFSU2SplitScreen.cpp: sound listeners (one per local player).
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- sound listeners (one per local player)
// The audio mixes each sound against one of 2 listeners (split screen: P1's and P2's cameras). Listener state is 14
// 2-entry arrays (old base, stride below) written by the listener update 0x46D7E0 (hand-unrolled for listener 0 and
// 1: audio player list [[0x82B884]+0xAC] nodes with index +0x10 0/1 -> node+0x30 -> +0x18 -> +0x238 -> +0x1C camera)
// and read per sound by its listener index (sound+0xD0, chosen as the nearer of 2 at 0x45E147). Arrays move to
// MAX_LOCAL_PLAYERS-entry storage (all .text references rewritten); listeners 2.. are filled by running the update again
// per pair, with players 3/4 (5/6) list nodes posing as 0/1 and their slots swapped in; each sound picks the nearest.
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
        char line[256];
        int len = wsprintfA(line, "listeners:");
        for (int k = 0; k < n; ++k)
            len += sprintf_s(line + len, sizeof(line) - len, " %s%.1f,%.1f", k ? "| " : "", pos(k)[0], pos(k)[1]);
        Log("%s", line);
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

// ---------------------------------------------------------------- DirectSound buffer log (Debug=1)
// How the EA sound engine feeds DirectSound (one device, DirectSoundCreate through the IAT thunk 0x770D16): per-voice
// secondary buffers, or one stream it mixes itself. Logs every buffer created or duplicated (flags, format, size) and
// every 5 s the Play / Lock calls and the distinct buffers they touched.
#define IMPORT_DIRECTSOUNDCREATE 0x78302C
typedef HRESULT(WINAPI* DirectSoundCreateFn)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
typedef HRESULT(WINAPI* CreateSoundBufferFn)(IDirectSound*, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER*, LPUNKNOWN);
typedef HRESULT(WINAPI* DuplicateSoundBufferFn)(IDirectSound*, LPDIRECTSOUNDBUFFER, LPDIRECTSOUNDBUFFER*);
typedef HRESULT(WINAPI* BufPlayFn)(IDirectSoundBuffer*, DWORD, DWORD, DWORD);
typedef HRESULT(WINAPI* BufLockFn)(IDirectSoundBuffer*, DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD);
static DirectSoundCreateFn g_origDsCreate;
static CreateSoundBufferFn g_origCreateBuffer;
static DuplicateSoundBufferFn g_origDuplicate;
struct DsBufVtable { void** vt; BufPlayFn play; BufLockFn lock; };
static DsBufVtable g_dsBufVtables[4];
static CRITICAL_SECTION g_dsLogLock;
static int g_dsCreated, g_dsDuplicated, g_dsPlays, g_dsLocks;
static std::unordered_set<void*> g_dsPlayed, g_dsLocked;
static DWORD g_dsWindowStart;

static void DsLogWindow() {      // under g_dsLogLock
    DWORD now = GetTickCount();
    if (now - g_dsWindowStart < 5000) return;
    Log("dsound 5s: %d plays on %d buffers, %d locks on %d buffers (created %d, duplicated %d)", g_dsPlays,
        static_cast<int>(g_dsPlayed.size()), g_dsLocks, static_cast<int>(g_dsLocked.size()), g_dsCreated,
        g_dsDuplicated);
    g_dsPlays = g_dsLocks = 0;
    g_dsPlayed.clear();
    g_dsLocked.clear();
    g_dsWindowStart = now;
}
static DsBufVtable* DsVtableOf(IDirectSoundBuffer* b) {
    void** vt = *reinterpret_cast<void***>(b);
    for (DsBufVtable& v : g_dsBufVtables)
        if (v.vt == vt) return &v;
    return nullptr;
}
static HRESULT WINAPI DsBufPlay(IDirectSoundBuffer* b, DWORD r, DWORD prio, DWORD flags) {
    EnterCriticalSection(&g_dsLogLock);
    ++g_dsPlays;
    g_dsPlayed.insert(b);
    DsLogWindow();
    LeaveCriticalSection(&g_dsLogLock);
    return DsVtableOf(b)->play(b, r, prio, flags);
}
static HRESULT WINAPI DsBufLock(IDirectSoundBuffer* b, DWORD off, DWORD n, LPVOID* p1, LPDWORD n1, LPVOID* p2,
                                LPDWORD n2, DWORD flags) {
    EnterCriticalSection(&g_dsLogLock);
    ++g_dsLocks;
    g_dsLocked.insert(b);
    DsLogWindow();
    LeaveCriticalSection(&g_dsLogLock);
    return DsVtableOf(b)->lock(b, off, n, p1, n1, p2, n2, flags);
}
static void DsHookBufferVtable(IDirectSoundBuffer* b) {     // under g_dsLogLock
    void** vt = *reinterpret_cast<void***>(b);
    if (DsVtableOf(b)) return;
    for (DsBufVtable& v : g_dsBufVtables) {
        if (v.vt) continue;
        v = {vt, reinterpret_cast<BufPlayFn>(vt[12]), reinterpret_cast<BufLockFn>(vt[11])};
        DWORD old;
        VirtualProtect(vt + 11, 2 * sizeof(void*), PAGE_READWRITE, &old);
        vt[11] = reinterpret_cast<void*>(&DsBufLock);
        vt[12] = reinterpret_cast<void*>(&DsBufPlay);
        VirtualProtect(vt + 11, 2 * sizeof(void*), old, &old);
        Log("dsound: buffer vtable %p hooked", vt);
        return;
    }
    Log("dsound: buffer vtable %p not hooked (table full)", vt);
}
static HRESULT WINAPI DsCreateBuffer(IDirectSound* ds, LPCDSBUFFERDESC d, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN u) {
    HRESULT hr = g_origCreateBuffer(ds, d, out, u);
    EnterCriticalSection(&g_dsLogLock);
    int n = ++g_dsCreated;
    const WAVEFORMATEX* f = d->lpwfxFormat;
    if (n <= 100 || n % 100 == 0)
        Log("dsound: create #%d hr %08lX flags %05lX bytes %lu fmt %s%u ch %lu Hz %u bit", n, hr, d->dwFlags,
            d->dwBufferBytes, f ? "" : "none ", f ? f->nChannels : 0, f ? f->nSamplesPerSec : 0,
            f ? f->wBitsPerSample : 0);
    if (SUCCEEDED(hr) && out && *out) DsHookBufferVtable(*out);
    LeaveCriticalSection(&g_dsLogLock);
    return hr;
}
static HRESULT WINAPI DsDuplicate(IDirectSound* ds, LPDIRECTSOUNDBUFFER src, LPDIRECTSOUNDBUFFER* out) {
    HRESULT hr = g_origDuplicate(ds, src, out);
    EnterCriticalSection(&g_dsLogLock);
    int n = ++g_dsDuplicated;
    if (n <= 20 || n % 100 == 0) Log("dsound: duplicate #%d of %p hr %08lX", n, src, hr);
    LeaveCriticalSection(&g_dsLogLock);
    return hr;
}
static HRESULT WINAPI DsCreateHook(LPCGUID guid, LPDIRECTSOUND* out, LPUNKNOWN u) {
    HRESULT hr = g_origDsCreate(guid, out, u);
    Log("dsound: DirectSoundCreate(%s) hr %08lX", guid ? "device guid" : "default device", hr);
    if (SUCCEEDED(hr) && out && *out && !g_origCreateBuffer) {
        void** vt = *reinterpret_cast<void***>(*out);
        g_origCreateBuffer = reinterpret_cast<CreateSoundBufferFn>(vt[3]);
        g_origDuplicate = reinterpret_cast<DuplicateSoundBufferFn>(vt[5]);
        DWORD old;
        VirtualProtect(vt + 3, 3 * sizeof(void*), PAGE_READWRITE, &old);
        vt[3] = reinterpret_cast<void*>(&DsCreateBuffer);
        vt[5] = reinterpret_cast<void*>(&DsDuplicate);
        VirtualProtect(vt + 3, 3 * sizeof(void*), old, &old);
    }
    return hr;
}
static void InstallDsoundLog() {
    InitializeCriticalSection(&g_dsLogLock);
    g_dsWindowStart = GetTickCount();
    auto* slot = reinterpret_cast<void**>(IMPORT_DIRECTSOUNDCREATE);
    g_origDsCreate = reinterpret_cast<DirectSoundCreateFn>(*slot);
    void* hook = reinterpret_cast<void*>(&DsCreateHook);
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof(void*), old, &old);
}
