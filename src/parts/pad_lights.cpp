// Part of NFSU2SplitScreen.cpp: DualSense lights (player LEDs; lightbar and triggers follow in later steps).
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
// The lightbar keeps a fixed dim blue until its own step.
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
    static const uint8_t kRgb[3] = {0, 0, 64};
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
        XPadSetLights(pad, kRgb, leds);
    }
}
