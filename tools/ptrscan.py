"""Find dwords in writable memory within [v - range, v] for each value. python -I ptrscan.py hexvalue [range]"""
import ctypes, ctypes.wintypes as w, sys
import struct
sys.path.insert(0, '.')
import trainer as t

class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', w.DWORD),
                ('RegionSize', ctypes.c_size_t), ('State', w.DWORD), ('Protect', w.DWORD), ('Type', w.DWORD)]
g = t.Game()
v = int(sys.argv[1], 16); rng = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
k32 = ctypes.windll.kernel32
k32.VirtualQueryEx.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
addr = 0x10000
while addr < 0x7FFF0000:
    m = MBI()
    if not k32.VirtualQueryEx(g.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)): break
    size = m.RegionSize
    if m.State == 0x1000 and m.Protect in (0x04, 0x40) and size < 0x4000000:
        try: data = g.read(addr, size & ~3)
        except Exception: data = b''
        for x in range(v - rng, v + 1):
            p = struct.pack('<I', x); i = data.find(p)
            while i != -1:
                if i % 4 == 0: print(f'{addr + i:08x} -> {x:08x}')
                i = data.find(p, i + 1)
    addr += size
