// NFSU2SplitScreen.asi - native 2-4 player split screen for Need for Speed Underground 2 (PC, speed2.exe v1.2).
//
// The PC build still contains the console's two-player split-screen code (game mode 4, the 2P HUDs, two-player race
// setup, a two-position streamer) but the frontend entry, the renderer's split path and the input separation were
// cut or never finished. This plugin restores them and extends the game from 2 to 4 local players: player arrays,
// input port tables, sound listeners and per-view render state are relocated or generalized, players 3/4 get their
// own views (5/6), HUD package copies, minimaps, cameras, streaming, reflections, weather and effects, and every
// player gets their car's single-player gauges.
//
// Everything is patched at load time (WriteCall / WriteJump / trampolines on fixed addresses). Each subsystem in parts/
// starts with a comment block explaining the game code involved; docs/TECHNICAL.md is the overview and index.
//
// Addresses are for speed2.exe v1.2 (image base 0x400000, no ASLR). IsSupportedExe() checks the patched bytes first.

#include <windows.h>
#include <intrin.h>
#include <shlwapi.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <cstring>

#include "XInputDevices.h"

// The code is split into src/parts/, one file per subsystem, included here in order (one translation
// unit: the parts share the static state and helpers declared before them).
#include "parts/core.cpp"
#include "parts/frontend.cpp"
#include "parts/log.cpp"
#include "parts/hud_split.cpp"
#include "parts/debug_tools.cpp"
#include "parts/seats.cpp"
#include "parts/input.cpp"
#include "parts/render_core.cpp"
#include "parts/render_passes.cpp"
#include "parts/render_weather.cpp"
#include "parts/render_envmap.cpp"
#include "parts/local_players.cpp"
#include "parts/sound.cpp"
#include "parts/racers.cpp"
#include "parts/hud_players.cpp"
#include "parts/gauges.cpp"
#include "parts/minimap_players.cpp"
#include "parts/threads_memory.cpp"
#include "parts/intro.cpp"
#include "parts/player_settings.cpp"
#include "parts/hud_gap.cpp"
#include "parts/garage.cpp"
#include "parts/free_run.cpp"
#include "parts/race_rules.cpp"
#include "parts/input_ports.cpp"
#include "parts/camera.cpp"
#include "parts/pad_lights.cpp"
#include "parts/streaming.cpp"

// ---------------------------------------------------------------- setup
static bool IsSupportedExe() {
    // original bytes at a few patch sites (v1.2, unpatched)
    const uint8_t loopMax[] = {0x83, 0xF8, 0x01};
    const uint8_t remapJnz[] = {0x75, 0x24};
    const uint8_t qrMode[] = {0xC7, 0x05, 0x78, 0x00, 0x85, 0x00, 0x02, 0x00, 0x00, 0x00};
    const uint8_t pressStart[] = {0x8B, 0x44, 0x24, 0x04, 0x3D, 0x64, 0xB3, 0x1A, 0x91};
    const uint8_t carSelect[] = {0x55, 0x8B, 0x6C, 0x24, 0x10};
    const uint8_t mainMenu[] = {0xE8, 0x83, 0x1B, 0x0C, 0x00};
    const uint8_t joypadTitle[] = {0xE8, 0xE3, 0xFE, 0x02, 0x00};
    return memcmp(reinterpret_cast<void*>(AT_VIEWLOOP_MAX - 2), loopMax, sizeof(loopMax)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_SECTION_REMAP), remapJnz, sizeof(remapJnz)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_QR_SET_MODE), qrMode, sizeof(qrMode)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_PRESSSTART), pressStart, sizeof(pressStart)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_CARSELECT), carSelect, sizeof(carSelect)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_MAINMENU_QR), mainMenu, sizeof(mainMenu)) == 0 &&
           memcmp(reinterpret_cast<void*>(AT_JOYPAD_TITLE), joypadTitle, sizeof(joypadTitle)) == 0;
}

static void LoadConfig(HMODULE self) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    char* dot = strrchr(path, '.');
    if (!n || !dot) return;
    strcpy_s(dot, MAX_PATH - (dot - path), ".ini");
    g_debug = GetPrivateProfileIntA("MAIN", "Debug", 0, path) != 0;
    g_quadTest = GetPrivateProfileIntA("DEBUG", "QuadTest", 0, path) != 0;
    g_extraPlayers = GetPrivateProfileIntA("DEBUG", "ExtraPlayers", 0, path);
    if (g_extraPlayers < 0) g_extraPlayers = 0;
    if (g_extraPlayers > SPLIT_PLAYERS_MAX - 2) g_extraPlayers = SPLIT_PLAYERS_MAX - 2;
    g_splitPlayers = 2 + g_extraPlayers;                                                      // debug default
    g_cloneViews = GetPrivateProfileIntA("DEBUG", "CloneViews", 0, path);
    if (g_cloneViews < 0) g_cloneViews = 0;
    if (g_cloneViews > 2) g_cloneViews = 2;
    g_splitOpponents = GetPrivateProfileIntA("DEBUG", "Opponents", 0, path);                   // debug default
    g_spGauges = GetPrivateProfileIntA("HUD", "SinglePlayerGauges", 1, path) != 0;
    g_gaugeScale = GetPrivateProfileIntA("HUD", "GaugeScalePercent", 60, path) / 100.0f;
    g_rumble = GetPrivateProfileIntA("INPUT", "Rumble", 1, path) != 0;
    g_rumbleStrength = GetPrivateProfileIntA("INPUT", "RumbleStrength", 100, path) / 100.0f;
    g_nativeDualSense = GetPrivateProfileIntA("INPUT", "NativeDualSense", 1, path) != 0;
    g_dsRumbleMode = GetPrivateProfileIntA("INPUT", "DualSenseRumbleMode", 0, path);
    g_playerLeds = GetPrivateProfileIntA("LIGHTS", "PlayerLeds", 1, path) != 0;
    g_lightbar = GetPrivateProfileIntA("LIGHTS", "Lightbar", 1, path) != 0;
    g_adaptiveTriggers = GetPrivateProfileIntA("LIGHTS", "AdaptiveTriggers", 1, path) != 0;
    g_triggerStrength = GetPrivateProfileIntA("LIGHTS", "TriggerStrength", 100, path);
    if (g_triggerStrength < 0) g_triggerStrength = 0;
    if (g_triggerStrength > 100) g_triggerStrength = 100;
    g_lightbarBrightness = GetPrivateProfileIntA("LIGHTS", "LightbarBrightness", 100, path);
    if (g_lightbarBrightness < 0) g_lightbarBrightness = 0;
    if (g_lightbarBrightness > 100) g_lightbarBrightness = 100;
    if (g_dsRumbleMode != 1 && g_dsRumbleMode != 2) g_dsRumbleMode = -1;   // 0 = auto
    strcpy_s(g_iniPath, path);
    strcpy_s(g_playersPath, path);
    strcpy_s(g_playersPath + (dot - path), MAX_PATH - (dot - path), "_Players.ini");
    LoadSeatStore();
    strcpy_s(g_logPath, path);
    strcpy_s(g_logPath + (dot - path), MAX_PATH - (dot - path), ".log");
}

static void Install(HMODULE self) {
    if (!IsSupportedExe()) {
        MessageBoxA(nullptr, "Unsupported speed2.exe (needs v1.2) or another mod patches the same code. Disabled.",
                    "NFSU2SplitScreen", MB_ICONWARNING);
        return;
    }
    LoadConfig(self);
    g_mainThreadId = GetCurrentThreadId();
    if (g_debug) {
        char path[MAX_PATH], key[16], val[32];
        GetModuleFileNameA(self, path, MAX_PATH);
        strcpy_s(strrchr(path, '.'), 5, ".ini");
        for (int i = 0; i < 4; ++i) {
            sprintf_s(key, "Watch%d", i);
            GetPrivateProfileStringA("DEBUG", key, "0", val, sizeof(val), path);
            if (uintptr_t a = strtoul(val, nullptr, 0)) SetWatch(i, a);
        }
    }
    Log("mailbox at %p", &g_mailbox);

    g_xinputPads = InstallXInputDevices(IMPORT_DIRECTINPUT8CREATE, g_nativeDualSense, g_dsRumbleMode, &Log);
    if (g_xinputPads)
        XPadSetResolver(&PadForDevice);
    else
        Log("XInput runtime not found; using DirectInput controllers");

    bool tablesBuilt = *reinterpret_cast<uint32_t*>(OLD_PORT_TABLES) == OLD_PORT_TABLES;
    int portRefs = RelocatePortTables();
    Log("port tables relocated: %d refs%s", portRefs, tablesBuilt ? " (WARNING: already constructed)" : "");
    WriteCall(AT_INPUT_INIT_CALL, &InputInit);
    g_origRumble = MakeTrampoline(FN_RUMBLE, 5);
    WriteJump(FN_RUMBLE, &RumbleHook, 5);
    WriteCall(AT_FF_UPDATE_CALL, &FfUpdate);
    WriteCall(AT_FF_PAD_OUTPUT, &FfPadOutput);
    {   // Gameplay > Map mode (0x83AA1B, handler 0x49AFE0): the console's split mode offers Off/Static only (max =
        // (mode != 4) + 1); with per-player minimaps Rotate works in split too -> `setne cl` becomes `mov cl, 1`
        static const uint8_t setne[3] = {0x0F, 0x95, 0xC1}, movCl1[3] = {0xB1, 0x01, 0x90};
        if (memcmp(reinterpret_cast<void*>(AT_MAP_MODE_MAX), setne, 3) == 0) Write(AT_MAP_MODE_MAX, movCl1, 3);
    }
    for (uintptr_t at : kSplitCarLodJumps) {
        static const uint8_t jmp = 0xEB;
        if (*reinterpret_cast<uint8_t*>(at) == 0x75) Write(at, &jmp, 1);
    }
    if (g_rumble) WriteJump(FN_FF_CAPABLE, &FfCapable, 5);   // Feedback selectable (not for the keyboard player)
    WriteJump(FN_IMPACT_RUMBLE, &ImpactRumbleHook, 6);
    WriteJump(FN_JOY_SLOT_PRESENT, &JoySlotPresent, 5);
    int relocated = RelocateLocalPlayers();
    // the one indexed reference that is "inOrder - 4" (0x8900B0 = old byIndex[1]): the race teardown loop
    // `while (count) delete inOrder[count - 1]` (mov esi,[eax*4+0x8900B0]); relocated as byIndex+4 it read
    // byIndex[count] (3 players: null forever -> hang on quit; 4 players: wrong player freed)
    uint32_t inOrderMinus1 = reinterpret_cast<uint32_t>(&g_localPlayers.inOrder[0]) - 4;
    Write(AT_PLAYERS_TEARDOWN_DISP, &inOrderMinus1, 4);
    Log("local player arrays relocated: %d refs", relocated);
    InstallCatchup();
    InstallHurry();
    InstallFreeRun();
    InstallScenePicks();
    PatchViewUpdateLoop();
    Log("sound listener arrays relocated: %d refs", RelocateListenerArrays());
    WriteCall(AT_LISTENER_UPDATE_CALL, &ListenerUpdate);
    WriteJump(AT_LISTENER_CHOICE, &ListenerChoiceHook, 5);
    WriteJump(AT_EMITTER_RANGE, &EmitterRangeHook, 6);
    WriteCall(AT_MAIN_WORLD_DRAW, &WorldDrawWithBeams);
    WriteCall(AT_BEAM_COLOR_CALL, &BeamColor);
    {   // CubicCameraMover::Update through the plugin (start zoom)
        void* hook = reinterpret_cast<void*>(&ChaseMoverUpdateHook);
        if (*reinterpret_cast<uint32_t*>(CUBIC_MOVER_VTABLE_UPDATE) == FN_CUBIC_MOVER_UPDATE)
            Write(CUBIC_MOVER_VTABLE_UPDATE, &hook, 4);
    }
    WriteCall(AT_SPLIT_PASS_CALL, &SplitPassWithHeadlightPools);
    WriteCall(AT_1P_POOLS_CALL, &PoolsForTraffic);
    WriteCall(AT_RAIN_CALL, &RainForViews);
    WriteCall(AT_RAIN_DROPS_CALL, &RainDropsForViews);
    WriteCall(AT_RAIN_DROP_GET, &RainDropGet);
    for (uintptr_t a : kRainDropsViewCalls) WriteCall(a, &RainDropsView);
    WriteJump(AT_REFLECTIONS_DONE, &ReflectionsDoneHook, 6);
    WriteJump(AT_NOS_FX_PLAYER, &NosFxPlayerHook, 13);
    WriteCall(AT_OVERBRIGHT_AMOUNT_CALL, &OverbrightAmounts);
    WriteCall(AT_OVERBRIGHT_ACCUM_CALL, &OverbrightAccumulate);
    WriteCall(AT_BRIGHT_PASS_CALL1, &BrightPassPerView);
    WriteCall(AT_BRIGHT_PASS_CALL2, &BrightPassPerView);
    WriteCall(AT_BRIGHT_PASS_CALL0, &BrightPassPerView);
    WriteJump(AT_REFLECT_CAR_PICK, &ReflectCarPickHook, 5);
    WriteCall(AT_ENV_DONE, &EnvMapsForPlayers);
    g_origReleaseTargets = reinterpret_cast<ReleaseTargetsFn>(MakeTrampoline(FN_RELEASE_TARGETS, 6));
    WriteJump(FN_RELEASE_TARGETS, &ReleaseTargetsHook, 6);
    WriteJump(AT_RAIN_CAR, &RainCarHook, 5);
    WriteCall(AT_RAIN_REGION_SLOT, &RainRegionSlot);
    WriteCall(AT_SPLASH_REGION_SLOT, &SplashRegionSlot);
    { const uint8_t nops[3] = {0x90, 0x90, 0x90}; Write(AT_SPLASH_REGION_SLOT + 5, nops, 3); }
    g_origNearestRacerDist = reinterpret_cast<NearestRacerDistFn>(MakeTrampoline(FN_NEAREST_RACER_DIST, 6));
    WriteJump(FN_NEAREST_RACER_DIST, &NearestRacerDistance, 6);
    WriteJump(FN_NEAREST_PLAYER_CAR, &NearestPlayerCar, 5);
    WriteJump(FN_NEAREST_PLAYER, &NearestPlayer, 5);
    WriteJump(AT_MAINMENU_QR, &MainMenuHook, 5);
    WriteCall(AT_JOYPAD_TITLE, &SetJoypadPageTitle);
    WriteCall(AT_CONTROLS_PAGES_CALL, &BuildControlsPages);
    WriteCall(AT_PROFILE_SAVE_BINDINGS, &ProfileSaveBindings);
    WriteCall(AT_PROFILE_LOAD_BINDINGS, &ProfileLoadBindings);
    WriteJump(AT_FE_OBJ_XFORM, &FeObjXformHook, 6);
    WriteJump(AT_QR_SET_MODE, &QuickRaceModeHook, 10);
    WriteJump(AT_PRESSSTART, &PressStartHook, 9);
    for (uintptr_t a : kCarselToP2Calls) WriteCall(a, &CarselToP2);
    for (uintptr_t a : kCarselStartCalls) WriteCall(a, &CarselStartHook);
    WriteCall(AT_CARSEL_BACK_TO_P1, &CarselBackToP1);
    for (uintptr_t a : kQrFinishRowsCalls) WriteCall(a, &QrFinishRows);
    WriteJump(AT_CARSELECT, &CarSelectHook, 5);
    g_origHelpBarSet = reinterpret_cast<HelpBarSetFn>(MakeTrampoline(FN_HELP_BAR_SET, 5));
    WriteJump(FN_HELP_BAR_SET, &HelpBarSet, 5);
    {   // split-mode Customize straight to the car shop (shared garage)
        const uint8_t* b = reinterpret_cast<const uint8_t*>(AT_CUSTOMIZE_MODE4);
        if (b[0] == 0x83 && b[1] == 0xFF && b[2] == 0x04 && b[3] == 0x75) {
            uint8_t jmp[5] = {0xEB, static_cast<uint8_t>(b[4] + 3), 0x90, 0x90, 0x90};
            Write(AT_CUSTOMIZE_MODE4, jmp, 5);
        }
    }

    g_origPollPort = reinterpret_cast<PollPortFn>(MakeTrampoline(FN_POLL_PORT, 5));
    WriteJump(FN_POLL_PORT, &PollPortHook, 5);

    g_origLayoutApply = reinterpret_cast<VoidFn>(MakeTrampoline(FN_LAYOUT_APPLY, 5));
    WriteJump(FN_LAYOUT_APPLY, &LayoutApplyHook, 5);

    WriteJump(AT_WORLD_VIEWS, &WorldViewsHook, 5);
    g_origStreamZones = reinterpret_cast<StreamZonesFn>(MakeTrampoline(FN_STREAM_ZONES, 7));
    WriteJump(FN_STREAM_ZONES, &StreamZonesHook, 7);
    WriteCall(AT_STREAM_PRIORITY_CALL, &StreamPriorities);
    g_origStreamSections = reinterpret_cast<StreamSectionsFn>(MakeTrampoline(FN_STREAM_SECTIONS, 6));
    WriteJump(FN_STREAM_SECTIONS, &StreamSectionsHook, 6);
    for (uintptr_t site : kCreateHumanRacersCalls) WriteCall(site, &CreateHumanRacers);
    const uint8_t humanCap = MAX_LOCAL_PLAYERS;
    Write(AT_HUMAN_MANAGER_CAP, &humanCap, 1);
    WriteCall(AT_RACER_SPEC_CALL, &RacerSpecIndexShift);
    WriteCall(AT_AI_SET_SKIN, &AiSetSkin);
    WriteCall(AT_AI_REFERENCE_CALL, &AiReferenceRacer);
    WriteCall(AT_PLAYER_HUD_CALL, &PlayerHudCtor);
    WriteCall(AT_HUD_UPDATE_CALL, &HudUpdateHook);
    WriteCall(AT_HUD_DTOR_CALL, &HudDtorHook);
    WriteCall(AT_HUD_TEX_LOAD_CALL, &HudTexLoadHook);
    WriteCall(AT_HUD_TEX_UNLOAD_CALL, &HudTexUnloadHook);
    WriteJump(FN_RACER_OF_PLAYER, &RacerOfPlayer, 5);
    WriteCall(AT_FE_RENDER_OBJ_CALL, &RenderFeObject);
    WriteCall(AT_MINIMAP_UPDATE_CALL, &MinimapUpdate);
    WriteCall(AT_MINIMAP_DESTROY_CALL, &DestroyMinimapManager);
    for (uintptr_t at : kBuildStandingsCalls) WriteCall(at, &BuildStandings);
    WriteCall(AT_RACE_RESULTS_CALL, &RaceResults);
    WriteCall(AT_FALL_RESET_CALL, &FallResetHook);
    g_origCarNotVisible = reinterpret_cast<CarNotVisibleFn>(MakeTrampoline(FN_CAR_NOT_VISIBLE, 5));
    WriteJump(FN_CAR_NOT_VISIBLE, &CarNotVisible, 5);
    g_origAssignGrid = reinterpret_cast<AssignGridFn>(MakeTrampoline(FN_ASSIGN_GRID, 6));
    WriteJump(FN_ASSIGN_GRID, &AssignGrid, 6);
    for (uintptr_t a : kNisBindCarsCalls) WriteCall(a, &NisBindCars);
    WriteCall(kRaceSetupCalls[0][0], &RaceSetup5260B0);
    WriteCall(kRaceSetupCalls[1][0], &RaceSetup526970);
    WriteCall(kRaceSetupCalls[2][0], &RaceSetup526970);
    const uint8_t markerPool = 0x7F;                    // 0x66 -> 127 marker descriptors
    Write(AT_MARKER_POOL_SIZE, &markerPool, 1);
    g_mainThread = GetCurrentThreadId();
    InitializeCriticalSection(&g_poolLock);
    g_origPoolAlloc = MakeTrampoline(FN_GAME_POOL_ALLOC, 8);
    WriteJump(FN_GAME_POOL_ALLOC, &PoolAllocLocked, 8);
    g_origPoolFree = MakeTrampoline(FN_GAME_POOL_FREE, 7);
    WriteJump(FN_GAME_POOL_FREE, &PoolFreeLocked, 7);
    WriteCall(AT_SOUND_FS_CREATE_CALL, &CreateSoundFs);
    WriteCall(AT_TRACK_POOL_CALL, &CreateTrackStreamingPool);
    WriteCall(AT_SECOND_POOL_CALL, &CreateSecondStreamingPool);
    WriteCall(AT_PAUSE_MASK_CALL, &PauseInputMask);
    g_origPausingPlayer = MakeTrampoline(FN_PAUSING_PLAYER, 5);
    WriteJump(FN_PAUSING_PLAYER, &PausingPlayer, 5);
    InitializeCriticalSection(&g_soundFsLock);
    {
        g_soundFsOpen = *reinterpret_cast<uintptr_t*>(SOUND_FS_VT_OPEN);
        void* open = reinterpret_cast<void*>(&SoundFsOpen);
        Write(SOUND_FS_VT_OPEN, &open, 4);
        g_soundFsClose = *reinterpret_cast<uintptr_t*>(SOUND_FS_VT_CLOSE);
        void* close = reinterpret_cast<void*>(&SoundFsClose);
        Write(SOUND_FS_VT_CLOSE, &close, 4);
    }
    g_origSendMsg = reinterpret_cast<SendMsgFn>(MakeTrampoline(FN_FE_SEND_MSG, 5));
    WriteJump(FN_FE_SEND_MSG, &SendFeMessage, 5);
    WriteCall(AT_HUD_STATE_CALL, &HudStateOf);
    WriteJump(AT_DRIFT_GAP, &DriftGapHook, 5);
    WriteJump(AT_RACE_GAP, &RaceGapHook, 6);
    {   // in-race name labels (0x4C7006 / 0x4C93CF): human names from the per-player block by index -> the player's own
        // name buffer (+0x30; same text for players 1/2, "Player 3/4" for the extra players)
        static const uint8_t leaEdx[15] = {0x8D, 0x4A, 0x30, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t leaEcx[15] = {0x8D, 0x49, 0x30, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        if (*reinterpret_cast<const uint32_t*>(0x4C7006) == 0x69104A8B) Write(0x4C7006, leaEdx, 15);
        if (*reinterpret_cast<const uint32_t*>(0x4C93CF) == 0x6910498B) Write(0x4C93CF, leaEcx, 15);
    }
    Log("shared garage: %d sites", ShareGarage());
    Log("player settings readers redirected: %d of %d", RedirectPlayerSettings(), static_cast<int>(sizeof(kSettingsReaders) / sizeof(kSettingsReaders[0])));
    WriteCall(AT_INTRO_CONTEXT_CALL, &IntroContext);
    WriteCall(AT_INTRO_2P_FILTER, &Intro2PFilter);
    WriteJump(AT_INTRO_EMPTY, &IntroEmptyHook, 5);
    WriteCall(AT_BINDINGS_CLEAR_CALL, &ClearHudBindings);
    WriteCall(AT_CREATE_PLAYERS_CALL, &CreatePlayers);
    WriteCall(AT_SKY_CALL, &DrawSky);
    WriteJump(AT_STREAKS_BRANCH, &StreaksBranchHook, 8);
    WriteCall(AT_STREAK_VIEW_CALL, &StreakView);
    WriteCall(AT_STREAK_COMPOSITE_CALL, &StreakComposite);
    WriteCall(AT_BLIT_QUAD_READY, &BlitQuadReadyHook);
    WriteCall(AT_STREAK_AA_COPY, &StreakAACopy);
    WriteCall(AT_BLUR_CALL, &MotionBlurForView);
    WriteCall(AT_BLUR_VIEW_CALL1, &BlurView);
    WriteCall(AT_BLUR_VIEW_CALL2, &BlurView);
    {
        uint8_t blurCopy[6] = {0xE8, 0, 0, 0, 0, 0x90};
        int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&BlurCopy) - (AT_BLUR_COPY + 5));
        memcpy(blurCopy + 1, &rel, 4);
        Write(AT_BLUR_COPY, blurCopy, 6);
    }
    WriteJump(AT_VIEWLOOP_TOP, &ViewLoopTopHook, 5);
    WriteJump(AT_VIEWLOOP_EXIT, &ViewLoopExitHook, 6);
    const uint8_t lastView = MAX_LOCAL_PLAYERS;  // loop counter 1..4 -> views 1, 2, 5, 6 (inactive ones are skipped)
    Write(AT_VIEWLOOP_MAX, &lastView, 1);

    WriteJump(AT_PROJECTION, &ProjectionHook, 10);

    const uint8_t jmp = 0xEB;                    // never remap to the reduced 2P sections
    Write(AT_SECTION_REMAP, &jmp, 1);
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Install(module);
    }
    return TRUE;
}
