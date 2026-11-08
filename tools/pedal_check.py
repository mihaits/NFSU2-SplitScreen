"""Pedals and the pause menu stick per player, in a split race on virtual pads (after go_race_nointro.sh).
python -I pedal_check.py [--no-pause]

Pedals: for each local player, sets their virtual pad (P1..P6 = pads 1, 2, 3, 0, 4, 5) to idle, LT, RT, both and half
pulls and reads the car's input object (car +0x30): throttle byte +0x2D1, brake byte +0x2D9 and brake +0x2DC (0..1).
Full pulls must give 255 / 1.0, both at once both, idle 1.
Pause: each player pauses with START; the joypad record of the menu's slot (port & 1) must then carry that player's
stick Y in +0x13 (up low, down high) and stay centred while only the triggers are pulled. A line per state; exits 1 on
the first failure. The game window must stay focused.
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin, fe

PADS = (1, 2, 3, 0, 4, 5)
INPUT_DEVICES = 0x874C40
def say(m): print(time.strftime('%H:%M:%S'), m, flush=True)

def main():
    g = t.Game()
    end = time.time() + 15
    while g.u32(0x8654A4) != 6 and time.time() < end: time.sleep(0.1)
    if g.u32(0x8654A4) != 6: sys.exit('FAIL: no race')
    time.sleep(1.0)                                       # the cars' input objects fill a moment after the load
    players = plugin.LocalPlayers(g)()
    say(f'race with {len(players)} players')

    def ctl(p): return g.u32(g.u32(p + 4) + 0x30)
    def pedals(c):
        b = g.read(c + 0x2D0, 0x10)
        return b[1], b[9], struct.unpack('<f', b[0xC:0x10])[0]
    # (state, pad, check(throttle, brake byte, brake))
    states = [('idle', {}, lambda th, bb, br: th == 1 and bb == 1),
              ('LT full', dict(lt=255), lambda th, bb, br: th == 1 and bb == 255 and br == 1.0),
              ('RT full', dict(rt=255), lambda th, bb, br: th == 255 and bb == 1),
              ('LT+RT full', dict(lt=255, rt=255), lambda th, bb, br: th == 255 and br == 1.0),
              ('LT half', dict(lt=128), lambda th, bb, br: th == 1 and 120 <= bb <= 136),
              ('RT quarter', dict(rt=64), lambda th, bb, br: 60 <= th <= 72 and bb == 1),
              ('left stick', dict(lx=32767, ly=32767), lambda th, bb, br: th == 1 and bb == 1)]
    for k, p in enumerate(players):
        c = ctl(p)
        for name, kw, check in states:
            fe.vpad(g, PADS[k], **kw); time.sleep(0.15)
            th, bb, br = pedals(c)
            ok = check(th, bb, br)
            say(f'P{k + 1} {name:11s} throttle {th:3d} brake {bb:3d} ({br:.3f}) {"ok" if ok else "FAIL"}')
            if not ok: fe.vpad(g, PADS[k]); sys.exit(1)
        fe.vpad(g, PADS[k])

    if '--no-pause' in sys.argv: return
    for k in range(len(players)):
        pad = PADS[k]
        fe.press(g, pad, 'start', 0.15)
        end = time.time() + 3
        paused = lambda: any(n == 'UI_pause.fng' for n, _, a in fe.packages(g) if a)
        while time.time() < end and not paused(): time.sleep(0.05)
        if not paused(): sys.exit(f'FAIL: P{k + 1}: no pause menu')
        time.sleep(0.5)
        top = 'UI_pause.fng'
        rec = INPUT_DEVICES + (1 + (k & 1)) * 0x370          # joypad records 1, 2 = slots 0, 1
        res = {}
        for name, kw in (('up', dict(ly=32767)), ('down', dict(ly=-32768)), ('LT+RT', dict(lt=255, rt=255)), ('idle', {})):
            fe.vpad(g, pad, **kw); time.sleep(0.15)
            res[name] = g.u8(rec + 0x13)
        fe.vpad(g, pad)
        ok = res['up'] < 0x40 and res['down'] > 0xBF and 0x7E <= res['LT+RT'] <= 0x82 and 0x7E <= res['idle'] <= 0x82
        say(f'P{k + 1} pause ({top}) slot {k & 1} +0x13: ' + ' '.join(f'{n} {v:#04x}' for n, v in res.items())
            + (' ok' if ok else ' FAIL'))
        time.sleep(0.3)
        fe.press(g, pad, 'start', 0.15)
        end = time.time() + 3
        while time.time() < end and paused(): time.sleep(0.05)
        time.sleep(0.3)
        if not ok: sys.exit(1)
    say('pedal and pause checks passed')

if __name__ == '__main__':
    main()
