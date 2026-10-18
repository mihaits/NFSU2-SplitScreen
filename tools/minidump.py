"""Print exception code/address, faulting module and a stack scan of code addresses from a minidump."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, rva = struct.unpack_from('<IIII', d, 0)
streams = {}
for i in range(nstreams):
    t, size, r = struct.unpack_from('<III', d, rva + i * 12)
    streams[t] = (size, r)
mods = []
if 4 in streams:
    _, r = streams[4]; n = struct.unpack_from('<I', d, r)[0]
    for i in range(n):
        o = r + 4 + i * 108
        base, size = struct.unpack_from('<QI', d, o)
        name_rva = struct.unpack_from('<I', d, o + 20)[0]
        ln = struct.unpack_from('<I', d, name_rva)[0]
        name = d[name_rva + 4:name_rva + 4 + ln].decode('utf-16le')
        mods.append((base, size, name.split(chr(92))[-1]))
def mod(a):
    for b, s, n in mods:
        if b <= a < b + s: return f'{n}+{a - b:#x}'
    return '?'
_, r = streams[6]
tid = struct.unpack_from('<I', d, r)[0]
code, flags, rec, addr = struct.unpack_from('<IIQQ', d, r + 8)
nparams = struct.unpack_from('<I', d, r + 8 + 24)[0]
params = struct.unpack_from('<4Q', d, r + 8 + 32)
print(f'thread {tid} code {code:#x} at {addr:#x} ({mod(addr)}) params {[hex(x) for x in params[:nparams]]}')
ctx_size, ctx_rva = struct.unpack_from('<II', d, r + 8 + 152)
c = d[ctx_rva:ctx_rva + ctx_size]
regs = dict(zip(['edi','esi','ebx','edx','ecx','eax','ebp','eip','cs','eflags','esp'], struct.unpack_from('<11I', c, 0x9c)))
print({k: hex(v) for k, v in regs.items()})
# stack memory
if 9 in streams or 5 in streams:
    pass
# Memory list stream (5)
if 5 in streams:
    _, r5 = streams[5]; n = struct.unpack_from('<I', d, r5)[0]
    for i in range(n):
        start, dsize, drva = struct.unpack_from('<QII', d, r5 + 4 + i * 16)
        if start <= regs['esp'] < start + dsize:
            off = regs['esp'] - start
            words = struct.unpack_from(f'<{min(400, (dsize - off) // 4)}I', d, drva + off)
            hits = [(i * 4, w, mod(w)) for i, w in enumerate(words) if 0x401000 <= w < 0x790000 or mod(w) != '?' and w > 0x10000000 and 'speed2' not in mod(w)]
            for o, w, m in hits[:40]:
                print(f'  [esp+{o:#x}] {w:#x} {m}')
