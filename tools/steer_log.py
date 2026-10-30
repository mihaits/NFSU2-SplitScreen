"""Live steering comparison between human players: samples the input object and the steering filter's output.
python -I steer_log.py [--players 4,5] [--csv path] [--seconds 240] [--speed 20] [--top 200] [--need 200]
Polls at about 200 Hz, prints a status line per second and writes every sample to the csv. Ends when every player has
--need samples steering at least half way between --speed and --top m/s, or after --seconds; then prints the comparison.
Reads only; needs a race, any plugin build. A finished player's input object is freed: stop before the finish.

Input object = car +0x30 (car +0x24 == 1): +0x2A8 car, +0x2AC index, +0x2C4/+0x2C5 raw left/right bytes, +0x2C8 steer
-1..1, +0x1E0 steering filter output, filter object at +0x220 (+0 car). The filter's inputs from the car: speed
car +0x42C, tables 0x7E9ED8.. (global), `*(car+0x20)` +0x270 and input +0x20C..+0x214."""
import csv, ctypes, os, statistics, struct, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin

T0 = time.time()
def say(msg):
    print(f'{time.strftime("%H:%M:%S")} +{time.time() - T0:5.1f}s {msg}', flush=True)

def opt(name, default):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default

want = [int(x) for x in opt('--players', '4,5').split(',')]
seconds = float(opt('--seconds', 240)); min_speed = float(opt('--speed', 20)); need = int(opt('--need', 200))
top_speed = float(opt('--top', 200))
path = opt('--csv', os.path.join(tempfile.gettempdir(), 'nfsu2_steer.csv'))
g = t.Game()
f32 = lambda a: struct.unpack('<f', g.read(a, 4))[0]
ctypes.windll.winmm.timeBeginPeriod(1)

def sample(k, p):
    car = g.u32(p + 4); ctl = g.u32(car + 0x30)
    if not ctl or g.u32(car + 0x24) != 1 or g.u32(ctl + 0x2A8) != car: return None
    body = g.u32(g.u32(car + 0x34) + 0x20); vx, vy, vz = struct.unpack('<3f', g.read(body + 0xA0, 12))
    b = g.read(ctl + 0x1E0, 2); fil = ctl + 0x220; stats = g.u32(car + 0x20); tune = ctl
    return dict(t=round(time.time() - T0, 4), player=k + 1, vx=round(vx, 3), vy=round(vy, 3), index=g.u32(ctl + 0x2AC), ground=round((vx * vx + vy * vy + vz * vz) ** 0.5, 2),
                speed=round(f32(car + 0x42C), 3), left=g.u8(ctl + 0x2C4), right=g.u8(ctl + 0x2C5),
                steer=round(f32(ctl + 0x2C8), 4), out=struct.unpack('<h', b)[0], fil_out=struct.unpack('<H', g.read(fil + 0x7C, 2))[0],
                fil_20=round(f32(fil + 0x20), 4), fil_car=hex(g.u32(fil)), lim=round(f32(stats + 0x270), 3),
                t20c=round(f32(tune + 0x20C), 3), t210=round(f32(tune + 0x210), 3), t214=round(f32(tune + 0x214), 3),
                flag_2f8=g.u8(ctl + 0x2F8), flag_2fb=g.u8(ctl + 0x2FB), wheel_mode=g.u32(0x81CB8C), car=hex(car), ctl=hex(ctl))

def good(s):
    return min_speed <= s['ground'] <= top_speed and abs(s['steer']) >= 0.5

say(f'waiting for the race; players {want}, speed >= {min_speed} m/s, {need} samples each, csv {path}')
end = time.time() + seconds
while True:
    world = g.u32(0x890118)
    if world and g.u32(0x8654A4) == 6 and g.u32(world) == 4: break
    if time.time() > end: sys.exit('FAIL: no race')
    time.sleep(0.1)
ps = plugin.race_players(g)
say(f'racing, {len(ps)} players')
if max(want) > len(ps): sys.exit(f'FAIL: only {len(ps)} human cars')

rows = []; count = {k: 0 for k in want}; last = {}; next_line = 0; warned = set()
with open(path, 'w', newline='') as f:
    w = None
    while time.time() < end:
        for k in want:
            try: s = sample(k - 1, ps[k - 1])
            except OSError: s = None
            if s is None:
                if k not in warned: say(f'P{k}: no human input object (finished?)'); warned.add(k)
                continue
            if w is None: w = csv.DictWriter(f, fieldnames=list(s)); w.writeheader()
            w.writerow(s); rows.append(s); last[k] = s
            count[k] += good(s)
        now = time.time()
        if now >= next_line:
            next_line = now + 1; f.flush()
            say(' | '.join(f'P{k} {last[k]["ground"] * 3.6:3.0f} km/h steer {last[k]["steer"]:+.2f} out {last[k]["out"]:+6d} '
                           f'[{min(count[k], need)}/{need}]' for k in want if k in last))
        if all(c >= need for c in count.values()): break
        time.sleep(0.005)

say('done: comparison (median |output| by speed and stick)')
for k in want:
    r = [s for s in rows if s['player'] == k]
    if r: say(f'P{k}: index {r[-1]["index"]} lim {r[-1]["lim"]} tune {r[-1]["t20c"]}/{r[-1]["t210"]}/{r[-1]["t214"]} '
              f'wheel_mode {r[-1]["wheel_mode"]} flags {r[-1]["flag_2f8"]}/{r[-1]["flag_2fb"]} speed/ground '
              f'{statistics.median(s["speed"] / s["ground"] for s in r if s["ground"] > 5) if any(s["ground"] > 5 for s in r) else 0:.3f}')
for lo, hi in ((10, 20), (20, 30), (30, 45), (45, 200)):
    for a, b in ((0.25, 0.6), (0.6, 0.95), (0.95, 1.01)):
        line = []
        for k in want:
            v = [abs(s['out']) for s in rows if s['player'] == k and lo <= s['ground'] < hi and a <= abs(s['steer']) < b]
            line.append(f'P{k} {statistics.median(v):7.0f} ({len(v):4d})' if v else f'P{k}       - (   0)')
        say(f'{lo:3d}-{hi:3d} m/s stick {a:.2f}-{b:.2f}: ' + '  '.join(line))
