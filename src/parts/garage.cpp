// Part of NFSU2SplitScreen.cpp: shared My Cars.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// Shared "My Cars": the quick race garage is part of each player's block (+0x3B0; P2's block is the 2P "Player 2"
// with an empty garage -> P2 (and P4) only got stock cars). Every garage lookup (`imul reg, index, 0xAB4C` followed by
// block +0x3B0) now resolves to P1's (the loaded profile's) garage: car select lists, category checks, customize,
// race setup (0x525F92 used the loop's block pointer). Single player always uses index 0.
static const uintptr_t kGarageIndexSites[] = {0x49524D, 0x495F83, 0x495FE3, 0x49605F, 0x497EF3, 0x4DF9F1, 0x4E0192,
    0x4E230C, 0x4EEBA4, 0x4EECB9, 0x4EF0A1, 0x511E1F, 0x515D15, 0x521140, 0x5158BB, 0x529CC9, 0x530A4B, 0x53477F,
    0x5347EE, 0x552D38};
#define AT_RACER_GARAGE    0x525F92      // mov edx,[esp+18h]; add edx,3B0h (10 bytes)
static int ShareGarage() {
    int n = 0;
    for (uintptr_t at : kGarageIndexSites) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(at);
        if (b[0] != 0x69 || *reinterpret_cast<const uint32_t*>(b + 2) != PLAYER_BLOCK_SIZE) continue;
        uint32_t zero = 0;
        Write(at + 2, &zero, 4);
        ++n;
    }
    uint8_t movGarage[10] = {0xBA, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90};   // mov edx, P1's garage
    uint32_t garage = PLAYER_BLOCKS + 0x3B0;
    memcpy(movGarage + 1, &garage, 4);
    if (*reinterpret_cast<const uint32_t*>(AT_RACER_GARAGE) == 0x1824548B) { Write(AT_RACER_GARAGE, movGarage, 10); ++n; }
    return n;
}

static void* FindRaceCar(int playerIndex) {
    using EndFn = void*(__cdecl*)();
    void* end = reinterpret_cast<EndFn>(FN_LIST_END)();
    void** list = *reinterpret_cast<void***>(RACE_CARS);
    for (void** node = static_cast<void**>(*list); node && node != end; node = static_cast<void**>(*node))
        if (*reinterpret_cast<short*>(reinterpret_cast<uint8_t*>(node) + 0xA) == playerIndex) return node;
    return nullptr;
}
