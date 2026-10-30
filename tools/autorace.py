"""Let the game's AI drive the local players' cars (the game's own autopilot) and follow the race to the end.
python -I autorace.py [--players 1,3] [--timeout 600] [--stall 20] [--shots DIR] [--shot-every 0] [--every 0] [--stagger 0]
--stagger s releases the selected cars one by one, s seconds apart, to spread them around the track.
python -I autorace.py off                    give every local player's car back to its pad

Run it once a split race is loading or running (e.g. after go_race_nointro.sh). Stages, one timestamped line each:
wait for racing, autopilot on (start screenshot), a status line each time the leader starts a lap (--every s: also
periodically), each finish, results screenshot (--shot-every s: also screenshots while racing). Fails fast: no race within --wait-race s, a car not taken over in 3 s, a car whose progress
stands still for --stall s (stuck), or the overall --timeout. On a failure or Ctrl+C the cars still racing go back to
their pads. Players not in --players keep their pads (e.g. drive P1 yourself, the AI finishes the rest).

How: player +0x230 is the racing mode the per-frame mode picker 0x5EA650 applies (2 car racing, 4 auto pilot; the
debug joy event 0xB5 toggles it at 0x602BD0). Entering mode 4 (0x605C70) calls 0x5FAE20: the car's driver controller
becomes the AI racer's (0x5EBA30(car, 3), car +0x24 = 3) and its mover the AI's; leaving it calls 0x5FAF10 (human
controller 1 back). The game uses the same for rolling starts and after the finish line (mode 9, 0x606CB0)."""
import os, struct, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe, plugin

MODE_RACING, MODE_AUTO_PILOT = 2, 4
STAGES = {0: 'none', 1: 'flyby', 2: 'prerace', 3: 'countdown', 4: 'racing'}

T0 = time.time()
def say(msg):
    print(f'{time.strftime("%H:%M:%S")} +{time.time() - T0:5.1f}s {msg}', flush=True)


def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


g = t.Game()
players = plugin.LocalPlayers(g)
f32 = lambda a: struct.unpack('<f', g.read(a, 4))[0]


def stage():
    mgr = g.u32(0x890118)
    return g.u32(mgr) if mgr and g.u32(0x8654A4) == 6 else -1


def car_state(p):
    car = g.u32(p + 4); racer = g.u32(car + 0x1C)
    body = g.u32(g.u32(car + 0x34) + 0x20)
    vx, vy, vz = struct.unpack('<3f', g.read(body + 0xA0, 12))
    # racer +0x14 = laps done + lap fraction; player +0x50 = finished (the mode picker then goes to mode 9)
    return dict(car=car, controller=g.u32(car + 0x24), progress=f32(racer + 0x14) if racer else 0.0,
                pos=g.u8(racer + 0xA) if racer else 0, kmh=3.6 * (vx * vx + vy * vy + vz * vz) ** 0.5,
                done=g.u32(p + 0x50) != 0)


def give_back(ps, which):
    for k in which:
        if k < len(ps) and g.u32(ps[k] + 0x230) == MODE_AUTO_PILOT:
            g.w32(ps[k] + 0x230, MODE_RACING)
    say('pads back: ' + ' '.join(f'P{k + 1}' for k in which))


def shot(d, name):
    path = os.path.join(d, name)
    g.screenshot(path)
    say(f'screenshot {path}')


if sys.argv[1:2] == ['off']:
    ps = players(); give_back(ps, range(len(ps))); sys.exit(0)

timeout = float(opt('--timeout', 600)); stall = float(opt('--stall', 20)); wait_race = float(opt('--wait-race', 45))
shots = opt('--shots', os.path.join(tempfile.gettempdir(), 'nfsu2_autorace')); shot_every = float(opt('--shot-every', 0))
every = float(opt('--every', 0)); stagger = float(opt('--stagger', 0))
os.makedirs(shots, exist_ok=True)

# 1. racing
say('waiting for the race (stage racing)')
end = time.time() + wait_race; last = None
while (s := stage()) != 4:
    if s != last: say(f'stage {STAGES.get(s, "not in race" if s == -1 else s)}'); last = s
    if time.time() > end: sys.exit(f'FAIL: no race after {wait_race:.0f} s (stage {s})')
    time.sleep(0.1)
ps = players()
sel = [int(x) - 1 for x in opt('--players', ','.join(str(k + 1) for k in range(len(ps)))).split(',')]
sel = [k for k in sel if 0 <= k < len(ps)]
say(f'racing, {len(ps)} local players; autopilot for ' + ' '.join(f'P{k + 1}' for k in sel))

# 2. autopilot on, checked on the car's controller
active = {k: time.time() for k in (sel[:1] if stagger else sel)}   # released car -> when
waiting = [k for k in sel if k not in active]
for k in active: g.w32(ps[k] + 0x230, MODE_AUTO_PILOT)
end = time.time() + 3
while any(car_state(ps[k])['controller'] != 3 for k in active):
    if time.time() > end:
        bad = [f'P{k + 1} controller {car_state(ps[k])["controller"]}' for k in active if car_state(ps[k])['controller'] != 3]
        give_back(ps, sel); sys.exit('FAIL: not taken over: ' + ', '.join(bad))
    time.sleep(0.05)
say('autopilot on: ' + ' '.join(f'P{k + 1}' for k in active))

# 3. drive to the finish
deadline = time.time() + timeout
moved = {k: (car_state(ps[k])['progress'], time.time()) for k in range(len(ps))}
next_line = next_shot = time.time(); last_lap = 0
finished = set()
next_release = time.time() + stagger
try:
    while True:
        now = time.time()
        if stage() == -1: sys.exit('FAIL: left the race before everyone finished')
        if waiting and now >= next_release:
            k = waiting.pop(0); g.w32(ps[k] + 0x230, MODE_AUTO_PILOT); active[k] = now
            moved[k] = (car_state(ps[k])['progress'], now); next_release = now + stagger
            say(f'autopilot on: P{k + 1}')
        st = [car_state(p) for p in ps]
        for k, c in enumerate(st):
            if c['done'] and k not in finished:
                finished.add(k); say(f'P{k + 1} finished, position {c["pos"]}')
            if k in active and now - active[k] > 3 and k not in finished and c['controller'] != 3:   # drag: 0x5F3AB2 rewrites +0x230 per frame
                sys.exit(f'FAIL: the game took P{k + 1} back (controller {c["controller"]}, '
                         f'mode {g.u32(ps[k] + 0x230)}); drag is not supported')
            if c['progress'] > moved[k][0] + 0.0005 or k in finished or k not in active: moved[k] = (c['progress'], now)
            elif now - moved[k][1] > stall:
                shot(shots, f'stuck_p{k + 1}.png')
                sys.exit(f'FAIL: P{k + 1} stuck for {stall:.0f} s (progress {c["progress"]:.3f}, '
                         f'{c["kmh"]:.0f} km/h, controller {c["controller"]})')
        if all(k in finished for k in sel): break
        if now > deadline: sys.exit(f'FAIL: timeout {timeout:.0f} s, finished ' + ' '.join(f'P{k + 1}' for k in sorted(finished)))
        lap = int(max(c['progress'] for c in st))
        if lap > last_lap or (every and now >= next_line):
            say('  '.join(f'P{k + 1} lap {c["progress"] + 1:4.2f} pos {c["pos"]} {c["kmh"]:3.0f} km/h'
                          + (' AI' if c['controller'] == 3 else '') for k, c in enumerate(st)))
            last_lap = max(lap, last_lap); next_line = now + every
        if now >= next_shot:   # the start, then every --shot-every s (0: no more)
            shot(shots, f'race_{int(now - T0):04d}.png'); next_shot = now + shot_every if shot_every else float('inf')
        time.sleep(0.2)
except BaseException:
    give_back(ps, [k for k in sel if k not in finished])
    raise

# 4. results
say('every autopiloted car finished; waiting for the results')
end = time.time() + 40
while time.time() < end and len(finished) < len(ps):
    for k, p in enumerate(ps):
        if k not in finished and car_state(p)['done']: finished.add(k); say(f'P{k + 1} finished')
    time.sleep(0.2)
time.sleep(2)
say(f'screen {fe.top(g)}')
shot(shots, 'results.png')
