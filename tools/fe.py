"""Fast frontend inspection and control (needs Debug=1 for the ASI mailbox).
python -I fe.py [state]                 active packages + mode
python -I fe.py msg <name|hex> [fng]    send an FE message to a package (default: top screen)
python -I fe.py goto <toFng> [fromFng]  switch screens (from default: top screen)
python -I fe.py poke <addr> <value>     write a dword on the game thread
python -I fe.py pad <n> <btn>[+btn] [hold s]  press buttons on virtual pad n (0-3 = XInput pads 4-7)
                                        btn: a b x y lb rb start back up down left right lt rt lx- lx+ ly- ly+
python -I fe.py padoff <n>              disconnect virtual pad n
python -I fe.py page <n>                Controls screen page (0 keyboard, 1 joypad P1, 2 joypad P2)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import paths, trainer as t

PKG_LIST = 0x8637B0          # FE package cache (0x51BB70): list head at +4, node = entry+4

def fe_hash(name):
    """0x505450: case-insensitive x33 hash."""
    h = 0xFFFFFFFF
    for c in name.upper().encode('latin1'):
        h = (h * 33 + c) & 0xFFFFFFFF
    return h

_names = None
def name_for_hash(h):
    """Resolve a package hash using every *.fng string in speed2.exe."""
    global _names
    if _names is None:
        import re
        exe = open(paths.EXE, 'rb').read()
        _names = {fe_hash(m.decode('latin1')): m.decode('latin1') for m in set(re.findall(rb'[A-Za-z0-9_]+\.fng', exe))}
    return _names.get(h)

def cstr(g, p, n=64):
    try:
        b = g.read(p, n).split(b'\0')[0]
        return b.decode('latin1')
    except OSError:
        return None

def packages(g):
    out = []
    node = g.u32(PKG_LIST + 4)
    for _ in range(256):
        if not node: break
        e = node - 4
        if e == PKG_LIST: break
        hdr = g.u32(e + 0x10); name = None
        if hdr and g.u32(hdr) == 0x30203:
            d = g.u32(e + 0xC)
            if g.u32(d) == 0xE76E4546 and g.u32(d + 8) == 0x64486B50: name = cstr(g, d + 0x28)
        elif hdr and g.u32(hdr) == 0x30210:
            h = g.u32(hdr + 8); name = name_for_hash(h) or f'#{h:08x}'
        out.append((name or f'?{e:08x}', g.u32(e + 0x14), g.u32(e + 0x18)))
        node = g.u32(e + 4)
    return out

MSGS = dict(accept=0x0C407210, back=0x911AB364, start=0xB5AF2461, activate=0x9803F6E2,
            prev=0x9120409E, next=0xB5971BF1)
HELPERS = ('UI_PC_Help_Bar.fng', 'GarageMain.fng', 'GarageMain_2P.fng')

def top(g):
    for name, obj, active in packages(g):
        if active and name not in HELPERS: return name

def mailbox(g):
    import re
    log = open(paths.LOG).read()
    a = int(re.findall(r'mailbox at ([0-9A-Fa-f]+)', log)[-1], 16)
    assert g.u32(a) == 0x424D464E, 'mailbox magic'
    return a

def command(g, cmd, arg=0, arg2=0, s1='', s2='', timeout=3.0):
    import struct, time
    a = mailbox(g)
    g.write(a + 12, struct.pack('<III', cmd, arg & 0xFFFFFFFF, arg2 & 0xFFFFFFFF)
            + (s1 if isinstance(s1, bytes) else s1.encode()).ljust(64, bytes(1)) + s2.encode().ljust(64, bytes(1)))
    seq = (g.u32(a + 4) + 1) & 0xFFFFFFFF
    g.w32(a + 4, seq)
    end = time.time() + timeout
    while g.u32(a + 8) != seq:
        if time.time() > end: raise TimeoutError('mailbox not serviced (game paused/minimised?)')
        time.sleep(0.01)

def msg(g, m, fng=None):
    command(g, 1, MSGS[m] if m in MSGS else int(m, 16), s1=fng or top(g))

def goto(g, to, frm=None):
    command(g, 2, 0, s1=frm or top(g), s2=to)

BTN = dict(up=0x1, down=0x2, left=0x4, right=0x8, start=0x10, back=0x20, ls=0x40, rs=0x80, lb=0x100, rb=0x200,
           a=0x1000, b=0x2000, x=0x4000, y=0x8000)

def vpad(g, n, buttons=0, lt=0, rt=0, lx=0, ly=0, rx=0, ry=0, connected=True):
    import struct
    command(g, 4, n | (connected << 8), buttons | lt << 16 | rt << 24,
            s1=struct.pack('<hhhh', lx, ly, rx, ry))

def press(g, n, combo, hold=0.08):
    """combo like 'a', 'start', 'lx-+rt'; hold seconds, then release (pad stays connected)."""
    import time
    kw = dict(buttons=0)
    for b in combo.split('+'):
        if b in BTN: kw['buttons'] |= BTN[b]
        elif b in ('lt', 'rt'): kw[b] = 255
        elif b[:2] in ('lx', 'ly', 'rx', 'ry'): kw[b[:2]] = 32767 if b[2] == '+' else -32768
        else: raise ValueError(b)
    vpad(g, n, **kw); time.sleep(hold); vpad(g, n)

def state(g):
    print('mode', g.u32(0x850078), 'feplayer', g.i32(0x85AD28), 'controls page', g.i32(0x8364EC))
    for name, obj, active in packages(g):
        if active: print(f'  {name:40s} obj={obj:08x} active={active:08x}')

if __name__ == '__main__':
    g = t.Game(); a = sys.argv[1:] or ['state']
    if a[0] == 'state': state(g)
    elif a[0] == 'msg': msg(g, a[1], a[2] if len(a) > 2 else None); state(g)
    elif a[0] == 'goto': goto(g, a[1], a[2] if len(a) > 2 else None); __import__('time').sleep(0.5); state(g)
    elif a[0] == 'pad': press(g, int(a[1]), a[2], float(a[3]) if len(a) > 3 else 0.08)
    elif a[0] == 'padoff': vpad(g, int(a[1]), connected=False)
    elif a[0] == 'page':
        command(g, 3, 0x8364EC, int(a[1])); command(g, 1, 0xAF0BBD92, s1='UI_Options_PC_Controller.fng')
    elif a[0] == 'poke': command(g, 3, int(a[1], 16), int(a[2], 0))
