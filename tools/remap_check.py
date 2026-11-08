"""Custom pad bindings and the analog bytes, in a race (after go_race_nointro.sh), on P1 (virtual pad 1, joypad record 1).
python -I remap_check.py

Writes P1's axis setup (record +0x344 steer axis, +0x348 throttle, +0x34C brake, +0x350 shared pedal axis, +0x354..
signs, +0x364..+0x368 flags), the fields the Controls page sets and the decoder 0x5C1840 reads, for each binding below,
reads the car's pedals (input object car +0x30: throttle +0x2D1, brake +0x2D9) and the record's +0x13, and puts the
setup back. Bindings: combined pedals on the right stick Y; steering on the right stick; throttle and brake on
separate axes. A line per state; exits 1 on the first failure. The game window must stay focused.
"""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin, fe

REC = 0x874C40 + 0x370          # joypad record 1 = slot 0 = P1's pad in a race
PAD = 1
def say(m): print(time.strftime('%H:%M:%S'), m, flush=True)

def main():
    g = t.Game()
    if g.u32(0x8654A4) != 6: sys.exit('FAIL: no race')
    car = g.u32(plugin.LocalPlayers(g)()[0] + 4); ctl = g.u32(car + 0x30)
    saved = g.read(REC + 0x344, 0x28)
    ofs = lambda: struct.unpack('<4i', g.read(REC + 0x344, 16))
    say(f'P1 axis setup: axes {[hex(x) for x in ofs()]} signs {struct.unpack("<4i", saved[0x10:0x20])} '
        f'flags {saved[0x20:0x25].hex()}')

    def setup(steer=None, shared=None, throttle=None, brake=None, shared_mode=True):
        g.write(REC + 0x344, saved)
        if steer is not None: g.w32(REC + 0x344, steer)
        if throttle is not None: g.w32(REC + 0x348, throttle); g.w8(REC + 0x365, 1)
        if brake is not None: g.w32(REC + 0x34C, brake); g.w8(REC + 0x366, 1)
        if shared is not None: g.w32(REC + 0x350, shared)
        g.write(REC + 0x367, bytes([1, 1]) if shared_mode else bytes([0, 0]))

    def sample(kw):
        fe.vpad(g, PAD, **kw); time.sleep(0.15)
        b = g.read(ctl + 0x2D0, 0x10)
        return b[1], b[9], g.u8(REC + 0x13)

    UP, DOWN = dict(ry=32767), dict(ry=-32768)
    TRIG = dict(lt=255, rt=255)
    cases = [
        ('pedals on right stick Y', dict(shared=0x10), [
            ('right stick up', UP, lambda th, br, y: (th == 1) != (br == 1) and max(th, br) == 255 and y == 0x80),
            ('right stick down', DOWN, lambda th, br, y: (th == 1) != (br == 1) and max(th, br) == 255 and y == 0x80),
            ('right stick centre', {}, lambda th, br, y: th == 1 and br == 1),
            ('triggers', TRIG, lambda th, br, y: th == 1 and br == 1),
            ('left stick up', dict(ly=32767), lambda th, br, y: th == 1 and br == 1 and y < 0x40)]),
        ('steering on right stick', dict(steer=0x0C), [
            ('right stick up', UP, lambda th, br, y: y < 0x40),
            ('right stick down', DOWN, lambda th, br, y: y > 0xBF),
            ('left stick up', dict(ly=32767), lambda th, br, y: 0x7E <= y <= 0x82),
            ('triggers', TRIG, lambda th, br, y: th == 255 and br == 255 and 0x7E <= y <= 0x82)]),
        ('separate axes: throttle Ry, brake Rx', dict(throttle=0x10, brake=0x0C, shared_mode=False), [
            ('right stick up', UP, None), ('right stick down', DOWN, None),
            ('right stick right', dict(rx=32767), None), ('right stick left', dict(rx=-32768), None),
            ('triggers', TRIG, lambda th, br, y: 0x7E <= y <= 0x82),
            ('left stick up', dict(ly=32767), lambda th, br, y: y < 0x40)]),
    ]
    try:
        for title, kw, states in cases:
            setup(**kw)
            say(f'-- {title}')
            seen = {}
            for name, pad, check in states:
                th, br, y = sample(pad)
                seen[name] = (th, br)
                ok = check(th, br, y) if check else True
                say(f'   {name:18s} throttle {th:3d} brake {br:3d} +0x13 {y:#04x} {"ok" if ok else "FAIL"}')
                if not ok: sys.exit(1)
            if title.startswith('separate'):
                # the game's own separate-axis decoding: each axis moves only its pedal
                ud = seen['right stick up'][0] != seen['right stick down'][0] and seen['right stick up'][1] == seen['right stick down'][1]
                lr = seen['right stick right'][1] != seen['right stick left'][1] and seen['right stick right'][0] == seen['right stick left'][0]
                say(f'   Ry moves only the throttle: {ud}, Rx moves only the brake: {lr}')
                if not (ud and lr): sys.exit('FAIL: separate axes')
    finally:
        fe.vpad(g, PAD)
        g.write(REC + 0x344, saved)
        say('P1 axis setup restored')
    say('remap checks passed')

if __name__ == '__main__':
    main()
