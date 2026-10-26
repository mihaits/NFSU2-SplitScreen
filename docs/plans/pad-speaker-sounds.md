# Plan: more sounds on the pad speakers, and a mute-button toggle

## Goal
1. Besides the engine, each player's own car sounds play on their DualSense speaker and not on the PC: skids,
   hits / scrapes, nitrous, rev limiter, turbo / blow-off, gear shifts (whatever the car's sound objects play).
2. The DualSense's mute button turns a player's pad speaker on or off during a race (the mute LED shows it).

## What goes where (decide before building)
- **A. Per car (proposed).** A player's own car sounds → their pad; every other sound (other cars, traffic, world
  ambience, music, menus, speech) → the PC. Players without a Bluetooth DualSense, or with it switched off by the mute
  button, keep everything on the PC. Simple to explain, and the PC mix still carries the shared world.
- **B. Per listener (later option).** Every 3D sound → the pad of the player whose listener it is bound to (nearest
  camera, `sound+0xD0`), so each pad hears "the world from my camera"; the PC keeps only music, menus and speech. More
  immersive but needs every 3D voice attributed (same work as A plus world sounds), and a 3P/4P PC would go quiet.
- In both: a routed sound is removed from the PC mix (that's how the engine works now: no doubling).

## Found so far (2026-10-26, 3P AI race, `tools/car_sounds.py`)
- Each car has ~29 sound objects, all with +0x44 = the car's audio node (audio player list `[[0x82B884]+0xAC]+0x10`,
  next +4; node +0x10 = player index). Vtables (one object per car each): 0x789EE0, 0x789F04, 0x789F28, 0x789F70,
  0x789FB8, 0x789FE4, 0x78A008, 0x78A064, 0x78A1F0, 0x78A228, 0x78AA04, 0x78AA3C, 0x78AAC0, 0x78AB68, 0x78ABA0,
  0x78AC10, 0x78AC58, 0x78ACB0, 0x78ACF8, 0x78ADB8, 0x78AE00, 0x78AE48, 0x78AF30 (engine), 0x78AF88, 0x78AFD0,
  0x78B028, 0x78B4E8, 0x78B5C0; plus 0x78BF88 (node list itself) and one 0x78A3B0. Which class is which sound is
  not known yet (names: the vtables' RTTI / the classes' own update methods).
- Several of them (0x78AA04, 0x78AA3C, 0x78AAC0, 0x78AB68, 0x78A1F0, 0x78A228) are also the 3D sounds the listener
  choice (`NearestListener`, 0x45E100 virtual) sees.
- Only the engine (0x78AF30 +0xE0, and its sub-object 0x78ABA0 +0x1E0) stores an EA channel handle. The other voices
  (20-30 playing in a race) are EA sample players in a pool (0x0C00xxxx; +0x10 channel handle, +8/+0xC bank data)
  with no reference back to any game object.
- A hardware write watch on four channels' handle field (+0) showed every allocation on the sound thread:
  0x720BB7 → 0x72438E → 0x729BF7 → 0x729773 → 0x7252DF (writes the new handle), no game code on the stack. So the
  game queues commands and EA runs them later at 0x720BB7; the game ↔ voice link has to be made when the command is
  queued (main thread, game object on the stack) and carried to when it runs.
- The rev limiter: user reports it "seems separate and not playing now". Unknown whether it plays on the PC, the pad
  or nowhere since the engine routing; check with the mute toggle (step 1) by A/B in one race.

## Approach
1. **Mute-button toggle** (independent, small; also gives an instant A/B for every later step).
   - Edge-detect DsExtra.mute in `XPadUpdate` like the touchpad click (`XPadMutePressed(pad)`).
   - sound.cpp: per seat `g_speakerOn[k]` (default on = `[AUDIO] PadSpeakers`), toggled by the player's pad;
     `SpeakerPlayerOfVoice` / `RenderChunk` skip players with it off (their sounds go back to the PC at once).
   - Mute LED (output common block byte 8 `mute_button_led`, valid_flag1 bit 0x01): on while the pad speaker is off,
     like a muted mic. Add a `muteLed` field to `DsOutput` and `XPadSetMuteLed(pad, on)`.
   - Optional: remember it per player in the players file (`[PlayerN] PadSpeaker`, like AdaptiveTriggers).
   - Test: 2P race, press mute: engine moves pad → PC and back; LED follows. Listen for the limiter both ways.
2. **Rev limiter**: with the toggle, compare on/off. If it plays with the speaker off but not on, it's a voice the
   engine routing drops (log the engine object's voices at the limiter: its 0x78ABA0 sub-object, `car_sounds.py
   --watch` while revving at the limiter) and fix it before step 3.
3. **Find the command queue** (reverse engineering, main thread side):
   - 0x720BB7's caller loop reads commands; find the queue (ring buffer) and the command layout (opcode, arguments,
     a request id or handle). The EA API functions the game calls (0x723E9D, 0x72412A, 0x723D9B, 0x72403E, 0x722DB5,
     0x722ECE ...) either act at once or queue; find the one that starts a sample (look at what queues an opcode
     whose handler reaches 0x72438E / 0x729BF7).
   - Hook that queueing function: on the main thread, take the car sound object from the stack (`CarSoundPlayer`
     check: vtable in 0x789000..0x78C000 and +0x44 is an audio node) and remember request → player; on the sound
     thread, when 0x7252DF's caller allocates the channel for that request, store channel handle → player
     (`g_engineVoicePlayer` generalised: per voice, valid while the channel's handle (+0) is unchanged).
   - Tools: `hwwatch.py` on the queue's write pointer, `ea_voices.py`, `car_sounds.py`, the Debug voice log
     (`ea voice N <- car sound vtable ... player k`).
4. **Route by class**: a table of the car sound vtables that go to the pad (start with all of a car's classes;
   drop any that sound wrong, e.g. a sound meant for the whole room). Reuse the mixer hooks as they are: they already
   route any voice `SpeakerPlayerOfVoice` names.
5. Docs: sound.md (voice owners, the queue), input.md (mute toggle), README (feature + `PadSpeakers` meaning).

## Files
- `src/parts/sound.cpp` (routing, attribution hooks), `src/XInputDevices.cpp/.h` (mute edge, mute LED),
  `src/DualSense.cpp/.h` (mute LED in the output report), `src/parts/pad_lights.cpp` (if the LED is driven there),
  `src/NFSU2SplitScreen.ini`, docs, `tools/car_sounds.py` (commit with step 3).

## Test steps
- Each step: `dev.sh deploy`, 2P race on real pads (P1 DualSense over Bluetooth, P2 Xbox), the user listens; the
  Debug log shows the routing (`pad speaker:` lines, 5 s speaker stats).
- Step 3/4: skid, hit a wall, use nitrous, hold the limiter: each should come from P1's pad only; P2's from the PC;
  mute toggles all of P1's back to the PC.

## Done when
- P1's own car sounds play only on P1's pad, P2's (no speaker) only on the PC; mute moves them back and forth with
  the LED showing it; no dropouts beyond the current rare Bluetooth ones; 2P and 4P race without a crash.
