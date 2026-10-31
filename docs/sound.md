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
- **Voice owners.** EA channel i is mixer voice i, and an EA channel handle's low byte is i (0x725480 checks the full
  handle against channel +0: the upper bits are a generation). Channel i's gains are its volume
  ([0x8B466C] + i*0x90 + 0x48) times its pan row ([0x8B4724] + i*0x88), applied by 0x724F5D(i) through 0x72905D(i, ch,
  gain); 0x729087 sets the aux sends. `InstallEaVoiceLog` (Debug = 1) hooks 0x724F5D and logs each (voice, owner) once.
  - **Engines**: one object per car (vtable 0x78AF30; per-frame method 0x474310 → 0x473E60, which sets volume
    0x72412A, pitch 0x723D9B and more on its handle). +0xE0 is the engine's EA channel handle; +0x9C is the car's
    node in the audio player list ([[0x82B884]+0xAC]+0x10, next +4): node +0x10 is the player index, +0x30 → +0x18
    the player. So engine voice = [obj+0xE0] & 0xFF, its player = [[obj+0x9C]+0x10] (checked live, 3 players).
  - **Everything else** (world, collisions, skids, ambience): the handles are only in EA's sample-player pool
    (0x0C00xxxx in a race; object +0x10 channel handle, +8/+0xC bank data, list at [0x8B446C]); no game object
    holds them. Their gains are applied from game code at 0x46C13F, 0x46C2DE, 0x46C841, 0x483E0D and from the
    sound thread. A car's sounds are mostly EA patches whose children are such sample players: found through the
    patch, below ("Which voice is whose"). World sounds (other cars' 3D sounds, ambience) aren't attributed.
  - EA "players" ([0x8B4068 + 4k], count byte 0x8B44DE; 0x722424(k)) are the stream players: music, speech. Their
    events are listed at player +0x144 ({+0 next, +0xC handle}, 0x727A12); player 0's event owns the music voice.

## Car sounds on the pad speakers (`[AUDIO] PadSpeakers`, `PadSpeakerGain`, `PadSpeakerSounds`)
In split racing each player's own car sounds play on the speaker of their DualSense (Bluetooth or USB) instead of the PC mix;
every other sound stays on the PC, and a player on another pad keeps their car there. Code: the "engines on the pad
speakers" block of `parts/sound.cpp`; the pad side is `DsSpeakerWrite` in `DualSense.cpp` (docs/input.md).
- **Which voice is whose** (main thread; voice → local player + 1 in `g_engineVoicePlayer`, with the channel handle
  and a time stamp). A voice is routed while its stamp is under 500 ms old, its channel still has that handle (EA
  reuses voices) and its player's pad (`g_assignedPad`, split racing only) is a native DualSense that took the last
  samples and isn't muted.
  - Engine master: the engine object's per-frame update (vtable 0x78AF30 +0x48 = 0x474310, hooked in the vtable)
    records voice = [obj+0xE0] & 0xFF → player [[[obj+0x9C]+0x30]+0x18] matched in `g_localPlayers`. Always routed.
  - Patches: the master alone is a small part of the engine; the audible layers, and turbo, shifts, nitrous, skids
    ..., are EA patches. A patch runs a program compiled into the sound bank that starts its own sample-player voices
    ("children"), which no game object holds. The game's per-frame sound object loop (0x469CC0:
    `mov eax, [esi]; mov ecx, esi; call [eax+0x28]`, next object at +0x50) is patched to call `SoundObjectUpdate`,
    which runs the update and collects the objects of one pass. When the loop starts over, every per-car object
    (vtable 0x789000..0x78C000, +0x44 = the car's audio node → player as above) is scanned from +0x40 up to the next
    object (sorted by address, at most 0x800) for patch fields, each frame (patches like nitrous are created when
    first used). A patch field holds a param struct whose [0] is the patch player {definition, ?, +8 block};
    definition [0] = program header (+8 = 0x721096 in every one seen), definition +4 = the patch's name; header
    +0x18 low byte = number of children, +0x24 instance size, +0x30 + 4i = child i's offset from the instance
    (block - 0x2C); a child is a sample player {+0/+4 bank, +8 channel handle, +0xC flags}. Each playing child of a
    patch named in `PadSpeakerSounds` (case-insensitive, comma list) is recorded for the car's player.
  - Patch names per car (3P race): CAR (engine layers, 8 children, engine object +0xB0), CAR_SWTN, CAR_Sputter
    (limiter), FX_TURBO_01, FX_SHIFTING_01, FX_NITROUS, FX_PURGE, FX_SKID, FX_Scrape, AEMS_StichCollision,
    AEMS_StichWoosh, FX_ROADNOISE(_TRANS), FX_WIND, FX_WIND_Weather, FX_Weather (rain), FX_Camera, FX_TRAFFIC,
    ENV_STATIC, PlayCommonSample. The default list keeps feedback about the player's own car (short, mid/high
    sounds a small speaker carries) on the pad; rain, wind and road noise (shared, broadband or low rumble) on the PC.
  - A new sound can play on the PC for up to a frame before it's claimed: EA starts it on the sound thread.
  - `tools/unclaimed_voices.py` lists playing voices that follow one car but aren't routed.
- **Splitting the mix** (sound thread). Every place 0x7292BC puts a voice into the output channels or aux sends is
  patched; a routed voice goes into its player's own two float buffers (same chunk offsets, 16-bit scale) instead,
  without reverb:
  - main loop: `call [0x8B4A04]` at 0x72942B (channels; esi = voice, edi = voice +0x1C + ch*4) and 0x729490 (aux);
  - after a gain change (voice +1 set by 0x72905D, i.e. every game frame for an engine) 0x728B48 first renders the
    voice's next 16 frames itself, ramping old → new gains with 0x728957(old, new, src, dst) at 0x728C9D (channels)
    / 0x728C11 (aux), or plain `call [0x8B4A04]` at 0x728CCB / 0x728C4B. Missing these left a 16-sample hole in the
    pad stream about every 10 ms (constant crackle) and leaked those samples to the PC.
  - The stubs keep ecx and edx: EA's code reads edx after the aux ramp (0x728C16) and crashed in the intro when a C
    function changed it.
- **Mute button.** A player's mute button toggles their pad speaker during split races (`g_speakerOn[k]`, per seat
  until the game closes); off, the player counts as having no speaker pad, so their car goes back to
  the PC mix at once and the pad stream stops after 1 s. The mute LED is lit while it's off.
- **Off with more than 2 Bluetooth speaker players (DualSense and DS4; USB pads don't count).** Each DualSense stream is ~200 kbit/s of Bluetooth output (47 reports of 547
  bytes a second), a DS4 stream ~230 kbit/s (62.5 reports of 462 bytes). With 5 DualSense pads at once on one adapter (+ an Xbox pad) the writes ran up to 100 ms late, failed with error
  31 now and then, and pads' input froze for seconds without being reported lost. Race setup (`CreateHumanRacers` in
  racers.cpp → `PadSpeakerDefaults`) sets every seat off when more than `kSpeakerDualSenseMax` (2) seats have a
  Bluetooth DualSense, on otherwise; only when that count changes, so the mute button toggles stay for the next races. A
  temporary fix until the bitrate adapts (below).
- **To do: adaptive bitrate** (needs planning before code). It has to cover both pad types, since their streams share
  one adapter. The DualSense's Opus bitrate is fixed at 160 kbit/s (`OPUS_SET_BITRATE` in DualSense.cpp, `kSpkOpusBytes`);
  the DS4's SBC at 216 kbit/s (bitpool 48 in `SbcEncoder.h`, 4 frames in report 0x17). A lower bitpool still needs the
  pad to accept smaller frames, and how much air time it saves depends on the report size. The console does this itself:
  psdevwiki says a PS4 streams 32 kHz to 1-2 pads and 16 kHz to 3 or more, and its sample 0x14 report has SBC header `9c 75 19`
  (32 kHz, 16 blocks, dual channel, bitpool 25), far below our bitpool 48, so low-bitpool frames should play. Ideas to discuss:
  - Pick it from the number of pads streaming, of either type (160 kbit/s for 1-2, lower for 3-6).
  - Lower it per pad when its writes run late or fail (error 31), and raise it again once they recover; the
    speaker thread already measures how late each write is.
  - Open questions: how to send less (smaller Opus packets in the same 47 reports/s, or fewer reports), the lowest
    bitrate that still sounds acceptable on the pad's speaker, and whether the input stalls seen with 5 pads came
    from the output load at all (they also happened at the menu with no speakers). Once it works, drop the
    more-than-2 default.
- **Sending.** 0x7292BC is wrapped at its call 0x7295DD (`RenderChunk`): the player buffers are cleared, the chunk is
  rendered, then (L + R) / 2 × PadSpeakerGain / 32768 goes to `XPadSpeakerWrite` at the mixer rate (word 0x8B44C6,
  44100).
- **Debug options** ([DEBUG]): `SpeakerTestPad = n` gives player 1 pad n's speaker in split races (automated
  tests); with Debug = 1, `SpeakerDumpSeconds = s` writes s seconds of float32 frames {PC left, PC right, player 1's
  pad} (16-bit scale) to `scripts/NFSU2SplitScreen_speaker.raw`, to check what leaks to the PC.
- **Timing.** The game fills its DirectSound buffer in bursts of up to ~65 ms, so the pad's queue is a sawtooth; the
  pad side holds the queue's lowest point after each report near ~17 ms (see input.md).

**Voice requests.** Removing a positional sound source (0x483E40 -> 0x481E50) leaves the request count at +0xC of the list, so
the arbitration could walk an empty list at 0x481AB4 with every voice in use. `RemoveSoundSource` (call at 0x5A9584) recounts it.
Not reproduced here; the bug was found by nfsu2-web.
