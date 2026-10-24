"""Live traffic around each local player, a line every 2 s.
python -I traffic_watch.py [sample_s=0.5] [seconds=0 (forever)] [--radius 250]
Per player: traffic cars within the radius (xy, as the game's keep/spawn checks), the nearest one, and how far the
player is from P1. Every sample also checks for teleports (a traffic car moved more than 80 since the last sample: the
game recycled it) and prints those that land within the radius of a player ("pop-in": spawned in that player's view).
Traffic = race cars whose racer spec type is 3 (World [0x890080]: count +0x10, cars +0x1C). At the end: teleports,
pop-ins per player and the average count near each player. Reads only; works with any plugin build (plugin.py)."""
import math, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin

g = t.Game()
players = plugin.LocalPlayers(g)
f32 = lambda a: struct.unpack('<f', g.read(a, 4))[0]
args = [a for a in sys.argv[1:] if not a.startswith('--')]
radius = float(sys.argv[sys.argv.index('--radius') + 1]) if '--radius' in sys.argv else 250.0
if '--radius' in sys.argv: args.remove(sys.argv[sys.argv.index('--radius') + 1])
interval = float(args[0]) if args else 0.5
print_every = 2.0
end = time.time() + float(args[1]) if len(args) > 1 and float(args[1]) > 0 else float('inf')

def xy(car): return f32(car + 0x60), f32(car + 0x64)

sums, samples = {}, 0
last_pos, teleports, popins, next_print = {}, 0, {}, 0.0
while time.time() < end:
    try:
        world = g.u32(0x890080); mgr = g.u32(0x890118)
        if not world or not mgr or g.u32(mgr) != 4:
            print(time.strftime('%H:%M:%S'), 'not racing', flush=True); time.sleep(interval); continue
        n = g.u32(world + 0x10)
        cars = [g.u32(world + 0x1C + 4 * i) for i in range(min(n, 64))]
        traffic = [c for c in cars if c and g.u32(g.u32(c + 0x14) + 4) == 3]
        tpos = [xy(c) for c in traffic]
        ps = players()
        pxy = [xy(g.u32(p + 4)) for p in ps]
        for c, (x, y) in zip(traffic, tpos):
            if c in last_pos and math.hypot(x - last_pos[c][0], y - last_pos[c][1]) > 80:
                teleports += 1
                ds = [math.hypot(x - a, y - b) for a, b in pxy]
                k = min(range(len(ds)), key=ds.__getitem__) if ds else -1
                if k >= 0 and ds[k] < radius:
                    popins[k] = popins.get(k, 0) + 1
                    print(time.strftime('%H:%M:%S'), f'pop-in {ds[k]:.0f} from P{k + 1}', flush=True)
            last_pos[c] = (x, y)
        p1 = xy(g.u32(ps[0] + 4)) if ps else (0, 0)
        parts = []
        for k, p in enumerate(ps):
            px, py = xy(g.u32(p + 4))
            ds = [math.hypot(x - px, y - py) for x, y in tpos]
            near = sum(d < radius for d in ds)
            sums[k] = sums.get(k, 0) + near
            parts.append(f'P{k + 1} {near:2d} near (min {min(ds) if ds else 0:5.0f}, {math.hypot(px - p1[0], py - p1[1]):5.0f} from P1)')
        samples += 1
        if time.time() < next_print: time.sleep(interval); continue
        next_print = time.time() + print_every
        print(time.strftime('%H:%M:%S'), f'traffic {len(traffic):2d} |', ' | '.join(parts), flush=True)
    except Exception as e:
        print(time.strftime('%H:%M:%S'), 'read error:', e, flush=True)
    time.sleep(interval)
if samples:
    print(f'teleports {teleports}, pop-ins:', ' '.join(f'P{k + 1} {popins.get(k, 0)}' for k in sorted(sums)), flush=True)
    print('average near:', ' '.join(f'P{k + 1} {v / samples:.1f}' for k, v in sorted(sums.items())), flush=True)
