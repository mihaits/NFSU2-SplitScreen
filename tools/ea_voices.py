"""EA sound engine voices and the game sounds that own them (per-player audio research, docs/sound.md).

Lists the playing mixer voices (index = EA channel = handle & 0xFF) with their channel gains, then searches each game
sound object the plugin logged ("sound <ptr> listener <k>", Debug = 1) two pointer levels deep for a playing channel's
handle or EA instance pointer, and prints the paths found.

  python ea_voices.py            # snapshot
  python ea_voices.py --watch 1  # every second
"""
import argparse, os, re, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t
from paths import LOG

VOICES, VOICE_STRIDE = 0x8B4A00, 0x5C           # mixer voices, byte +0 == 2 playing, gains +0x1C + ch*4
CHANNELS, CHANNEL_COUNT, CHANNEL_STRIDE = 0x8B466C, 0x8B460C, 0x90   # EA channels: +0 handle, +0x6D active
INSTANCES, INSTANCE_COUNT = 0x8B4068, 0x8B44DE  # game handle k -> instance [INSTANCES + 4k], instance +4 = channel handle


def sounds_from_log():
    out = {}
    with open(LOG, errors='replace') as f:
        for line in f:
            m = re.search(r'sound ([0-9A-F]{8}) listener (\d) vtable ([0-9A-F]{8})', line)
            if m:
                out[int(m.group(1), 16)] = (int(m.group(2)), int(m.group(3), 16))
    return out


def snapshot(g):
    voices = g.u32(VOICES)
    playing = {}
    for i in range(64):
        b = g.read(voices + i * VOICE_STRIDE, VOICE_STRIDE)
        if b[0] == 2:
            playing[i] = struct.unpack_from('<2f', b, 0x1C)
    nch = struct.unpack('<h', g.read(CHANNEL_COUNT, 2))[0]
    chans = g.u32(CHANNELS)
    handles = {}
    for i in range(nch):
        b = g.read(chans + i * CHANNEL_STRIDE, CHANNEL_STRIDE)
        if b[0x6D]:
            handles[struct.unpack_from('<I', b, 0)[0]] = i
    ninst = g.read(INSTANCE_COUNT, 1)[0]
    insts = {}
    for k in range(ninst):
        p = g.u32(INSTANCES + 4 * k)
        if p:
            insts[p] = (k, g.u32(p + 4))
    return playing, handles, insts


def search(g, obj, targets, depth=2, size=0x140):
    """[(path, kind, value)] for dwords in obj (and the objects it points to) that are in targets."""
    found = []
    try:
        data = g.read(obj, size)
    except Exception:
        return found
    for off in range(0, size - 3, 4):
        v = struct.unpack_from('<I', data, off)[0]
        if v in targets:
            found.append(([off], targets[v], v))
        elif depth > 1 and 0x00800000 <= v < 0x7FFF0000 and v != obj:
            for path, kind, val in search(g, v, targets, depth - 1, 0x100):
                found.append(([off] + path, kind, val))
    return found


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--watch', type=float, default=0)
    args = ap.parse_args()
    g = t.Game()
    while True:
        playing, handles, insts = snapshot(g)
        print(time.strftime('%H:%M:%S'), f'{len(playing)} voices playing, {len(handles)} channels active, '
              f'{len(insts)} instances', flush=True)
        for i, (l, r) in sorted(playing.items()):
            owners = [f'inst {k}' for p, (k, h) in insts.items() if h & 0xFF == i and h in handles]
            print(f'  voice {i:2d} gains {l:.3f} {r:.3f} {" ".join(owners)}')
        targets = {h: f'channel handle (voice {h & 0xFF})' for h in handles}
        targets.update({p: f'instance {k} (voice {h & 0xFF})' for p, (k, h) in insts.items()})
        for obj, (listener, vt) in sorted(sounds_from_log().items()):
            hits = search(g, obj, targets)
            if hits:
                for path, kind, val in hits[:4]:
                    print(f'  sound {obj:08x} vt {vt:08x} L{listener}: '
                          f'{"->".join("+%X" % o for o in path)} = {val:08x} {kind}')
        if not args.watch:
            break
        time.sleep(args.watch)


if __name__ == '__main__':
    main()
