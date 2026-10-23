// Part of NFSU2SplitScreen.cpp: input port tables (4 ports).
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- input port tables (4 ports)
// 0x8655A0: 2 x 0x1C8 port tables (binding nodes, port id +0x1B4, device type +0x1B8, ...), built by a static
// vector ctor. 32 code refs (all inside .text); 0x865930 is both "end of tables" (two loop bounds) and the next
// global (mov ecx, 0x865930) -> only the two cmp operands move. Done before the game's static constructors run.
#define OLD_PORT_TABLES    0x8655A0
#define PORT_TABLE_SIZE    0x1C8
#define OLD_PORT_TABLES_END 0x865930
#define AT_PORT_TABLES_CTOR_COUNT 0x77D9CB  // imm8 of the static vector ctor's count
#define AT_PORT_TABLES_DTOR_COUNT 0x782296  // imm8 of the atexit vector dtor's count
#define AT_PORT_BOUND_1    0x57B3AC          // cmp ebp, 0x865930 (operand)
#define AT_PORT_BOUND_2    0x580AD4          // cmp esi, 0x865930 (operand)
#define AT_INPUT_NODE_POOL 0x579559          // imm32 0xA0: binding node pool size
#define AT_INPUT_INIT_CALL 0x57ED80          // call 0x579550 (input init: port ids, pool, action 1 per port)
#define FN_INPUT_INIT      0x579550
#define FN_ADD_BINDING     0x579410          // void __cdecl (port, action, handler, arg)
alignas(16) static uint8_t g_portTables[INPUT_PORTS * PORT_TABLE_SIZE];

static int RelocatePortTables() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(0x400000);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(0x400000 + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    auto base = reinterpret_cast<uint32_t>(g_portTables);
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
            if (v < OLD_PORT_TABLES || v >= OLD_PORT_TABLES_END) continue;
            uint32_t nv = base + (v - OLD_PORT_TABLES);
            memcpy(p + o, &nv, 4);
            ++patched;
            o += 3;
        }
        VirtualProtect(p, n, old, &old);
    }
    uint32_t end = base + INPUT_PORTS * PORT_TABLE_SIZE;
    Write(AT_PORT_BOUND_1, &end, 4);
    Write(AT_PORT_BOUND_2, &end, 4);
    const uint8_t count = INPUT_PORTS;
    Write(AT_PORT_TABLES_CTOR_COUNT, &count, 1);
    Write(AT_PORT_TABLES_DTOR_COUNT, &count, 1);
    const uint32_t pool = 0x50 * INPUT_PORTS;   // the game's 0xA0 for 2 ports
    Write(AT_INPUT_NODE_POOL, &pool, 4);
    return patched;
}

// Rumble, as on the console. The game's rumble engine is shared with the PS2 version (SLUS_210.65): events
// 0x5727B0(type, port, amount) pick an effect from the table 0x7FB7D8 (PS2 0x523070; same parameter blocks
// 0x79DC6C.. = PS2 0x487148..) and queue its curve on that port's channel (0x5726D0 -> 0x572390; PS2 0x2E89D8 ->
// 0x2E8888 -> 0x2E8388). Channels: 0x8653B0 + port * 0x60 = 2 sub-channels of 0x30 (PS2 0x5231C0), each summing up to
// 3 running curves into an int 0..255 at +0x28 every frame (0x572470; PS2 0x2E8510). The per-frame update 0x572610
// (PS2 0x2E8748) then hands the port's values to the pad: on PS2 0x333530(port, sub0, sub1) -> DualShock actuators
// (0x333530(port, v, v), v = sub-channel 0's value); on PC that call is an empty stub (0x4022C0 at 0x57266E).
// Both actuators get sub-channel 0's value v (large motor = v, small motor on when v > 128); restored here with that
// conversion on the player's own XInput pad (large -> left motor, small -> right motor at full speed). Collisions (0x572E50 -> 0x865360) only drive the wheels' constant force, as on PS2.
// The channel state exists for 2 ports (channels 0x8653B0 [0xC0], effect strengths 0x864728 [0x60, indexed type * 2 +
// port]); players 3/4 (ports 2/3) get a shadow copy that is swapped in for their events and for a second run of the
// per-frame update.
#define FN_RUMBLE          0x5727B0          // void __cdecl (type, port, amount); entry 5 bytes: mov eax,[esp+4]; push esi
#define FN_RUMBLE_RET      0x5727B5
#define AT_FF_UPDATE_CALL  0x5814AD          // `call 0x572610` (per frame)
#define FN_FF_UPDATE       0x572610
#define AT_FF_PAD_OUTPUT   0x57266E          // `call 0x4022C0` (stub; cdecl (port, value, value))
#define FF_CHANNELS        0x8653B0
#define FF_CHANNELS_SIZE   0xC0
#define FF_STRENGTHS       0x864728
#define FF_STRENGTHS_SIZE  0x60
// Player options "Feedback" = per-player settings +4 (byte; toggled by 0x49B260). The menu greys it unless the last
// DirectInput device enumerated reported DIDC_FORCEFEEDBACK (0x5B8240 returns 0x87078C, only read by the 2 option
// screens); the plugin's virtual pads don't, so it is forced selectable while rumble is on.
#define FN_FF_CAPABLE      0x5B8240
#define AT_MAP_MODE_MAX    0x49AFF9      // Map mode option: `setne cl` (split mode -> max Static)
// Car detail: with the split layout (0x5B98A0 == 3) the car renderers 0x620AE0 and 0x6162E0 scale the car's screen size
// by 0.7 (0x7A6700) and force the LOD range to 1..1 (the low model), and the car parts renderer 0x615630 scales the size
// too. Each `jne` past that block becomes `jmp`: split cars use the Car Geometry Detail option like single player.
static const uintptr_t kSplitCarLodJumps[] = {0x620C8C, 0x616316, 0x6156CC};
static uint8_t g_ffShadowChannels[MAX_EXTRA_PLAYERS / 2][FF_CHANNELS_SIZE];      // per pair of ports 2/3, 4/5
static uint8_t g_ffShadowStrengths[MAX_EXTRA_PLAYERS / 2][FF_STRENGTHS_SIZE];
static int g_ffPortBase = 0;                 // 2 / 4 while the players 3/4 / 5/6 state is swapped in
static int g_ffMotor[MAX_LOCAL_PLAYERS];     // per port: motor value 0..255
static bool g_rumble = true;
static float g_rumbleStrength = 1.0f;
static int g_rumblePad[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1, -1, -1};   // pad last sent to (to silence it)
static void* g_origRumble;

static void SwapFfShadow(int pair) {              // pair 1: ports 2/3, pair 2: ports 4/5
    auto swap = [](uint8_t* a, uint8_t* b, size_t n) { for (size_t i = 0; i < n; ++i) { uint8_t t = a[i]; a[i] = b[i]; b[i] = t; } };
    swap(reinterpret_cast<uint8_t*>(FF_CHANNELS), g_ffShadowChannels[pair - 1], FF_CHANNELS_SIZE);
    swap(reinterpret_cast<uint8_t*>(FF_STRENGTHS), g_ffShadowStrengths[pair - 1], FF_STRENGTHS_SIZE);
}

static void __cdecl RumbleExtraPort(int type, int port, float amount) {
    if (port - 2 >= g_extraPlayers || port >= MAX_LOCAL_PLAYERS) return;
    int pair = port / 2;
    SwapFfShadow(pair);
    g_ffPortBase = 2 * pair;
    reinterpret_cast<void(__cdecl*)(int, int, float)>(g_origRumble)(type, port & 1, amount);
    g_ffPortBase = 0;
    SwapFfShadow(pair);
}

// the stub's arguments: (port, value, value) - sub-channel 0's motor value twice (the PS2 passes the same byte for
// both actuators too: 0x2E87E4 / 0x2E87EC load +0x28 of the same sub-channel)
static void __cdecl FfPadOutput(int port, int value, int) {
    g_ffMotor[(port & 1) + g_ffPortBase] = value;
}

static bool PauseMenuOpen() {
    using ActiveFn = bool(__cdecl*)(const char*);
    auto active = reinterpret_cast<ActiveFn>(FN_FE_PACKAGE_ACTIVE);
    return active("UI_Pause.fng") || active("UI_PauseOptions.fng") || active("UI_PauseOptionsMain.fng") ||
           active("UI_PC_Customize_Options.fng");
}
