// Part of NFSU2SplitScreen.cpp: debug log.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- debug log (Debug=1 in the ini)
static char g_logPath[MAX_PATH];
static char g_iniPath[MAX_PATH];
static bool g_debug = false;

static void Log(const char* fmt, ...) {
    if (!g_debug) return;
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (FILE* f = fopen(g_logPath, "a")) {
        fprintf(f, "%8lu %s\n", GetTickCount(), line);
        fclose(f);
    }
}

// Debug: values left on the x87 register stack (a game function returning a float in ST0 called as void leaks one
// value per call; after 8 every x87 calculation returns garbage).
static int X87Depth() {
    uint16_t env[14];
    __asm fnstenv env
    __asm fldcw word ptr env              // fnstenv masks exceptions; restore the control word
    int used = 0;
    for (int i = 0; i < 8; ++i) used += ((env[4] >> (2 * i)) & 3) != 3;
    return used;
}
static void CheckX87(const char* where) {
    static char last[64];
    int d = X87Depth();
    if (d && strcmp(last, where)) { Log("x87 stack: %d values at %s", d, where); strcpy_s(last, where); }
}
