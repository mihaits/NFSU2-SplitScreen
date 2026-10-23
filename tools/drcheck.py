"""Lists game threads that still have hardware breakpoints armed (DR7), e.g. after a hwwatch.py run that reported
fewer cleared than armed threads. python -I drcheck.py [--clear]  (--clear zeroes DR0-3/6/7 on those threads)."""
import os, sys, ctypes, ctypes.wintypes as W
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import trainer as t
k = ctypes.WinDLL('kernel32', use_last_error=True); k.OpenThread.restype = W.HANDLE
class CTX(ctypes.Structure):
    _fields_ = [('ContextFlags', W.DWORD), ('Dr0', W.DWORD), ('Dr1', W.DWORD), ('Dr2', W.DWORD), ('Dr3', W.DWORD),
                ('Dr6', W.DWORD), ('Dr7', W.DWORD), ('rest', ctypes.c_byte * 700)]
class TE(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('u', W.DWORD), ('tid', W.DWORD), ('pid', W.DWORD), ('b', W.LONG), ('d', W.LONG), ('f', W.DWORD)]
pid = t.find_pid(); clear = '--clear' in sys.argv
snap = k.CreateToolhelp32Snapshot(4, 0); e = TE(); e.dwSize = ctypes.sizeof(e); ok = k.Thread32First(snap, ctypes.byref(e)); n = 0
while ok:
    if e.pid == pid:
        n += 1
        h = k.OpenThread(0x1FFFFF, False, e.tid)
        c = CTX(); c.ContextFlags = 0x10010
        if h and k.Wow64GetThreadContext(h, ctypes.byref(c)) and (c.Dr7 & 0xFF):
            print('tid', e.tid, 'DR7', hex(c.Dr7), hex(c.Dr0), hex(c.Dr1), hex(c.Dr2), hex(c.Dr3))
            if clear:
                k.SuspendThread(h); c.Dr7 = 0; c.Dr0 = c.Dr1 = c.Dr2 = c.Dr3 = 0; c.Dr6 = 0
                print(' cleared', bool(k.Wow64SetThreadContext(h, ctypes.byref(c)))); k.ResumeThread(h)
        if h: k.CloseHandle(h)
    ok = k.Thread32Next(snap, ctypes.byref(e))
print('threads', n)
