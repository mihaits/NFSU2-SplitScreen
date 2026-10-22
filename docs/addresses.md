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
| 0x83AA64 + i*0x28 | per-player settings (camera +0x10, HUD options); 2 entries, race data follows at 0x83AAB4 |
| 0x83AAB8[2] | selected car per player |
| 0x7F4454[2] | FE port per FE player (mask 1/2/4/8); 0x85AD28 = current FE player |
| 0x8900AC[2] / 0x8900B4[2] / 0x8900BC | local players byIndex / inOrder / count (relocated, [players](players.md)) |
| 0x832DE0 | views `eView[22]`, stride 0x70 |
| 0x870974 | IDirect3DDevice9*; 0x870980/84 screen width/height |
| 0x8637B0 | FE package cache (list head +4, node = entry+4) |
| 0x870CB8.. | graphics options in registry order (0x870CB8 car env map, 0x870CD4 road reflection, 0x870CDC blur, 0x870CE0 light streaks, 0x870CE4 glow, 0x870CF0 DoF, 0x870CFC overbright, 0x870D08 FSAA level, 0x870D10 rain) |
| 0x89E7C4 / 0x89E7C8 / 0x89E7CC | race counters: AI racers / count B (the plugin adds the extra racers here for the results) / one more; the results use 0x89E7C0 + all three |
| 0x89E7B0 | race state (7 = Free Run's free state) |
| 0x8707DC | layout computed by 0x5C2120 (applied copy 0x8707E0) |
| 0x88EB00 | track streamer (position blocks +0xB0 + k*0x5C, +0 = the car array index it follows, copied by 0x5E4F20) |

## Race cars and racers
| address / field | meaning |
|---|---|
| [0x890080]+0x1C+4i | race car i; count [[0x890118]+0x24] |
| car +0x14 | racer spec (+3 grid slot, +4 type: 1 human, 2 AI, 3 traffic; ids 50+ = traffic) |
| car +0x28 | mover mode (5 = NISMvr, driven by an intro/outro scene) |
| car +0x34 | mover; physics body = [mover+0x20]: position +0x20 (z +0x28), velocity +0xA0 |
| racer +0x14 | progress (>= 1 = finished); +0xA position |
| 0x827B98 + track*0x14 | NIS car binding table (+0 track, +4 x, +8 y, +0x10 car array index, -1 none) |
| 0x83AAC4 + type*0x14 | quick race settings per type: +0 local players, +1 opponents, +8 track |

Race types (0x83AAB4): 0/1/7 circuit/sprint/lap KO, 2 drag, 3 drift, 4 URL, 5/10 Street X, 8 outrun, 9 Free Run.

## Other
| address | meaning |
|---|---|
| player +0x9A4 | HUD; HUD +0x20 position element, +0x6C outrun element |
| 0x8364D4 | minimap manager |
| 0x7F4460 / 0x7F446C | quick race options screen / Free Run screen |

The game reads graphics options from the per-user VirtualStore registry copy
(`HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\EA Games\Need for Speed Underground 2`). If you
launch it from an elevated shell, HKLM is read instead and the settings differ.
