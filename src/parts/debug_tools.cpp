// Part of NFSU2SplitScreen.cpp: debug write watch and command mailbox.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- debug write watch (hardware breakpoints)
// Logs the code address of every write to up to 4 watched dwords on the game's main thread.
static DWORD g_mainThreadId = 0;
static uintptr_t g_watch[4];

static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD hit = e->ContextRecord->Dr6 & 0xF;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 4; ++i) {
        if (!(hit & (1u << i))) continue;
        // return addresses on the stack: dwords in the game's code right after a call instruction
        uint32_t rets[10] = {};
        int n = 0;
        auto* sp = reinterpret_cast<const uint32_t*>(e->ContextRecord->Esp);
        for (int k = 0; k < 2048 && n < 10; ++k) {
            if (k == 4 && n == 1) k = 0x204;   // skip the 0x800-byte text buffer of 0x532670 (FE text setter)
            uint32_t v = sp[k];
            if (v < 0x401005 || v >= 0x780000) continue;
            auto* c = reinterpret_cast<const uint8_t*>(v);
            if (c[-5] == 0xE8 || (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10))
                rets[n++] = v;
        }
        Log("watch %08X written by eip %08X value %08X (calls %08X %08X %08X %08X %08X %08X %08X %08X %08X %08X)",
            static_cast<unsigned>(g_watch[i]), static_cast<unsigned>(e->ContextRecord->Eip),
            *reinterpret_cast<uint32_t*>(g_watch[i]), rets[0], rets[1], rets[2], rets[3], rets[4], rets[5], rets[6],
            rets[7], rets[8], rets[9]);
    }
    e->ContextRecord->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static DWORD WINAPI ApplyWatchThread(LPVOID) {
    HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_mainThreadId);
    if (!t) return 0;
    SuspendThread(t);
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(t, &c);
    c.Dr0 = g_watch[0]; c.Dr1 = g_watch[1]; c.Dr2 = g_watch[2]; c.Dr3 = g_watch[3];
    c.Dr7 = 0;
    for (int i = 0; i < 4; ++i)
        if (g_watch[i]) c.Dr7 |= (1u << (i * 2)) | (0xDu << (16 + i * 4));   // local enable, write, 4 bytes
    SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}

static void SetWatch(int slot, uintptr_t addr) {
    static bool handler = false;
    if (!handler) { AddVectoredExceptionHandler(1, WatchHandler); handler = true; }
    g_watch[slot & 3] = addr;
    if (HANDLE h = CreateThread(nullptr, 0, ApplyWatchThread, nullptr, 0, nullptr)) {
        if (GetCurrentThreadId() != g_mainThreadId) WaitForSingleObject(h, 1000);
        CloseHandle(h);
    }
    Log("watch slot %d = %08X", slot & 3, static_cast<unsigned>(addr));
}

// ---------------------------------------------------------------- debug command mailbox (Debug=1)
// Test tools write a command here; it runs on the game thread at the next frame. Address is in the log.
struct Mailbox {
    uint32_t magic;          // 'NFMB'
    volatile uint32_t seq;   // writer bumps after filling the fields
    volatile uint32_t done;  // = seq when executed
    uint32_t cmd;            // 1 send FE message, 2 switch screen, 3 write dword
    uint32_t arg, arg2;
    char s1[64], s2[64];
};
static Mailbox g_mailbox = {0x424D464E};

// debug (mailbox 7, arg = address of an IDirect3DSurface9* global): dump that render target as raw BGRA
void DumpSurface(uintptr_t at_) {
    void* dev = at<void*>(D3D_DEVICE);
    void* surf = at_ ? *reinterpret_cast<void**>(at_) : nullptr;
    if (!dev || !surf) return;
    struct { DWORD Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality; UINT Width, Height; } desc = {};
    reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>((*reinterpret_cast<void***>(surf))[12])(surf, &desc);
    auto vt = *reinterpret_cast<void***>(dev);
    void* sys = nullptr;
    HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, DWORD, void**, HANDLE*)>(vt[36])(
        dev, desc.Width, desc.Height, desc.Format, 2, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*, void*)>(vt[32])(dev, surf, sys);
    if (SUCCEEDED(hr)) {
        struct { INT Pitch; void* pBits; } lr;
        auto svt = *reinterpret_cast<void***>(sys);
        if (SUCCEEDED(reinterpret_cast<HRESULT(__stdcall*)(void*, void*, const RECT*, DWORD)>(svt[13])(sys, &lr, nullptr, 0))) {
            if (FILE* f = fopen("surfdump.raw", "wb")) {
                for (UINT y = 0; y < desc.Height; ++y) fwrite(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, 4, desc.Width, f);
                fclose(f);
            }
            reinterpret_cast<HRESULT(__stdcall*)(void*)>(svt[14])(sys);
        }
    }
    if (sys) reinterpret_cast<ULONG(__stdcall*)(void*)>((*reinterpret_cast<void***>(sys))[2])(sys);
    Log("surface dump %08X: %ux%u fmt %u hr %08X", static_cast<unsigned>(at_), desc.Width, desc.Height, desc.Format, hr);
}

static void RunMailbox() {
    if (!g_debug || g_mailbox.seq == g_mailbox.done) return;
    uint32_t seq = g_mailbox.seq;
    switch (g_mailbox.cmd) {
    case 1: reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(g_mailbox.arg, g_mailbox.s1); break;
    case 2:   // switching from a package that isn't the active screen crashes the game
        if (reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)(g_mailbox.s1))
            reinterpret_cast<void(__cdecl*)(const char*, const char*, uint32_t)>(FN_FE_SWITCH)(g_mailbox.s1, g_mailbox.s2,
                                                                                             g_mailbox.arg);
        else
            Log("mailbox: %s is not active, switch skipped", g_mailbox.s1);
        break;
    case 3: at<uint32_t>(g_mailbox.arg) = g_mailbox.arg2; break;
    case 5: SetWatch(g_mailbox.arg2, g_mailbox.arg); break;   // arg = address (0 = clear), arg2 = slot
    case 6: g_vpLogFrames = g_mailbox.arg; break;            // debug: log SetViewport calls for arg frames
    case 7: DumpSurface(g_mailbox.arg); break;
    case 4: {  // virtual pad: arg = index | connected << 8, arg2 = buttons | lt << 16 | rt << 24, s1 = 4 x int16 sticks
        const int16_t* st = reinterpret_cast<const int16_t*>(g_mailbox.s1);
        XPadSetVirtual(g_mailbox.arg & 0xFF, (g_mailbox.arg >> 8) & 1, g_mailbox.arg2 & 0xFFFF, (g_mailbox.arg2 >> 16) & 0xFF,
                       g_mailbox.arg2 >> 24, st[0], st[1], st[2], st[3]);
        break;
    }
    }
    if (g_mailbox.cmd != 4) Log("mailbox cmd %u arg %08X %s %s", g_mailbox.cmd, g_mailbox.arg, g_mailbox.s1, g_mailbox.s2);
    g_mailbox.done = seq;
}
