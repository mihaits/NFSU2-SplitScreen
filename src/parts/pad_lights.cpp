// Part of NFSU2SplitScreen.cpp: DualSense lights (player LEDs, lightbar) and adaptive triggers.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- player LEDs
// A native DualSense (pad 10..17, see docs/input.md) shows who controls the game with its 5 player LEDs:
// - from the car-select join until the race is left, a pad that a player joined with shows that player's number
//   (patterns of PS5: 1 = --x--, 2 = -x-x-, 3 = x-x-x, 4 = xx-xx; the mod's 5 = xxxxx, 6 = -xxx-). Seats are g_assignedPad[k]; the session counts as
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
// Countdown (stage 3): red for the 3 and the 2, amber for the 1, each flashing (on 0.6 s, off 0.4 s), then green for 1 s from GO; then the rpm again.
// Finished: position 1 gold, 2 silver, 3 bronze, off outside the podium, until the game state leaves 6.
// While the pause menu is open every driving pad shows its player's colour at 25% (the click toggle waits).
// In split racing the touchpad click toggles the player's solid colour (below). Outside racing the bar keeps a fixed dim blue until the later steps (pause, countdown, finish, player colours).
static bool g_lightbar = true;            // [LIGHTS] Lightbar
static int  g_lightbarBrightness = 100;   // [LIGHTS] LightbarBrightness, percent

// Split racing: a click on the DualSense touchpad switches that player's lightbar between rpm and the player's solid
// colour (their minimap marker's hue, fully saturated: P1 green, P2 red, P3 blue, P4 amber; the bar washes pale
// colours out, so no channel is mixed in except where the hue needs it). Kept per seat until the game closes.
static const uint8_t kPlayerColour[MAX_LOCAL_PLAYERS][3] = {{0, 255, 0}, {255, 0, 0}, {0, 64, 255}, {255, 150, 0},
                                                             {160, 0, 255}, {0, 220, 220}};   // P5 purple, P6 cyan
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

// Finished (player +0x50 != 0; racer progress +0x14 counts laps), position = *(u8*)(racer+0xA), 1-based, racer =
// *(car+0x1C).
static int FinishedPosition(void* player) {
    __try {
        auto* car = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4);
        auto* racer = car ? *reinterpret_cast<uint8_t**>(car + 0x1C) : nullptr;
        if (racer && *reinterpret_cast<int*>(static_cast<uint8_t*>(player) + 0x50)) return racer[0xA] ? racer[0xA] : 99;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
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

static const uint8_t kPlayerLedPattern[MAX_LOCAL_PLAYERS] = {0x04, 0x0A, 0x15, 0x1B, 0x1F, 0x0E};   // 5 = xxxxx, 6 = -xxx-

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

// ---------------------------------------------------------------- adaptive triggers
// While a player races (stage >= 4, not paused, not finished) the pad that drives them gets trigger effects from the
// tyres' grip. Per wheel w = *(*(car+0x34) + 0x38 + 4*i) (0 FL, 1 FR, 2 RL, 3 RR): *(float*)(w+0x170) is the grip used
// (request / limit, 1.0 = at the limit; all four < 0.005 = airborne, which never reads as the limit) and
// *(float*)(w+0x124) the longitudinal force. Effects (modes of docs/input.md; never the calibration modes 0xFC..0xFE):
//   L2 (brake): feedback from zone 2, strength 3; vibration (zone 2, amplitude 5, 35 Hz) while braking with the front axle at the limit (the rear reads 1.0
//     when slow or stopped, so it isn't used; wheel speed *(float*)(w+0x28) must be above 15)
//   R2 (throttle): feedback from zone 3, strength 2; vibration (zone 3, amplitude 4, 25 Hz) while the driven wheels
//     are at the limit (wheelspin); (zone 2, amplitude 3, 60 Hz) at the shift-light rpm; feedback strength 8 for the
//     first 300 ms of a nitrous burn
// "At the limit" = grip >= 0.98 for 50 ms, released after 100 ms below, so a one-frame kerb spike doesn't buzz.
// Driven wheels: the axle with the larger longitudinal force, all four if both axles are within 10%.
// Anything else (menus, countdown, pause, finish, the player turned them off) sends Off on both triggers.
static bool g_adaptiveTriggers = true;    // [LIGHTS] AdaptiveTriggers: the default for players without their own choice
static int  g_triggerStrength = 100;      // [LIGHTS] TriggerStrength, percent
static int  g_triggersOn[MAX_LOCAL_PLAYERS] = {-1, -1, -1, -1};   // per player: 1 on, 0 off, -1 the ini default

static bool TriggersOn(int k) { return g_triggersOn[k] < 0 ? g_adaptiveTriggers : g_triggersOn[k] != 0; }

// The players' own choices: [Player1..4] AdaptiveTriggers in NFSU2SplitScreen_Players.ini.
static void LoadTriggerChoices() {
    for (int k = 0; k < MAX_LOCAL_PLAYERS; ++k) {
        char section[16];
        sprintf_s(section, "Player%d", k + 1);
        g_triggersOn[k] = GetPrivateProfileIntA(section, "AdaptiveTriggers", -1, g_playersPath);
    }
}

// "Adaptive Triggers" On/Off row in the pause menu's player options (UI_PauseOptions, builder around 0x4C4800: the
// Feedback row, vtable 0x791DB0, is added by `call 0x54F170` at 0x4C4985 and the result kept in screen+0x154). The row
// follows Feedback when the pausing player's pad is a DualSense; it is built like the Players row (seats.cpp).
#define AT_PAUSE_FEEDBACK_ADD 0x4C4985
static uint32_t g_triggersRowVt[18];
static int PausingLocalPlayer() { return SplitRacing() ? g_pausePort : 0; }
static int PadOfPlayer(int k) {
    if (!SplitMode()) return XPadLastActive();
    return k >= 0 && k < MAX_LOCAL_PLAYERS ? g_assignedPad[k] : PAD_NONE;
}
static void __fastcall TriggersRowDraw(uint8_t* row) {
    auto printf_ = reinterpret_cast<void(__cdecl*)(void*, const char*, ...)>(FN_FE_PRINTF);
    int k = PausingLocalPlayer();
    printf_(*reinterpret_cast<void**>(row + 0x2C), "%s", "Adaptive Triggers");
    printf_(*reinterpret_cast<void**>(row + 0x30), "%s", k >= 0 && TriggersOn(k) ? "On" : "Off");
}
static void __fastcall TriggersRowAct(uint8_t* row, void*, const char* fng, uint32_t msg) {
    int k = PausingLocalPlayer();
    if (k >= 0 && (msg == MSG_LEFT || msg == MSG_RIGHT)) {
        g_triggersOn[k] = TriggersOn(k) ? 0 : 1;
        char section[16];
        sprintf_s(section, "Player%d", k + 1);
        WritePrivateProfileStringA(section, "AdaptiveTriggers", g_triggersOn[k] ? "1" : "0", g_playersPath);
        Log("adaptive triggers: player %d %s", k + 1, g_triggersOn[k] ? "on" : "off");
    }
    reinterpret_cast<void(__cdecl*)(uint32_t, const char*)>(FN_FE_SEND_MSG)(0x92B703B5, fng);
    auto* vt = *reinterpret_cast<uintptr_t**>(row);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(vt[0x40 / 4])(row, msg);
    reinterpret_cast<void(__thiscall*)(void*)>(vt[3])(row);
}
static int __fastcall PauseFeedbackAdd(uint8_t* screen, void*, void* row, int flag) {
    int r = reinterpret_cast<int(__thiscall*)(void*, void*, int)>(FN_SCREEN_ADD_ROW)(screen, row, flag);
    int pad = PadOfPlayer(PausingLocalPlayer());
    if (pad != PAD_NONE && XPadIsDualSense(pad)) {
        if (!g_triggersRowVt[0]) {
            memcpy(g_triggersRowVt, reinterpret_cast<void*>(ROW_VTABLE_LAPS), sizeof(g_triggersRowVt));
            g_triggersRowVt[1] = reinterpret_cast<uint32_t>(&TriggersRowAct);
            g_triggersRowVt[3] = reinterpret_cast<uint32_t>(&TriggersRowDraw);
        }
        AddRow(screen, g_triggersRowVt);
    }
    return r;                            // the Feedback row's result (kept in screen+0x154)
}

static bool ReadWheels(void* player, float grip[4], float force[4], float speed[4]) {
    __try {
        auto* car = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(player) + 4);
        auto* mover = car ? *reinterpret_cast<uint8_t**>(car + 0x34) : nullptr;
        if (!mover) return false;
        for (int i = 0; i < 4; ++i) {
            auto* w = *reinterpret_cast<uint8_t**>(mover + 0x38 + 4 * i);
            if (!w) return false;
            grip[i] = *reinterpret_cast<float*>(w + 0x170);
            force[i] = *reinterpret_cast<float*>(w + 0x124);
            speed[i] = *reinterpret_cast<float*>(w + 0x28);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Hold { DWORD onSince, offSince; bool active; };
static bool HoldUpdate(Hold& h, bool raw, DWORD now) {
    if (raw) {
        h.offSince = 0;
        if (!h.onSince) h.onSince = now;
        if (now - h.onSince >= 50) h.active = true;
    } else {
        h.onSince = 0;
        if (h.active) {
            if (!h.offSince) h.offSince = now;
            if (now - h.offSince >= 100) h.active = false;
        }
    }
    return h.active;
}

static int TrigScale(int v) {
    int s = v * g_triggerStrength / 100;
    return v > 0 && s < 1 ? 1 : s > 8 ? 8 : s;
}
static void TrigOff(uint8_t* b) { memset(b, 0, 11); b[0] = 0x05; }
static void TrigPack(uint8_t* b, int start, int v) {      // zones start..9, strength / amplitude v-1 in 3 bits each
    uint16_t act = 0;
    uint32_t f = 0;
    for (int i = start; i < 10; ++i) { act |= 1u << i; f |= static_cast<uint32_t>((v - 1) & 7) << (3 * i); }
    b[1] = act & 0xFF; b[2] = act >> 8; b[3] = f & 0xFF; b[4] = f >> 8; b[5] = f >> 16; b[6] = f >> 24;
}
static void TrigFeedback(uint8_t* b, int start, int strength) {
    int v = TrigScale(strength);
    memset(b, 0, 11);
    if (v <= 0) { TrigOff(b); return; }
    b[0] = 0x21;
    TrigPack(b, start, v);
}
static void TrigVibration(uint8_t* b, int start, int amp, int hz) {
    int v = TrigScale(amp);
    memset(b, 0, 11);
    if (v <= 0 || hz <= 0) { TrigOff(b); return; }
    b[0] = 0x26;
    TrigPack(b, start, v);
    b[9] = static_cast<uint8_t>(hz);
}

// player k (>= 0) races with this pad now; -1 = no effects
static void UpdatePadTriggers(int pad, int k) {
    uint8_t left[11], right[11];
    TrigOff(left);
    TrigOff(right);
    void* player = k >= 0 && TriggersOn(k) ? GetLocalPlayer(k) : nullptr;
    static Hold brakeHold[MAX_LOCAL_PLAYERS], spinHold[MAX_LOCAL_PLAYERS];
    static DWORD nosStart[MAX_LOCAL_PLAYERS];
    static bool nosWas[MAX_LOCAL_PLAYERS];
    float grip[4], force[4], speed[4], t;
    bool shiftLight, nitrous;
    if (player && ReadWheels(player, grip, force, speed) && ReadCarLight(player, &t, &shiftLight, &nitrous)) {
        DWORD now = GetTickCount();
        uint16_t buttons;
        uint8_t lt = 0, rt = 0;
        int16_t lx;
        XPadRawState(pad, &buttons, &lt, &rt, &lx);
        bool airborne = grip[0] < 0.005f && grip[1] < 0.005f && grip[2] < 0.005f && grip[3] < 0.005f;
        float maxGrip = 0;
        for (float g : grip) if (g > maxGrip) maxGrip = g;
        // braking limit: the front axle (the rear reads 1.0 whenever the car is slow or standing still); needs speed
        float frontGrip = grip[0] > grip[1] ? grip[0] : grip[1], frontSpeed = (speed[0] + speed[1]) * 0.5f;
        float front = fabsf(force[0]) + fabsf(force[1]), rear = fabsf(force[2]) + fabsf(force[3]);
        float drivenGrip = maxGrip;                                 // both axles within 10%: all four
        if (fabsf(front - rear) > 0.1f * (front > rear ? front : rear))
            drivenGrip = front > rear ? (grip[0] > grip[1] ? grip[0] : grip[1]) : (grip[2] > grip[3] ? grip[2] : grip[3]);
        bool braking = HoldUpdate(brakeHold[k], !airborne && lt > 40 && frontGrip >= 0.98f && frontSpeed > 15.0f, now);
        bool spinning = HoldUpdate(spinHold[k], !airborne && rt > 40 && drivenGrip >= 0.98f, now);
        if (nitrous && !nosWas[k]) nosStart[k] = now;
        nosWas[k] = nitrous;
        TrigFeedback(left, 2, 3);
        TrigFeedback(right, 3, 2);
        if (!airborne) {
            if (braking) TrigVibration(left, 2, 5, 35);
            if (nitrous && now - nosStart[k] < 300) TrigFeedback(right, 0, 8);
            else if (shiftLight && rt > 40) TrigVibration(right, 2, 3, 60);
            else if (spinning) TrigVibration(right, 3, 4, 25);
        }
    }
    XPadSetTriggers(pad, left, right);
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
    int stage = state == 6 && mgr ? *mgr : 0;
    // countdown (stage 3, always 3.0 s): red, red, amber on the 3 / 2 / 1; GO (stage 4) shows green for 1 s
    static int prevStage = 0;
    static DWORD stageStart = 0, greenUntil = 0;
    DWORD tick = GetTickCount();
    if (stage != prevStage) {
        if (stage == 3) stageStart = tick;
        greenUntil = stage == 4 && prevStage == 3 ? tick + 1000 : 0;
        prevStage = stage;
    }
    bool countdown = stage == 3;
    bool racing = stage >= 4;       // racing, and whatever follows the finish while the state is still 6
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
        else if (racing || countdown) {
            int k = -1;                    // local player this pad drives
            if (SplitMode()) { for (int q = 0; q < MAX_LOCAL_PLAYERS; ++q) if (g_assignedPad[q] == pad) k = q; }
            else if (pad == XPadLastActive()) k = 0;
            bool toggle = k >= 0 && racing && !paused && SplitMode() && XPadTouchClickPressed(pad);
            if (toggle) {
                g_lightColourMode[k] = !g_lightColourMode[k];
                Log("lightbar: player %d shows %s", k + 1, g_lightColourMode[k] ? "player colour" : "rpm");
            }
            uint8_t full[3] = {0, 0, 0};
            bool cue = countdown;           // countdown / GO colours replace everything else
            if (countdown) {
                int second = (tick - stageStart) / 1000;
                if ((tick - stageStart) % 1000 < 600) { full[0] = 255; full[1] = second >= 2 ? 120 : 0; }   // flash per count
            } else if (racing && !paused && static_cast<int>(greenUntil - tick) > 0) {
                full[1] = 255;
                cue = true;
            }
            if (k < 0) {
            } else if (cue) {
                for (int c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(full[c] * g_lightbarBrightness / 100);
            } else if (!paused && racing && FinishedPosition(GetLocalPlayer(k))) {   // finish colour until state 6 ends
                static const uint8_t kPlace[3][3] = {{255, 180, 0}, {200, 200, 220}, {205, 100, 30}};
                int place = FinishedPosition(GetLocalPlayer(k));
                for (int c = 0; c < 3; ++c)
                    rgb[c] = static_cast<uint8_t>((place <= 3 ? kPlace[place - 1][c] : 0) * g_lightbarBrightness / 100);
            } else if (paused) {            // the player's colour at 25%
                for (int c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(kPlayerColour[k][c] * g_lightbarBrightness / 400);
            } else if (SplitMode() && g_lightColourMode[k]) {
                for (int c = 0; c < 3; ++c) rgb[c] = static_cast<uint8_t>(kPlayerColour[k][c] * g_lightbarBrightness / 100);
            } else CarLightbar(k, rgb);
        }
        XPadSetLights(pad, rgb, leds);
        int drives = -1;                   // local player this pad drives, while it races and isn't paused or finished
        if (racing && !paused) {
            if (SplitMode()) { for (int q = 0; q < MAX_LOCAL_PLAYERS; ++q) if (g_assignedPad[q] == pad) drives = q; }
            else if (pad == XPadLastActive()) drives = 0;
            if (drives >= 0 && FinishedPosition(GetLocalPlayer(drives))) drives = -1;
        }
        UpdatePadTriggers(pad, drives);
    }
}
