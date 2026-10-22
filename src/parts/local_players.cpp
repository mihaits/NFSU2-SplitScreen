// Part of NFSU2SplitScreen.cpp: local player arrays (4 players).
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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
