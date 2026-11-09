// Test plugin: hooks game code before NFSU2SplitScreen.asi loads, the way other ASI mods do, to check that the
// split-screen plugin chains its own hooks on the same sites. Installed as scripts/AAChainTest.asi so the ASI loader
// loads it first (alphabetical).
// - scripts/AAChainTest.txt, one "hexaddr len" per line: function starts. A 5-byte jmp there, NOPs up to len, and a
//   stub that counts, runs the stolen bytes and jumps back.
// - scripts/AAChainTest_calls.txt, one hexaddr per line: `call rel32` sites. The call goes to a stub that counts and
//   jumps on to the original target.
// AAChainTest.log: "jmp|call site len calls" per site (len 0: not a call), rewritten every second.
// AAChainTest.order: "before" if NFSU2SplitScreen.asi wasn't loaded yet. Build and run: tools/chain_test.sh.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct Site { uintptr_t at; int len; volatile LONG calls; };
static Site g_sites[64];
static int g_count;
static Site g_calls[200];
static int g_callCount;
static char g_dir[MAX_PATH];

static void Protected(uintptr_t at, const void* data, size_t n) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(reinterpret_cast<void*>(at), data, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
}

static uint8_t* CountingStub(Site& s, uint8_t*& p) {
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    p = stub;
    *p++ = 0xF0; *p++ = 0xFF; *p++ = 0x05;   // lock inc dword [&calls]
    LONG* c = const_cast<LONG*>(&s.calls);
    memcpy(p, &c, 4); p += 4;
    return stub;
}
static void JumpTo(uint8_t* p, uintptr_t target) {
    *p = 0xE9;
    int32_t rel = static_cast<int32_t>(target - (reinterpret_cast<uintptr_t>(p) + 5));
    memcpy(p + 1, &rel, 4);
}

static void Hook(Site& s) {
    uint8_t* p;
    uint8_t* stub = CountingStub(s, p);
    memcpy(p, reinterpret_cast<void*>(s.at), s.len); p += s.len;
    JumpTo(p, s.at + s.len);
    uint8_t patch[32];
    JumpTo(patch, 0);   // placeholder, displacement set below
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (s.at + 5));
    memcpy(patch + 1, &to, 4);
    memset(patch + 5, 0x90, s.len - 5);
    Protected(s.at, patch, s.len);
}

static void Redirect(Site& s) {
    auto* b = reinterpret_cast<const uint8_t*>(s.at);
    if (b[0] != 0xE8) { s.len = 0; return; }
    uintptr_t target = s.at + 5 + *reinterpret_cast<const int32_t*>(b + 1);
    uint8_t* p;
    uint8_t* stub = CountingStub(s, p);
    JumpTo(p, target);
    int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(stub) - (s.at + 5));
    Protected(s.at + 1, &to, 4);
    s.len = 5;
}

static DWORD WINAPI Report(void*) {
    char path[MAX_PATH], line[64];
    sprintf_s(path, "%s\\AAChainTest.log", g_dir);
    for (;;) {
        Sleep(1000);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "w") || !f) continue;
        for (int i = 0; i < g_count; ++i) {
            sprintf_s(line, "jmp %08X %d %ld\n", static_cast<unsigned>(g_sites[i].at), g_sites[i].len, g_sites[i].calls);
            fputs(line, f);
        }
        for (int i = 0; i < g_callCount; ++i) {
            sprintf_s(line, "call %08X %d %ld\n", static_cast<unsigned>(g_calls[i].at), g_calls[i].len, g_calls[i].calls);
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
    FILE* f = nullptr;
    sprintf_s(path, "%s\\AAChainTest.order", g_dir);
    if (!fopen_s(&f, path, "w") && f) {
        fprintf(f, "%s\n", GetModuleHandleA("NFSU2SplitScreen.asi") ? "after" : "before");
        fclose(f);
    }
    unsigned at; int len;
    sprintf_s(path, "%s\\AAChainTest.txt", g_dir);
    if (!fopen_s(&f, path, "r") && f) {
        while (g_count < 64 && fscanf_s(f, "%x %d", &at, &len) == 2)
            if (len >= 5 && len <= 16) g_sites[g_count++] = {at, len, 0};
        fclose(f);
    }
    sprintf_s(path, "%s\\AAChainTest_calls.txt", g_dir);
    if (!fopen_s(&f, path, "r") && f) {
        while (g_callCount < 200 && fscanf_s(f, "%x", &at) == 1) g_calls[g_callCount++] = {at, 0, 0};
        fclose(f);
    }
    for (int i = 0; i < g_count; ++i) Hook(g_sites[i]);
    for (int i = 0; i < g_callCount; ++i) Redirect(g_calls[i]);
    CreateThread(nullptr, 0, Report, nullptr, 0, nullptr);
    return TRUE;
}
