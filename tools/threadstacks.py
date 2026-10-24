"""Every game thread's EIP and the return addresses on its stack (values just after a call instruction, as
module+offset), for a hung game: python -I threadstacks.py [stack bytes, default 0x3000]. Each thread is suspended
while it is read, then resumed; nothing is written."""
import os, sys, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

k = ctypes.WinDLL('kernel32', use_last_error=True); k.OpenThread.restype = W.HANDLE
psapi = ctypes.WinDLL('psapi', use_last_error=True)

class CTX(ctypes.Structure):    # WOW64_CONTEXT
    _fields_ = [('ContextFlags', W.DWORD), ('Dr0', W.DWORD), ('Dr1', W.DWORD), ('Dr2', W.DWORD), ('Dr3', W.DWORD),
                ('Dr6', W.DWORD), ('Dr7', W.DWORD), ('Float', ctypes.c_byte * 112), ('SegGs', W.DWORD),
                ('SegFs', W.DWORD), ('SegEs', W.DWORD), ('SegDs', W.DWORD), ('Edi', W.DWORD), ('Esi', W.DWORD),
                ('Ebx', W.DWORD), ('Edx', W.DWORD), ('Ecx', W.DWORD), ('Eax', W.DWORD), ('Ebp', W.DWORD),
                ('Eip', W.DWORD), ('SegCs', W.DWORD), ('EFlags', W.DWORD), ('Esp', W.DWORD), ('SegSs', W.DWORD),
                ('Ext', ctypes.c_byte * 512)]
class TE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('u', W.DWORD), ('tid', W.DWORD), ('pid', W.DWORD), ('b', W.LONG), ('d', W.LONG),
                ('f', W.DWORD)]
class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', W.DWORD), ('entry', ctypes.c_void_p)]

g = t.Game()
span = int(sys.argv[1], 0) if len(sys.argv) > 1 else 0x3000
hproc = k.OpenProcess(0x0410, False, g.pid)   # query information | vm read

mods = []
arr = (ctypes.c_void_p * 1024)(); need = W.DWORD()
psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x01)   # 32-bit modules
for h in arr[:need.value // ctypes.sizeof(ctypes.c_void_p)]:
    name = ctypes.create_unicode_buffer(260); mi = MI()
    psapi.GetModuleBaseNameW(hproc, ctypes.c_void_p(h), name, 260)
    psapi.GetModuleInformation(hproc, ctypes.c_void_p(h), ctypes.byref(mi), ctypes.sizeof(mi))
    mods.append((mi.base or 0, (mi.base or 0) + mi.size, name.value))
def where(a):
    for lo, hi, n in mods:
        if lo <= a < hi: return f'{n}+{a - lo:x}' if not n.lower().endswith('.exe') else f'{a:x}'
    return None
def after_call(a):
    try: b = g.read(a - 7, 7)
    except Exception: return False
    return b[2] == 0xE8 or b[5] == 0xFF and (b[6] & 0x38) == 0x10 or b[4] == 0xFF and (b[5] & 0x38) == 0x10 or \
        b[1] == 0xFF and (b[2] & 0x38) == 0x10

snap = k.CreateToolhelp32Snapshot(4, 0); e = TE(); e.dwSize = ctypes.sizeof(e)
ok = k.Thread32First(snap, ctypes.byref(e))
while ok:
    if e.pid == g.pid:
        h = k.OpenThread(0x1FFFFF, False, e.tid)
        if h:
            k.Wow64SuspendThread(h)
            c = CTX(); c.ContextFlags = 0x10007
            if k.Wow64GetThreadContext(h, ctypes.byref(c)):
                frames = []
                try: stack = g.read(c.Esp, span)
                except Exception: stack = b''
                for i in range(0, len(stack) - 3, 4):
                    v = int.from_bytes(stack[i:i + 4], 'little')
                    w = where(v)
                    if w and after_call(v): frames.append(f'[+{i:x}] {w}')
                print(f'tid {e.tid} eip {where(c.Eip) or hex(c.Eip)} esp {c.Esp:x}', flush=True)
                for f in frames[:24]: print('   ', f)
            k.ResumeThread(h)
            k.CloseHandle(h)
    ok = k.Thread32Next(snap, ctypes.byref(e))
