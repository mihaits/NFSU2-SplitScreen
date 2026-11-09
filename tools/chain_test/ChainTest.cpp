// Test plugin: hooks game functions before NFSU2SplitScreen.asi loads, the way other ASI mods do (a 5-byte jmp at the
// function start, NOPs up to the stolen length, a stub that runs the stolen bytes and jumps back). Used to check that
// the split-screen plugin chains its own hooks on the same sites. Installed as scripts/AAChainTest.asi so the ASI
// loader loads it first (alphabetical). Sites come from scripts/AAChainTest.txt, one "hexaddr len" per line.
// AAChainTest.log: the load order line, then the call count per site, rewritten every second.
// Build and run: tools/chain_test.sh.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct Site { uintptr_t at; int len; volatile LONG calls; };
static Site g_sites[64];
static int g_count;
static char g_dir[MAX_PATH];

static void Hook(Site& s) {
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t* p = stub;
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05;   // lock inc dword [&calls]
    LONG* c = const_cast<LONG*>(&s.calls);
    memcpy(p, &c, 4); p += 4;
    memcpy(p, reinterpret_cast<void*>(s.at), s.len); p += s.len;
    *p = 0xE9;
    int32_t back = static_cast<int32_t>((s.at + s.len) - (reinterpret_cast<uintptr_t>(p) + 5));
    memcpy(p + 1, &back, 4);
    uint8_t patch[32];
    patch[0] = 0xE9;
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (s.at + 5));
    memcpy(patch + 1, &to, 4);
    memset(patch + 5, 0x90, s.len - 5);
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(s.at), s.len, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(s.at), patch, s.len);
    VirtualProtect(reinterpret_cast<void*>(s.at), s.len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(s.at), s.len);
}

static DWORD WINAPI Report(void*) {
    char path[MAX_PATH], line[64];
    sprintf_s(path, "%s\\AAChainTest.log", g_dir);
    for (;;) {
        Sleep(1000);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") || !f) continue;
        for (int i = 0; i < g_count; ++i) {
            sprintf_s(line, "%08X %d %ld\n", static_cast<unsigned>(g_sites[i].at), g_sites[i].len, g_sites[i].calls);
            fputs(line, f);
        }
        fclose(f);
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, void*) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    GetModuleFileNameA(self, g_dir, MAX_PATH);
    if (char* s = strrchr(g_dir, '\\')) *s = 0;
    char path[MAX_PATH];
    sprintf_s(path, "%s\\AAChainTest.txt", g_dir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f) return TRUE;
    unsigned at; int len;
    while (g_count < 64 && fscanf_s(f, "%x %d", &at, &len) == 2)
        if (len >= 5 && len <= 16) g_sites[g_count++] = {at, len, 0};
    fclose(f);
    // written once at load: whether the split-screen plugin was already in the process
    sprintf_s(path, "%s\\AAChainTest.order", g_dir);
    if (!fopen_s(&f, path, "w") && f) {
        fprintf(f, "%s\n", GetModuleHandleA("NFSU2SplitScreen.asi") ? "after" : "before");
        fclose(f);
    }
    for (int i = 0; i < g_count; ++i) Hook(g_sites[i]);
    CreateThread(nullptr, 0, Report, nullptr, 0, nullptr);
    return TRUE;
}
