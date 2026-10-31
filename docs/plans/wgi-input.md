# Xbox input through Windows.Gaming.Input

## Goal
Read Xbox pads through WGI instead of XInput, so a pad's input, id and vibration come from one object. This replaces the
pairing of XInput users with WGI pads, and with it the swap fix's guesswork. XInput stays only as a fallback. More than 4
Xbox pads is a separate change afterwards.

## State at the start
The working copy has an **uncommitted** swap fix (`XboxWgi.cpp`, `XboxWgi.h`, `XInputDevices.cpp`, `docs/input.md`,
`docs/open-issues.md`): pad numbers 0..3 kept per pad by `NonRoamableId`, mapped to XInput users by `WgiPadUser`. Tested
with 2 pads, reconnecting in both orders. Held back because this plan would replace most of it. If WGI readings aren't
live even when focused, commit that fix as it is instead.

## Known
- `IRawGameController2.NonRoamableId` stays the same across reconnects within a run; it was different in a later run.
  Keep numbers per run only.
- `XboxWgi.cpp`: one MTA thread owns every WGI object and polls the list once a second. `Pad` holds the gamepad, id,
  VID:PID and whether it has trigger motors. `Pair`/`Unpair`, `SlotOfUser`, `FreeSlot`, `ClaimDroppedSlot`, `MatchFree`,
  `PairPads`, `Same`, `Active` are the pairing code.
- `XInputDevices.cpp`: `GetPad` is the single read path; `XPadSetRumble` sends through WGI or XInput. Pads 0..3 are the
  Xbox block (`kRealPads`), 4..9 test pads, 10..17 native PlayStation pads (`kDsFirst`).
- Pads played for a long time without pairing by input. Suspected cause: empty WGI readings without focus. Not measured.
- The game reads XInput today even when its window isn't focused.

## Step 1: focus check (decides the rest)
Log, once a second, each WGI pad's reading and each XInput user's state, with the game focused and unfocused, while the
user holds a button. Pass: WGI readings are live when focused. Pads going idle while unfocused is acceptable (user,
2026-10-31): note it in docs/input.md, and test real pads with the game window focused.

Also log the WGI list with a native DualSense and with DS4Windows running, to see whether a pad shows up twice.

## Step 2: the switch
1. `XboxWgi.cpp`: remove the pairing code. A pad takes the number holding its id, else a number no pad has, else the one
   that dropped longest ago (`FreeSlot` without the user index).
2. Replace the 1 s list poll with `GamepadAdded`/`GamepadRemoved`, keeping a slow poll if events prove unreliable. The
   current `AddedHandler` does nothing and can grow into this.
3. Publish the latest reading per number as an XInput-shaped state, converting from `GamepadReading`: button bits (the
   `kMap` table in `Same`), triggers 0..255 (a full pull must round to exactly 255: `StateOf` needs it for full brake,
   see "Pedals on the shared Z axis" in docs/input.md), sticks to +-32767. The thread's Sleep(8) adds up to 8 ms of delay: shorten
   it, or read on the game thread if the MTA allows it.
4. `XInputDevices.cpp`: `GetPad` reads WGI while it runs; else plain `XInputGetState(pad)`, with the number equal to
   the user index. `WgiPadUser` and `g_rumbleUser` go. Vibration goes through WGI for all pads, XInput only in the
   fallback. Check Xbox 360 class pads rumble through WGI before removing their XInput path.
5. Filter duplicates found in step 1, if any.

## Files to touch
`src/XboxWgi.cpp`, `src/XboxWgi.h`, `src/XInputDevices.cpp`, `docs/input.md`, `docs/open-issues.md`, README if behaviour
or the to-do list changes.

## Test steps
Real pads, one check at a time, the user tests:
1. Menus and a 2P race: steering, triggers, buttons, D-pad, sticks all as before, no added lag.
2. Both pads off, back in reversed order, in a race: each keeps its player.
3. Rumble on both pads, impulse triggers on the Xbox One pad.
4. Takeover: START on a spare pad while a player's pad is off.
5. Mixed race with a native DualSense: no doubled pad.
6. Linux (user's setup): pads read, no doubled pad, rumble rate limit still applies.

## Done criteria
Pairing code is gone. A returning pad keeps its player with no wait for a press. Steps 1-5 pass and step 6 is checked or
listed as open. Docs current. This plan deleted in the commit.
