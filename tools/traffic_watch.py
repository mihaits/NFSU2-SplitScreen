"""Live traffic around each local player, a line every 2 s.
python -I traffic_watch.py [sample_s=0.5] [seconds=0 (forever)] [--radius 250]
Per player: traffic cars within the radius (xy, as the game's keep/spawn checks), the nearest one, and how far the
player is from P1. Every sample also checks for teleports (a traffic car moved more than 80 since the last sample: the
game recycled it) and prints those that land within the radius of a player ("pop-in": spawned in that player's view) or left from within it
("pop-out": removed in view).
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
ROUTE = 0x88F298
def road_delta(a, b):
    # 0x5D5CD0 on the race route: a - b, wrapped on a looped route (+0x24 looped, +0x18 length)
    d = a - b
    if g.read(ROUTE + 0x24, 1)[0]:
        L = f32(ROUTE + 0x18)
        if L > 0: d = (d + L / 2) % L - L / 2
    return d
def car_state(car):
    # road position, mover +0x798 (removed once no player faster than 20 is within 50 on the road: 0x4094A0) / +0x77C
    mover = g.u32(car + 0x2C)
    b = lambda o: g.read(mover + o, 1)[0] if mover else 0
    return (f32(car + 0x8FC), b(0x798), b(0x77C), b(0x77D), b(0x710), g.u32(car + 0x96C), g.u32(mover) if mover else 0,
            g.read(car + 0x550, 1)[0])

sums, samples = {}, 0
last_pos, teleports, popins, popouts, next_print = {}, 0, {}, {}, 0.0
hides = {}
last_pxy, last_state, last_pmove, last_proad, last_pspeed = [], {}, [], [], []
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
                ox, oy = last_pos[c]
                dold = [math.hypot(ox - a, oy - b) for a, b in last_pxy] if len(last_pxy) == len(pxy) else []
                j = min(range(len(dold)), key=dold.__getitem__) if dold else -1
                if k >= 0 and ds[k] < radius:
                    popins[k] = popins.get(k, 0) + 1
                    mv = last_pmove[k] if k < len(last_pmove) else (0, 0)
                    ahead = (x - pxy[k][0]) * mv[0] + (y - pxy[k][1]) * mv[1]
                    print(time.strftime('%H:%M:%S'), f'pop-in {ds[k]:.0f} from P{k + 1} ({"ahead" if ahead > 0 else "behind"},'
                          f' road {road_delta(car_state(c)[0], f32(g.u32(ps[k] + 4) + 0x8FC)):+.0f})', flush=True)
                if j >= 0 and dold[j] < radius:
                    popouts[j] = popouts.get(j, 0) + 1
                    road, f798, f77c, f77d, f710, c96c, vt, _ = last_state.get(c, (0,) * 8)
                    pr = last_proad[j] if j < len(last_proad) else 0
                    mv = last_pmove[j] if j < len(last_pmove) else (0, 0)
                    ahead = (ox - last_pxy[j][0]) * mv[0] + (oy - last_pxy[j][1]) * mv[1]
                    print(time.strftime('%H:%M:%S'), f'pop-out {dold[j]:.0f} from P{j + 1} ({"ahead" if ahead > 0 else "behind"},'
                          f' road {road_delta(road, pr):+.0f}, +798 {f798}, +77c {f77c}, +77d {f77d}, +710 {f710}, car +96c {c96c:x},'
                          f' mover vt {vt:x}, P{j + 1} speed {last_pspeed[j]:.0f})', flush=True)
            last_pos[c] = (x, y)
        state = {c: car_state(c) for c in traffic}
        for c, (x, y) in zip(traffic, tpos):
            # +77C 0 -> 1: the range check called the car out of range and no respawn spot was found, so the tick
            # parked it out of the world (0x42FAB0 after 0x4020D0): it vanishes here and comes back on a later respawn
            if state[c][2] and c in last_state and not last_state[c][2]:
                ds = [math.hypot(x - a, y - b) for a, b in pxy]
                k = min(range(len(ds)), key=ds.__getitem__) if ds else -1
                if k >= 0 and ds[k] < radius:
                    hides[k] = hides.get(k, 0) + 1
                    mv = last_pmove[k] if k < len(last_pmove) else (0, 0)
                    ahead = (x - pxy[k][0]) * mv[0] + (y - pxy[k][1]) * mv[1]
                    roads = ' '.join(f'P{i + 1} {road_delta(state[c][0], f32(g.u32(p + 4) + 0x8FC)):+.0f}' for i, p in enumerate(ps))
                    print(time.strftime('%H:%M:%S'), f'hidden {ds[k]:.0f} from P{k + 1} ({"ahead" if ahead > 0 else "behind"};'
                          f' road {roads}; car road {state[c][0]:.0f})', flush=True)
        hidden_now = sum(1 for c in traffic if state[c][2] and c in last_state and not last_state[c][2])
        if hidden_now >= 3:
            print(time.strftime('%H:%M:%S'), f'FLUSH: {hidden_now} traffic cars parked at once', flush=True)
        for c, (x, y) in zip(traffic, tpos):
            # drawn flag car +0x550 (the draw loop 0x6316D0 skips the car without it) going off without a park
            if c in last_state and last_state[c][7] and not state[c][7] and not state[c][2]:
                ds = [math.hypot(x - a, y - b) for a, b in pxy]
                k = min(range(len(ds)), key=ds.__getitem__) if ds else -1
                if k >= 0 and ds[k] < radius:
                    print(time.strftime('%H:%M:%S'), f'undrawn {ds[k]:.0f} from P{k + 1} (+550 off, not parked)', flush=True)
        last_state = state
        pcars = [g.u32(p + 4) for p in ps]
        last_pmove = [(a - b[0], c2 - b[1]) for (a, c2), b in zip(pxy, last_pxy)] if len(last_pxy) == len(pxy) else [(0, 0)] * len(pxy)
        last_proad = [f32(pc + 0x8FC) for pc in pcars]
        last_pspeed = [f32(pc + 0x42C) for pc in pcars]
        last_pxy = pxy
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
    print(f'teleports {teleports}, pop-ins:', ' '.join(f'P{k + 1} {popins.get(k, 0)}' for k in sorted(sums)),
          '| pop-outs:', ' '.join(f'P{k + 1} {popouts.get(k, 0)}' for k in sorted(sums)),
          '| hidden:', ' '.join(f'P{k + 1} {hides.get(k, 0)}' for k in sorted(sums)), flush=True)
    print('average near:', ' '.join(f'P{k + 1} {v / samples:.1f}' for k, v in sorted(sums.items())), flush=True)
