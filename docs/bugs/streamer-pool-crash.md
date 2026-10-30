# Crash in the track streamer's pool (0x5DEEE1, 6P circuit)
Status:       open
Report:       6 players (5 native DualSense over Bluetooth, 1 Xbox), circuit, track 4081, no AI. Crashed after a
              few laps (~250 s of racing). The session before (same setup, 5 pad speakers on) had pads freezing and
              was quit by the user; this one had 4 of 5 speakers muted. Built from about 48594fb (0.11.0 + README).
Repro:        not reproduced yet; seen once (2026-10-29 22:45). First crash at this address in the WER archive.
Area:         streaming.md (players 3..6 positions, pool sizes), architecture.md (thread safety)
Known:
- Saved locally (not in the repo) in `_splitscreen_mod/crashes/2026-10-29-streamer-pool/`: the WER minidump
  `speed2.exe.36996.dmp` (pid 0x9084, 23 MB; no pool memory in it), `minidump.txt`, both sessions' logs
  (`session2-crash.log` is the crash), `Report.wer`, the asi and inis that ran.
- Exception c0000005 reading 0x3F7E0010 at 0x5DEEE1 (`mov bl, [ecx+0x10]`), on the **main thread** (28084; the
  log's `pool: free from thread 49656 (main 28084)` line).
- 0x5DEED0(pool) walks a circular list at pool+0x14 (next at node+0, size +0xC, used flag +0x10) and adds the free
  sizes: the "amount free" callback of the memory-pool override the streamer installs at 0x5E4D22 (callbacks
  0x5DEE30 alloc, 0x5DEEB0 free → 0x5DA6D0, 0x5DEED0 amount free, 0x5DEF00 largest free; table at 0x82802C[pool],
  read by 0x43E8E0 / 0x43E910 and by the pool alloc 0x440390 / free 0x440540).
- Registers: ecx 0x3F7E0000 (the bad "next": looks like a float, ~0.992, not a pointer), edx 0x0D456FBC (list head
  = pool 0x0D456FA8 + 0x14), esi 0x88EB00 (the streamer), eax 0 (free sum so far), esp 0x1AFE3C.
- Stack: 0x5DF361 (after `call 0x43E8E0` at 0x5DF35C in the streamer's update) ← 0x5E6B9D ← 0x5E79B1 ← 0x5814C1
  (main loop) ← 0x580E75.
- The log has nothing unusual before it: streaming lines for players 3..6 (140 sections, 48 ranked by players 3..6)
  until 275166500, the crash ~0.3 s later. Track pool 210000 KB, second pool 33000 KB.
Ruled out:
- A reader racing a writer on the sound thread is unlikely as the only cause: the crashing walk is on the main thread,
  and the bad next pointer holds float-like data, so a node's header was overwritten (overflow from the previous
  block, or a block written after it was freed and its header reused) rather than caught mid-relink.
Leads:
- Find who frees / reuses track pool blocks while something still writes floats into them: section data the extra
  positions (players 3..6) keep using after the streamer unloads them, or per-view world updates on unloaded sections.
- Next crash: the plugin now writes `scripts/NFSU2SplitScreen_crash.txt` (memory at each register, so the overwritten
  node and its neighbour show) and a fuller minidump. Check whether the faulting node's bytes look like section data.
- Possible check in Debug: walk the track pool list each frame (0x5DEED0-style, under __try) and log the first frame
  it breaks, with the last streaming change; then watch that header with `hwwatch.py` to catch the writer.
Tools:        `python tools/minidump.py <dmp>`, `python tools/dumpmem.py <dmp> addr..`, `python tools/sdis.py 5DEED0 20`,
              `python tools/callers.py 43E8E0`
