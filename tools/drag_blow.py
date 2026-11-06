"""Drives a running drag race to the results without steering: every engine is blown (full throttle in 1st on every
virtual pad, players released STAGGER s apart from the go signal so they don't bump at the start). Prints each car's
over-rev heat (*(*(car+0x34)+0x48) +0x8C: 0.. rising, 2 = blown) every 0.5 s and every controller change, and stops
when a results package is active (or a timeout). Start the race first (go_race_nointro.sh 2).
python -I drag_blow.py <shot dir> [timeout_s=45] [stagger_s=1]
  PERCAR=1    screenshot when each car's heat crosses 1.0 (pc_c<i>.png; c0 = P2, c1 = P1, c2.. = P3..)
  HEATSHOT=x  screenshots when car 0's heat crosses 0.3 / 0.6 / 0.9 / 1.15 (x_h<heat>.png; for single player)
  otherwise   six screenshots 1.5 s apart (heat_<n>.png)"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe
g = t.Game(); shots = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 45
stagger = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
say = lambda m: print(time.strftime('%H:%M:%S'), m, flush=True)
f32 = lambda a: struct.unpack('<f', g.read(a, 4))[0]
end = time.time() + 15
while True:
    mgr = g.u32(0x890118)
    if mgr and g.u32(0x8654A4) == 6 and g.u32(mgr) >= 4: break
    if time.time() > end: sys.exit('FAIL: no go signal')
    time.sleep(0.03)
pads = [1, 2, 3, 0, 4, 5]                    # P1..P6
world = g.u32(0x890080)
cars = [g.u32(world + 0x1C + 4 * i) for i in range(g.u32(mgr + 0x24))]
def heat(c):
    eng = g.u32(g.u32(c + 0x34) + 0x48)
    try: return f32(eng + 0x8C) if eng else -9
    except OSError: return -9
say(f'go: {len(cars)} cars, releasing P1..P{len(pads)} {stagger:.1f} s apart, full throttle in 1st')
thr = [0.3, 0.6, 0.9, 1.15]; percar = set()
t0 = time.time(); released = 0; ctl = {}; next_line = next_shot = t0 + 1.5; n_shot = 0
try:
    while time.time() - t0 < timeout:
        now = time.time()
        while released < len(pads) and now - t0 >= released * stagger:
            fe.vpad(g, pads[released], rt=255); say(f'P{released + 1} released'); released += 1
        gs = g.u32(0x8654A4)
        if gs == 6 and g.u32(0x890118):
            for i, c in enumerate(cars):
                v = g.u32(c + 0x24)
                if ctl.get(i) != v: ctl[i] = v; say(f'c{i} controller {v}')
            if now >= next_line:
                next_line += 0.5
                say('  heat ' + ' '.join(f'c{i}={heat(c):.2f}' for i, c in enumerate(cars)))
            if os.environ.get('PERCAR'):          # each car's view when its heat crosses 1.0 (c0 = P2, c1 = P1)
                for i, c in enumerate(cars):
                    h = heat(c)
                    if i not in percar and 1.0 <= h < 2:
                        percar.add(i); p = os.path.join(shots, f'pc_c{i}.png'); g.screenshot(p, max_w=4000)
                        say(f'screenshot {p}  c{i} heat {h:.2f}')
            if os.environ.get('HEATSHOT'):
                h0 = heat(cars[0])
                if thr and 0 <= h0 < 2 and h0 >= thr[0]:
                    p = os.path.join(shots, f'{os.environ["HEATSHOT"]}_h{thr.pop(0):.2f}.png'); g.screenshot(p, max_w=4000)
                    say(f'screenshot {p}  heat {h0:.2f}')
            elif now >= next_shot and n_shot < 6:
                next_shot += 1.5; n_shot += 1
                p = os.path.join(shots, f'heat_{n_shot}.png'); g.screenshot(p, max_w=4000)
                say(f'screenshot {p}  heat ' + ' '.join(f'{heat(c):.2f}' for c in cars))
        res = [n for n, o, a in fe.packages(g) if a and 'result' in n.lower()]
        if res:
            say(f'results package {res[0]}'); time.sleep(2.5)
            p = os.path.join(shots, 'blow_results.png'); g.screenshot(p, max_w=4000); say(f'screenshot {p}'); break
        time.sleep(0.05)
    else:
        say('packages: ' + ' '.join(n for n, o, a in fe.packages(g) if a))
        sys.exit(f'FAIL: no results package after {timeout:.0f} s')
finally:
    for n in pads: fe.vpad(g, n)
say('results reached')
