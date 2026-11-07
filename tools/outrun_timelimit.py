"""Split Outrun time-limit check, on a split Outrun round with Time Limit = 2 min set before the round loaded (poke
0x83ABC0 = 1 during the loading screen after go_outrun.sh; switched on mid-round, the 2P HUD crashed in its text copy
0x511648, so the script only checks it). Moves the round's start time (race manager +0x10; race time +0x14 = now - start, 4000 per second) to
--drive + 2 s before the limit, lets P1 drive on autopilot for --drive s so the time in the lead differs, and follows
the finish. Passes when every racer finished with places 1..n, one winner (state 6), and the places follow the seconds
in the lead the plugin logs at the limit ("outrun: time limit, seconds in the lead: P1 1.25 ..."), most first.
Prints a line per stage; exits 1 on the first failure.  python -I outrun_timelimit.py [--drive 4]"""
import os, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

T0 = time.time()
def say(msg): print(f'{time.strftime("%H:%M:%S")} +{time.time() - T0:5.1f}s {msg}', flush=True)
def fail(msg): say('FAIL: ' + msg); sys.exit(1)
drive = float(sys.argv[sys.argv.index('--drive') + 1]) if '--drive' in sys.argv else 4.0

g = t.Game(); g.focus()
end = time.time() + 30
while time.time() < end:
    m = g.u32(0x890118)
    if m and g.u32(m) == 4 and g.u32(0x89E7B0) == 7: break
    time.sleep(0.1)
else: fail('no Outrun round racing')
if g.u32(0x83ABC0) != 1: fail('Time Limit is not 2 min: poke 0x83ABC0 = 1 during the loading screen')
say('racing, time limit 2 min')

def racers():
    m = g.u32(0x890118); out = []
    for k in range(g.i32(m + 0x24)):
        e = g.u32(m + 0xBD0 + 4 * k); car = g.u32(e); info = g.u32(car + 0x18)
        out.append(dict(p=g.u8(info + 0x10) + 1, place=struct.unpack('<b', g.read(e + 0xC, 1))[0],
                        state=struct.unpack('<h', g.read(e + 0x10, 2))[0]))
    return out

import paths, re
logsize = os.path.getsize(paths.LOG)
m = g.u32(0x890118)
g.w32(m + 0x10, g.i32(m + 0x10) - (2 * 60 * 4000 - g.i32(m + 0x14) - int((drive + 2) * 4000)))
say('race time moved to %.1f s, limit in %.0f s' % (g.i32(m + 0x14) / 4000, drive + 2))
p = subprocess.Popen([sys.executable, '-I', 'autorace.py', '--players', '1', '--timeout', str(drive), '--stall', '100'],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=os.path.dirname(os.path.abspath(__file__)))
p.wait()
say('P1 drove %.0f s' % drive)
if any(r['state'] for r in racers()) and g.u32(m + 0x14) < 2 * 60 * 4000: fail('a racer finished before the limit (300 m): use a shorter --drive')
end = time.time() + 8
while time.time() < end:
    rs = racers()
    if g.i32(m + 0x28) >= len(rs): break
    time.sleep(0.05)
else: fail('not every racer finished within 8 s: %s' % racers())
time.sleep(0.3)
rs = racers()
say('finished: ' + ' | '.join('P%d place %d state %d' % (r['p'], r['place'], r['state']) for r in rs))
with open(paths.LOG, 'rb') as f: f.seek(logsize); new = f.read().decode('latin1')
lines = re.findall(r'outrun: time limit, seconds in the lead:(.*)', new)
if not lines: fail('no time limit line in the log')
lead = {int(a): float(b) for a, b in re.findall(r'P(\d) ([\d.]+)', lines[-1])}
say('seconds in the lead:' + lines[-1])
places = sorted(r['place'] for r in rs)
if places != list(range(1, len(rs) + 1)): fail('places %s' % places)
byplace = sorted(rs, key=lambda r: r['place'])
if any(lead[byplace[i]['p']] < lead[byplace[i + 1]['p']] for i in range(len(rs) - 1)): fail("places don't follow the lead time")
if [r['state'] for r in byplace] != [6] + [7] * (len(rs) - 1): fail('states %s' % [r['state'] for r in byplace])
say('ok: places follow the time in the lead')
