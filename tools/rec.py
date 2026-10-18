"""Record real-pad menu input and replay it on the plugin's virtual pads, synchronised on the active FE screens.
python -I rec.py record <name>     (stop with Ctrl+C or by creating scripts/rec_stop)
python -I rec.py play <name> [--from N] [--to N]
python -I rec.py show <name>
Recordings: _splitscreen_mod/recordings/<name>.json. Real pad k is replayed on virtual pad k (XInput pad 4+k)."""
import ctypes, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t
import fe

HERE = os.path.dirname(os.path.abspath(__file__))
RECS = os.path.join(HERE, 'recordings')

class XS(ctypes.Structure):
    _fields_ = [('packet', ctypes.c_uint32), ('buttons', ctypes.c_uint16), ('lt', ctypes.c_uint8), ('rt', ctypes.c_uint8),
                ('lx', ctypes.c_int16), ('ly', ctypes.c_int16), ('rx', ctypes.c_int16), ('ry', ctypes.c_int16)]

NAMES = {v: k for k, v in fe.BTN.items()}

IGNORE = {n.lower() for n in fe.HELPERS} | {'chyron_fe.fng'}   # overlays whose timing varies

def screens(g):
    return sorted({n.lower() for n, o, a in fe.packages(g) if a} - IGNORE)

def pressed(s):
    """Set of logical inputs held (buttons, triggers, sticks as directions)."""
    out = {NAMES[b] for b in NAMES if s.buttons & b}
    if s.lt > 128: out.add('lt')
    if s.rt > 128: out.add('rt')
    if s.lx < -16000: out.add('left')
    if s.lx > 16000: out.add('right')
    if s.ly > 16000: out.add('up')
    if s.ly < -16000: out.add('down')
    return out

def record(name):
    xi = ctypes.WinDLL('xinput1_4')
    g = t.Game(); events = []; held = [set() for _ in range(4)]; t0 = time.time()
    stop = os.path.join(HERE, 'rec_stop')
    if os.path.exists(stop): os.remove(stop)
    print('recording... Ctrl+C or create scripts/rec_stop to finish', flush=True)
    try:
        while not os.path.exists(stop):
            for pad in range(4):
                s = XS()
                if xi.XInputGetState(pad, ctypes.byref(s)) != 0: held[pad] = set(); continue
                now = pressed(s)
                for b in sorted(now - held[pad]):
                    ev = dict(t=round(time.time() - t0, 3), pad=pad, btn=b, screens=screens(g))
                    events.append(ev); print(ev, flush=True)
                held[pad] = now
            time.sleep(0.005)
    except KeyboardInterrupt:
        pass
    os.makedirs(RECS, exist_ok=True)
    json.dump(events, open(os.path.join(RECS, name + '.json'), 'w'), indent=1)
    print(f'saved {len(events)} events')

def play(name, start=0, end=None, timeout=20.0):
    events = json.load(open(os.path.join(RECS, name + '.json')))[start:end]
    g = t.Game(); prev_t = None
    for i, ev in enumerate(events, start):
        want = sorted({n.lower() for n in ev['screens']} - IGNORE); deadline = time.time() + (90 if i == start else timeout)
        last_skip = 0
        while (have := screens(g)) != want:
            # intro movies / title splash that are not part of the recording: press A to move on
            if (not have or have == ['ug_ls_splash.fng']) and want and time.time() - last_skip > 1.0:
                fe.press(g, 0, 'a'); last_skip = time.time()
            if time.time() > deadline:
                print(f'#{i}: timeout waiting for {want}, have {screens(g)}'); return False
            time.sleep(0.02)
        # keep the player's rhythm within the same screen set, but never slower than needed
        gap = 0.12 if prev_t is None else min(max(ev['t'] - prev_t, 0.12), 0.6)
        time.sleep(gap); prev_t = ev['t']
        fe.press(g, ev['pad'], ev['btn'])
        print(f'#{i} pad{ev["pad"]} {ev["btn"]:6s} {",".join(want)}', flush=True)
    return True

if __name__ == '__main__':
    a = sys.argv[1:]
    if a[0] == 'record': record(a[1])
    elif a[0] == 'show':
        for i, ev in enumerate(json.load(open(os.path.join(RECS, a[1] + '.json')))): print(i, ev)
    elif a[0] == 'play':
        opt = dict(zip(a[2::2], a[3::2]))
        play(a[1], int(opt.get('--from', 0)), int(opt['--to']) if '--to' in opt else None)
