// Part of NFSU2SplitScreen.cpp: crash report (always on, also without Debug).
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- crash report
// An unhandled exception writes a text report (code, faulting thread, registers, memory at each register, stack with
// module offsets, EBP chain, game state) to scripts\NFSU2SplitScreen_crash.txt and to the log (Debug = 1), plus a
// minidump next to it (NFSU2SplitScreen_crash_<date>_<time>.dmp: threads, stacks, the modules' data sections with the
// game's globals, memory the stacks point at). The game and the other ASIs install filters of their own, so ours is
// installed at load and claimed back once a second from the frame hook; it calls the filter it replaced afterwards.
// Exceptions the plugin's own __try blocks catch never get here.
#include <dbghelp.h>

typedef BOOL(WINAPI* MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static MiniDumpWriteDumpFn g_miniDumpWrite;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevCrashFilter;
static volatile LONG g_crashing;
static char g_crashDir[MAX_PATH];        // the plugin's folder, with the trailing backslash
static DWORD g_loadTick;
static HANDLE g_crashOut[2];             // report file, log

static void CrashOut(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof(line)) - 3) n = sizeof(line) - 3;
    line[n++] = '\r'; line[n++] = '\n';
    char stamp[16];
    int s = sprintf_s(stamp, "%8lu ", GetTickCount());
    for (HANDLE h : g_crashOut) {
        if (h == INVALID_HANDLE_VALUE || !h) continue;
        DWORD w;
        WriteFile(h, stamp, s, &w, nullptr);
        WriteFile(h, line, n, &w, nullptr);
    }
}

static bool CrashReadable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// "speed2.exe+1DEEE1" for an address inside a loaded module, "" otherwise
static void CrashModuleOf(uintptr_t a, char* out, size_t len) {
    out[0] = 0;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(a), &m) || !m)
        return;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return;
    const char* name = strrchr(path, '\\');
    sprintf_s(out, len, "%s+%X", name ? name + 1 : path, static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(m)));
}

static void CrashMemoryAt(const char* what, uintptr_t a) {
    char mod[MAX_PATH + 16];
    CrashModuleOf(a, mod, sizeof(mod));
    uint32_t v[8];
    if (CrashReadable(reinterpret_cast<void*>(a), sizeof(v))) {
        memcpy(v, reinterpret_cast<void*>(a), sizeof(v));
        CrashOut("  %s %08X%s%s: %08X %08X %08X %08X %08X %08X %08X %08X", what, static_cast<unsigned>(a),
                 mod[0] ? " " : "", mod, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    } else {
        CrashOut("  %s %08X %s(not readable)", what, static_cast<unsigned>(a), mod[0] ? " " : "");
    }
}

struct CrashDumpJob { EXCEPTION_POINTERS* ep; DWORD thread; HANDLE file; BOOL ok; DWORD error; };
static DWORD WINAPI CrashDumpThread(LPVOID p) {   // own thread: a stack overflow leaves the faulting one no room
    CrashDumpJob* j = static_cast<CrashDumpJob*>(p);
    MINIDUMP_EXCEPTION_INFORMATION mei = {j->thread, j->ep, FALSE};
    MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
                                                    MiniDumpWithProcessThreadData | MiniDumpWithThreadInfo |
                                                    MiniDumpWithUnloadedModules | MiniDumpWithHandleData);
    j->ok = g_miniDumpWrite(GetCurrentProcess(), GetCurrentProcessId(), j->file, type, &mei, nullptr, nullptr);
    j->error = j->ok ? 0 : GetLastError();
    return 0;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_crashing, 1))              // a second fault, or a filter chain that loops back here
        return EXCEPTION_CONTINUE_SEARCH;
    char path[MAX_PATH];
    sprintf_s(path, "%sNFSU2SplitScreen_crash.txt", g_crashDir);
    g_crashOut[0] = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    g_crashOut[1] = g_debug ? CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_ALWAYS, 0, nullptr)
                            : INVALID_HANDLE_VALUE;
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* c = ep->ContextRecord;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char mod[MAX_PATH + 16];
    CrashModuleOf(reinterpret_cast<uintptr_t>(er->ExceptionAddress), mod, sizeof(mod));
    DWORD tid = GetCurrentThreadId();
    CrashOut("CRASH %04d-%02d-%02d %02d:%02d:%02d, %lu s after load: exception %08lX at %08X %s, thread %lu (%s)",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, (GetTickCount() - g_loadTick) / 1000,
             er->ExceptionCode, static_cast<unsigned>(reinterpret_cast<uintptr_t>(er->ExceptionAddress)), mod, tid,
             tid == g_mainThreadId ? "main" : "not main");
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        CrashOut("  access violation: %s %08X", er->ExceptionInformation[0] == 0 ? "read" :
                 er->ExceptionInformation[0] == 1 ? "write" : "execute",
                 static_cast<unsigned>(er->ExceptionInformation[1]));
    __try {
        CrashOut("  game: mode %d, state %d, local players %d, split players %d, extra %d, track %d",
                 at<int>(GAME_MODE), at<int>(0x8654A4), at<int>(NUM_PLAYERS), g_splitPlayers, g_extraPlayers,
                 at<int>(0x89E7A0));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    CrashOut("  eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX", c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi,
             c->Edi);
    CrashOut("  ebp %08lX esp %08lX eip %08lX eflags %08lX", c->Ebp, c->Esp, c->Eip, c->EFlags);
    CrashMemoryAt("[eip]", c->Eip);
    const char* names[] = {"[eax]", "[ebx]", "[ecx]", "[edx]", "[esi]", "[edi]", "[ebp]"};
    DWORD regs[] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
    for (int i = 0; i < 7; ++i) CrashMemoryAt(names[i], regs[i]);
    CrashOut("  stack from esp (values inside a module: return addresses and module data):");
    for (int i = 0; i < 128; ++i) {
        uintptr_t a = c->Esp + i * 4;
        if (!CrashReadable(reinterpret_cast<void*>(a), 4)) break;
        uint32_t v = *reinterpret_cast<uint32_t*>(a);
        CrashModuleOf(v, mod, sizeof(mod));
        if (mod[0]) CrashOut("    esp+%03X %08X %s", i * 4, v, mod);
    }
    CrashOut("  ebp chain:");
    uintptr_t fp = c->Ebp;
    for (int i = 0; i < 32 && CrashReadable(reinterpret_cast<void*>(fp), 8); ++i) {
        uint32_t ret = reinterpret_cast<uint32_t*>(fp)[1];
        CrashModuleOf(ret, mod, sizeof(mod));
        CrashOut("    frame %08X -> %08X %s", static_cast<unsigned>(fp), ret, mod);
        uintptr_t next = reinterpret_cast<uint32_t*>(fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    if (HMODULE self = GetModuleHandleA("NFSU2SplitScreen.asi"))
        CrashOut("  NFSU2SplitScreen.asi at %08X (offsets match build\\NFSU2SplitScreen.map at base 10000000)",
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(self)));
    if (g_miniDumpWrite) {
        sprintf_s(path, "%sNFSU2SplitScreen_crash_%04d%02d%02d_%02d%02d%02d.dmp", g_crashDir, t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond);
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            CrashDumpJob job = {ep, tid, f, FALSE, 0};
            if (HANDLE h = CreateThread(nullptr, 0, CrashDumpThread, &job, 0, nullptr)) {
                WaitForSingleObject(h, 60000);
                CloseHandle(h);
            }
            CloseHandle(f);
            const char* name = strrchr(path, '\\');
            if (job.ok) CrashOut("  minidump written: %s", name ? name + 1 : path);
            else CrashOut("  minidump failed (error %lu)", job.error);
        } else {
            CrashOut("  minidump not created (error %lu)", GetLastError());
        }
    } else {
        CrashOut("  no dbghelp.dll: no minidump");
    }
    for (HANDLE& h : g_crashOut) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    return g_prevCrashFilter ? g_prevCrashFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// Claims the filter back when another module replaced it (frame hook, once a second); the replaced one is chained.
static void ClaimCrashFilter() {
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 1000) return;
    last = now;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev != CrashFilter) {
        g_prevCrashFilter = prev;
        char mod[MAX_PATH + 16];
        CrashModuleOf(reinterpret_cast<uintptr_t>(prev), mod, sizeof(mod));
        Log("crash handler: claimed back from %08X %s", static_cast<unsigned>(reinterpret_cast<uintptr_t>(prev)), mod);
    }
}

static void InstallCrashHandler(HMODULE self) {
    g_loadTick = GetTickCount();
    GetModuleFileNameA(self, g_crashDir, MAX_PATH);
    if (char* slash = strrchr(g_crashDir, '\\')) slash[1] = 0;
    if (HMODULE dbghelp = LoadLibraryA("dbghelp.dll"))
        g_miniDumpWrite = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    g_prevCrashFilter = SetUnhandledExceptionFilter(CrashFilter);
    Log("crash handler: installed (minidumps %s)", g_miniDumpWrite ? "on" : "off, no dbghelp.dll");
}
