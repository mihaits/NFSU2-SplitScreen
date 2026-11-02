// Part of NFSU2SplitScreen.cpp: high resolution Split-Screen menu icon.
// Included by NFSU2SplitScreen.cpp in a fixed order (one translation unit); not compiled on its own.

// HD interface mods redraw the main menu icons but not MAIN_ICON_SPLIT_SCREEN, which only the console menus used. When
// the Quick Race icon is larger than the stock size, this icon's D3D texture is replaced by the embedded 512x256 one.
// The menu object doesn't depend on the texture size. The texture table stays as it is: the D3D texture takes the
// place of the game's own and the game releases it like its own.
#define FN_GET_TEXTURE_INFO 0x4901D0     // TextureInfo* __cdecl (hash, default if missing, include unloaded)
#define QUICK_RACE_ICON     0x4E6FBB02
#define TEXINFO_PLAT        0x00         // -> { ..., +0x18 IDirect3DTexture9* }
#define TEXINFO_HASH        0x24
#define TEXINFO_WIDTH       0x44         // uint16
#define SPLIT_ICON_RESOURCE 101
#define SPLIT_ICON_W        512
#define SPLIT_ICON_H        256
#define SPLIT_ICON_STOCK_W  128
#define SPLIT_ICON_STOCK_H  64
#define DDS_HEADER_SIZE     128

static uint8_t* FindMenuTexture(uint32_t hash) {
    auto* info = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, int, int)>(FN_GET_TEXTURE_INFO)(hash, 0, 0);
    return info && *reinterpret_cast<uint32_t*>(info + TEXINFO_HASH) == hash ? info : nullptr;
}

static const uint8_t* SplitIconPixels() {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<const char*>(&SplitIconPixels), &self);
    HRSRC res = FindResourceA(self, MAKEINTRESOURCEA(SPLIT_ICON_RESOURCE), reinterpret_cast<const char*>(RT_RCDATA));
    HGLOBAL data = res ? LoadResource(self, res) : nullptr;
    auto* dds = data ? static_cast<const uint8_t*>(LockResource(data)) : nullptr;
    return dds && SizeofResource(self, res) == DDS_HEADER_SIZE + SPLIT_ICON_W * SPLIT_ICON_H ? dds + DDS_HEADER_SIZE : nullptr;
}

static void SwapSplitMenuIcon() {
    static IDirect3DTexture9* s_hd;
    uint8_t* split = FindMenuTexture(MMSPLIT_ICON);
    uint8_t* quick = FindMenuTexture(QUICK_RACE_ICON);
    if (!split || !quick) { Log("split icon: textures not found"); return; }
    auto*& slot = *reinterpret_cast<IDirect3DTexture9**>(*reinterpret_cast<uint8_t**>(split + TEXINFO_PLAT) + 0x18);
    if (slot == s_hd) return;
    int quickW = *reinterpret_cast<uint16_t*>(quick + TEXINFO_WIDTH);
    if (quickW <= SPLIT_ICON_STOCK_W) { Log("split icon: stock (Quick Race icon %d wide)", quickW); return; }
    D3DSURFACE_DESC desc;
    if (!slot || FAILED(slot->GetLevelDesc(0, &desc)) || desc.Width != SPLIT_ICON_STOCK_W || desc.Height != SPLIT_ICON_STOCK_H) {
        Log("split icon: unexpected texture, left alone");
        return;
    }
    const uint8_t* pixels = SplitIconPixels();
    IDirect3DDevice9* device = nullptr;
    if (!pixels || FAILED(slot->GetDevice(&device))) { Log("split icon: no pixels or device"); return; }
    IDirect3DTexture9* hd = nullptr;
    HRESULT hr = device->CreateTexture(SPLIT_ICON_W, SPLIT_ICON_H, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &hd, nullptr);
    device->Release();
    D3DLOCKED_RECT lock;
    if (FAILED(hr) || FAILED(hd->LockRect(0, &lock, nullptr, 0))) {
        Log("split icon: texture failed (%08lX)", hr);
        if (hd) hd->Release();
        return;
    }
    for (int row = 0; row < SPLIT_ICON_H / 4; ++row)     // DXT5: 16 bytes per 4x4 block
        memcpy(static_cast<uint8_t*>(lock.pBits) + row * lock.Pitch, pixels + row * SPLIT_ICON_W * 4, SPLIT_ICON_W * 4);
    hd->UnlockRect(0);
    slot->Release();
    slot = s_hd = hd;
    Log("split icon: high resolution (Quick Race icon %d wide)", quickW);
}
