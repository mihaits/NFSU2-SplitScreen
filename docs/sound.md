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
