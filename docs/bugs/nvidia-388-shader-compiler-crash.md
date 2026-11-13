# nvidia-388-shader-compiler-crash
Status:       fixed (headlight shaders read each texcoord once, 2026-11-13); the reporter's 2P race on the GT 630
              with 388.13 works
Report:       [issue 3](https://github.com/mihaits/NFSU2-SplitScreen/issues/3): GeForce GT 630 (PCI 10DE:0F00, GF108 Fermi;
              first reported as a GT 640, the dump's adapter identifier shows the 630), driver 388.13 from Windows
              Update (`nv_ref_pubwu.inf`, nvd3dum.dll 23.21.13.8813, PE timestamp 59F351C5), Windows 10 22H2 x64.
              Every 2P race crashes on the first race frame, every track; 1P works; the same install on a newer laptop
              works. Extra Options and WidescreenFix installed.
Repro:        every 2P race on that driver (5 crash reports, 2026-11-05..11, tracks 4000, 4002, 4045, 4102). Not
              reproducible here (no Fermi card).
Area:         [rendering](../rendering.md) (headlights: every player's light in one draw)

Known:
- All 5 crashes: C0000005 writing 0x00000025 at nvd3dum.dll+5DF5C6, on a driver worker thread, game state 6 (race
  start). In the one debug log, the crash is ~50 ms after "headlights: vertex shader ... extended" and "pixel shader
  0/2 created"; nothing else unusual in the log. Creation itself succeeds (hr 0).
- nvd3dum.dll+5DF5C6 is in NVIDIA's D3D9 shader translator (strings nearby: `parseasm`, `fp40_optx`, `vp40_optx`,
  `!!FP2.0`, register names `f[TEX0]..f[TEX9]`, `f[COL0]`, `f[FOGC]`, `v[OPOS]`). The function at +5DF4B0 builds a
  register name (via +5DC080, optional suffix `P` from the string at RVA 0x9EFAC0), interns it to an id and, when the
  id is new, appends it to a per-shader table: ids array, row-pointer array, count, capacity. There is no check of
  count against capacity.
- Capacity is set once at +5DF3D0 (only caller +5E0425): `[info+0x14] * (32 or 1) + [info+0x2D0]` (32 when info+0x48
  has 0x21000, 0x10000 or 0x8000 set). info+0x14 and +0x18 are limits, not counts: +5D8010 sets each to 16 when it is
  0. In the dump, info (176F1ED0) has +0x14 = 16, +0x18 = 16, +0x48 = 0x40000 (no x32 bit), so the table holds 16
  inputs. What sets the x32 bits (GPU family or shader profile) is not known.
- Driver 474.64 (2023-10; nvd3dum.dll timestamp 65252E16; Kepler and newer, its inf has no 0F00) has the same
  constructor at +7213C0, now `[info+0x14] * (32 or 1) + [info+0x338] + [info+0x18] + 0x61`: 16 + 16 + 97 = at least
  129 for this shader instead of 16, with the arrays zero-filled and rows allocated per insert (+721E5B). The insert
  still has no explicit bounds check, but the slack covers the one fog entry, so 474.x should not crash here. It
  does not install on this card, nor does 475.14 (2024-07, the newest the NVIDIA site lists for "GT 630"): their
  infs list only the Kepler GT 630 IDs 0FC2 and 1284. The site lists drivers by product name, not chip.
- Driver 391.35 (2018-03, the last for desktop Fermi, lists 0F00; nvd3dum.dll timestamp 5AB58378) has the 388.13
  code unchanged: constructor at +5FA3A0 (`[info+0x14] * (32 or 1) + [info+0x2D8]`, no slack) and the same insert
  with no bounds check (the faulting store would be at +5FA596). So no driver fixes this card.
- The compile runs lazily at draw time on a driver thread: in the dump the main thread waits inside a game-side d3d9
  call, not in our `CreatePixelShader`. So the log's last "pixel shader N created" line does not say which variant was
  compiling. A 2P race creates only variants 0 (world) and 2 (reflect). 1P never crashes because the shader swap is
  only active with other players' lights.
- In the dump (crash frame ebp 03CFE888): the name being added is `f[FOGC]` (stack 03CFE860), the fragment program's
  fog input. Table at 1A7F5430: count 16, capacity 16, ids 4..0x13, and the new id 0x14 already stored one past the
  ids array (over the row-pointer array's first word); row pointer 17 is past its array and reads the next heap word,
  0x25, which is the faulting write address. So the fragment program already has 16 inputs and the driver adds fog
  (fixed-function fog behind a ps_2_0 shader) as a 17th.
- What counts as an input: one per declared or read texcoord and colour, plus one more per texcoord read with
  `texldp` (the `P` name). That gives the old reflect shader exactly 16: t0..t7 (t3 declared, read only projected), v0,
  six light texcoords read with `texldp` and t3 with `texldp`. The old world shader had 14, depth 14, depthreflect 17.
- The old headlight pixel shaders were valid ps_2_0 (within limits, created with hr 0); they read six light
  texcoords both as `.w` (ramp) and with `texldp` (beam). The game's own ps_1_1 headlight shaders read only t0..t3 and
  v0, so the many inputs were ours.

Ruled out:
- Invalid shader / unsupported feature: within ps_2_0 / vs_1_1 limits, the runtime creates them, GT 640 is SM 3.0+.
- Track or content specific: 4 tracks.

Fix:
- The headlight pixel shaders divide each light's beam coordinate by w themselves and sample with `texld`, so each
  texcoord is one input: 8 (world, depth) to 11 (depthreflect) before fog, 12 at most with it. Two batches of three
  lights in separate registers keep the assembler's dependent-read order within 2. Old/new screenshots of 2P and 6P
  grids (variants 0, 2, 3) match in every lit area. The reporter's test build ran a 2P race on the GT 630 without a
  crash (2026-11-13).

Tools:
- Static disassembly of a driver DLL without loading it, PE-timestamp check, and minidump memory reads: small capstone
  scripts (PE section mapping, MemoryListStream / Memory64ListStream parsing). The 388.13 package
  (`388.13-desktop-win10-64bit-international-whql.exe`) unpacks with 7-Zip: `Display.Driver/nvd3dum.dl_` is the 32-bit
  D3D9 driver.
