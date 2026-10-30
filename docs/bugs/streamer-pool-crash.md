# Crash in the track streamer's pool (0x5DEEE1, 6P circuit)
Status:       fixed 2026-10-30 (`AddPoolRecords`: 512 records)
Report:       6 players (5 native DualSense over Bluetooth, 1 Xbox), circuit, track 4081, no AI. Crashed after a
              few laps (~250 s of racing). The session before (same setup, 5 pad speakers on) had pads freezing and
              was quit by the user; this one had 4 of 5 speakers muted. Built from about 48594fb (0.11.0 + README).
Repro:        Debug build, `ExtraPlayers = 4`, `TRACK=4081 sh tools/go_race_nointro.sh 0`, then
              `python -I tools/autorace.py --stagger 25`: crashed 111 s into the race with P6 not yet released. Same
              run on 4001 for 300 s: no crash, lowest 23 free records. First seen 2026-10-29 22:45.
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
Reproduced 2026-10-30 (`_splitscreen_mod/crashes/2026-10-30-pool-4081/`), same address and stack as the first crash:
- The `pool:` line 250 ms before it: 8 free records, 138 blocks, 46 gaps, 148 sections wanted, 160 MB unused.
- Memory at the list head (`NFSU2SplitScreen_crash.txt`, pool+0x14): 1FBBFE80 0000F900 0CA64701. That is the sentinel
  used as a record: a block address (0x80-aligned, in the pool) as head, a block size at +0x18, and used = 1 at +0x1C,
  which is the low byte of record 0's next (the odd 0x01).
- ecx 0x413A8BA9 (float, about 11.66), bl 0xD2 (a used byte that is neither 0 nor 1), eax 0.
Cause (found by static analysis, confirmed by the above):
- The pool's block records run out. They are not headers next to the data: the 0xF38-byte pool object (ctor 0x5E4B70)
  holds a fixed array of 0xC0 = 192 records of 0x14 bytes at +0x1C, linked into a free list at +0xC. One record
  describes each used block and each free gap.
- The allocator 0x5DA4A0 takes records at 0x5DA5C1 (the block) and 0x5DA61A (the remaining gap) without checking that
  the free list is empty. With it empty, the "record" is the list's sentinel at pool+0xC, so its +8 (block address)
  lands on pool+0x14, the head of the block list. The list now starts in section data.
- The crash fits: 0x5DEED0 read that block as a node and followed its first dword, 0x3F7E0000. ebx was 1 on entry
  (saved at esp) but 0xC0 at the crash, so the previous "node" had a used byte of 0xC0, which a real record never has
  (0 or 1). eax 0: nothing was summed.
- Blocks follow the sections wanted about 1:1 and gaps grow to about 45, so 150 sections reach the limit. The list at
  streamer +0x298 held 147 sections at the first crash, 148 at the second.
Ruled out:
- A block overrun or use after free writing a node header: the nodes do not sit next to the blocks.
- A race with the sound thread: the walk is on the main thread and the bad value is section data, not a stale link.
Fix: after 0x5E4E70, `CreateTrackStreamingPool` links 320 records from a static array of the plugin into the free
list at pool+0xC ([streamer +0x6A4]). Nothing indexes the array: the walkers follow links, and the dtor 0x5E4D70
empties both lists first, so its unlink loops skip them. The `pool:` Debug lines (`LogPoolRecords`, streaming.cpp)
show the free count. The Repro run with the fix: no crash in 420 s, P1 and P2 finished, peak 236 of 512 records.
Leads if it comes back:
- Next crash: the plugin now writes `scripts/NFSU2SplitScreen_crash.txt` (memory at each register, so the overwritten
  node and its neighbour show) and a fuller minidump. Check whether the faulting node's bytes look like section data.
- Possible check in Debug: walk the track pool list each frame (0x5DEED0-style, under __try) and log the first frame
  it breaks, with the last streaming change; then watch that header with `hwwatch.py` to catch the writer.
Tools:        `python tools/minidump.py <dmp>`, `python tools/dumpmem.py <dmp> addr..`, `python tools/sdis.py 5DEED0 20`,
              `python tools/callers.py 43E8E0`
