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

static void DsNoteSound(const uint8_t* sound, int listener);   // DirectSound/EA voice log below (Debug = 1)

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
    if (g_debug) DsNoteSound(sound, best);
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
    static uintptr_t callers[8];         // each new caller once, with the game-code return addresses on its stack
    uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    for (uintptr_t& c : callers) {
        if (c == ret) break;
        if (c) continue;
        c = ret;
        uint32_t rets[8] = {};
        int m = 0;
        auto* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
        auto* top = reinterpret_cast<const uint32_t*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        for (int k = 1; sp + k < top && k < 512 && m < 8; ++k) {   // the sound thread's stack is shallow
            uint32_t v = sp[k];
            if (v < 0x401005 || v >= 0x780000) continue;
            auto* c5 = reinterpret_cast<const uint8_t*>(v);
            if (c5[-5] == 0xE8 || (c5[-2] == 0xFF && (c5[-1] & 0x38) == 0x10) ||
                (c5[-3] == 0xFF && (c5[-2] & 0x38) == 0x10) || (c5[-6] == 0xFF && (c5[-5] & 0x38) == 0x10))
                rets[m++] = v;
        }
        Log("dsound: Lock(%p, off %lu, %lu bytes) from %08X thread %lu (calls %08X %08X %08X %08X %08X %08X %08X %08X)",
            b, off, n, static_cast<unsigned>(ret), GetCurrentThreadId(), rets[0], rets[1], rets[2], rets[3],
            rets[4], rets[5], rets[6], rets[7]);
        break;
    }
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

// EA channel -> game sound (per-player audio plan). EA channel i is mixer voice i; 0x724F5D(i) applies its channel
// gains (volume [0x8B466C + i*0x90 + 0x48] times the pan matrix [0x8B4724 + i*0x88]) through 0x72905D. The game calls
// EA from inside its sound objects' methods, so the hook looks for a known 3D sound object (seen by NearestListener) on
// the stack and logs each (channel, owner) once; without one, the game-code return addresses instead.
#define FN_EA_APPLY_GAINS 0x724F5D       // push ebp; mov ebp, esp; push ebx; push esi (5 bytes)
typedef void(__cdecl* EaApplyGainsFn)(int);
static EaApplyGainsFn g_origEaApplyGains;
static std::unordered_set<const uint8_t*> g_dsSounds;
static std::unordered_set<uint64_t> g_dsChannelSeen;    // (channel, owner or first game caller) already logged

static void DsNoteSound(const uint8_t* sound, int listener) {
    EnterCriticalSection(&g_dsLogLock);
    bool added = g_dsSounds.size() < 2000 && g_dsSounds.insert(sound).second;
    LeaveCriticalSection(&g_dsLogLock);
    if (added) Log("sound %p listener %d vtable %08X", sound, listener, *reinterpret_cast<const uint32_t*>(sound));
}
// A per-car sound object (vtable 0x789000..0x78C000, +0x44 = the car's node in the audio player list): its player
// index (node +0x10), else -1.
static int CarSoundPlayer(uint32_t v) {
    if (v < 0x01000000 || v >= 0x7FFF0000 || (v & 3)) return -1;
    __try {
        uint32_t vt = *reinterpret_cast<const uint32_t*>(v);
        if (vt < 0x789000 || vt >= 0x78C000) return -1;
        uint8_t* node = *reinterpret_cast<uint8_t**>(v + 0x44);
        auto* root = at<uint8_t*>(0x82B884);
        auto* list = root ? *reinterpret_cast<uint8_t**>(root + 0xAC) : nullptr;
        int n = 0;
        for (auto* nd = list ? *reinterpret_cast<uint8_t**>(list + 0x10) : nullptr; nd && n < 16;
             nd = *reinterpret_cast<uint8_t**>(nd + 4), ++n)
            if (nd == node) return *reinterpret_cast<int*>(nd + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}
static void __cdecl EaApplyGainsHook(int channel) {
    auto* sp = reinterpret_cast<const uint32_t*>(_AddressOfReturnAddress());
    auto* top = reinterpret_cast<const uint32_t*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    const uint8_t* owner = nullptr;
    uint32_t car = 0;                    // a per-car sound object on the stack
    uint32_t rets[6] = {};
    int m = 0;
    EnterCriticalSection(&g_dsLogLock);
    for (int k = 1; sp + k < top && k < 1024; ++k) {
        uint32_t v = sp[k];
        if (!owner && g_dsSounds.count(reinterpret_cast<const uint8_t*>(v))) owner = reinterpret_cast<const uint8_t*>(v);
        if (!car && CarSoundPlayer(v) >= 0) car = v;
        if (m < 6 && v >= 0x401005 && v < 0x700000 && reinterpret_cast<const uint8_t*>(v)[-5] == 0xE8) rets[m++] = v;
    }
    uint32_t key = car ? *reinterpret_cast<const uint32_t*>(car) ^ CarSoundPlayer(car) << 28
                       : owner ? static_cast<uint32_t>(reinterpret_cast<uintptr_t>(owner)) : rets[0];
    if (g_dsChannelSeen.size() < 4000 && g_dsChannelSeen.insert(static_cast<uint64_t>(channel & 0x3F) << 32 | key).second) {
        if (car)
            Log("ea voice %d <- car sound vtable %08X player %d (%08X; game calls %08X %08X %08X %08X)", channel,
                *reinterpret_cast<const uint32_t*>(car), CarSoundPlayer(car), car, rets[0], rets[1], rets[2], rets[3]);
        else if (owner)
            Log("ea voice %d <- sound %p listener %d vtable %08X", channel, owner,
                *reinterpret_cast<const int*>(owner + 0xD0), *reinterpret_cast<const uint32_t*>(owner));
        else
            Log("ea voice %d <- no sound (game calls %08X %08X %08X %08X %08X %08X)", channel, rets[0], rets[1],
                rets[2], rets[3], rets[4], rets[5]);
    }
    LeaveCriticalSection(&g_dsLogLock);
    g_origEaApplyGains(channel);
}
static void InstallEaVoiceLog() {
    static const uint8_t prologue[5] = {0x55, 0x8B, 0xEC, 0x53, 0x56};
    if (memcmp(reinterpret_cast<void*>(FN_EA_APPLY_GAINS), prologue, 5) != 0) return;
    g_origEaApplyGains = reinterpret_cast<EaApplyGainsFn>(MakeTrampoline(FN_EA_APPLY_GAINS, 5));
    WriteJump(FN_EA_APPLY_GAINS, &EaApplyGainsHook, 5);
}


// ---------------------------------------------------------------- engines on the pad speakers ([AUDIO] PadSpeakers)
// Split racing: each player's engine plays on their Bluetooth DualSense's speaker instead of the PC mix (other sounds
// stay on the PC). EA mixes every voice itself (docs/sound.md): the render 0x7292BC (called at 0x7295DD per chunk of
// up to 256 frames) adds each voice into the output channels' float buffers ([0x8B48F0 + ch*4], 16-bit scale) with
// `call [0x8B4A04]` (0x72942B; esi = voice, edi = its gain for the channel at voice +0x1C + ch*4) and into the aux
// sends (0x729490). After a gain change (voice +1 set) 0x728B48 first renders the voice's next 16 frames itself,
// ramping from the old gains (voice +4 + ch*4) to the new ones with 0x728957(old, new, src, dst) or mixing them with
// [0x8B4A04], into the same buffers (channels 0x728C9D / 0x728CCB, aux sends 0x728C11 / 0x728C4B; esi = voice, edi =
// gain as above). All six calls are patched: an engine voice whose player has a speaker goes into that player's own
// buffers instead (no reverb). The engine objects (vtable 0x78AF30, per-frame update 0x474310 at vtable +0x48)
// tell which voice is whose: EA channel = mixer voice = [obj+0xE0] & 0xFF, player = [[[obj+0x9C]+0x30]+0x18].
// After each chunk, a player's buffers go to their pad (L+R mono, PadSpeakerGain) through XPadSpeakerWrite.
#define ENGINE_UPDATE_SLOT 0x78AF78      // vtable 0x78AF30 +0x48 = 0x474310 (thiscall, no arguments)
#define FN_ENGINE_UPDATE 0x474310
#define AT_MIX_CHANNEL_CALL 0x72942B     // call [0x8B4A04] (6 bytes)
#define AT_MIX_AUX_CALL 0x729490         // call [0x8B4A04] (6 bytes)
#define AT_RAMP_CHANNEL_CALL 0x728C9D    // call 0x728957 (5 bytes)
#define AT_RAMP_CHANNEL_MIX 0x728CCB     // call [0x8B4A04] (6 bytes)
#define AT_RAMP_AUX_CALL 0x728C11        // call 0x728957
#define AT_RAMP_AUX_MIX 0x728C4B         // call [0x8B4A04]
#define FN_EA_MIX_RAMP 0x728957          // mix-add of 16 frames with the gain ramping old -> new (cdecl)
#define AT_RENDER_CALL 0x7295DD          // call 0x7292BC
#define FN_RENDER 0x7292BC
#define EA_MIX_ADD 0x8B4A04              // mix-add(frames, gain, src, dst), cdecl
#define EA_VOICES 0x8B4A00               // mixer voices, stride 0x5C
#define EA_OUT_BUFS 0x8B48F0
#define EA_OUTPUT_RATE 0x8B44C6          // word
static bool g_padSpeakers = true;        // [AUDIO] PadSpeakers
static float g_padSpeakerGain = 4.0f;    // [AUDIO] PadSpeakerGain, percent / 100
// [AUDIO] PadSpeakerSounds default: what tells a player about their own car; weather, wind and road noise (shared,
// broadband or low: the pad's small speaker can't carry them) stay on the PC
static const char kPadSpeakerSoundsDefault[] = "CAR, CAR_SWTN, CAR_Sputter, FX_TURBO_01, FX_SHIFTING_01, FX_NITROUS, "
                                               "FX_PURGE, FX_SKID, FX_Scrape, AEMS_StichCollision";
static int g_speakerTestPad = -1;        // [DEBUG] SpeakerTestPad: player 1's speaker is this pad (automated tests)
static int g_speakerDumpSeconds = 0;     // [DEBUG] SpeakerDumpSeconds: PC mix + player 1's pad stream to a raw file
typedef void(__cdecl* EaMixAddFn)(int, float, const float*, float*);
static void* g_origEngineUpdate;
static volatile LONG g_engineVoicePlayer[64];        // local player + 1 (0: none), written by the main thread
                                                     // (engines, and the cars' patch sounds: NotePatchVoices)
static volatile DWORD g_engineVoiceSeen[64];
static volatile LONG g_engineVoiceHandle[64];        // the channel handle it was claimed with (EA reuses voices)
static volatile LONG g_speakerPad[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1, -1, -1};   // pad with a speaker, or -1
static volatile DWORD g_speakerSeen[MAX_LOCAL_PLAYERS];
static volatile LONG g_speakerFailed[MAX_LOCAL_PLAYERS];   // the pad took no samples (no audio endpoint yet): PC mix
static float g_speakerMix[MAX_LOCAL_PLAYERS][2][256];
// The player's mute button turns their pad speaker off and on during split races (pad_lights.cpp; the mute LED is lit
// while it's off); kept per seat until the game closes.
static bool g_speakerOn[MAX_LOCAL_PLAYERS] = {true, true, true, true, true, true};
static void PadSpeakerToggle(int k) {
    if (k < 0 || k >= MAX_LOCAL_PLAYERS || !g_padSpeakers) return;
    g_speakerOn[k] = !g_speakerOn[k];
    Log("pad speaker: player %d turned it %s (mute button)", k + 1, g_speakerOn[k] ? "on" : "off");
}
static bool PadSpeakerOff(int k) { return g_padSpeakers && k >= 0 && k < MAX_LOCAL_PLAYERS && !g_speakerOn[k]; }
// Each speaker stream is ~200 kbit/s of Bluetooth output (47 reports of 547 bytes a second): with 5 at once the radio
// fell behind (writes up to 100 ms late, error 31) and pads froze. So with more than kSpeakerDualSenseMax Bluetooth
// DualSense players the speakers start off (the mute button still turns one on); USB pads don't count, they use no
// radio. Set at race setup (racers.cpp) when that count changes, so the players' own toggles stay for the next races
// with the same pads. TODO: adaptive bitrate.
constexpr int kSpeakerDualSenseMax = 2;
static int g_speakerDefaultFor = -1;     // Bluetooth DualSense players the defaults were last set for
static void PadSpeakerDefaults(int players) {
    if (!g_padSpeakers) return;
    int n = 0;
    for (int k = 0; k < players && k < MAX_LOCAL_PLAYERS; ++k)
        if (g_assignedPad[k] != PAD_NONE && XPadIsBluetoothSpeakerPad(g_assignedPad[k])) ++n;
    if (n == g_speakerDefaultFor) return;
    g_speakerDefaultFor = n;
    bool on = n <= kSpeakerDualSenseMax;
    for (bool& s : g_speakerOn) s = on;
    Log("pad speakers: %d Bluetooth players, speakers start %s", n, on ? "on" : "off (mute button or DS4 touchpad turns one on)");
}

static void __fastcall EngineUpdateHook(uint8_t* obj, void* edx) {
    reinterpret_cast<void(__fastcall*)(void*, void*)>(g_origEngineUpdate)(obj, edx);
    __try {
        int32_t h = *reinterpret_cast<int32_t*>(obj + 0xE0);
        uint8_t* node = *reinterpret_cast<uint8_t**>(obj + 0x9C);
        uint8_t* n30 = node ? *reinterpret_cast<uint8_t**>(node + 0x30) : nullptr;
        void* player = n30 ? *reinterpret_cast<void**>(n30 + 0x18) : nullptr;
        int k = -1;
        for (int i = 0; i < g_localPlayers.count && i < MAX_LOCAL_PLAYERS; ++i)
            if (player && g_localPlayers.byIndex[i] == player) k = i;
        if (h < 0 || k < 0) return;
        int pad = SplitRacing() ? g_assignedPad[k] : PAD_NONE;
        if (k == 0 && g_speakerTestPad >= 0 && SplitRacing()) pad = g_speakerTestPad;
        if (pad == PAD_NONE || !XPadHasSpeaker(pad) || !g_speakerOn[k]) pad = -1;
        if (pad != g_speakerPad[k]) {
            Log("pad speaker: player %d -> pad %d (engine voice %d)", k + 1, pad, h & 0x3F);
            InterlockedExchange(&g_speakerFailed[k], 0);
            InterlockedExchange(&g_speakerPad[k], pad);
        }
        if (g_speakerFailed[k] && pad >= 0 && XPadSpeakerReady(pad)) {   // a USB pad's audio endpoint came back
            Log("pad speaker: player %d's pad %d has its speaker back", k + 1, pad);
            InterlockedExchange(&g_speakerFailed[k], 0);
        }
        g_speakerSeen[k] = GetTickCount();
        g_engineVoicePlayer[h & 0x3F] = k + 1;
        g_engineVoiceHandle[h & 0x3F] = h;
        g_engineVoiceSeen[h & 0x3F] = GetTickCount();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Patch sounds of a car: an EA patch runs a compiled program from the sound bank that starts its own sample-player
// voices ("children"), so those voices have no handle in any game object. Each car has a dozen (names from the
// definitions: CAR = the engine's layers (engine object +0xB0, 8 children), CAR_Sputter, FX_SHIFTING_01, FX_SKID,
// FX_ROADNOISE(_TRANS), FX_WIND(_Weather), FX_TURBO_01). The game's per-frame sound object loop (0x469CC0:
// `mov eax, [esi]; mov ecx, esi; call [eax+0x28]`, next object at +0x50) is patched to call SoundObjectUpdate, which
// runs the update and then, for a per-car sound object (vtable 0x789000..0x78C000, +0x44 = the car's audio node),
// records the children's voices of every patch it holds for that car's player. A patch field holds a param struct
// whose [0] is the patch player {definition, ?, +8 block}; definition [0] = program header (+8 = 0x721096 in every one
// seen), header +0x18 low byte = number of children, +0x24 instance size, +0x30 + 4i = child i's offset from the
// instance (block - 0x2C); a child is a sample player {+0/+4 bank, +8 channel handle, ...}; definition +4 = the
// patch's name. Only patches named in [AUDIO] PadSpeakerSounds go to the pad. A voice is routed while its channel
// still has the handle it was claimed with.
#define AT_SOUND_OBJECT_LOOP_CALL 0x469CC0   // 8B 06 8B CE FF 50 28 (7 bytes)
#define EA_PATCH_HEADER_SIGNATURE 0x721096
#define EA_CHANNELS 0x8B466C                  // channels, stride 0x90, +0 handle
static int CarSoundLocalPlayer(const uint8_t* obj) {   // local player of a per-car sound object, or -1
    __try {
        uint32_t vt = *reinterpret_cast<const uint32_t*>(obj);
        if (vt < 0x789000 || vt >= 0x78C000) return -1;
        uint8_t* node = *reinterpret_cast<uint8_t* const*>(obj + 0x44);
        uint8_t* n30 = node ? *reinterpret_cast<uint8_t**>(node + 0x30) : nullptr;
        void* player = n30 ? *reinterpret_cast<void**>(n30 + 0x18) : nullptr;
        for (int i = 0; player && i < g_localPlayers.count && i < MAX_LOCAL_PLAYERS; ++i)
            if (g_localPlayers.byIndex[i] == player) return i;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}
static uint8_t* PatchHeaderAt(const uint8_t* obj, int off, uint8_t** block, const char** name) {   // patch in `off`
    __try {
        uint8_t* param = *reinterpret_cast<uint8_t* const*>(obj + off);
        if (reinterpret_cast<uintptr_t>(param) < 0x10000 || (reinterpret_cast<uintptr_t>(param) & 3)) return nullptr;
        uint8_t* patch = *reinterpret_cast<uint8_t**>(param);
        if (reinterpret_cast<uintptr_t>(patch) < 0x10000) return nullptr;
        uint8_t* def = *reinterpret_cast<uint8_t**>(patch);
        if (reinterpret_cast<uintptr_t>(def) < 0x10000) return nullptr;
        uint8_t* hdr = *reinterpret_cast<uint8_t**>(def);
        if (reinterpret_cast<uintptr_t>(hdr) < 0x10000 || *reinterpret_cast<uint32_t*>(hdr + 8) != EA_PATCH_HEADER_SIGNATURE)
            return nullptr;
        *block = *reinterpret_cast<uint8_t**>(patch + 8);
        *name = *reinterpret_cast<const char**>(def + 4);
        return *block && *name ? hdr : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}
static void RecordPatchChildren(int k, const uint8_t* hdr, uint8_t* block) {
    __try {
        int n = hdr[0x18];
        uint32_t size = *reinterpret_cast<const uint32_t*>(hdr + 0x24);
        uint8_t* inst = block - 0x2C;
        auto* chans = at<uint8_t*>(EA_CHANNELS);
        DWORD now = GetTickCount();
        for (int i = 0; i < n && i < 16; ++i) {
            uint32_t off = *reinterpret_cast<const uint32_t*>(hdr + 0x30 + 4 * i);
            if (off >= size) break;
            int32_t h = *reinterpret_cast<int32_t*>(inst + off + 8);
            if (h < 0 || *reinterpret_cast<int32_t*>(chans + (h & 0xFF) * 0x90) != h) continue;   // not playing now
            g_engineVoicePlayer[h & 0x3F] = k + 1;
            g_engineVoiceHandle[h & 0x3F] = h;
            g_engineVoiceSeen[h & 0x3F] = now;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// Which patches go to the pad: [AUDIO] PadSpeakerSounds, patch names (case-insensitive), comma-separated.
static std::vector<std::string> g_padSpeakerSounds;
static void SetPadSpeakerSounds(const char* list) {
    g_padSpeakerSounds.clear();
    std::string cur;
    for (const char* c = list;; ++c) {
        if (*c == ',' || !*c) {
            size_t a = cur.find_first_not_of(" 	"), b = cur.find_last_not_of(" 	");
            if (a != std::string::npos) g_padSpeakerSounds.push_back(cur.substr(a, b - a + 1));
            cur.clear();
            if (!*c) break;
        } else {
            cur += *c;
        }
    }
}
static bool PatchGoesToPad(const char* name) {
    char n[32] = {};
    __try {
        strncpy_s(n, name, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (const std::string& s : g_padSpeakerSounds)
        if (!_stricmp(s.c_str(), n)) return true;
    return false;
}
// Sound objects sit back to back, so an object's fields end where the next object starts: the loop's objects are
// collected over a frame (the loop starts over at its first object) and processed then, each scanned up to the next
// every frame (a patch can be created at any time: the engine's CAR patch when the engine starts, FX_NITROUS when
// nitrous is used).
static void NotePatchesOfFrame(std::vector<const uint8_t*>& objs) {
    std::vector<const uint8_t*> sorted(objs);
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); ++i) {
        const uint8_t* obj = sorted[i];
        int k = CarSoundLocalPlayer(obj);
        if (k < 0) continue;
        uint32_t size = i + 1 < sorted.size() ? static_cast<uint32_t>(sorted[i + 1] - obj) : 0x400;
        if (size > 0x800) size = 0x800;
        for (uint32_t off = 0x40; off + 4 <= size; off += 4) {
            uint8_t* block;
            const char* name;
            if (uint8_t* hdr = PatchHeaderAt(obj, off, &block, &name))
                if (PatchGoesToPad(name)) RecordPatchChildren(k, hdr, block);
        }
    }
}
static void __stdcall NotePatchVoices(const uint8_t* obj) {   // main thread, every object of the loop
    static std::vector<const uint8_t*> frame;
    if (!frame.empty() && (obj == frame.front() || frame.size() >= 4096)) {   // loop started over (or no end seen)
        NotePatchesOfFrame(frame);
        frame.clear();
    }
    frame.push_back(obj);
}
static __declspec(naked) void SoundObjectUpdate() {
    __asm {
        mov eax, [esi]
        mov ecx, esi
        call dword ptr [eax + 0x28]
        push esi
        call NotePatchVoices
        ret
    }
}

// The player whose speaker gets this voice, or -1 (mixer thread).
static int SpeakerPlayerOfVoice(const uint8_t* voice) {
    int v = static_cast<int>((voice - at<uint8_t*>(EA_VOICES)) / 0x5C);
    if (v < 0 || v >= 64 || GetTickCount() - g_engineVoiceSeen[v] > 500) return -1;
    if (*reinterpret_cast<int32_t*>(at<uint8_t*>(EA_CHANNELS) + v * 0x90) != g_engineVoiceHandle[v]) return -1;   // reused
    int k = g_engineVoicePlayer[v] - 1;
    return k >= 0 && k < MAX_LOCAL_PLAYERS && g_speakerPad[k] >= 0 && !g_speakerFailed[k] ? k : -1;
}
static void __cdecl MixChannel(const uint8_t* voice, const uint8_t* gainPtr, int frames, float gain,
                               const float* src, float* dst) {
    auto mixAdd = at<EaMixAddFn>(EA_MIX_ADD);
    int k = SpeakerPlayerOfVoice(voice);
    int ch = static_cast<int>(gainPtr - voice - 0x1C) / 4;
    if (k < 0 || ch < 0 || ch > 1) { mixAdd(frames, gain, src, dst); return; }
    ptrdiff_t off = dst - at<float*>(EA_OUT_BUFS + 4 * ch);
    if (off >= 0 && off + frames <= 256) mixAdd(frames, gain, src, g_speakerMix[k][ch] + off);
}
typedef void(__cdecl* EaMixRampFn)(float, float, const float*, float*);
static void __cdecl RampChannel(const uint8_t* voice, const uint8_t* gainPtr, float from, float to,
                                const float* src, float* dst) {
    auto ramp = reinterpret_cast<EaMixRampFn>(FN_EA_MIX_RAMP);
    int k = SpeakerPlayerOfVoice(voice);
    int ch = static_cast<int>(gainPtr - voice - 0x1C) / 4;
    if (k < 0 || ch < 0 || ch > 1) { ramp(from, to, src, dst); return; }
    ptrdiff_t off = dst - at<float*>(EA_OUT_BUFS + 4 * ch);
    if (off >= 0 && off + 16 <= 256) ramp(from, to, src, g_speakerMix[k][ch] + off);
}
static void __cdecl RampAux(const uint8_t* voice, float from, float to, const float* src, float* dst) {
    if (SpeakerPlayerOfVoice(voice) < 0) reinterpret_cast<EaMixRampFn>(FN_EA_MIX_RAMP)(from, to, src, dst);
}
static __declspec(naked) void RampChannelHook() {
    __asm {
        push ecx                         // EA's callers keep values in ecx / edx across these calls
        push edx                         // (0x728C16 reads edx after the ramp): a C function may change them
        mov eax, [esp + 0x18]            // copy the original call's 4 arguments (after edx, ecx, ret)
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        push edi
        push esi
        call RampChannel
        add esp, 0x18
        pop edx
        pop ecx
        ret
    }
}
static __declspec(naked) void RampAuxHook() {
    __asm {
        push ecx                         // EA's callers keep values in ecx / edx across these calls
        push edx                         // (0x728C16 reads edx after the ramp): a C function may change them
        mov eax, [esp + 0x18]            // copy the original call's 4 arguments (after edx, ecx, ret)
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        push esi
        call RampAux
        add esp, 0x14
        pop edx
        pop ecx
        ret
    }
}
static void __cdecl MixAux(const uint8_t* voice, int frames, float gain, const float* src, float* dst) {
    if (SpeakerPlayerOfVoice(voice) < 0) at<EaMixAddFn>(EA_MIX_ADD)(frames, gain, src, dst);
}
static __declspec(naked) void MixChannelHook() {
    __asm {
        push ecx                         // EA's callers keep values in ecx / edx across these calls
        push edx                         // (0x728C16 reads edx after the ramp): a C function may change them
        mov eax, [esp + 0x18]            // copy the original call's 4 arguments (after edx, ecx, ret)
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        push edi
        push esi
        call MixChannel
        add esp, 0x18
        pop edx
        pop ecx
        ret
    }
}
static __declspec(naked) void MixAuxHook() {
    __asm {
        push ecx                         // EA's callers keep values in ecx / edx across these calls
        push edx                         // (0x728C16 reads edx after the ramp): a C function may change them
        mov eax, [esp + 0x18]            // copy the original call's 4 arguments (after edx, ecx, ret)
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        mov eax, [esp + 0x18]
        push eax
        push esi
        call MixAux
        add esp, 0x14
        pop edx
        pop ecx
        ret
    }
}
static void __cdecl RenderChunk(void* channels, int frames) {
    DWORD now = GetTickCount();
    bool on[MAX_LOCAL_PLAYERS];
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        on[k] = g_speakerPad[k] >= 0 && !g_speakerFailed[k] && now - g_speakerSeen[k] < 500 && frames <= 256;
        if (on[k]) memset(g_speakerMix[k], 0, sizeof(g_speakerMix[k]));
    }
    reinterpret_cast<void(__cdecl*)(void*, int)>(FN_RENDER)(channels, frames);
    if (g_speakerDumpSeconds > 0 && on[0]) {   // debug: float32 frames {PC left, PC right, player 1's pad (16-bit scale)}
        static FILE* dump;
        static int left = -1;
        if (left < 0) {
            char path[MAX_PATH];
            strcpy_s(path, g_logPath);
            strcpy_s(strrchr(path, '.'), 16, "_speaker.raw");
            fopen_s(&dump, path, "wb");
            left = g_speakerDumpSeconds * at<uint16_t>(EA_OUTPUT_RATE);
            Log("speaker dump: %s, %d frames", path, left);
        }
        for (int i = 0; dump && i < frames && left > 0; ++i, --left) {
            float f[3] = {at<float*>(EA_OUT_BUFS)[i] - 12582912.0f, at<float*>(EA_OUT_BUFS + 4)[i] - 12582912.0f,
                          (g_speakerMix[0][0][i] + g_speakerMix[0][1][i]) * 0.5f};
            fwrite(f, 4, 3, dump);
        }
        if (dump && !left) { fclose(dump); dump = nullptr; Log("speaker dump: done"); }
    }
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        if (!on[k]) continue;
        float mono[256];
        float scale = g_padSpeakerGain * 0.5f / 32768.0f;
        for (int i = 0; i < frames; ++i) mono[i] = (g_speakerMix[k][0][i] + g_speakerMix[k][1][i]) * scale;
        if (!XPadSpeakerWrite(g_speakerPad[k], mono, frames, at<uint16_t>(EA_OUTPUT_RATE))) {
            InterlockedExchange(&g_speakerFailed[k], 1);
            Log("pad speaker: player %d's pad %ld has no speaker; engine stays on the PC", k + 1,
                g_speakerPad[k]);
        }
    }
}
static void InstallPadSpeakers() {
    static const uint8_t mixCall[6] = {0xFF, 0x15, 0x04, 0x4A, 0x8B, 0x00};
    if (*reinterpret_cast<uint32_t*>(ENGINE_UPDATE_SLOT) != FN_ENGINE_UPDATE ||
        memcmp(reinterpret_cast<void*>(AT_MIX_CHANNEL_CALL), mixCall, 6) ||
        memcmp(reinterpret_cast<void*>(AT_MIX_AUX_CALL), mixCall, 6) ||
        memcmp(reinterpret_cast<void*>(AT_RAMP_CHANNEL_MIX), mixCall, 6) ||
        memcmp(reinterpret_cast<void*>(AT_RAMP_AUX_MIX), mixCall, 6) ||
        *reinterpret_cast<uint8_t*>(AT_RAMP_CHANNEL_CALL) != 0xE8 || *reinterpret_cast<uint8_t*>(AT_RAMP_AUX_CALL) != 0xE8 ||
        *reinterpret_cast<uint8_t*>(AT_RENDER_CALL) != 0xE8) {
        Log("pad speakers: unexpected code, not installed");
        return;
    }
    g_origEngineUpdate = reinterpret_cast<void*>(FN_ENGINE_UPDATE);
    void* hook = reinterpret_cast<void*>(&EngineUpdateHook);
    Write(ENGINE_UPDATE_SLOT, &hook, 4);
    static const uint8_t nop = 0x90;
    WriteCall(AT_MIX_CHANNEL_CALL, &MixChannelHook);
    Write(AT_MIX_CHANNEL_CALL + 5, &nop, 1);
    WriteCall(AT_MIX_AUX_CALL, &MixAuxHook);
    Write(AT_MIX_AUX_CALL + 5, &nop, 1);
    WriteCall(AT_RAMP_CHANNEL_MIX, &MixChannelHook);
    Write(AT_RAMP_CHANNEL_MIX + 5, &nop, 1);
    WriteCall(AT_RAMP_AUX_MIX, &MixAuxHook);
    Write(AT_RAMP_AUX_MIX + 5, &nop, 1);
    WriteCall(AT_RAMP_CHANNEL_CALL, &RampChannelHook);
    WriteCall(AT_RAMP_AUX_CALL, &RampAuxHook);
    WriteCall(AT_RENDER_CALL, &RenderChunk);
    static const uint8_t loopCall[7] = {0x8B, 0x06, 0x8B, 0xCE, 0xFF, 0x50, 0x28};
    if (memcmp(reinterpret_cast<void*>(AT_SOUND_OBJECT_LOOP_CALL), loopCall, 7) == 0) {
        static const uint8_t nops[2] = {0x90, 0x90};
        WriteCall(AT_SOUND_OBJECT_LOOP_CALL, &SoundObjectUpdate);
        Write(AT_SOUND_OBJECT_LOOP_CALL + 5, nops, 2);
    } else {
        Log("pad speakers: sound object loop not as expected, patch sounds stay on the PC");
    }
    Log("pad speakers: installed (gain %.2f)", g_padSpeakerGain);
}
