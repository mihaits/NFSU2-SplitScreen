"""Per-car sound objects in a running race (per-player audio, docs/sound.md).

Every car has a node in the audio player list ([[0x82B884]+0xAC]+0x10, next +4; node +0x10 = player index, +0x30 ->
+0x18 = player). This finds every object (vtable in .rdata) that points to a node, groups them by class and offset,
and shows which playing EA voices (mixer voice = channel handle & 0xFF) each one holds.

  python -I car_sounds.py              # snapshot
  python -I car_sounds.py --watch 0.5  # repeat; prints a class's voices when they change
"""
import argparse, ctypes, ctypes.wintypes as w, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t
import ea_voices as e


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', w.DWORD),
                ('RegionSize', ctypes.c_size_t), ('State', w.DWORD), ('Protect', w.DWORD), ('Type', w.DWORD)]


k32 = ctypes.windll.kernel32
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]


def regions(g):
    addr, out = 0x10000, []
    while addr < 0x7FFF0000:
        m = MBI()
        if not k32.VirtualQueryEx(g.h, addr, ctypes.byref(m), ctypes.sizeof(m)):
            break
        base, size = m.BaseAddress or 0, m.RegionSize
        if m.State == 0x1000 and m.Protect in (4, 0x40) and m.Type != 0x1000000 and size <= 64 << 20:
            out.append((base, size))
        addr = base + size
    return out


def audio_nodes(g):
    lst = g.u32(g.u32(0x82B884) + 0xAC)
    node, nodes = g.u32(lst + 0x10), {}
    while node and len(nodes) < 16:
        n30 = g.u32(node + 0x30)
        nodes[node] = (g.u32(node + 0x10), g.u32(n30 + 0x18) if n30 else 0)
        node = g.u32(node + 4)
    return nodes


def find_objects(g, nodes):
    """{obj: (vtable, node offset, node)} for objects pointing to a node within 0x400 bytes of their vtable."""
    pats = {struct.pack('<I', n): n for n in nodes}
    objs = {}
    for base, size in regions(g):
        try:
            d = g.read(base, size)
        except Exception:
            continue
        for p, n in pats.items():
            i = d.find(p)
            while i >= 0:
                if i % 4 == 0:
                    for back in range(0, 0x400, 4):         # nearest dword before it that is a vtable
                        j = i - back
                        if j < 0:
                            break
                        v = struct.unpack_from('<I', d, j)[0]
                        if 0x780000 <= v < 0x800000 and 0x401000 <= g.u32(v) < 0x780000:
                            objs.setdefault(base + j, (v, back, n))
                            break
                i = d.find(p, i + 1)
    return objs


def voices_of(g, obj, handles, size=0x200):
    try:
        b = g.read(obj, size)
    except Exception:
        return []
    return [(o, handles[v]) for o in range(0, size - 3, 4) for v in [struct.unpack_from('<I', b, o)[0]] if v in handles]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--watch', type=float, default=0)
    args = ap.parse_args()
    g = t.Game()
    nodes = audio_nodes(g)
    print('audio nodes:', {f'{n:08x}': f'player {i} ({p:08x})' for n, (i, p) in nodes.items()}, flush=True)
    objs = find_objects(g, nodes)
    print(f'{len(objs)} objects point to a node', flush=True)
    last = {}
    while True:
        playing, handles, _ = e.snapshot(g)
        H = {h: i for h, i in handles.items() if h != 0xFFFFFFFF and i in playing}
        rows = {}
        for obj, (vt, off, n) in sorted(objs.items(), key=lambda x: (x[1][0], nodes[x[1][2]][0])):
            vs = voices_of(g, obj, H)
            key = (vt, obj)
            line = (f'vt {vt:08x} obj {obj:08x} node@+{off:X} player {nodes[n][0]}: ' +
                    (' '.join(f'+{o:X}=v{v}({playing[v][0]:.2f},{playing[v][1]:.2f})' for o, v in vs) or '-'))
            rows[key] = line
        changed = [l for k, l in rows.items() if last.get(k, '').split(':', 1)[-1] != l.split(':', 1)[-1]]
        if not args.watch or changed:
            print(time.strftime('%H:%M:%S'), f'{len(playing)} voices playing', flush=True)
            for l in (rows.values() if not last else changed):
                print('  ' + l, flush=True)
        last = rows
        if not args.watch:
            break
        time.sleep(args.watch)


if __name__ == '__main__':
    main()
