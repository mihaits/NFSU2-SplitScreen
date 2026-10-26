"""Car sounds the pad speakers don't route yet (per-player audio, docs/sound.md). Read-only; run during a split race.

Samples every playing EA voice for a few seconds and prints the ones whose stereo balance follows one car's engine
master (correlation over time) but that the plugin hasn't claimed (g_engineVoicePlayer / g_engineVoiceHandle), with
what they are: the patch (name, owning object and field) if they're a patch child, else their sample player's bank.

  python -I unclaimed_voices.py [seconds=8]
"""
import array, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, plugin, ea_voices as e, car_sounds as cs

g = t.Game()
MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'NFSU2SplitScreen.map')


def sym(name):
    base = plugin.module_base(g.pid)
    for l in open(MAP):
        if name in l:
            return int(l.split()[2], 16) - 0x10000000 + base
    raise KeyError(name)


def corr(a, b):
    n = len(a); ma = sum(a) / n; mb = sum(b) / n
    va = sum((x - ma) ** 2 for x in a); vb = sum((y - mb) ** 2 for y in b)
    return 0 if va <= 0 or vb <= 0 else sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (va * vb) ** .5


def bal(lr):
    return (lr[0] - lr[1]) / (lr[0] + lr[1]) if lr[0] + lr[1] > 1e-4 else None


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 8
    vp, vh = sym('?g_engineVoicePlayer@@'), sym('?g_engineVoiceHandle@@')
    nodes = cs.audio_nodes(g)
    objs = cs.find_objects(g, nodes)
    masters = {g.u32(o + 0xE0) & 0xFF: nodes[n][0] for o, (vt, off, n) in objs.items() if vt == 0x78AF30}
    print(f'{time.strftime("%H:%M:%S")} masters (voice: player): {masters}; sampling {seconds:.0f} s', flush=True)
    S = []
    end = time.time() + seconds
    while time.time() < end:
        playing, handles, _ = e.snapshot(g)
        tb = struct.unpack('<64i', g.read(vp, 256)); th = struct.unpack('<64i', g.read(vh, 256))
        S.append((playing, {i: h for h, i in handles.items()}, tb, th))
        time.sleep(0.04)
    print(f'{time.strftime("%H:%M:%S")} {len(S)} samples; identifying', flush=True)
    mem = []
    for b, s in cs.regions(g):
        try:
            mem.append((b, g.read(b, s)))
        except Exception:
            pass
    playing, inv, _, _ = S[-1]
    live = {h: i for i, h in inv.items() if h is not None and h != 0xFFFFFFFF}
    child = {}                                    # voice -> (patch name, patch player)
    for b, d in mem:
        a = array.array('I', d[:len(d) // 4 * 4])
        for k in range(len(a) - 4):
            if a[k + 3] != (a[k + 2] - 0x14) & 0xFFFFFFFF or not 0x01000000 < a[k + 2] < 0x7FFF0000:
                continue
            try:
                hdr = g.u32(a[k])
                if g.u32(hdr + 8) != 0x721096:
                    continue
                nm = g.read(g.u32(a[k] + 4), 24).split(b'\0')[0].decode('latin1')
                for i in range(g.read(hdr + 0x18, 1)[0]):
                    h = g.u32(a[k + 2] - 0x2C + g.u32(hdr + 0x30 + 4 * i) + 8)
                    if h in live:
                        child[live[h]] = (nm, b + 4 * k)
            except Exception:
                pass

    def owner(pl):
        for b, d in mem:
            j = d.find(struct.pack('<I', pl))
            while j >= 0:
                param = b + j
                for bb, dd in mem:
                    jj = dd.find(struct.pack('<I', param))
                    if jj >= 0:
                        at = bb + jj
                        blk = g.read(at - 0x800, 0x804)
                        for k in range(0x800, -4, -4):
                            v = struct.unpack_from('<I', blk, k)[0]
                            if 0x780000 <= v < 0x800000:
                                try:
                                    if 0x401000 <= g.u32(v) < 0x780000:
                                        o = at - 0x800 + k
                                        return f'object vt {v:08x} at {o:08x}, field +{at - o:X}'
                                except Exception:
                                    pass
                        return f'param {param:08x} held at {at:08x}'
                j = d.find(struct.pack('<I', pl), j + 1)
        return '?'

    def req_bank(h):
        for b, d in mem:
            j = d.find(struct.pack('<I', h))
            while j >= 0:
                if j % 4 == 0 and 8 <= j <= len(d) - 8 and (struct.unpack_from('<I', d, j + 4)[0] & 0xFFFF) == 1:
                    return struct.unpack_from('<I', d, j - 8)[0]
                j = d.find(struct.pack('<I', h), j + 1)
        return 0

    found = 0
    for v in range(64):
        if v in masters:
            continue
        rows = [p[v] for p, _, _, _ in S if v in p]
        if len(rows) < 40:
            continue
        claimed = sum(1 for p, iv, tb, th in S if v in p and tb[v] and iv.get(v) == th[v])
        if claimed > len(rows) // 2:
            continue
        best = []
        for m, pl in masters.items():
            pr = [(bal(p[v]), bal(p[m])) for p, _, _, _ in S if v in p and m in p]
            pr = [(x, y) for x, y in pr if x is not None and y is not None]
            if len(pr) > 40:
                best.append((corr([x for x, _ in pr], [y for _, y in pr]), pl))
        best.sort(reverse=True)
        if not best or best[0][0] < 0.85 or (len(best) > 1 and best[1][0] > 0.6):
            continue
        found += 1
        nm, pl = child.get(v, (None, None))
        what = f'patch {nm} ({owner(pl)})' if nm else f'not a patch child, bank {req_bank(inv.get(v, 0)):08x}'
        gain = sum(sum(p[v]) for p, _, _, _ in S if v in p) / len(rows)
        print(f'  voice {v:2d} follows P{best[0][1]} (r {best[0][0]:+.2f}), avg gain {gain:.3f}: {what}', flush=True)
    print(f'{time.strftime("%H:%M:%S")} {found} unclaimed voices follow one car', flush=True)


if __name__ == '__main__':
    main()
