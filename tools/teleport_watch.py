"""Catches cars that jump: samples every race car as fast as it can and prints each move the car's velocity can't explain.
python -I teleport_watch.py [seconds=60] [--min 0.5] [--csv path] [--grid]
Each sample reads position, velocity, mover mode (car +0x28) and the fall flag (car +0x96C) of every car in the World
list. A jump is |moved - velocity * dt| > --min metres between two samples. Lines print live: stage changes, the first
position of every car when the race starts (--grid: also its spot once per stage), jumps with the car's state on both
sides. Traffic cars (racer spec type 3, as traffic_watch.py) that jump are the game recycling them elsewhere: they
print as RECYCLED and count apart, so JUMP lines are racers only. Every sample goes to the csv. A car whose memory can't be read (a traffic car freed while sampled) is skipped and
tracked afresh when it comes back. Read-only."""
import csv, os, struct, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

STAGES = {0: 'none', 1: 'flyby', 2: 'prerace', 3: 'countdown', 4: 'racing'}
T0 = time.time()
def say(msg):
    print(f'{time.strftime("%H:%M:%S")} +{time.time() - T0:6.2f}s {msg}', flush=True)

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

g = t.Game()
args = [a for a in sys.argv[1:] if not a.startswith('--')]
seconds = float(args[0]) if args and args[0].replace('.', '').isdigit() else 60
limit = float(opt('--min', 0.5)); grid = '--grid' in sys.argv
path = opt('--csv', os.path.join(tempfile.gettempdir(), 'nfsu2_teleport.csv'))

def stage():
    mgr = g.u32(0x890118)
    return g.u32(mgr) if mgr and g.u32(0x8654A4) == 6 else -1

def cars():
    """(label, car) of every car in the World list, by list index: car +0x18's index isn't stable within a frame."""
    world = g.u32(0x890080)
    if not world: return []
    out = []
    for i in range(min(g.u32(world + 0x10), 16)):
        car = g.u32(world + 0x1C + 4 * i)
        try:
            if car and g.u32(car + 0x14): out.append((f'c{i}', car))
        except OSError:
            pass
    return out

GONE = 'gone'
def state(car):
    """The car's state; None while it has no body; GONE when its memory can't be read (freed after the list read)."""
    try:
        mover = g.u32(car + 0x34); body = g.u32(mover + 0x20) if mover else 0
        if not body: return None
        x, y, z = struct.unpack('<3f', g.read(body + 0x20, 12))
        vx, vy, vz = struct.unpack('<3f', g.read(body + 0xA0, 12))
        return dict(x=x, y=y, z=z, vx=vx, vy=vy, vz=vz, mode=g.u32(car + 0x28), fall=g.u32(car + 0x96C),
                    ctl=g.u32(car + 0x24), traffic=g.u32(g.u32(car + 0x14) + 4) == 3)
    except OSError:
        return GONE

def fmt(s):
    return (f'({s["x"]:8.2f} {s["y"]:8.2f} {s["z"]:6.2f}) v {(s["vx"] ** 2 + s["vy"] ** 2 + s["vz"] ** 2) ** 0.5:5.1f} '
            f'mode {s["mode"]} fall {s["fall"]} ctl {s["ctl"]}')

f = open(path, 'w', newline=''); w = csv.writer(f)
w.writerow('t stage car x y z vx vy vz mode fall ctl'.split())
say(f'watching {seconds:.0f} s, jumps over {limit} m, csv {path}')
end = time.time() + seconds
last = {}; last_stage = None; samples = 0; started = time.time(); gaps = 0.0; jumps = 0; recycled = 0; shown = set()
while time.time() < end:
    s = stage(); now = time.time()
    if s != last_stage:
        say(f'stage {STAGES.get(s, "not in race" if s == -1 else s)}'); last_stage = s; shown = set()
    if s == -1:
        last = {}; time.sleep(0.05); continue
    for label, car in cars():
        st = state(car)
        if st is GONE: last.pop(label, None)
        if not isinstance(st, dict): continue
        w.writerow([f'{now - T0:.4f}', s, label] + [f'{st[k]:.3f}' for k in 'x y z vx vy vz'.split()] + [st['mode'], st['fall'], st['ctl']])
        if label not in shown and (grid or s == 4):
            shown.add(label); say(f'  {label} at {fmt(st)}')
        if label in last and (st['x'], st['y'], st['z']) == tuple(last[label][1][p] for p in 'xyz'):
            continue                       # between physics ticks: dt runs from the sample where the car last moved
        if label in last:
            t0, a = last[label]; dt = max(now - t0, 1 / 60); gaps = max(gaps, dt)
            # both ends' velocity: a car braking hard or hit in between stays inside the average
            ex, ey, ez = [(st[p] - a[p]) - (st['v' + p] + a['v' + p]) / 2 * dt for p in 'xyz']
            miss = (ex * ex + ey * ey + ez * ez) ** 0.5
            if miss > limit and st['traffic']:
                recycled += 1
                say(f'RECYCLED {label} (traffic) moved {miss:.0f} m')
            elif miss > limit:
                jumps += 1
                moved = ((st['x'] - a['x']) ** 2 + (st['y'] - a['y']) ** 2 + (st['z'] - a['z']) ** 2) ** 0.5
                say(f'JUMP {label} {miss:.2f} m off (moved {moved:.2f} m in {dt * 1000:.0f} ms)\n'
                    f'    before {fmt(a)}\n    after  {fmt(st)}')
        last[label] = (now, st)
    samples += 1
    if samples % 200 == 0: f.flush()
    time.sleep(0.001)
f.close()
say(f'done: {samples} samples ({samples / (time.time() - started):.0f}/s), longest gap {gaps * 1000:.0f} ms, {jumps} jumps, {recycled} traffic recycled')
