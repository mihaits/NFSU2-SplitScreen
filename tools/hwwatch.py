"""Hardware data breakpoints on the running game: who writes (or reads) an address. Attaches as a debugger, puts up to
4 addresses in DR0-3 of every game thread and prints each new code site that triggers, live (eip = the instruction
AFTER the access, as for every data breakpoint; then the code addresses found on the stack, newest first).
python -I hwwatch.py addr[,addr...] [--size 1|2|4] [--rw] [--seconds 10] [--max 200] [--all]
  --rw       break on reads too (default: writes only)
  --all      print every hit, not only the first of each site
Detaches cleanly on Ctrl+C or when time/hits run out (breakpoints are cleared first). Killing it hard (Task Manager)
leaves the breakpoints armed and the game crashes on the next access: stop it with Ctrl+C."""
import argparse, collections, ctypes, ctypes.wintypes as W, os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t

ap = argparse.ArgumentParser()
ap.add_argument('addrs')
ap.add_argument('--size', type=int, default=4, choices=(1, 2, 4))
ap.add_argument('--rw', action='store_true')
ap.add_argument('--seconds', type=float, default=10)
ap.add_argument('--max', type=int, default=200)
ap.add_argument('--all', action='store_true')
a = ap.parse_args()
ADDRS = [int(x, 16) for x in a.addrs.split(',')]
assert 1 <= len(ADDRS) <= 4, 'at most 4 addresses'
assert all(x % a.size == 0 for x in ADDRS), 'addresses must be aligned to --size'

k = ctypes.WinDLL('kernel32', use_last_error=True)
k.OpenThread.restype = W.HANDLE

class CTX(ctypes.Structure):    # WOW64_CONTEXT
    _fields_ = [('ContextFlags', W.DWORD), ('Dr0', W.DWORD), ('Dr1', W.DWORD), ('Dr2', W.DWORD), ('Dr3', W.DWORD),
                ('Dr6', W.DWORD), ('Dr7', W.DWORD), ('Float', ctypes.c_byte * 112), ('SegGs', W.DWORD),
                ('SegFs', W.DWORD), ('SegEs', W.DWORD), ('SegDs', W.DWORD), ('Edi', W.DWORD), ('Esi', W.DWORD),
                ('Ebx', W.DWORD), ('Edx', W.DWORD), ('Ecx', W.DWORD), ('Eax', W.DWORD), ('Ebp', W.DWORD),
                ('Eip', W.DWORD), ('SegCs', W.DWORD), ('EFlags', W.DWORD), ('Esp', W.DWORD), ('SegSs', W.DWORD),
                ('Ext', ctypes.c_byte * 512)]

class EXREC(ctypes.Structure):
    _fields_ = [('Code', W.DWORD), ('Flags', W.DWORD), ('Rec', ctypes.c_void_p), ('Addr', ctypes.c_void_p),
                ('N', W.DWORD), ('Info', ctypes.c_size_t * 15)]

class DE(ctypes.Structure):     # DEBUG_EVENT; only the exception part of the union is read
    _fields_ = [('Code', W.DWORD), ('Pid', W.DWORD), ('Tid', W.DWORD), ('Ex', EXREC), ('First', W.DWORD),
                ('rest', ctypes.c_byte * 64)]

CTX_DEBUG, CTX_ALL = 0x10010, 0x1001F   # WOW64_CONTEXT_DEBUG_REGISTERS; control | integer | segments | debug
DBG_CONTINUE, DBG_NOT_HANDLED = 0x00010002, 0x80010001
LEN = {1: 0, 2: 1, 4: 3}[a.size]
DR7 = 0
for i in range(len(ADDRS)):
    DR7 |= (1 << (2 * i)) | ((3 if a.rw else 1) << (16 + 4 * i)) | (LEN << (18 + 4 * i))

def set_drs(tid, on):
    h = k.OpenThread(0x1FFFFF, 0, tid)
    if not h: return False
    c = CTX(); c.ContextFlags = CTX_DEBUG
    ok = k.Wow64GetThreadContext(h, ctypes.byref(c))
    if ok:
        for i in range(4): setattr(c, 'Dr%d' % i, ADDRS[i] if on and i < len(ADDRS) else 0)
        c.Dr7 = DR7 if on else 0; c.Dr6 = 0
        ok = k.Wow64SetThreadContext(h, ctypes.byref(c))
    k.CloseHandle(h)
    return bool(ok)

g = t.Game()
threads = set()
armed = False
sites = collections.Counter()
hits = 0
assert k.DebugActiveProcess(g.pid), 'DebugActiveProcess failed: %d' % ctypes.get_last_error()
k.DebugSetProcessKillOnExit(0)
print('watching %s (%s, %d bytes) for %.0f s / %d hits' % (', '.join(hex(x) for x in ADDRS),
      'read/write' if a.rw else 'write', a.size, a.seconds, a.max), flush=True)
de = DE(); t0 = time.time()
try:
    while time.time() - t0 < a.seconds and hits < a.max:
        if not k.WaitForDebugEvent(ctypes.byref(de), 200): continue
        st = DBG_CONTINUE
        if de.Code in (2, 3):                                   # thread / process created (also reported on attach)
            threads.add(de.Tid)
            if armed: set_drs(de.Tid, True)
        elif de.Code == 4:                                      # thread exit
            threads.discard(de.Tid)
        elif de.Code == 5:                                      # process exit
            print('game exited', flush=True); break
        elif de.Code == 1:
            code = de.Ex.Code
            if code in (0x80000003, 0x4000001F):                # the attach breakpoint: every thread is known now
                if not armed:
                    n = sum(set_drs(tid, True) for tid in threads); armed = True
                    print('armed %d threads' % n, flush=True)
            elif code in (0x80000004, 0x4000001E):              # single step: ours if DR6 says so
                h = k.OpenThread(0x1FFFFF, 0, de.Tid); c = CTX(); c.ContextFlags = CTX_ALL
                k.Wow64GetThreadContext(h, ctypes.byref(c))
                which = [i for i in range(len(ADDRS)) if c.Dr6 & (1 << i)]
                if which:
                    hits += 1
                    try: stk = struct.unpack('<128I', g.read(c.Esp, 0x200))
                    except OSError: stk = ()
                    rets = tuple(hex(x) for x in stk if 0x401000 <= x < 0x7A0000)[:10]
                    key = (hex(c.Eip),) + rets
                    sites[key] += 1
                    if sites[key] == 1 or a.all:
                        vals = ' '.join('%s=%#x' % (hex(ADDRS[i]), g.u32(ADDRS[i])) for i in which)
                        print('%s%s tid %d eip %#x  %s  eax %#x ecx %#x edx %#x ebx %#x esi %#x edi %#x  stack %s' % (
                            'new site ' if sites[key] == 1 else 'hit ', '[%d]' % hits, de.Tid, c.Eip, vals, c.Eax,
                            c.Ecx, c.Edx, c.Ebx, c.Esi, c.Edi, ' '.join(rets)), flush=True)
                    c.Dr6 = 0; k.Wow64SetThreadContext(h, ctypes.byref(c))
                else:
                    st = DBG_NOT_HANDLED
                k.CloseHandle(h)
            else:
                st = DBG_NOT_HANDLED                            # the game's own exceptions go to the game
        k.ContinueDebugEvent(de.Pid, de.Tid, st)
except KeyboardInterrupt:
    pass
finally:
    cleared = sum(set_drs(tid, False) for tid in threads)
    for _ in range(20):                                         # let pending events through before detaching
        if k.WaitForDebugEvent(ctypes.byref(de), 50): k.ContinueDebugEvent(de.Pid, de.Tid, DBG_CONTINUE)
    print('cleared %d threads, detached: %s' % (cleared, bool(k.DebugActiveProcessStop(g.pid))), flush=True)
print('%d hits, %d sites' % (hits, len(sites)))
for key, n in sites.most_common(): print('%6d  %s' % (n, ' '.join(key)))
