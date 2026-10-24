// fxdump: the game's shaders, offline. 32-bit (d3dx9_33.dll from SysWOW64); build with build.bat.
//   fxdump dis <file>              disassemble every shader in a file (an effect resource from extract.py)
//   fxdump hash <file>             per shader: offset, version, hash of its instruction tokens (comments skipped, the
//                                  hash the plugin computes in ShaderHash), and whether its CTAB names HeadlightWVP /
//                                  HeadlightColor
//   fxdump asm <file.asm> <name>   assemble; print a C array `name` (instruction tokens only, no comments)
//   fxdump frag <file.asm> <name>  assemble; print the instructions after the dcl's, without the end token (a block
//                                  the plugin appends to a game shader)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { void** vt; } Buf;
typedef HRESULT (WINAPI *DisFn)(const DWORD*, BOOL, const char*, Buf**);
typedef HRESULT (WINAPI *AsmFn)(const char*, UINT, const void*, void*, DWORD, Buf**, Buf**);
static void* BufPtr(Buf* b) { return ((void*(WINAPI*)(Buf*))b->vt[3])(b); }
static DWORD BufSize(Buf* b) { return ((DWORD(WINAPI*)(Buf*))b->vt[4])(b); }

static unsigned char* ReadAll(const char* path, long* n) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* d = malloc(*n + 1); fread(d, 1, *n, f); fclose(f); d[*n] = 0;
    return d;
}
static int IsVersion(DWORD t) { return (t & 0xFFFE0000) == 0xFFFE0000 && (t & 0xFFFF) >= 0x100 && (t & 0xFFFF) <= 0x300; }

// FNV-1a over the tokens up to the end token, comment blocks (0xFFFE, length in bits 16..30) skipped
static DWORD Hash(const DWORD* p, DWORD maxTokens) {
    DWORD h = 2166136261u;
    for (DWORD i = 0; i < maxTokens; ++i) {
        DWORD t = p[i];
        if ((t & 0xFFFF) == 0xFFFE && i) { i += (t >> 16) & 0x7FFF; continue; }
        h = (h ^ t) * 16777619u;
        if (t == 0x0000FFFF) break;
    }
    return h;
}

static Buf* Assemble(const char* path, DWORD flags) {
    AsmFn as = (AsmFn)GetProcAddress(LoadLibraryA("d3dx9_33.dll"), "D3DXAssembleShader");
    long n; unsigned char* src = ReadAll(path, &n);
    Buf *code = 0, *err = 0;
    if (FAILED(as((const char*)src, (UINT)n, 0, 0, flags, &code, &err))) {
        fprintf(stderr, "%s: %s\n", path, err ? (char*)BufPtr(err) : "assembly failed");
        exit(1);
    }
    return code;
}

static void PrintArray(const char* name, const DWORD* t, DWORD n) {
    printf("static const DWORD %s[] = {", name);
    for (DWORD i = 0; i < n; ++i) printf("%s0x%08lX,", i % 8 ? " " : "\n    ", t[i]);
    printf("\n};\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: fxdump dis|hash <file> | asm|frag <file.asm> <name>\n"); return 2; }
    if (!strcmp(argv[1], "dis") || !strcmp(argv[1], "hash")) {
        DisFn dis = (DisFn)GetProcAddress(LoadLibraryA("d3dx9_33.dll"), "D3DXDisassembleShader");
        if (!dis) { fprintf(stderr, "d3dx9_33.dll missing\n"); return 1; }
        long n; unsigned char* d = ReadAll(argv[2], &n);
        for (long i = 0; i + 8 <= n; i += 4) {
            const DWORD* p = (const DWORD*)(d + i);
            // a shader starts with its version and then a comment (CTAB) or an instruction; the effect also holds
            // copies of the version token in its parameter blocks, which the disassembler rejects
            if (!IsVersion(p[0])) continue;
            Buf* b = 0;
            if (FAILED(dis(p, FALSE, 0, &b)) || !b) continue;
            const char* text = (const char*)BufPtr(b);
            if (strstr(text, "???")) continue;          // not a shader start
            if (!strcmp(argv[1], "dis")) printf("==== shader at 0x%lx\n%s\n", i, text);
            else {
                DWORD left = (DWORD)((n - i) / 4);
                const char* body = (const char*)p;
                int wvp = 0, col = 0;
                for (long j = 0; j + 12 < n - i && j < 4096; ++j) {
                    if (!memcmp(body + j, "HeadlightWVP", 12)) wvp = 1;
                    if (!memcmp(body + j, "HeadlightColor", 14)) col = 1;
                    if (j > 8 && *(const DWORD*)(body + (j & ~3)) == 0x0000FFFF) break;
                }
                printf("0x%05lx %s_%lu_%lu hash 0x%08lX%s%s\n", i, (p[0] & 0x10000) ? "ps" : "vs",
                       (p[0] >> 8) & 0xFF, p[0] & 0xFF, Hash(p, left), wvp ? " HeadlightWVP" : "",
                       col ? " HeadlightColor" : "");
            }
            ((ULONG(WINAPI*)(Buf*))b->vt[2])(b);
        }
        return 0;
    }
    if ((!strcmp(argv[1], "asm") || !strcmp(argv[1], "frag")) && argc >= 4) {
        Buf* code = Assemble(argv[2], strcmp(argv[1], "frag") ? 0 : 2 /* D3DXSHADER_SKIPVALIDATION: a fragment */);
        const DWORD* t = (const DWORD*)BufPtr(code);
        DWORD n = BufSize(code) / 4, i = 0;
        // operand tokens can look like opcodes (vs_1_1 has no instruction lengths), so only the head is parsed:
        // the version, comments and (frag) the dcl's, then everything up to the end token, the last token
        if (!strcmp(argv[1], "frag")) {
            i = 1;
            for (;;) {
                if ((t[i] & 0xFFFF) == 0xFFFE) i += 1 + ((t[i] >> 16) & 0x7FFF);
                else if ((t[i] & 0xFFFF) == 0x1F) i += 3;                    // dcl: usage token + register
                else break;
            }
            --n;                                                              // the end token
        }
        printf("// %s (fxdump %s)\n", argv[2], argv[1]);
        PrintArray(argv[3], t + i, n - i);
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", argv[1]);
    return 2;
}
