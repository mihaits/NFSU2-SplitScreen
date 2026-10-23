// Part of NFSU2SplitScreen.cpp: start zoom, WidescreenFix free camera per player.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- start zoom for every player
// After the race intro the chase camera eases out from close to the car (its offset spring, CubicCameraMover +0x38:
// +0 distance (negative = behind), +8 its target, +0x60 height, +0x68 its target, +4/+0x10/+0x14/+0x18/+0x64 rates).
// Only player 1's camera is new then (view 1 showed the intro); the other players' cameras exist from the race load
// and finished the same ease-out unseen. On the first split frame, once player 1's spring starts, the other players'
// springs are set to the same fractions of their targets, so every view zooms out together.
#define CUBIC_MOVER_VTABLE_UPDATE 0x787F2C   // CubicCameraMover vtable 0x787F28, slot 1: Update 0x453570
#define FN_CUBIC_MOVER_UPDATE     0x453570
static uint8_t* g_chaseMover[MAX_LOCAL_PLAYERS];
static int g_chaseMoverFrame[MAX_LOCAL_PLAYERS];
static int g_startZoomFrames;            // frames left to wait for player 1's ease-out
static int g_lastLayout;
static void __fastcall NoteChaseMover(uint8_t* mover) {
    const uint8_t* cam = *reinterpret_cast<uint8_t* const*>(mover + 0x1C);
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k)
        if (cam && GetView(kPlayerViewId[k]).camera == cam) { g_chaseMover[k] = mover; g_chaseMoverFrame[k] = g_mmFrame; }
}
static __declspec(naked) void ChaseMoverUpdateHook() {
    __asm {
        push ecx
        call NoteChaseMover              // fastcall: ecx = mover
        pop ecx
        mov eax, FN_CUBIC_MOVER_UPDATE
        jmp eax
    }
}
static float* ChaseSpring(int k) {       // only a mover updated in the last frames (it is freed with its camera)
    if (!g_chaseMover[k] || g_mmFrame - g_chaseMoverFrame[k] > 2) return nullptr;
    return *reinterpret_cast<float**>(g_chaseMover[k] + 0x38);
}
static void UpdateStartZoom() {
    int layout = at<int>(LAYOUT);
    if (layout == 3 && g_lastLayout != 3 && SplitMode()) g_startZoomFrames = 30;
    g_lastLayout = layout;
    if (g_startZoomFrames <= 0) return;
    --g_startZoomFrames;
    float* p1 = ChaseSpring(0);
    if (!p1 || fabsf(p1[2]) < 1.0f || fabsf(p1[26]) < 0.1f || fabsf(p1[0] - p1[2]) < 0.5f) return;   // not started
    float dist = p1[0] / p1[2], height = p1[24] / p1[26];
    for (int k = 1; k < 2 + g_extraPlayers && k < MAX_LOCAL_PLAYERS; ++k)
        if (float* s = ChaseSpring(k)) {
            s[0] = s[7] = s[2] * dist;   // +0x1C follows +0 at the start (player 1's dump)
            s[24] = s[26] * height;
            s[1] = s[4] = s[5] = s[6] = s[25] = 0.0f;
        }
    Log("start zoom: other players' cameras from %.2f of their distance", dist);
    g_startZoomFrames = 0;
}

// ---------------------------------------------------------------- WidescreenFix free camera per player
// NFSU2 WidescreenFix ([CAMERA] Enable) adds a free look camera: it replaces the eCreateLookAtMatrix call in
// CubicCameraMover::Update (0x453B44, call 0x5BA9A0(out, from, to, up); esi = mover: +0x1C camera, +0x48 mode, +0x51
// look behind) and turns the camera around the car by one global yaw/pitch offset, from the right stick (the game's
// last polled joystick slot) and the mouse, returning after a timeout once the car moves. In split screen every
// player's camera went through it: one offset (and one stick) for everyone.
// Once WidescreenFix has patched the call, the plugin chains in front of it: outside split racing everything goes to
// WidescreenFix as before; while split racing each player's camera (mover camera == that player's view camera) gets
// WidescreenFix's behaviour with its own state, that player's right stick, and the mouse for player 1. Settings come
// from WidescreenFix's ini ([CAMERA], [MISC] RightStickDeadzone).
#define AT_LOOKAT_CALL     0x453B44
#define FN_LOOKAT          0x5BA9A0      // void __cdecl eCreateLookAtMatrix(bMatrix4*, bVector3* from, to, up)
typedef void(__cdecl* LookAtFn)(void*, float*, float*, float*);
static LookAtFn g_lookAtNext = nullptr;  // WidescreenFix's (or the game's) function at the call site
struct FreeCam { float yaw, pitch, idle, prevX, prevY, speed; bool prevValid; LARGE_INTEGER last; };
static FreeCam g_freeCam[MAX_LOCAL_PLAYERS];
static struct { bool loaded, enabled, invert, horizontalOnly; float timeout, returnSpeed, stickSens, mouseSens, deadzone; } g_freeCamCfg;
static int g_frameMouseDx, g_frameMouseDy;   // mouse movement of this frame (player 1's camera)

static void LoadFreeCamConfig() {
    g_freeCamCfg.loaded = true;
    char path[MAX_PATH];
    strcpy_s(path, g_iniPath);
    char* slash = strrchr(path, '\\');
    if (!slash) return;
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "NFSUnderground2.WidescreenFix.ini");
    auto readF = [&](const char* sec, const char* key, float def) {
        char buf[32];
        GetPrivateProfileStringA(sec, key, "", buf, sizeof(buf), path);
        return buf[0] ? static_cast<float>(atof(buf)) : def;
    };
    g_freeCamCfg.enabled = GetPrivateProfileIntA("CAMERA", "Enable", 1, path) != 0;
    g_freeCamCfg.timeout = readF("CAMERA", "CameraReturnTimeout", 3.0f);
    g_freeCamCfg.returnSpeed = readF("CAMERA", "CameraReturnSpeed", 2.0f);
    g_freeCamCfg.stickSens = readF("CAMERA", "StickLookSensitivity", 1.0f);
    g_freeCamCfg.mouseSens = readF("CAMERA", "MouseLookSensitivity", 1.0f) * 0.15f;
    g_freeCamCfg.invert = GetPrivateProfileIntA("CAMERA", "InvertLook", 0, path) != 0;
    g_freeCamCfg.horizontalOnly = GetPrivateProfileIntA("CAMERA", "OnlyAllowHorizontalStickMovement", 0, path) != 0;
    float dz = readF("MISC", "RightStickDeadzone", 10.0f);
    g_freeCamCfg.deadzone = (dz < 0 ? 0 : dz > 100 ? 100 : dz) / 100.0f;
    Log("free camera: WidescreenFix camera %s", g_freeCamCfg.enabled ? "on" : "off");
}

static float LerpAngleDeg(float from, float to, float t) {
    float diff = to - from;
    while (diff > 180.0f) diff -= 360.0f;
    while (diff < -180.0f) diff += 360.0f;
    return from + diff * t;
}

static void __cdecl SplitLookAt(const uint8_t* mover, void* out, float* from, float* to, float* up) {
    int k = -1;
    if (SplitRacing() && g_freeCamCfg.enabled)
        for (int i = 0; i < 2 + g_extraPlayers && i < MAX_LOCAL_PLAYERS; ++i)
            if (GetView(kPlayerViewId[i]).camera == *reinterpret_cast<uint8_t* const*>(mover + 0x1C)) k = i;
    if (k < 0) { g_lookAtNext(out, from, to, up); return; }
    if (k == 0) {   // WidescreenFix's own function on a throwaway copy: it also hides the mouse cursor while racing
        float scratchOut[16], f[4] = {from[0], from[1], from[2], 0}, t[4] = {to[0], to[1], to[2], 0}, u[4] = {up[0], up[1], up[2], 0};
        g_lookAtNext(scratchOut, f, t, u);
    }
    auto lookAt = reinterpret_cast<LookAtFn>(FN_LOOKAT);
    FreeCam& c = g_freeCam[k];
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = c.last.QuadPart ? static_cast<float>(now.QuadPart - c.last.QuadPart) / freq.QuadPart : 0.0f;
    if (dt > 0.25f) dt = 0.25f;
    c.last = now;
    int mode = *reinterpret_cast<const int*>(mover + 0x48);
    if (mover[0x51] || (mode != 0 && mode != 1 && mode != 5)) {   // look behind / other cameras: original
        c.yaw = c.pitch = c.idle = 0.0f;
        c.prevValid = false;
        lookAt(out, from, to, up);
        return;
    }
    if (c.prevValid && dt > 0.0f)
        c.speed = sqrtf((to[0] - c.prevX) * (to[0] - c.prevX) + (to[1] - c.prevY) * (to[1] - c.prevY)) / dt;
    c.prevX = to[0]; c.prevY = to[1]; c.prevValid = true;
    float stickX = 0.0f, stickY = 0.0f;
    int16_t rx, ry;
    if (XPadRightStick(g_assignedPad[k], &rx, &ry)) {
        float x = rx / 32768.0f, y = -(ry + 1) / 32768.0f;   // DirectInput Ry points down
        stickX = fabsf(x) <= g_freeCamCfg.deadzone ? 0.0f : -x;
        stickY = fabsf(y) <= g_freeCamCfg.deadzone || g_freeCamCfg.horizontalOnly ? 0.0f : y;
    }
    float mouseX = 0.0f, mouseY = 0.0f;
    if (k == (g_keyboardSeat >= 0 ? g_keyboardSeat : 0)) {     // the mouse goes with the keyboard
        mouseX = static_cast<float>(g_frameMouseDx); mouseY = static_cast<float>(g_frameMouseDy);
        g_frameMouseDx = g_frameMouseDy = 0;
    }
    float deltaX = stickX - mouseX * g_freeCamCfg.mouseSens;
    float deltaY = stickY + mouseY * g_freeCamCfg.mouseSens;
    bool input = fabsf(deltaX) > 0.01f || fabsf(deltaY) > 0.01f;
    if (input && !PauseMenuOpen()) {
        c.yaw += deltaX * g_freeCamCfg.stickSens;
        c.pitch += deltaY * g_freeCamCfg.stickSens * (g_freeCamCfg.invert ? -1.0f : 1.0f);
        c.idle = 0.0f;
    } else {
        c.idle += dt;
        if (c.idle >= g_freeCamCfg.timeout && c.speed > 0.5f) {   // back to the game's camera once the car moves
            float blend = 1.0f - expf(-g_freeCamCfg.returnSpeed * dt);
            c.yaw = LerpAngleDeg(c.yaw, 0.0f, blend);
            c.pitch += (0.0f - c.pitch) * blend;
            if (fabsf(c.yaw) < 0.3f && fabsf(c.pitch) < 0.3f) c.yaw = c.pitch = 0.0f;
        }
    }
    if (c.yaw == 0.0f && c.pitch == 0.0f) { lookAt(out, from, to, up); return; }
    if (c.pitch > 60.0f) c.pitch = 60.0f;
    if (c.pitch < -20.0f) c.pitch = -20.0f;
    const float deg = 0.0174533f;
    float dx = from[0] - to[0], dy = from[1] - to[1], dz = from[2] - to[2];
    float dist = sqrtf(dx * dx + dy * dy + dz * dz), dist2D = sqrtf(dx * dx + dy * dy);
    float yaw = atan2f(dy, dx) + c.yaw * deg;
    float pitch = atan2f(dz, dist2D) + c.pitch * deg;
    if (pitch > 1.39626f) pitch = 1.39626f;
    if (pitch < -1.39626f) pitch = -1.39626f;
    from[0] = to[0] + dist * cosf(pitch) * cosf(yaw);
    from[1] = to[1] + dist * cosf(pitch) * sinf(yaw);
    from[2] = to[2] + dist * sinf(pitch);
    lookAt(out, from, to, up);
}

static __declspec(naked) void LookAtHook() {
    __asm {
        push dword ptr [esp + 16]        // up
        push dword ptr [esp + 16]        // to
        push dword ptr [esp + 16]        // from
        push dword ptr [esp + 16]        // out
        push esi                         // the camera mover
        call SplitLookAt
        add esp, 20
        ret
    }
}

// Per frame: chain in front of WidescreenFix's camera once it has patched the call (it does so during its own init),
// and take this frame's mouse movement.
static void UpdateFreeCamHook() {
    XMouseTakeDelta(&g_frameMouseDx, &g_frameMouseDy);
    if (!SplitRacing()) for (FreeCam& c : g_freeCam) c = FreeCam{};
    const uint8_t* site = reinterpret_cast<const uint8_t*>(AT_LOOKAT_CALL);
    if (site[0] != 0xE8) return;
    uintptr_t target = AT_LOOKAT_CALL + 5 + *reinterpret_cast<const int32_t*>(site + 1);
    if (target == reinterpret_cast<uintptr_t>(&LookAtHook) || target == FN_LOOKAT) return;   // chained / no WSF camera
    if (!g_freeCamCfg.loaded) LoadFreeCamConfig();
    g_lookAtNext = reinterpret_cast<LookAtFn>(target);
    WriteCall(AT_LOOKAT_CALL, &LookAtHook);
    Log("free camera: chained in front of WidescreenFix's camera (%p)", reinterpret_cast<void*>(target));
}

void* GetLocalPlayer(int k);
static void __cdecl FfUpdate() {
    reinterpret_cast<void(__cdecl*)()>(FN_FF_UPDATE)();
    for (int pair = 1; SplitMode() && 2 * pair < 2 + g_extraPlayers && pair < MAX_LOCAL_PLAYERS / 2; ++pair) {
        SwapFfShadow(pair);
        g_ffPortBase = 2 * pair;
        reinterpret_cast<void(__cdecl*)()>(FN_FF_UPDATE)();
        g_ffPortBase = 0;
        SwapFfShadow(pair);
    }
    bool on = g_rumble && at<int>(GAME_STATE) == 6 && !PauseMenuOpen() &&
              (!SplitMode() || SplitRacing());
    int players = SplitMode() ? 2 + g_extraPlayers : 1;
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        int pad = -1;
        auto* player = static_cast<uint8_t*>(k < players ? GetLocalPlayer(k) : nullptr);
        bool feedback = SplitMode() ? g_settings[k][PLAYER_FEEDBACK] != 0
                                    : at<uint8_t>(PLAYER_SETTINGS + PLAYER_FEEDBACK) != 0;
        if (on && player && feedback) pad = SplitMode() ? g_assignedPad[k] : (k == 0 ? XPadLastActive() : -1);
        if (g_rumblePad[k] >= 0 && g_rumblePad[k] != pad) XPadSetRumble(g_rumblePad[k], 0, 0);
        g_rumblePad[k] = pad;
        int port = player ? *reinterpret_cast<int*>(player + 8) : -1;
        if (pad < 0 || port < 0 || port >= MAX_LOCAL_PLAYERS) continue;
        int v = g_ffMotor[port] < 0 ? 0 : g_ffMotor[port] > 255 ? 255 : g_ffMotor[port];
        float largeMotor = v / 255.0f, smallMotor = v > 128 ? 1.0f : 0.0f;   // PS2 0x333530: act[1] = v, act[0] = v > 0x80
        XPadSetRumble(pad, largeMotor * g_rumbleStrength, smallMotor * g_rumbleStrength);
    }
}

static __declspec(naked) void RumbleHook() {
    __asm {
        cmp dword ptr [esp + 8], 2       // port
        jb play
        jmp RumbleExtraPort              // players 3..6: their shadow state (cdecl, same arguments)
    play:
        mov eax, dword ptr [esp + 4]
        push esi
        push FN_RUMBLE_RET
        ret
    }
}

// Collision rumble per player index (0x865370 channels, 2 entries): ignore players 3/4.
#define FN_IMPACT_RUMBLE   0x572E50          // void __cdecl (playerIndex, vector); entry: push ebp; mov ebp,esp; and esp,-16
#define FN_IMPACT_RUMBLE_RET 0x572E56
static __declspec(naked) void ImpactRumbleHook() {
    __asm {
        cmp dword ptr [esp + 4], 2       // player index
        jb play
        ret
    play:
        push ebp
        mov ebp, esp
        and esp, 0xFFFFFFF0
        push FN_IMPACT_RUMBLE_RET
        ret
    }
}

// "Joystick present on this slot" (0x5BFD70: 0x86FFB4[slot] != 0, 2 slots) gates the driving force feedback
// (0x406460, the tyre/drift effects, slot = input port); ports 2/3 read past the array -> garbage effect pointers.
#define FN_JOY_SLOT_PRESENT 0x5BFD70         // bool __stdcall (slot)
static __declspec(naked) void JoySlotPresent() {
    __asm {
        mov ecx, dword ptr [esp + 4]
        xor eax, eax
        cmp ecx, 2
        jae done
        cmp dword ptr [ecx * 4 + 0x86FFB4], 0
        setne al
    done:
        ret 4
    }
}

// Action 1's handler 0x571A20 keeps per-port state in arrays of 4: bytes [port + 0x864788] / [port + 0x8647A0]
// (0x86478C is another variable) and the frontend's per-port flags 0x50C5D0(port): [0x7F91D4 + row * 0x1C + port * 4],
// 21 rows of 4 port columns + 3 other dwords. Ports 4/5 with action 1 zeroed those other dwords: no pad worked in the
// menus any more. Ports 4/5 get their id only; their state comes from PollExtraPort like ports 2/3.
static void __cdecl InputInit() {
    reinterpret_cast<void(__cdecl*)()>(FN_INPUT_INIT)();
    for (int port = 2; port < INPUT_PORTS; ++port) {
        *reinterpret_cast<int*>(g_portTables + port * PORT_TABLE_SIZE + 0x1B4) = port;
        if (port < 4) reinterpret_cast<void(__cdecl*)(int, int, uint32_t, int)>(FN_ADD_BINDING)(port, 1, 0x571A20, 0);
    }
}
