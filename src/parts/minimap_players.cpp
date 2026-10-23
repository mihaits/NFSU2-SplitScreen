// Part of NFSU2SplitScreen.cpp: minimap car markers and manager, HUD binding list.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// A free car marker (type 4) for an FE object, as 0x4DB900 registers them. P1's package nearly fills the
// descriptor allocator, so its size is raised (and a failed allocation adds nothing: a null descriptor in the pool
// crashes its cleanup).
static bool AddCarMarker(uint8_t* obj) {
    auto* desc = reinterpret_cast<uint32_t*(__cdecl*)(void*)>(FN_POOL_ALLOC)(at<void*>(MARKER_ALLOCATOR));
    if (!desc) return false;
    desc[0] = reinterpret_cast<uint32_t>(obj);
    desc[1] = 4;
    auto* node = reinterpret_cast<uint32_t*(__cdecl*)()>(FN_LIST_NODE)();
    if (!node) return false;
    node[2] = reinterpret_cast<uint32_t>(desc);
    auto* tail = at<uint32_t*>(MARKER_POOL + 4);
    tail[0] = reinterpret_cast<uint32_t>(node);
    at<uint32_t*>(MARKER_POOL + 4) = node;
    node[1] = reinterpret_cast<uint32_t>(tail);
    node[0] = MARKER_POOL;
    return true;
}

// Players 3/4 get car entities (type 4, id 2/3) in the minimap manager; their markers are P3's package copy's.
// Redone whenever the manager is rebuilt (0x4F92A0 recreates it with a fresh pool).
static void ExtendMinimapManager(uint8_t* mgr) {
    if (mgr == g_mmExtendedMgr || !g_mmExtraIcon[0]) return;
    g_mmExtendedMgr = mgr;
    for (int idx = 2; idx < 2 + g_extraPlayers && idx < MAX_LOCAL_PLAYERS; ++idx) {
        if (!g_mmExtraIcon[idx - 2] || !AddCarMarker(g_mmExtraIcon[idx - 2])) {
            Log("minimap: no marker for player %d", idx + 1);
            continue;
        }
        auto* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, int, int)>(FN_MINIMAP_ADD)(mgr, 4, idx);
        Log("minimap: player %d car entity %p (marker %p)", idx + 1, e, e ? *reinterpret_cast<void**>(e + 0x1C) : 0);
    }
}

// The manager is destroyed with player 1's HUD (0x4FBFD0 -> 0x4ECB80, call at 0x4FBFF2), the last one to go: the
// destructor parks every entity's marker (0x51D4D0), but players 3/4's markers belong to player 3's HUD package,
// gone by then. Their entities are taken out first (entity and descriptor back to their allocators, 0x43F920).
#define AT_MINIMAP_DESTROY_CALL 0x4FBFF2
#define FN_MINIMAP_DESTROY 0x4ECB80
#define ENTITY_ALLOCATOR   0x8364B8
#define FN_POOL_FREE       0x43F920      // void __cdecl (allocator, item)
static void __fastcall DestroyMinimapManager(uint8_t* mgr) {
    auto free_ = reinterpret_cast<void(__cdecl*)(void*, void*)>(FN_POOL_FREE);
    for (auto* e = *reinterpret_cast<uint8_t**>(mgr); e && e != mgr;) {
        auto* next = *reinterpret_cast<uint8_t**>(e);
        auto* desc = *reinterpret_cast<uint8_t**>(e + 0x1C);
        if (desc && *reinterpret_cast<int*>(desc + 4) == 4 && *reinterpret_cast<int*>(e + 8) >= 2) {
            auto* prev = *reinterpret_cast<uint8_t**>(e + 4);
            *reinterpret_cast<uint8_t**>(prev) = next;
            *reinterpret_cast<uint8_t**>(next + 4) = prev;
            free_(at<void*>(MARKER_ALLOCATOR), desc);
            free_(at<void*>(ENTITY_ALLOCATOR), e);
            Log("minimap: player %d car entity removed", *reinterpret_cast<int*>(e + 8) + 1);
        }
        e = next;
    }
    g_mmExtendedMgr = nullptr;
    reinterpret_cast<void(__fastcall*)(void*)>(FN_MINIMAP_DESTROY)(mgr);
}

static void __fastcall MinimapUpdate(uint8_t* mgr, void*) {
    auto update = reinterpret_cast<void(__fastcall*)(void*, void*)>(FN_MINIMAP_UPDATE);
    int n = g_quadHud && g_mmObjCount > 0 ? (g_screenViews < 2 + g_extraPlayers ? g_screenViews : 2 + g_extraPlayers) : 0;
    g_mmPlayers = 0;
    if (!n) { update(mgr, nullptr); return; }
    ExtendMinimapManager(mgr);
    for (int i = 0; i < MAX_EXTRA_PLAYERS && i < g_extraPlayers; ++i)
        if (uint8_t* icon = g_mmExtraIcon[i]) {
            auto* data = *reinterpret_cast<uint32_t**>(icon + 0x2C);
            for (int c = 0; c < 4; ++c) data[c] = kMarkerColour[i][c];
            *reinterpret_cast<uint32_t*>(icon + 0x1C) &= ~1u;            // shown (P3's copy starts hidden)
        }
    for (int k = n - 1; k >= 0; --k) {                                    // ends with the game's own (centre P1)
        uint8_t id = static_cast<uint8_t>(k);
        Write(AT_MINIMAP_CENTRE_ID, &id, 1);
        update(mgr, nullptr);
        TakeMinimapSnapshot(k);
    }
    g_mmPlayers = n;
}

void* GetLocalPlayer(int k);

// HUD element at hud+0x70 (0x4F89D0, 0x48 bytes): its data bindings (small objects, vtables 0x795Bxx, list node at
// +4) are deleted by its dtor through the generic list clear 0x4358E0 (call at 0x4F8BAE). With 4 players one of player
// 4's bindings sometimes loses its link back to the head (last node, next == 0: quit to menu crashed at 0x4358F7).
// Cause not found yet; a binding with next == 0 is taken as the last one, and the clear stops at anything else that
// isn't a linked binding (the rest is leaked, nothing is touched).
#define AT_BINDINGS_CLEAR_CALL 0x4F8BAE
static void __fastcall ClearHudBindings(uint32_t* head) {
    for (int guard = 0; guard < 256; ++guard) {
        auto* node = reinterpret_cast<uint32_t*>(head[0]);
        if (node == head) return;
        auto* next = reinterpret_cast<uint32_t*>(node[0]);
        auto* prev = reinterpret_cast<uint32_t*>(node[1]);
        uint32_t vt = node[-1];
        if (!next && prev && prev[0] == reinterpret_cast<uint32_t>(node) && vt >= 0x795000 && vt < 0x79C000) {
            Log("HUD bindings %p: last node %p lost its link to the head", head, node);
            next = head;
        }
        bool linked = next && prev && (next == head || next[1] == reinterpret_cast<uint32_t>(node)) &&
                      prev[0] == reinterpret_cast<uint32_t>(node);
        if (!linked || vt < 0x795000 || vt >= 0x79C000) {
            Log("HUD bindings %p: broken node %p (next %p, vt %08X), rest leaked", head, node, next, vt);
            head[0] = head[1] = reinterpret_cast<uint32_t>(head);
            return;
        }
        prev[0] = reinterpret_cast<uint32_t>(next);
        next[1] = reinterpret_cast<uint32_t>(prev);
        void* obj = node - 1;
        reinterpret_cast<void(__thiscall*)(void*, int)>(*reinterpret_cast<uintptr_t*>(vt))(obj, 1);
    }
}
