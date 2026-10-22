// Part of NFSU2SplitScreen.cpp: HUD packages for players 3/4, racer of a player.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

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
