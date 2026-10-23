// Part of NFSU2SplitScreen.cpp: DualSense lights (player LEDs, RPM lightbar; triggers follow in a later step).
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- player LEDs
// A native DualSense (pad 8..11, see docs/input.md) shows who controls the game with its 5 player LEDs:
// - from the car-select join until the race is left, a pad that a player joined with shows that player's number
//   (patterns of PS5: 1 = --x--, 2 = -x-x-, 3 = x-x-x, 4 = xx-xx). Seats are g_assignedPad[k]; the session counts as
//   on in the split car select and in game states 4..6 (loading, racing, results). Back in the frontend (state 3)
//   after a race the numbers stop (g_assignedPad itself is left alone);
// - at all other times the pad that controls the game shows 1: the last pad used, or on the car select the pad of the
//   player whose turn it is (as PadForDevice picks it);
// - while a Press START popup is open nobody controls: pads that haven't joined all flash;
// - every other DualSense flashes all 5 LEDs (1 Hz), so a pad that doesn't drive the game is recognisable.
//
// ---------------------------------------------------------------- lightbar
// While racing (state 6, race stage 4 = *[0x890118], not paused) the lightbar of the pad that drives local player k
// (split: its seat's pad; 1P: the last pad used) shows the engine rpm: green at idle through yellow to red at the
// shift-light rpm, flashing red (4 Hz) from there on, solid blue while nitrous burns. Addresses (live-verified):
//   car = *(player + 4); rpm = what the tach shows: with spec = *(car+0x14), *(int*)(spec+4) == 1, !*(u8*)(spec+0x900),
//   r = *(car+0x968) non-null and not drag/drift -> *(float*)(r+0x58), else *(float*)(car+0x440);
//   idle = *(float*)(eng+0x1F0), eng = *(car+0x20); gear = *(int*)(car+0x434) (0 reverse, 1 neutral, 2 = 1st ...);
//   shift (player cars, *(int*)(car+0x28) == 1): drv = *(*(car+0x34)+0x4C), up = *(float*)(drv+0x84+4*gear),
//   f = *(float*)(0x7FBF60 + 4*(gear + 9**(int*)(drv+0xF0))) * *(float*)(drv+0xE4), shift = up - *(float*)0x7FBF5C * f
//   (the tach's shift light, 0x4C6ACF..0x4C6B45); nitrous burns while burn = *(u32*)(car+0x460) is not 0 / 0x7FFFFFFF.
// While the pause menu is open every driving pad shows its player's colour at 25% (the click toggle waits).
// In split racing the touchpad click toggles the player's solid colour (below). Outside racing the bar keeps a fixed dim blue until the later steps (pause, countdown, finish, player colours).
static bool g_lightbar = true;            // [LIGHTS] Lightbar
static int  g_lightbarBrightness = 100;   // [LIGHTS] LightbarBrightness, percent

// Split racing: a click on the DualSense touchpad switches that player's lightbar between rpm and the player's solid
// colour (their minimap marker's hue, fully saturated: P1 green, P2 red, P3 blue, P4 amber; the bar washes pale
// colours out, so no channel is mixed in except where the hue needs it). Kept per seat until the game closes.
static const uint8_t kPlayerColour[MAX_LOCAL_PLAYERS][3] = {{0, 255, 0}, {255, 0, 0}, {0, 64, 255}, {255, 150, 0}};
static bool g_lightColourMode[MAX_LOCAL_PLAYERS];

static bool ReadCarLight(void* player, float* t, bool* shiftLight, bool* nitrous) {
    __try {
        auto* car = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4);
        if (!car) return false;
        auto* spec = *reinterpret_cast<uint8_t**>(car + 0x14);
        auto* eng = *reinterpret_cast<uint8_t**>(car + 0x20);
        auto* r968 = *reinterpret_cast<uint8_t**>(car + 0x968);
        if (!spec || !eng) return false;
        bool use968 = *reinterpret_cast<int*>(spec + 4) == 1 && !spec[0x900] && r968 && !IsDragRace() && !IsDriftRace();
        float rpm = use968 ? *reinterpret_cast<float*>(r968 + 0x58) : *reinterpret_cast<float*>(car + 0x440);
        float idle = *reinterpret_cast<float*>(eng + 0x1F0), top = *reinterpret_cast<float*>(eng + 0x1F4);
        int gear = *reinterpret_cast<int*>(car + 0x434);
        float shift = 0;
        if (*reinterpret_cast<int*>(car + 0x28) == 1 && gear >= 0 && gear < 9) {
            auto* mover = *reinterpret_cast<uint8_t**>(car + 0x34);
            auto* drv = mover ? *reinterpret_cast<uint8_t**>(mover + 0x4C) : nullptr;
            if (drv) {
                float f = at<float>(0x7FBF60 + 4 * (gear + 9 * *reinterpret_cast<int*>(drv + 0xF0))) *
                          *reinterpret_cast<float*>(drv + 0xE4);
                shift = *reinterpret_cast<float*>(drv + 0x84 + 4 * gear) - at<float>(0x7FBF5C) * f;
            }
        }
        float range = (shift > idle ? shift : top) - idle;
        *t = range > 1 ? (rpm - idle) / range : 0;
        *t = *t < 0 ? 0 : *t > 1 ? 1 : *t;
        *shiftLight = shift > idle && rpm >= shift;
        uint32_t burn = *reinterpret_cast<uint32_t*>(car + 0x460);
        *nitrous = burn != 0 && burn != 0x7FFFFFFF;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Lightbar colour of local player k's car; false if there is none to show.
static bool CarLightbar(int k, uint8_t rgb[3]) {
    float t;
    bool shiftLight, nitrous;
    void* player = GetLocalPlayer(k);
    if (!player || !ReadCarLight(player, &t, &shiftLight, &nitrous)) return false;
    float r, g, b = 0;
    if (nitrous) { r = 0; g = 0; b = 1; }
    else if (shiftLight) { r = (GetTickCount() % 250) < 125 ? 1.0f : 0.0f; g = 0; }
    else { r = t * 2 > 1 ? 1.0f : t * 2; g = (1 - t) * 2 > 1 ? 1.0f : (1 - t) * 2; }
    float scale = 255.0f * g_lightbarBrightness / 100.0f;
    rgb[0] = static_cast<uint8_t>(r * scale); rgb[1] = static_cast<uint8_t>(g * scale); rgb[2] = static_cast<uint8_t>(b * scale);
    return true;
}
static bool g_playerLeds = true;          // [LIGHTS] PlayerLeds

static const uint8_t kPlayerLedPattern[MAX_LOCAL_PLAYERS] = {0x04, 0x0A, 0x15, 0x1B};

// The pad that drives the frontend now (-1: none, in a split race every seat drives its own player).
static int ControllingPad() {
    if (!SplitMode()) return XPadLastActive();
    if (SplitRacing()) return -1;
    if (g_inCarSelect) {
        int fe = at<int>(FE_PLAYER);
        int seat = (g_seat & 1) == fe ? g_seat : fe;
        int pad = seat >= 0 && seat < MAX_LOCAL_PLAYERS ? g_assignedPad[seat] : PAD_NONE;
        return pad != PAD_NONE ? pad : XPadLastActive();
    }
    return XPadLastActive();
}

static void UpdatePadLights() {
    int state = at<int>(GAME_STATE);
    // Game state 3 is the frontend (menus before and after a race), 4/5 loading, 6 racing and results, 7/1 the way back.
    static bool sessionOn = false;
    if (!SplitMode() || state == 3 || state == 0) sessionOn = false;
    if (SplitMode() && (g_inCarSelect || (state >= 4 && state <= 6))) sessionOn = true;
    // Press START popup: any pad may join, so none is "the" controller; the unjoined ones all flash
    bool popup = SplitMode() && reinterpret_cast<bool(__cdecl*)(const char*)>(FN_FE_PACKAGE_ACTIVE)("2P_PressStart.fng");
    int ctl = popup ? -1 : ControllingPad();
    // Seats from the previous round stay in g_assignedPad until player 1 joins again; during a popup only the seats
    // before the joining one are real (as PressStartFilter sees it)
    int seatLimit = MAX_LOCAL_PLAYERS;
    if (popup) {
        int player = at<int>(FE_PLAYER);
        seatLimit = (g_seat & 1) == player ? g_seat : player;
    }
    int* mgr = at<int*>(0x890118);
    bool racing = state == 6 && mgr && *mgr == 4;
    bool paused = racing && PauseMenuOpen();
    bool flashOn = (GetTickCount() % 1000) < 500;
    for (int pad = 0; pad < XPadMax(); ++pad) {
        if (!XPadIsDualSense(pad)) continue;
        uint8_t leds = 0;
        if (g_playerLeds) {
            int seat = -1;
            for (int k = 0; sessionOn && k < seatLimit; ++k)
                if (g_assignedPad[k] == pad) seat = k;
            if (seat >= 0) leds = kPlayerLedPattern[seat];
            else if (pad == ctl) leds = kPlayerLedPattern[0];
            else leds = flashOn ? 0x1F : 0;
        }
        uint8_t rgb[3] = {0, 0, 64};
        if (!g_lightbar) rgb[2] = 0;
        else if (racing) {
            int k = -1;                    // local player this pad drives
            if (SplitMode()) { for (int q = 0; q < MAX_LOCAL_PLAYERS; ++q) if (g_assignedPad[q] == pad) k = q; }
            else if (pad == XPadLastActive()) k = 0;
            bool toggle = k >= 0 && !paused && SplitMode() && XPadTouchClickPressed(pad);
            if (toggle) {
                g_lightColourMode[k] = !g_lightColourMode[k];
                Log("lightbar: player %d shows %s", k + 1, g_lightColourMode[k] ? "player colour" : "rpm");
            }
            if (k < 0) {
            } else if (paused) {            // the player's colour at 25%
                for (int c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(kPlayerColour[k][c] * g_lightbarBrightness / 400);
            } else if (SplitMode() && g_lightColourMode[k]) {
                for (int c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(kPlayerColour[k][c] * g_lightbarBrightness / 100);
            } else CarLightbar(k, rgb);
        }
        XPadSetLights(pad, rgb, leds);
    }
}
