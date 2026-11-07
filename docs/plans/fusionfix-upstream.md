# Plan: offer the plugin's base-game fixes to FusionFix

Status (2026-11-07): not started. Some fixes in this plugin correct the game itself and matter in single player too.
They fit better in ThirteenAG's NFSU2 FusionFix (`ThirteenAG/WidescreenFixesPack`, `source/NFSUnderground2.FusionFix`),
which every player can use. FusionFix's source was last checked at `c0b7d7dc` (2026-11-04); no NFSU2 FusionFix build is
released yet (see [fusionfix-compat](fusionfix-compat.md)).

## Goal
Each fix below is either proposed upstream (an issue or PR the user has approved and posted), or written off with a
reason. Once FusionFix ships one, the plugin either stops applying its own copy when FusionFix is loaded, or the README
explains why it keeps it. That second step belongs to the fusionfix-compat plan.

## Candidates
The fixes in this table are measured in this plugin, and each one applies in single player too.

| Fix | Base-game bug | Plugin code, doc | In FusionFix (c0b7d7dc)? |
|---|---|---|---|
| One sound per mouse click on list screens | The list screens' handler 0x556020 plays the accept sound for both the package click 0x406415E3 and the accept 0x0C407210. A left click on the help bar's Continue sends both one frame apart, so the sound plays twice (race options, single player too). | `ListClickEcho` in `parts/seats.cpp`; [frontend.md](../frontend.md) "List screen sounds"; [bugs/list-click-double-sound.md](../bugs/list-click-double-sound.md) | no |
| Tuned headlight colours | `HeadlightColor` is the car's bytes / 128 (255 = 2.0), and ps_1_1 clamps constants to 1, so a colour with all bytes above 128 turns white. | `CarLightColor` in `parts/render_headlights.cpp` (only the colour scaling is relevant upstream, not the per-player lights); [rendering.md](../rendering.md) | no |
| Full analog brake | The pedal decoder XORs the byte, so an axis that stops at 0xFD (a real Xbox pad through DirectInput, Z -9961..9961) gives at most 0.965 brake. | `StateOf` in `XInputDevices.cpp` (scales LT to the axis end); [input.md](../input.md) | partly: `Input.ixx` `ReadTriggerPedals` fills the separate pedal bytes (0..254) only when the triggers are bound to separate axes, and returns early in the game's default combined-pedals binding |
| Both triggers at once | In the default binding the shared axis is LT minus RT, so holding both cancels them (no brake-throttle or burnout). | `SeparatePedals` in `parts/input.cpp` (fills the separate pedal bytes +0x10 / +0x11 at 0x5C2044, which clears them in shared mode); [input.md](../input.md) | partly, same as above |
| Voice request count | Removing a positional sound source (0x483E40 -> 0x481E50) leaves the request count at list +0xC, so the arbitration could walk an empty list at 0x481AB4 with every voice in use. | `RemoveSoundSource` in `parts/sound.cpp` (call at 0x5A9584); [sound.md](../sound.md) | no; found by nfsu2-web, never reproduced here, so offer it only with a repro or as a note |

The two pedal fixes depend on the plugin's own pad reading (it carries the triggers in `rglSlider[0..1]`), so
upstream they need a DirectInput version, likely an extension of `ReadTriggerPedals` to the combined binding.

## Ask first: native pads
FusionFix reads pads through Xidi (XInput pads presented as DirectInput devices), and its input module is built around
that: bindings, dead zones, menu labels and trigger pedals for Xidi devices. For PlayStation and Switch pads it points to
Steam Input or DS4Windows. The plugin replaces the game's pad reading instead (`XInputDevices.cpp`: XInput, plus
`DualSense.cpp` for the DualSense and DualShock 4 over HID, `XboxWgi.cpp` for impulse triggers; [input.md](../input.md)).
That is a feature proposal, not a fix, and it goes against FusionFix's chosen design, so open an issue first asking
whether they want any of it, and write nothing for upstream before they say yes:
- PlayStation pads natively, without DS4Windows or Steam Input
- DualSense adaptive triggers, lightbar and player LEDs; DualShock 4 lightbar and rumble
- Xbox impulse triggers

The per-player parts (fixed ports per seat, pad speakers per player) stay in the plugin.

Left out: fixes that only matter with more than one local player (audio pool size, streaming pools, car pool block,
pool records).

## Approach
1. Read FusionFix's current source again (`tools/mod_overlap.py ThirteenAG/WidescreenFixesPack --names` lists its
   patches next to ours) and update the last column. Drop anything it already fixes.
2. For each remaining fix, write it the way FusionFix code looks: a module (`.ixx`) with `hook::pattern` signatures
   instead of fixed addresses, and SafetyHook for inline hooks. Keep it to the single-player fix; the plugin's
   split-screen parts stay here. Show the user each patch or issue text before anything is posted.
3. Test each patch in single player with the plugin removed (WidescreenFix or FusionFix alone): the sound with a mouse
   click on Continue, the headlights with a bright tuned colour, the brake with an Xbox pad through DirectInput, and
   both triggers at once.
4. Posting an issue or PR (the native pads issue too) is the user's call: ask first, with the final text.

## Done when
- Every candidate is posted upstream or written off, with the link or reason added here, and the native pads issue
  is answered.
- The fusionfix-compat plan's table has a row for each fix FusionFix takes over (how the plugin avoids doing it twice).
- Then delete this plan.
