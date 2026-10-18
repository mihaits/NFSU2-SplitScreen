"""Scan the game's writable memory for a regex (ASCII and UTF-16). python -I strscan.py REGEX [max]"""
import ctypes, ctypes.wintypes as w, re, sys
sys.path.insert(0, '.')
import trainer as t

class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', w.DWORD),
                ('RegionSize', ctypes.c_size_t), ('State', w.DWORD), ('Protect', w.DWORD), ('Type', w.DWORD)]

g = t.Game()
pat = sys.argv[1].encode()
wide = re.compile(b''.join(re.escape(bytes([c])) + b'\x00' if chr(c).isalnum() or c in b'+-.:' else bytes([c]) + b'\x00?'
                           for c in pat) if False else b'(?:' + pat.replace(b'\\', b'\\') + b')')
rx = re.compile(pat)
k32 = ctypes.windll.kernel32
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
addr, hits, limit = 0x10000, 0, int(sys.argv[2]) if len(sys.argv) > 2 else 60
while addr < 0x7FFF0000 and hits < limit:
    m = MBI()
    if not k32.VirtualQueryEx(g.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x40) and size < 0x4000000:
        try: data = g.read(addr, size)
        except Exception: data = b''
        for mm in rx.finditer(data):
            print(f'{addr + mm.start():08x} ascii {mm.group()!r}'); hits += 1
        for off in (0, 1):
            hi = data[off + 1::2]
            for mm in rx.finditer(data[off::2]):
                if any(hi[mm.start():mm.end()]): continue          # real UTF-16: high bytes 0
                print(f'{addr + off + mm.start() * 2:08x} utf16 {mm.group()!r}'); hits += 1
    addr += size
