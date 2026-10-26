"""Static reader for the tracks' position markers (start grids, start/finish lines, shop and crib spots) and the track
table, straight from the game files (read-only, no game needed).

python trackmarkers.py [--track 4101] [--type Starting] [--json out.json] [--verify] [--tracks]

Files (see docs/tooling.md):
- GLOBAL/GLOBALB.BUN chunk 0x34201: the track table (131 x 0x128, the static copy of [0x883DA0]): +0x00 name,
  +0x20 path, +0x40 region (L4RA = city, L4RB drag, L4RC drift, L4RD organic drag, L4RF Street X, L4RG speedways,
  L4RH tuning, L4RR replay), +0x8A id, +0xAC/+0xB0/+0xB4 its TRACKMAP's left x, top y and size in metres.
- TRACKS/ROUTES<region>/TrackPosMarkersAll.bin: one chunk 0x34146, 8 bytes padding, then 48-byte records:
  +0x08 name hash (bStringHash, 0x43DB50), +0x0C index, +0x10 x y z, +0x20 heading word (angle = h * 2pi / 65536,
  forward = (cos, sin)), +0x24 track id (4000 = the free roam city: shops, cribs and its own grid). +0x22 and +0x28
  hold leftover bytes. 0x5D8560(hash) counts a name's markers of the current track, 0x5D8620(hash, i) returns them.
  TrackPosMarkersFreeRoam.bin is the 4000 subset.
- TRACKS/TRACKMAP<id>.BIN: TPK with one 512x512 DXT3 texture at file offset 0x300 (alpha unused).
  px = (x - left) / size * 512, py = (top - y) / size * 512.
"""
import argparse, json, math, os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from paths import GAME

# Every name the markers use (from speed2.exe and GLOBALB.BUN strings); unknown hashes print as hex.
MARKER_NAMES = [
    'ForwardStartingPosition', 'BackwardStartingPosition', 'ForwardStartLine', 'BackwardStartLine',
    'ForwardFinishLine', 'BackwardFinishLine',
    'CRIB_1', 'CC_CAR_LOT_1', 'CC_CAR_SPECIALTY', 'CC_BODYSHOP_1', 'CC_BODYSHOP_2', 'CC_GRAPHICS', 'CC_MEGALOW',
    'CC_PERF_SHOP', 'UC_CAR_LOT', 'UC_CAR_SPECIALTY', 'UC_BODYSHOP', 'UC_GRAPHICS', 'UC_PERF_SHOP', 'SH_BODYSHOP',
    'SH_GRAPHICS', 'IND_CAR_LOT', 'IND_CAR_SPECIALTY_1', 'IND_CAR_SPECIALTY_2', 'IND_BODYSHOP_1', 'IND_BODYSHOP_2',
    'IND_GRAPHICS', 'IND_PERF_SHOP_1', 'IND_PERF_SHOP_2',
]
TRACK_TABLE_CHUNK, MARKERS_CHUNK, TRACK_SIZE, MARKER_SIZE = 0x34201, 0x34146, 0x128, 48
# Spot 0 of each track as 0x5EB5D0 returned it in game (track, x, y, z, heading).
VERIFIED = [(4101, -293.6, -67.0, 12.7, 48352), (4121, -1613.0, 551.2, 3.8, 49152),
            (4141, 1130.3, -1233.0, 18.0, 32768), (4061, -1891.5, 2621.6, 323.4, 57891),
            (4063, -2077.0, 1928.2, 190.1, 57436), (4162, -1467.4, 1177.8, 52.4, 527),
            (4701, -1646.6, -1983.0, -15.0, 38034), (4311, -1183.9, -674.9, 6.6, 0)]


def bhash(s):
    h = 0xFFFFFFFF
    for c in s.encode():
        h = (h * 33 + c) & 0xFFFFFFFF
    return h


NAMES = {bhash(n): n for n in MARKER_NAMES}


def chunks(data, o=0, end=None):
    end = len(data) if end is None else end
    while o + 8 <= end:
        cid, size = struct.unpack_from('<II', data, o)
        yield cid, o + 8, size
        o += 8 + size


def _str(b):
    return b.split(b'\0')[0].decode('latin1')


def load_tracks():
    """The track table: list of dicts in table order (4400 appears twice in the game's table; kept once)."""
    data = open(os.path.join(GAME, 'GLOBAL', 'GLOBALB.BUN'), 'rb').read()
    o, size = next((o, s) for cid, o, s in chunks(data) if cid == TRACK_TABLE_CHUNK)
    tracks, seen = [], set()
    for r in range(o, o + size - TRACK_SIZE + 1, TRACK_SIZE):
        tid, = struct.unpack_from('<H', data, r + 0x8A)
        if tid in seen:
            continue
        seen.add(tid)
        left, top, msize = struct.unpack_from('<fff', data, r + 0xAC)
        tracks.append({'id': tid, 'name': _str(data[r:r + 0x20]), 'path': _str(data[r + 0x20:r + 0x40]),
                       'region': _str(data[r + 0x40:r + 0x60]), 'map': [left, top, msize]})
    return tracks


def load_markers(region):
    path = os.path.join(GAME, 'TRACKS', 'ROUTES' + region, 'TrackPosMarkersAll.bin')
    if not os.path.isfile(path):
        return []
    data = open(path, 'rb').read()
    out = []
    for cid, o, size in chunks(data):
        if cid != MARKERS_CHUNK:
            continue
        for r in range(o + 8, o + size - MARKER_SIZE + 1, MARKER_SIZE):
            h, idx, x, y, z = struct.unpack_from('<IIfff', data, r + 8)
            heading, = struct.unpack_from('<H', data, r + 0x20)
            tid, = struct.unpack_from('<I', data, r + 0x24)
            out.append({'track': tid, 'region': region, 'type': NAMES.get(h, '%08X' % h), 'index': idx,
                        'x': round(x, 2), 'y': round(y, 2), 'z': round(z, 2), 'heading': heading})
    return out


def load_all():
    tracks = load_tracks()
    markers = [m for reg in sorted({t['region'] for t in tracks}) for m in load_markers(reg)]
    return tracks, markers


def heading_deg(h):
    return h * 360.0 / 65536


def trackmap_rgb(tid, size=512):
    """TRACKMAP<tid>.BIN's texture as an RGB numpy array (None if the file is missing). Needs numpy."""
    import numpy as np
    path = os.path.join(GAME, 'TRACKS', 'TRACKMAP%d.BIN' % tid)
    if not os.path.isfile(path):
        return None
    b = np.frombuffer(open(path, 'rb').read(), np.uint8, size * size, 0x300).reshape(-1, 16)
    c0 = b[:, 8].astype(np.uint32) | b[:, 9].astype(np.uint32) << 8
    c1 = b[:, 10].astype(np.uint32) | b[:, 11].astype(np.uint32) << 8
    def rgb(c):
        return np.stack([(c >> 11 & 31) * 255 / 31, (c >> 5 & 63) * 255 / 63, (c & 31) * 255 / 31], -1)
    p0, p1 = rgb(c0), rgb(c1)
    pal = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], 1)   # DXT3 colour blocks are always 4-colour
    bits = b[:, 12:16].astype(np.uint32)
    bits = bits[:, 0] | bits[:, 1] << 8 | bits[:, 2] << 16 | bits[:, 3] << 24
    sel = np.stack([(bits >> (2 * i)) & 3 for i in range(16)], 1)
    px = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    return px.reshape(size // 4, size // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(size, size, 3).astype(np.uint8)


def verify(markers):
    ok = True
    for tid, x, y, z, h in VERIFIED:
        m = next((m for m in markers if m['track'] == tid and m['type'] == 'ForwardStartingPosition'
                  and m['index'] == 0), None)
        good = m and abs(m['x'] - x) < 0.1 and abs(m['y'] - y) < 0.1 and abs(m['z'] - z) < 0.1 and m['heading'] == h
        ok &= bool(good)
        print('%s %d expected %.1f %.1f %.1f %d, read %s' % ('ok  ' if good else 'FAIL', tid, x, y, z, h,
              '%.1f %.1f %.1f %d' % (m['x'], m['y'], m['z'], m['heading']) if m else 'nothing'))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--track', type=int, help='only this track id')
    ap.add_argument('--type', help='only marker types containing this text (case-insensitive)')
    ap.add_argument('--json', help='write tracks and markers to this JSON file')
    ap.add_argument('--verify', action='store_true', help='check spot 0 of the tracks measured in game')
    ap.add_argument('--tracks', action='store_true', help='list the track table instead')
    a = ap.parse_args()
    tracks, markers = load_all()
    if a.verify:
        sys.exit(0 if verify(markers) else 1)
    if a.tracks:
        for t in tracks:
            print('%d %-5s %-34s map %.1f %.1f %.1f' % (t['id'], t['region'], t['name'], *t['map']))
        return
    sel = [m for m in markers if (a.track is None or m['track'] == a.track)
           and (a.type is None or a.type.lower() in m['type'].lower())]
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'tracks': tracks, 'markers': sel}, f, indent=1)
        print('%d tracks, %d markers -> %s' % (len(tracks), len(sel), a.json))
        return
    names = {t['id']: t['name'] for t in tracks}
    for m in sel:
        print('%d %-4s %-26s %d  %8.1f %8.1f %6.1f  %5d (%5.1f deg)  %s' % (
            m['track'], m['region'], m['type'], m['index'], m['x'], m['y'], m['z'], m['heading'],
            heading_deg(m['heading']), names.get(m['track'], '')))
    types = {}
    for m in sel:
        types[m['type']] = types.get(m['type'], 0) + 1
    print('%d markers: %s' % (len(sel), ', '.join('%s %d' % kv for kv in sorted(types.items(), key=lambda kv: -kv[1]))))


if __name__ == '__main__':
    main()
