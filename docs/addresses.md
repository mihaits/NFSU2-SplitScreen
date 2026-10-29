# Addresses

The globals and structures that come up everywhere. Each subsystem doc lists its own functions and hook sites; to
find an address, grep `docs/` (and the source's comment blocks) for it.

## Core globals

| address | meaning |
|---|---|
| 0x850078 | game mode: 1 career, 2 quick race, **4 split screen** |
| 0x89E7C0 | NUM_PLAYERS: local players of the active race (stays 2 with 3-4 players) |
| 0x89E7A0 | race params object (+0x20 = NUM_PLAYERS, +0x68 int[3] human racer slots (0x89E808), +0x74 racer count) |
| 0x89E7E8 | race ports per player [2] (copied from 0x83A9D8, the ports chosen at the joins) |
| 0x8707E0 | applied view layout (1 single, 2 + mirror, 3 = two player cameras) |
| 0x8654A4 | game state (3-5 loading, 6 racing) |
| 0x83AAB4 | quick race type index; 0x83AAC4 + type*0x14 = per-type settings (byte +0 = local player count) |
| 0x83A9E0 + i*0xAB4C | per-player block (name +0, quick race garage +0x3B0); only 2 blocks exist |
| 0x83AA64 + i*0x28 | per-player settings (camera +0x10, HUD options, Transmission +0x15, Stability Control +0x16 (0 = on)); 2 entries, race data follows at 0x83AAB4 |
| 0x83AAB8[2] | selected car per player |
| 0x7F4454[2] | FE port per FE player (mask 1/2/4/8); 0x85AD28 = current FE player |
| 0x8900AC[2] / 0x8900B4[2] / 0x8900BC | local players byIndex / inOrder / count (relocated, [players](players.md)) |
| 0x832DE0 | views `eView[22]`, stride 0x70 |
| 0x870974 | IDirect3DDevice9*; 0x870980/84 screen width/height |
| 0x8637B0 | FE package cache (list head +4, node = entry+4) |
| 0x870CB8.. | graphics options in registry order (0x870CB8 car env map, 0x870CD4 road reflection, 0x870CDC blur, 0x870CE0 light streaks, 0x870CE4 glow, 0x870CF0 DoF, 0x870CFC overbright, 0x870D08 FSAA level, 0x870D10 rain) |
| 0x89E7C4 / 0x89E7C8 / 0x89E7CC | race counters: AI racers / count B (the plugin adds the extra racers here for the results) / one more; the results use 0x89E7C0 + all three |
| 0x89E7B0 | race state (7 = Free Run's free state) |
| 0x89E7A0 | the loaded race's track id |
| [0x883DA0] | track table, count [0x883DA4], stride 0x128; lookup by id 0x5D3E40(id) (+0x8A id): +0x00 name, +0x20 path, +0x40 region, +0x80 sprint drift, +0x86 reversible, +0xAC region bounds (same for every city track) |
| 0x5EB5D0(n, out) | start spots from the Forward/BackwardStartingPosition markers: n entries of 0x20 (x, y, z +0, heading word +0x10, angle = h × 2π / 65536, forward = (cos, sin)); called by the race start placement 0x6065B0 at 0x6065C6 during the loading screen |
| 0x8707DC | layout computed by 0x5C2120 (applied copy 0x8707E0) |
| 0x88EB00 | track streamer (position blocks +0xB0 + k*0x5C, +0 = the car array index it follows, copied by 0x5E4F20) |

## Race cars and racers
| address / field | meaning |
|---|---|
| [0x890080]+0x1C+4i | race car i; count [[0x890118]+0x24] |
| car +0x14 | racer spec (+3 grid slot, +4 type: 1 human, 2 AI, 3 traffic; ids 50+ = traffic) |
| car +0x28 | mover mode (5 = NISMvr, driven by an intro/outro scene) |
| car +0x34 | mover; physics body = [mover+0x20]: position +0x20 (z +0x28), velocity +0xA0 |
| racer +0x14 | progress: laps done + lap fraction (finished only in a 1-lap race at >= 1); +8 laps done; +0xA position |
| [0x890080]+0x80+0x10i, +0xE0+4i | World slots of racer i (i < +0x14, max 6): position, road position (car +0x8FC); 0x609CE0 |
| [0x890080]+0x118 | traffic update object (0x42F910 per World update) |
| car +0x24 | driver controller (0x5EBA30(car, type), object at car +0x2C): 1 human (pad), 2 drag, 3 AI racer, 0/4/5 other |
| 0x827B98 + track*0x14 | NIS car binding table (+0 track, +4 x, +8 y, +0x10 car array index, -1 none) |
| 0x83AAC4 + type*0x14 | quick race settings per type: +0 local players, +1 opponents, +8 track |

Race types (0x83AAB4): 0/1/7 circuit/sprint/lap KO, 2 drag, 3 drift, 4 URL, 5/10 Street X, 8 outrun, 9 Free Run.

## Other
| address | meaning |
|---|---|
| player +0x9A4 | HUD; HUD +0x20 position element, +0x6C outrun element |
| player +0x22C / +0x230 | player mode (applied by 0x605C70) / the racing mode the per-frame picker 0x5EA650 applies: 2 car racing, 3 drag, 4 auto pilot (AI drives the car: 0x5FAE20, back 0x5FAF10), 9 finish race. Names at 0x802090 |
| player +0x50 | finished (the picker then picks mode 9) |
| 0x8364D4 | minimap manager |
| 0x7F4460 / 0x7F446C | quick race options screen / Free Run screen |

Car and race values the DualSense lights and triggers read (`parts/pad_lights.cpp`; `car = *(player + 4)`):

| address | meaning |
|---|---|
| car +0x440 | wheel-driven engine rpm; the tach's rpm is `*(*(car+0x968) + 0x58)` (not drag/drift, spec +4 == 1, spec +0x900 == 0, spec = `*(car+0x14)`) |
| `*(car+0x20)` +0x1F0 / +0x1F4 | idle / max rpm |
| car +0x434 | gear: 0 reverse, 1 neutral, 2 = 1st |
| `*(*(car+0x34)+0x4C)` | drivetrain: +0x84 + 4*gear up-shift rpm, +0xE4 and +0xF0 shift-light factors; shift rpm = up - `*(float*)0x7FBF5C` * `*(float*)(0x7FBF60 + 4*(gear + 9**(drv+0xF0)))` * `*(float*)(drv+0xE4)` (tach shift light 0x4C6ACF..0x4C6B45) |
| car +0x460 | nitrous burn start time (0 / 0x7FFFFFFF = off); nitrous object at car +0x40 (+0x41C remaining, +0x418 capacity) |
| `*(car+0x34)` +0x38 + 4*i | wheels (0 FL, 1 FR, 2 RL, 3 RR): +0x170 grip used (1.0 = at the limit, 0 on all four = airborne), +0x124 longitudinal force, +0x28 angular speed, `*(+0x14)` tyre data (+0x10 radius, 0.3015 m on the Miata). The rear reads 1.0 when slow or stopped. |
| `*(car+0x1C)` (racer) | +0x14 progress (laps done + fraction), +0xA position (1-based); finished = player +0x50 |
| `**(int**)0x890118` | race stage: 0 none, 1 flyby, 2 prerace, 3 countdown (3.0 s), 4 racing |
| 0x7FB718 | game clock (seconds) |
| 0x8654A4 | game state: 3 frontend, 4/5 loading, 6 race (results included) |

The game reads graphics options from the per-user VirtualStore registry copy
(`HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\EA Games\Need for Speed Underground 2`). If you
launch it from an elevated shell, HKLM is read instead and the settings differ.
