"""Print the race manager's racer list during an Outrun (or any race), live, every --every s, for --secs s.
python -I outrun_watch.py [--secs 60] [--every 0.5]
Race manager [0x890118]: +0x14 race time, +0x24 racer count, +0x28 finished count, +0xBD0 racer* [count]
(the order the outrun logic 0x607CA9.. walks). Per racer: +9 byte, +0xC byte, +0x10 word state, +0x24 float gap,
[+0]+0x18 -> +0x10 byte (the index the split Outrun winner byte 0x83ABCC copies). Prints a line when anything but the
gap changes, and the gaps every --every s. Also the split Outrun points 0x83ABC8/9 and winner 0x83ABCC."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

T0 = time.time()
def say(msg): print(f'{time.strftime("%H:%M:%S")} +{time.time() - T0:5.1f}s {msg}', flush=True)
def opt(name, default): return type(default)(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default

g = t.Game()
secs, every = opt('--secs', 60.0), opt('--every', 0.5)
last_key, last_gap = None, 0
end = time.time() + secs
while time.time() < end:
    mgr = g.u32(0x890118)
    if not mgr: time.sleep(0.1); continue
    n = g.i32(mgr + 0x24)
    if not 0 < n <= 8: time.sleep(0.1); continue
    rows, gaps = [], []
    for i in range(n):
        e = g.u32(mgr + 0xBD0 + 4 * i)
        if not e: rows.append('-'); gaps.append('-'); continue
        b9, bc = struct.unpack('<bb', g.read(e + 9, 1) + g.read(e + 0xC, 1))
        state = struct.unpack('<h', g.read(e + 0x10, 2))[0]
        gap = struct.unpack('<f', g.read(e + 0x24, 4))[0]
        p = g.u32(e); q = g.u32(p + 0x18) if p else 0
        idx = g.u8(q + 0x10) if q else -1
        rows.append(f'{e:08x}:i{idx} b9={b9} c={bc} st={state}')
        gaps.append(f'{gap:.2f}')
    key = (n, g.i32(mgr + 0x28), tuple(rows), g.u8(0x83ABC8), g.u8(0x83ABC9), g.u8(0x83ABCC), g.u32(0x89E7B0))
    if key != last_key:
        say(f'n={n} fin={key[1]} type={key[6]} pts={key[3]}/{key[4]} win={key[5]} | ' + ' | '.join(rows))
        last_key = key
    if time.time() - last_gap >= every:
        say(f't={g.i32(mgr + 0x14)} gaps ' + ' '.join(gaps)); last_gap = time.time()
    time.sleep(0.05)
say('done')
