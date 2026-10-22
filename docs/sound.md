# Sound

- **Listeners.** The audio mixes each sound against one of 2 listeners. Their state is 14 two-entry arrays (0x82BE70,
  0x82BE80, 0x82BECC, 0x82BEDC, 0x82BEF0, 0x82BF10, 0x82BF20, 0x82BF34, 0x82C610, 0x82C670, 0x82C6C4, 0x82BB40,
  0x82BB48, 0x82BB50), written by the listener update 0x46D7E0 (call 0x483CB3) from the audio player list
  ([[0x82B884]+0xAC]). Each sound picks one (sound+0xD0) through the nearest-of-2 choice at 0x45E147.
- The arrays are relocated to 4 entries (`RelocateListenerArrays`, 145 refs). `ListenerUpdate` runs the update a second
  time with P3/P4's nodes posing as 0/1, and `ListenerChoiceHook` / `NearestListener` pick from all four.
- **Intro after a track change.** Sounds created during the load/intro bind to the nearest listener and keep it, and
  listeners 1-3 still had the previous track's positions. From the race load until a player's camera moves, that
  player's listener copies listener 0 and is skipped by NearestListener ("parked").
- Engine sounds use their own path (0x462D51); it's left as is, and every player hears their own engine.
