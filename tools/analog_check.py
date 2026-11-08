"""Check a joypad record's analog bytes (+0x10 throttle, +0x11 brake, +0x12 steering stick X, +0x13 its Y) live.
python -I analog_check.py [pad n] [--expect-binding default|...]

Sets virtual pad n (fe.py pad numbering, 0-3) to a series of states and prints, per state, the analog bytes of every
joypad record and of the merged port state 0x86EAF0, then checks them (within 2 steps) against the default binding's expectations
(pedals on the triggers, steering on the left stick). Needs the game in a menu with Debug = 1. Prints each state as it
goes; exits 1 on the first mismatch.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, fe

INPUT_DEVICES = 0x874C40

def records(g):
    out = []
    for d in range(8):
        a = INPUT_DEVICES + d * 0x370
        if g.u32(a + 0x234) == 0x15: out.append((d, a))
    return out

def analog(g, a): return tuple(g.read(a + 0x10, 4))

def stamp(): return time.strftime('%H:%M:%S')

# (name, vpad kwargs, expected (+0x10, +0x11, +0x12, +0x13) or None for "print only")
STATES = [
    ('idle',           {},                          (0x00, 0x00, 0x80, 0x80)),
    ('LT full',        dict(lt=255),                (0x00, 0xFE, 0x80, 0x80)),   # trigger XOR 1 (decoded back)
    ('RT full',        dict(rt=255),                (0xFE, 0x00, 0x80, 0x80)),
    ('LT+RT full',     dict(lt=255, rt=255),        (0xFE, 0xFE, 0x80, 0x80)),
    ('LT half',        dict(lt=128),                (0x00, 0x81, 0x80, 0x80)),
    ('left stick up',  dict(ly=32767),              (0x00, 0x00, 0x80, 0x01)),
    ('left stick down', dict(ly=-32768),            (0x00, 0x00, 0x80, 0xFF)),
    ('left stick right', dict(lx=32767),            (0x00, 0x00, 0xFE, 0x80)),
    ('right stick up', dict(ry=32767),              (0x00, 0x00, 0x80, 0x80)),
]

def main():
    args = sys.argv[1:]
    pad = int(args[args.index('pad') + 1]) if 'pad' in args else 0
    only_print = '--print' in args
    g = t.Game()
    recs = records(g)
    print(stamp(), 'joypad records:', [d for d, _ in recs], flush=True)
    fails = 0
    for name, kw, want in STATES:
        fe.vpad(g, pad, **kw)
        time.sleep(0.15)
        per = {d: analog(g, a) for d, a in recs}
        merged = tuple(g.read(0x86EAF0 + 8, 4))
        close = lambda v: all(abs(a - b) <= 2 for a, b in zip(v, want))   # the game truncates its axis scaling
        ok = only_print or any(close(v) for v in per.values())
        line = ' '.join(f'rec{d} {bytes(v).hex()}' for d, v in per.items())
        print(stamp(), f'{name:18s} {line}  merged {bytes(merged).hex()}  want {bytes(want).hex()}  {"ok" if ok else "FAIL"}',
              flush=True)
        if not ok:
            fails += 1
            break
    fe.vpad(g, pad)
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
