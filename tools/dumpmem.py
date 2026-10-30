"""Print 64 bytes at each address from a minidump, if it holds that memory. python dumpmem.py <dmp> addr [addr..]"""
import struct, sys
d = open(sys.argv[1], 'rb').read()
_, _, n, rva = struct.unpack_from('<IIII', d, 0)
st = {}
for i in range(n):
    t, size, r = struct.unpack_from('<III', d, rva + i * 12); st[t] = (size, r)
ranges = []
if 5 in st:   # MemoryListStream
    _, r = st[5]; k = struct.unpack_from('<I', d, r)[0]
    for i in range(k):
        start, sz, loc = struct.unpack_from('<QII', d, r + 4 + i * 16); ranges.append((start, sz, loc))
if 9 in st:   # Memory64ListStream
    _, r = st[9]; k, base = struct.unpack_from('<QQ', d, r)
    for i in range(k):
        start, sz = struct.unpack_from('<QQ', d, r + 16 + i * 16); ranges.append((start, sz, base)); base += sz
def read(a, l):
    for s, sz, loc in ranges:
        if s <= a and a + l <= s + sz: return d[loc + a - s: loc + a - s + l]
    return None
print('streams', sorted(st), 'ranges', len(ranges), 'bytes', sum(r[1] for r in ranges))
for a in [int(x, 16) for x in sys.argv[2:]]:
    b = read(a, 0x40)
    print(hex(a), b.hex(' ', 4) if b else 'not in dump')
