# Sound

- **Listeners.** The audio mixes each sound against one of 2 listeners. Their state is 14 two-entry arrays (0x82BE70,
  0x82BE80, 0x82BECC, 0x82BEDC, 0x82BEF0, 0x82BF10, 0x82BF20, 0x82BF34, 0x82C610, 0x82C670, 0x82C6C4, 0x82BB40,
  0x82BB48, 0x82BB50), written by the listener update 0x46D7E0 (call 0x483CB3) from the audio player list
  ([[0x82B884]+0xAC]). Each sound picks one (sound+0xD0) through the nearest-of-2 choice at 0x45E147.
- The arrays are relocated to `MAX_LOCAL_PLAYERS` (6) entries (`RelocateListenerArrays`, 145 refs). `ListenerUpdate`
  runs the update once more per pair, with P3/P4's (then P5/P6's) nodes posing as 0/1, and `ListenerChoiceHook` /
  `NearestListener` pick from all of them.
- **Intro after a track change.** Sounds created during the load/intro bind to the nearest listener and keep it, and
  listeners 1-3 still had the previous track's positions. From the race load until a player's camera moves, that
  player's listener copies listener 0 and is skipped by NearestListener ("parked").
- Engine sounds use their own path (0x462D51): listener 0 if the nearest player car (0x459AC0, `NearestPlayer`) is
  P1's, else 1. It's left as is, and every player hears their own engine. In single-view screens (post-race scene,
  results) only listeners 0/1 are updated and chosen; listener 1 takes P2's camera (player +0x238 → +0x1C).
- **Audio memory.** Sound banks load into the audio pool ([0x82C6D4], created at boot by 0x4859B0: 4 MB with sound
  on). The race's bank jobs are a 32-entry array at 0x82B098 (0x2C each: +8 file, +0xC folder, +0x14 file buffer,
  +0x18 bank memory, +0x1C size, +0x20/+0x24 completion callback, +0x28 status/id), built by 0x4869A0 and loaded in
  order; the track loader's callback (step 3, 0x486EA0 → 0x486C10) rides on the last job. Engine banks come from
  0x457A60, which refuses a bank bigger than the largest free block − 1 KB; the EA bank load (0x7238D2) then returns
  −6 and the job never completes, so the race load stays in game state 4 (no error). Every different player car adds
  an engine bank (~115 KB, `sound\Engine\CAR_nn_ENG_MB_SPU.abk`): with 6 different cars the 5th did not fit.
  `EnlargeAudioPool` makes the pool 6 MB (imm32 at 0x4859BA).
- **DirectSound output.** The EA engine mixes every voice itself into one streaming buffer: `DirectSoundCreate` (IAT
  0x78302C, thunk 0x770D16) on the default device, a primary buffer, probe buffers of 1/2/4/6 channels at boot, then
  one 2-channel 44.1 kHz 16-bit buffer of 88192 bytes (0.5 s, flags 0x18000) that it locks about 100 times a second;
  the device is recreated on the way into a race. No per-voice buffers exist, so per-player output (e.g. a DualSense
  speaker each, see `tools/ds_bt_speaker.py`) needs the mix split inside the EA mixer. `InstallDsoundLog` (Debug = 1)
  logs the buffers and a 5 s summary of Play / Lock calls ("dsound:" lines).
- **EA mixer.** The sound thread (winmm timer, 0x7263C1) fills the stream at 0x726200: Lock (call 0x726268), then
  0x725C8E(&ptr, frames), then Unlock. 0x725C8E works in 10 ms blocks: when its block count [0x8B478C] runs out it
  runs the voice tick 0x721E13 (100 Hz voice state) and 0x721DE6 (callbacks), then 0x729574(&ptr, frames) renders and
  converts in chunks of up to 256 frames: one planar float buffer per output channel ([0x8B48F0 + ch*4], channel
  count byte 0x8B47F9, 2 here), rendered by **0x7292BC(channel buffers, frames)**, then interleaved to 16 bit (2/4/6
  channel orders). 0x7292BC clears the channel buffers (0x727030), then for each of the 64 voices (byte 0x8B47F8;
  array [0x8B4A00], stride 0x5C, byte +0 == 2 playing) calls the voice's source (+0x40, `call [src]`) to render mono
  floats into the scratch buffer [0x8B48D4], and mix-adds them into each channel with gain +0x1C + ch*4 through
  [0x8B4A04] (0x72C3E0: scratch, gain, frames, destination), and into the aux sends (count [0x8180B8], gains +0x38,
  buses [0x8B47C4]) the same way. Then the optional post hook [0x8B4800] and the per-channel output stages
  [0x8B4940 + ch*4]. A per-player mix is this voice loop run once per player over that player's voices.
- Which player owns a mixer voice isn't known yet. The game's 3D sound objects (listener index +0xD0) compute volume
  (+8) and azimuth (+0x10/+0xC) against their listener in 0x45DF00 and pass them to EA through an API not traced yet.
