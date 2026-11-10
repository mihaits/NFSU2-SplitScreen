# External sources on the game's internals

Other projects that document `speed2.exe` or the same engine, and what each is good for. There is no complete
decompilation of `speed2.exe`. State as of 2026-11; everything here was read on the web (READMEs, file listings, source
viewed as text), nothing was built or run.

Rules for using them:
- A name or address from any of these is a lead. Check it against the disassembly (`tools/sdis.py`) before it goes into
  code or into [addresses](addresses.md).
- Names are their authors' own unless the entry says otherwise; only the sibling-game decompilations carry EA's symbols.
- Mind the licence before copying anything: several have none (facts only).
- Some repositories ship game code or binaries. Don't download those (marked below).

The exe these docs describe is the build other projects call "North American v1.2": 4,800,512 bytes, entry 0x75BCC7.

## At this exe's addresses

| source | what it has | best for |
|---|---|---|
| [yugecin/nfsu2-re](https://github.com/yugecin/nfsu2-re) ([docs](https://yugecin.github.io/nfsu2-re/)) | An IDA export (`SPEED2.idc` on the `idc` branch: about 18,000 functions, 500 structs, 5,000 hand-given names), C structs in `nfsu2-re-hooks/a_structs.c`, hash and language dumps. No licence. | Frontend (FNG screens and their message handlers, car select), pools, bin loading, career. Thin on input, sound, physics, rendering. |
| [th0mk/nfsu2-web](https://github.com/th0mk/nfsu2-web) | `runtime/game.h`: a commented address list (race parameters 0x89E7A0, race status 0x890118, World 0x890080, player settings, car fields). Holds a port of this plugin (`runtime/splitscreen.c`) with co-op free roam and career on top. MIT, archived. | Players and racers, race flow, free roam, network races. |
| [nlgxzef/NFSU2Unlimiter](https://github.com/nlgxzef/NFSU2Unlimiter), [ExOptsTeam/NFSU2ExOpts](https://github.com/ExOptsTeam/NFSU2ExOpts) | `InGameFunctions.h` / `GlobalVariables.h`: about 330 addresses with names in EA's style (several match the Most Wanted source: `eView::GetPixelSize`, `RaceParameters::GetDriverInfoByPlayerNumber`, `FEngSendMessageToPackage`). | FEng API, eView, streaming textures, traffic, car render patch sites. |
| [troikoss/speed-re](https://github.com/troikoss/speed-re) | `config/SPEED2_EXE_1_2/*.json`: function name -> address, size, hash per subsystem; `OriginalSymbols.ld`: about 1,300 globals; subsystem docs (`docs/pc-hud-minimap.md`, `docs/pc-controls.md`). AI-assisted, names invented, matching not established. | HUD masks, minimap, controls screen, FEng, chase camera: as leads. |
| [xan1242/NFSU-XtendedInput](https://github.com/xan1242/NFSU-XtendedInput) | `NFSU2_Addresses.h` (joystick init, scanner configs, event generator span 0x5C19D7..0x5C20D4), `NFSU2_EventNames.h` (the JoyEvent name table). MIT. | Input. |
| [ThirteenAG/WidescreenFixesPack](https://github.com/ThirteenAG/WidescreenFixesPack/tree/master/source/NFSUnderground2.FusionFix) (FusionFix) | Pattern-based, addresses in comments. `Input.ixx`: the device record's mapping tables and axis setup by name; `Rendering.ixx`: view scale per view id, rain droplet sites. GPL-3.0. | Input record layout, view scaling, the code this plugin has to coexist with. |
| [AeroWidescreen/NFSHDReflections](https://github.com/AeroWidescreen/NFSHDReflections) | `SOURCE/NFSU2`: about 90 render patch sites by feature (road reflection and cube map sizes, sky / headlights / particles in the reflection passes, vehicle LOD, motion blur, depth of field). GPL-3.0. | Per-view reflections, headlights, blur. |
| [TalhaMudo/NFSU2Recomp](https://github.com/TalhaMudo/NFSU2Recomp) | `GRAPHICS_RESEARCH.md`: reflection pipeline, view ids, rain and sky state. Its reference exe is another build: most code addresses between 0x51xxxx and 0x68xxxx are 0x10 higher than here (`.data` matches); `runtime/build_na.h` has the mapping. | Rendering and weather, after remapping. |
| [codepdbh/nfsu2-recompiled](https://github.com/codepdbh/nfsu2-recompiled) | `docs/NFSU2_BINARY_REPORT.md`: the exe still has its relocation data (unnamed section at 0x8F5000, about 91,000 entries), an exact list of every absolute address operand. `config/hooks.txt`: about 30 widescreen sites. | Finding every reference to a global (what the relocation scans in `Install` do by value). |

Smaller ones, each a few addresses: xan1242/NFSU2_EAXSound_DeltaTimeFix (the `EAXSound::Update` call site),
xan1242/NFSU2_TrafficRacers (AI goal constructors).

## Same engine, real symbols (addresses need mapping)

| source | what it has | best for |
|---|---|---|
| [dbalatoni13/nfsmw](https://github.com/dbalatoni13/nfsmw) | Matching decompilation of Most Wanted (2005), the next game on this codebase: about 2,000 source files with EA's names, type and function dumps from a build with debug symbols. CC0. Underground leftovers remain (`RaceParameters` with `MAX_NUM_PLAYERS = 2`, `PlayerJoyports[2]`, `bDragRaceFlag`). Offsets differ from this game (`eView` is 0x68 there, 0x70 here). Physics, AI and gameplay were rewritten and don't map. | Field names and semantics of eView / Ecstasy, TrackStreamer, FEng, EAXSound, ResourceLoader, CameraMover, chunk ids. |
| [dbalatoni13/nfsug](https://github.com/dbalatoni13/nfsug) | Underground 1 (the direct predecessor, with 2-player split screen), from a prototype with symbols: `config/GNDP8P/symbols.txt`, about 10,000 functions with mangled signatures and sizes. CC0. | The split-screen functions' names and signatures (`eUpdateViewMode`, `GetPlayerView(int)`, `eUpdateHeadlightCamera(eView*, int)`, `RaceCoordinator::InitPlayers`). |

No build of this game with debug symbols is confirmed.

## File formats

- [whoismept/OpenUG2](https://github.com/whoismept/OpenUG2) `docs/FORMATS.md`: the chunk container, scenery and
  instance sections, scenery groups, route chunks, start grids (`TrackPosMarkers`), HUD bundles. No exe addresses. MIT.
- [NFSTools/FEngLib](https://github.com/NFSTools/FEngLib): FNG reader / writer, a class per chunk. No licence.

## No use for addresses

- cobanov/nfsu2-decomp: an empty scaffold (no function matched yet). Its `docs/getting-started.md` lists the builds.
- daniiichan/Need-for-Speed-Underground-2-macOS, leonardo-ferioli/nfsu2-static-recomp: machine-lifted code with no
  names; both commit game-derived code or exe dumps. **Don't download.**
- Console ports (antoxa2584x/nfsu2-sw: Xbox; mad0xStrixhadorami/nfsu2-native: PS2): other platforms' addresses.
