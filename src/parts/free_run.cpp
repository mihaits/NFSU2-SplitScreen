// Part of NFSU2SplitScreen.cpp: Free Run in split screen.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// ---------------------------------------------------------------- Free Run in split screen
// The quick race mode list (UIQRModeSelect::Setup, 0x4B2E..) adds Outrun as the 7th mode in modes 8/0x200/4 (split
// screen = 4) and Free Run (race type 9, icon class 0x790FFC) only in the others: Free Run is added in split screen too.
#define AT_MODE_LIST_FREE_RUN 0x4B2F99   // je (mode 4: skip Free Run), 2 bytes
// The HUD's outrun element (HUD +0x6C, ctor 0x4F87E0: Getaway_Distance_Bar, Distance_Number, ...; outrun challenges
// in Free Run) binds objects only the outrun HUD packages have: with the split race HUDs they are null and its update
// crashed once driving started (0x4C67E9, user). Without its objects the element is not updated.
#define AT_OUTRUN_ELEMENT_UPDATE 0x4EC69A   // call 0x4DABD0 (thiscall element; arg)
#define FN_OUTRUN_ELEMENT_UPDATE 0x4DABD0
static void __fastcall OutrunElementUpdate(uint8_t* element, void*, int arg) {
    if (!*reinterpret_cast<void**>(element + 0x28) || !*reinterpret_cast<void**>(element + 0x2C)) return;
    reinterpret_cast<void(__thiscall*)(void*, int)>(FN_OUTRUN_ELEMENT_UPDATE)(element, arg);
}
// When driving starts, Free Run switches the race to its free state (0x89E7B0 = 7, 0x607190) and sets the local player
// count from the human cars ([[0x890080]+0x14], 0x6071D5): 3-4 with players 3/4, and the game's 2-player paths (split
// views) stopped -> dark screen (user). Split screen keeps 2.
#define AT_FREE_STATE_PLAYERS 0x6071D2   // mov ecx, [eax+0x14]; mov [0x89E7C0], ecx (9 bytes)
#define FREE_STATE_PLAYERS_RET 0x6071DB
static __declspec(naked) void FreeStatePlayersHook() {
    __asm {
        mov ecx, dword ptr [eax + 0x14]
        cmp ecx, 2
        jle store
        cmp dword ptr ds:[GAME_MODE], 4
        jne store
        mov ecx, 2
    store:
        mov dword ptr ds:[NUM_PLAYERS], ecx
        push FREE_STATE_PLAYERS_RET
        ret
    }
}
// The screen after the mode select (0x4B2A80): in split screen Outrun goes to the race options (0x7F4460), the rest to
// the track list - except Free Run, which goes to its own screen (0x7F446C: straight to the car select, no Players
// row). Split screen's Free Run goes to the race options like Outrun, on track 4000 (the whole city from the airport;
// user: no track choice).
#define FN_NEXT_SCREEN_AFTER_MODE 0x4B2A80   // const char* __cdecl (): 5 bytes stolen
#define SCREEN_RACE_OPTIONS 0x7F4460
#define FREE_RUN_TRACK     4000
using NextScreenFn = uint32_t(__cdecl*)();
static NextScreenFn g_origNextScreen;
static uint32_t __cdecl NextScreenAfterMode() {
    if (SplitMode() && at<int>(QR_RACE_TYPE) == 9) {
        at<uint16_t>(QR_SETTINGS + 9 * 0x14 + 8) = FREE_RUN_TRACK;
        at<uint32_t>(QR_SETTINGS + 9 * 0x14 + 0xC) = 0;   // track direction forward (its row is hidden)
        return at<uint32_t>(SCREEN_RACE_OPTIONS);
    }
    return g_origNextScreen();
}
// Back from the race options (0x4B2AF0, the screen's "previous" function): split screen's Outrun goes back to the mode
// select (0x7F445C), the rest to the track list, which Free Run skips. Free Run goes back to the mode select too.
#define FN_PREV_SCREEN_RACE_OPTIONS 0x4B2AF0   // const char* __cdecl (): 5 bytes stolen
#define SCREEN_MODE_SELECT 0x7F445C
static NextScreenFn g_origPrevScreen;
static uint32_t __cdecl PrevScreenRaceOptions() {
    if (SplitMode() && at<int>(QR_RACE_TYPE) == 9) return at<uint32_t>(SCREEN_MODE_SELECT);
    return g_origPrevScreen();
}
// Free Run's option rows (0x4B4A10): the Track Direction row (vtable 0x791608, settings +4) when the track can be
// reversed, then traffic. Split screen drives the whole city from the airport and skips the direction row.
#define AT_FREE_RUN_DIRECTION_ROW 0x4B4A42   // mov ecx, 0x83AAB4 (5 bytes)
#define FREE_RUN_DIRECTION_ROW_RET  0x4B4A47
#define FREE_RUN_DIRECTION_ROW_SKIP 0x4B4AD2
static __declspec(naked) void FreeRunDirectionRowHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        mov ecx, 0x83AAB4
        push FREE_RUN_DIRECTION_ROW_RET
        ret
    skip:
        push FREE_RUN_DIRECTION_ROW_SKIP
        ret
    }
}
// Outrun challenges in Free Run (user: "the point is FREE roam"): in the free state (0x89E7B0 == 7) the race update runs
// the challenge logic (0x607CA9..0x608C01: picks an opponent, race +0xBD0, and ends the race when the player pulls
// away), and the player update points a 3D direction arrow at that opponent (0x5F3AE0, MARKER_DIRECTION_AID). Both
// are skipped in split screen.
#define AT_FREE_STATE_RACE  0x607C9C     // cmp dword ptr [0x89E7B0], 7; jne 0x608C01 (13 bytes)
#define FREE_STATE_RACE_RUN 0x607CA9
#define FREE_STATE_RACE_END 0x608C01
static __declspec(naked) void FreeStateRaceHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        cmp dword ptr ds:[0x89E7B0], 7
        jne skip
        push FREE_STATE_RACE_RUN
        ret
    skip:
        push FREE_STATE_RACE_END
        ret
    }
}
#define FN_DIRECTION_ARROW  0x5F3AE0     // void __thiscall (player; int): 6 bytes stolen; ret 4
static void* g_origDirectionArrow;
static __declspec(naked) void DirectionArrowHook() {
    __asm {
        cmp dword ptr ds:[GAME_MODE], 4
        je skip
        jmp dword ptr [g_origDirectionArrow]
    skip:
        ret 4
    }
}
static void InstallFreeRun() {
    const uint8_t nops[2] = {0x90, 0x90};
    Write(AT_MODE_LIST_FREE_RUN, nops, 2);
    g_origNextScreen = reinterpret_cast<NextScreenFn>(MakeTrampoline(FN_NEXT_SCREEN_AFTER_MODE, 5));
    WriteJump(FN_NEXT_SCREEN_AFTER_MODE, &NextScreenAfterMode, 5);
    g_origPrevScreen = reinterpret_cast<NextScreenFn>(MakeTrampoline(FN_PREV_SCREEN_RACE_OPTIONS, 5));
    WriteJump(FN_PREV_SCREEN_RACE_OPTIONS, &PrevScreenRaceOptions, 5);
    WriteJump(AT_FREE_RUN_DIRECTION_ROW, &FreeRunDirectionRowHook, 5);
    WriteCall(AT_OUTRUN_ELEMENT_UPDATE, &OutrunElementUpdate);
    WriteJump(AT_FREE_STATE_PLAYERS, &FreeStatePlayersHook, 9);
    WriteJump(AT_FREE_RUN_SKIN, &FreeRunSkinHook, 7);
    WriteJump(AT_FREE_STATE_RACE, &FreeStateRaceHook, 13);
    g_origDirectionArrow = MakeTrampoline(FN_DIRECTION_ARROW, 6);
    WriteJump(FN_DIRECTION_ARROW, &DirectionArrowHook, 6);
}
