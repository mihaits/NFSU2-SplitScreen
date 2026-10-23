# NFSU2 Split Screen — technical reference

How the plugin works and the game internals it relies on. These docs describe the **current, confirmed** state only:
everything here was verified in game. The source is split by subsystem into `src/parts/` (see
[architecture](architecture.md)); every subsystem starts with a comment block that goes into more detail.

- Target: `speed2.exe` v1.2 (the cracked v1.2 exe's `.rdata` matches the SecuROM v1.2 one). Image base 0x400000, no ASLR, x86.
- All addresses are virtual addresses in that exe. "imm" = immediate operand, "disp" = displacement.
- Calling conventions: `__thiscall` (ecx = this), `__cdecl`, `__stdcall` as noted. "ret N" = callee pops N bytes.

## Start here
- [architecture](architecture.md): install, patch helpers, the per-frame entry, mode predicates, how players 3..6 are
  added on top of a 2-player game; thread safety; **gotchas**.
- [addresses](addresses.md): core globals, race cars and racers, race types.

## Subsystems
| doc | covers |
|---|---|
| [frontend](frontend.md) | main menu entry, Players row, joining, seats and car select, shared garage, FE API |
| [input](input.md) | DirectInput side, XInput virtual pads, pad resolver, keyboard player, P3/P4 bindings, rumble |
| [players](players.md) | players 3..6: relocated arrays, racers, settings, seat store, names, catch-up, AI opponents, free camera |
| [rendering](rendering.md) | views, layout, viewports, projection; per-view passes (headlights, skins, reflections, weather, blur, streaks) |
| [streaming](streaming.md) | track sections, streaming positions, pools, load priority, post-race |
| [hud](hud.md) | FE objects, HUD packages and copies, quad HUD, minimaps, gauges, Free Run HUD |
| [sound](sound.md) | listeners |
| [race-flow](race-flow.md) | intros and grid, post-race scene, results counting, standings, "Hurry!" timer |
| [free-run](free-run.md) | Free Run (free roam) in split screen |
| [open-issues](open-issues.md) | known open bugs and leads |
| [tooling](tooling.md) | static and live tools, debug mailbox, automation |
| [bug-report](bug-report.md) | bug report template and the session handoff format |

When a feature changes, update the subsystem doc it belongs to (and [addresses](addresses.md) for new shared globals).
