"""Dump an active FE package's object tree (name, type, colour, flags, texture key, position, scale).
python -I fe_objects.py <package object address in hex>   (addresses: fe.py state / the cache list)
flags bit 0 = hidden, 0x400000 = transform changed; names resolved from strings in speed2.exe."""
import os, sys, struct, re
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fe, trainer as t
g = t.Game()
exe = open(os.path.join(HERE, '..', '..', 'speed2.exe'), 'rb').read()
names = {fe.fe_hash(m.decode()): m.decode() for m in set(re.findall(rb'[A-Za-z0-9_%]{3,40}', exe))}
def walk(o, d=0):
    n = 0
    while o and n < 400:
        ty = g.u32(o + 0x18); dp = g.u32(o + 0x2C)
        c = g.u32(dp) if dp else 0
        x = y = sx = sy = 0
        if dp: x, y = struct.unpack('<ff', g.read(dp + 28, 8)); sx, sy = struct.unpack('<ff', g.read(dp + 56, 8))
        print('  ' * d + f'{names.get(g.u32(o + 0x10), hex(g.u32(o + 0x10)))} t{ty} col {c:08x} fl {g.u32(o + 0x1c):08x} '
              f'tex {g.u32(o + 0x24):08x} ({x:.0f},{y:.0f}) s({sx:.0f},{sy:.0f})')
        if ty == 5: walk(g.u32(o + 0x64), d + 1)
        o = g.u32(o + 4); n += 1
walk(g.u32(int(sys.argv[1], 16) + 0x4C))
