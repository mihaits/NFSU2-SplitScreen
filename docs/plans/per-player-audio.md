# Plan: per-player audio on the DualSense speakers

## Goal
Each local player hears their own car and world sounds on their pad's speaker (DualSense over Bluetooth). Shared
sounds (music, menus, announcer) stay on the PC output. Pads without a speaker keep the PC mix.

## Found so far
- **Pad side works** (`tools/ds_bt_speaker.py`): Opus frames in HID output report 0x39 (547 bytes, two 480-frame
  packets of 200 bytes, 48 kHz stereo CBR 160 kbit/s, one report per 21.333 ms; the pad plays 45 kHz, so resample to
  45 kHz and label it 48 kHz), after a 0x31 speaker route. Layout in the script's docstring.
- **Bandwidth** (one pad measured): input 494 reports/s (78 bytes); audio costs ~12-15 % of the adapter's air time
  per pad and the input rate gives way (494 → 418/s). Estimate: 4 pads fine, 6 marginal on one adapter.
- **Game side**: one software mix into one DirectSound stream; the EA mixer and its voice loop are in
  `docs/sound.md` (0x7292BC, 64 voices at [0x8B4A00], stride 0x5C, mix-add 0x72C3E0).

## Open
1. Map each mixer voice to its game sound and so its listener/player (sound+0xD0). Ideas: find the EA call that sets
   a voice's volume/pan from the values 0x45DF00 computes (+8 volume, +0x10/+0xC azimuth), or follow who writes voice
   +0x1C (the channel gains) back to the game object; engine sounds take their own path (0x462D51).
2. Mixer split: a hook on 0x7292BC that, after the normal render, renders each speaker player's voices again into
   their own buffers (or skips them in the PC mix). Voice sources are stateful (`call [src]` advances the stream),
   so a voice must be rendered once and routed, not rendered twice: replace the mix-add per voice instead.
3. Per pad: resample 44.1 kHz → 45 kHz, libopus (new dependency, accepted), sender thread at 21.333 ms per pad
   (`DualSense.cpp` already opens the pads over HID).
4. Ini option and doc; which sounds count as a player's (own engine, own collisions; world sounds by listener).

## Test
Each step: `dev.sh deploy`, 2P race on real pads, user listens on each pad. Done when each pad plays only its own
player's car with no stutter in a 2P and a 4P race.
