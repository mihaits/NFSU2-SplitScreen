"""Sampling profiler for the game's main thread: where the frame time goes.
python -I hotspots.py [seconds=12] [--stage n] [--wait s]    wait up to s (30) for race stage n (3 = countdown)
Each sample suspends the main thread briefly and reads EIP and the return addresses on its stack, so the game runs
a little slower while it samples. Prints a line per second, then time by module (self), and the hottest return addresses
inclusively (a sample counts once per address), which shows the game functions the time is spent under, and every
thread's CPU use over the run."""
import collections, os, re, sys, time, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t, paths

k = ctypes.WinDLL('kernel32', use_last_error=True); k.OpenThread.restype = W.HANDLE
psapi = ctypes.WinDLL('psapi', use_last_error=True)

class CTX(ctypes.Structure):    # WOW64_CONTEXT
    _fields_ = [('ContextFlags', W.DWORD), ('Dr', W.DWORD * 6), ('Float', ctypes.c_byte * 112), ('Seg', W.DWORD * 4),
                ('Edi', W.DWORD), ('Esi', W.DWORD), ('Ebx', W.DWORD), ('Edx', W.DWORD), ('Ecx', W.DWORD), ('Eax', W.DWORD),
                ('Ebp', W.DWORD), ('Eip', W.DWORD), ('SegCs', W.DWORD), ('EFlags', W.DWORD), ('Esp', W.DWORD),
                ('SegSs', W.DWORD), ('Ext', ctypes.c_byte * 512)]
class TE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('u', W.DWORD), ('tid', W.DWORD), ('pid', W.DWORD), ('b', W.LONG), ('d', W.LONG),
                ('f', W.DWORD)]
class MI(ctypes.Structure):
    _fields_ = [('base', ctypes.c_void_p), ('size', W.DWORD), ('entry', ctypes.c_void_p)]

g = t.Game()
seconds = float(sys.argv[1]) if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else 12.0
stage = int(sys.argv[sys.argv.index('--stage') + 1]) if '--stage' in sys.argv else None
hproc = k.OpenProcess(0x0410, False, g.pid)
arr = (ctypes.c_void_p * 1024)(); need = W.DWORD()
psapi.EnumProcessModulesEx(hproc, arr, ctypes.sizeof(arr), ctypes.byref(need), 0x01)
mods = []
for h in arr[:need.value // ctypes.sizeof(ctypes.c_void_p)]:
    name = ctypes.create_unicode_buffer(260); mi = MI()
    psapi.GetModuleBaseNameW(hproc, ctypes.c_void_p(h), name, 260)
    psapi.GetModuleInformation(hproc, ctypes.c_void_p(h), ctypes.byref(mi), ctypes.sizeof(mi))
    mods.append((mi.base or 0, (mi.base or 0) + mi.size, name.value))


def module(a):
    for lo, hi, n in mods:
        if lo <= a < hi: return n
    return '?'


def main_tid():
    log = open(os.path.join(paths.GAME, 'scripts', 'NFSU2SplitScreen.log'), errors='replace').read()
    m = re.search(r'\(main (\d+)\)', log)
    return int(m.group(1))


asi_lo = next(lo for lo, hi, n in mods if n == 'NFSU2SplitScreen.asi')
asi_hi = next(hi for lo, hi, n in mods if n == 'NFSU2SplitScreen.asi')
syms = []
for line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'NFSU2SplitScreen.map'), errors='replace'):
    m = re.match(r'\s+[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{8})\s', line)
    if m: syms.append((int(m.group(2), 16) - 0x10000000, m.group(1)))
syms.sort()
def symbol(a):
    import bisect
    i = bisect.bisect_right(syms, (a - asi_lo, '~')) - 1
    return re.sub(r'^\?|@.*$', '', syms[i][1])[:44] if i >= 0 else hex(a)


call_cache = {}
def after_call(a):
    r = call_cache.get(a)
    if r is None:
        try:
            b = g.read(a - 7, 7)
            r = b[2] == 0xE8 or b[5] == 0xFF and (b[6] & 0x38) == 0x10 or b[4] == 0xFF and (b[5] & 0x38) == 0x10 or \
                b[1] == 0xFF and (b[2] & 0x38) == 0x10
        except Exception: r = False
        call_cache[a] = r
    return r


def race_stage():
    mgr = g.u32(0x890118)
    return g.u32(mgr) if mgr and g.u32(0x8654A4) == 6 else -1


if stage is not None:
    end = time.time() + (float(sys.argv[sys.argv.index('--wait') + 1]) if '--wait' in sys.argv else 30)
    while race_stage() != stage and time.time() < end: time.sleep(0.05)
    if race_stage() != stage: sys.exit(f'FAIL: stage {stage} not reached')
def thread_times():
    out = {}
    snap = k.CreateToolhelp32Snapshot(4, 0); e = TE(); e.dwSize = ctypes.sizeof(e)
    ok = k.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.pid == g.pid:
            th = k.OpenThread(0x0040, False, e.tid)     # query information
            if th:
                a, b, c1, d = W.FILETIME(), W.FILETIME(), W.FILETIME(), W.FILETIME()
                start = ctypes.c_void_p()
                ntdll.NtQueryInformationThread(th, 9, ctypes.byref(start), ctypes.sizeof(start), None)
                if k.GetThreadTimes(th, ctypes.byref(a), ctypes.byref(b), ctypes.byref(c1), ctypes.byref(d)):
                    ft = lambda f: (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7
                    out[e.tid] = (ft(c1), ft(d), start.value or 0)
                k.CloseHandle(th)
        ok = k.Thread32Next(snap, ctypes.byref(e))
    k.CloseHandle(snap)
    return out


ntdll = ctypes.WinDLL('ntdll')
times0 = thread_times()
h = k.OpenThread(0x1FFFFF, False, main_tid())
if not h: sys.exit('FAIL: no main thread')
self_mod, incl = collections.Counter(), collections.Counter()
own, own_incl, waits = collections.Counter(), collections.Counter(), collections.Counter()
n, start, last = 0, time.time(), 0
while time.time() - start < seconds:
    k.Wow64SuspendThread(h)
    c = CTX(); c.ContextFlags = 0x10007
    ok = k.Wow64GetThreadContext(h, ctypes.byref(c))
    stack = b''
    if ok:
        try: stack = g.read(c.Esp, 0x1800)
        except Exception: pass
    k.ResumeThread(h)
    if not ok: continue
    n += 1
    self_mod[module(c.Eip)] += 1
    seen = set()
    for i in range(0, len(stack) - 3, 4):
        v = int.from_bytes(stack[i:i + 4], 'little')
        if 0x401000 <= v < 0x790000 and v not in seen and after_call(v): seen.add(v)
    incl.update(seen)
    if asi_lo <= c.Eip < asi_hi: own[symbol(c.Eip)] += 1
    chain, oseen = [], set()
    for i in range(0, len(stack) - 3, 4):
        v = int.from_bytes(stack[i:i + 4], 'little')
        if asi_lo <= v < asi_hi and after_call(v):
            oseen.add(symbol(v))
            if len(chain) < 1: chain.append(symbol(v))
    own_incl.update(oseen)
    if module(c.Eip) == 'ntdll.dll':
        first = next((int.from_bytes(stack[i:i + 4], 'little') for i in range(0, len(stack) - 3, 4)
                      if module(int.from_bytes(stack[i:i + 4], 'little')) not in ('ntdll.dll', 'KERNELBASE.dll', 'KERNEL32.DLL', '?')
                      and after_call(int.from_bytes(stack[i:i + 4], 'little'))), 0)
        waits[f'{module(first)} {symbol(first) if asi_lo <= first < asi_hi else hex(first)}'] += 1
    if int(time.time() - start) != last:
        last = int(time.time() - start)
        print(f'{time.strftime("%X")} {n} samples, stage {race_stage()}', flush=True)
    time.sleep(0.003)
k.CloseHandle(h)
times1 = thread_times(); span = time.time() - start
use = sorted(((times1[i][0] + times1[i][1] - times0[i][0] - times0[i][1]) / span, i) for i in times1 if i in times0)[::-1]
print('\nCPU by thread, % of one core:')
for u, i in use[:10]:
    a = times1[i][2]
    print(f'  {100 * u:5.1f}%  tid {i}{" (main)" if i == main_tid() else ""} {module(a)} {symbol(a) if asi_lo <= a < asi_hi else hex(a)}')
print(f'\n{n} samples\nself time by module:')
for m, c in self_mod.most_common(10): print(f'  {100 * c / n:5.1f}%  {m}')
print('this plugin, self:')
for m, c in own.most_common(12): print(f'  {100 * c / n:5.1f}%  {m}')
print('this plugin, inclusive:')
for m, c in own_incl.most_common(16): print(f'  {100 * c / n:5.1f}%  {m}')
print('ntdll self time, first caller:')
for m, c in waits.most_common(10): print(f'  {100 * c / n:5.1f}%  {m}')
print('inclusive, return addresses in speed2.exe:')
for a, c in incl.most_common(36): print(f'  {100 * c / n:5.1f}%  {a:#x}')
