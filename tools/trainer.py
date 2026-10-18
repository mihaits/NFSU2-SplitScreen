"""Tiny external trainer toolkit for NFSU2 (speed2.exe v1.2, 32-bit, base 0x400000).

Process memory r/w, code caves with jmp hooks, keyboard input via SendInput, window screenshots to PNG.
Used for automated experiments; the eventual mod is an ASI plugin.
"""
import ctypes, ctypes.wintypes as wt, struct, time, zlib

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32 = ctypes.WinDLL('user32', use_last_error=True)
gdi = ctypes.WinDLL('gdi32', use_last_error=True)

k32.VirtualAllocEx.restype = ctypes.c_void_p
k32.VirtualAllocEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualProtectEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.FlushInstructionCache.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_size_t]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD),
                ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD), ('szExeFile', ctypes.c_char * 260)]


def find_pid(name=b'speed2.exe'):
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32First(snap, ctypes.byref(e))
    try:
        while ok:
            if e.szExeFile.lower() == name:
                return e.th32ProcessID
            ok = k32.Process32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)


def rel32(src, dst):
    """rel32 operand for a 5-byte jmp/call located at src."""
    return struct.pack('<i', dst - (src + 5))


class Game:
    def __init__(self, pid=None):
        self.pid = pid or find_pid()
        if not self.pid:
            raise RuntimeError('speed2.exe not running')
        self.h = k32.OpenProcess(0x1F0FFF, False, self.pid)
        if not self.h:
            raise OSError(ctypes.get_last_error(), 'OpenProcess')

    # ---- memory
    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n); got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, addr, buf, n, ctypes.byref(got)):
            raise OSError(ctypes.get_last_error(), f'read {addr:#x}')
        return buf.raw

    def write(self, addr, data, code=False):
        old = wt.DWORD()
        if code:
            k32.VirtualProtectEx(self.h, addr, len(data), 0x40, ctypes.byref(old))
        got = ctypes.c_size_t()
        ok = k32.WriteProcessMemory(self.h, addr, data, len(data), ctypes.byref(got))
        if code:
            k32.VirtualProtectEx(self.h, addr, len(data), old.value, ctypes.byref(old))
            k32.FlushInstructionCache(self.h, addr, len(data))
        if not ok:
            raise OSError(ctypes.get_last_error(), f'write {addr:#x}')

    def u32(self, a): return struct.unpack('<I', self.read(a, 4))[0]
    def i32(self, a): return struct.unpack('<i', self.read(a, 4))[0]
    def u8(self, a): return self.read(a, 1)[0]
    def w32(self, a, v): self.write(a, struct.pack('<I', v & 0xFFFFFFFF))
    def w8(self, a, v): self.write(a, bytes([v & 0xFF]))

    def alloc(self, size=0x1000):
        p = k32.VirtualAllocEx(self.h, None, size, 0x3000, 0x40)
        if not p:
            raise OSError(ctypes.get_last_error(), 'VirtualAllocEx')
        self.write(p, b'\0' * size)
        return p

    def hook(self, at, stolen_len, body, cave):
        """Detour `at` to `cave`: cave runs `body` (bytes, position independent or built for `cave`),
        then the original `stolen_len` bytes, then jumps back. Returns the original bytes for unhook."""
        orig = self.read(at, stolen_len)
        code = body + orig
        code += b'\xE9' + rel32(cave + len(code), at + stolen_len)
        self.write(cave, code)
        self.write(at, b'\xE9' + rel32(at, cave) + b'\x90' * (stolen_len - 5), code=True)
        return orig

    def unhook(self, at, orig):
        self.write(at, orig, code=True)

    # ---- window / input
    def hwnd(self, cls_name='GameFrame'):
        found = []
        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def cb(h, _):
            pid = wt.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            if pid.value == self.pid and u32.IsWindowVisible(h):
                found.append(h)
            return True
        u32.EnumWindows(cb, 0)
        def cls(h):
            b = ctypes.create_unicode_buffer(64); u32.GetClassNameW(h, b, 64); return b.value
        # the game's own window class; other visible windows belong to wrappers (e.g. D3DProxyWindow)
        game = [h for h in found if cls(h) == cls_name]
        return game[0] if game else (found[0] if found else None)

    def focus(self):
        h = self.hwnd()
        # Alt tap lets a background process take foreground
        u32.keybd_event(0x12, 0, 0, 0); u32.keybd_event(0x12, 0, 2, 0)
        u32.SetForegroundWindow(h)
        if u32.IsIconic(h):
            u32.ShowWindow(h, 9)          # SW_RESTORE; fullscreen game minimizes when it loses focus
        # wait for the device to come back and DirectInput to reacquire
        for _ in range(50):
            if not u32.IsIconic(h) and u32.GetForegroundWindow() == h:
                break
            time.sleep(0.1)
        time.sleep(1.5)
        return u32.GetForegroundWindow() == h and not u32.IsIconic(h)

    def click(self, fx, fy):
        """Left-click at a point given as fractions of the game window's client area."""
        h = self.hwnd()
        r = wt.RECT(); u32.GetClientRect(h, ctypes.byref(r))
        pt = wt.POINT(int(r.right * fx), int(r.bottom * fy)); u32.ClientToScreen(h, ctypes.byref(pt))
        u32.SetCursorPos(pt.x, pt.y); time.sleep(0.1)
        u32.mouse_event(2, 0, 0, 0, 0); time.sleep(0.08); u32.mouse_event(4, 0, 0, 0, 0)

    def screenshot(self, path, max_w=1280):
        """Capture the game window (PrintWindow w/ PW_RENDERFULLCONTENT) and save a downscaled PNG."""
        # D3D output is only capturable from the wrapper's proxy window; fall back to the game window
        h = self.hwnd('D3DProxyWindow')
        r = wt.RECT(); u32.GetClientRect(h, ctypes.byref(r))
        w, hgt = r.right, r.bottom
        hdc = u32.GetDC(h); mdc = gdi.CreateCompatibleDC(hdc)
        bmp = gdi.CreateCompatibleBitmap(hdc, w, hgt); gdi.SelectObject(mdc, bmp)
        if not u32.PrintWindow(h, mdc, 3):
            gdi.BitBlt(mdc, 0, 0, w, hgt, hdc, 0, 0, 0x00CC0020)
        class BIH(ctypes.Structure):
            _fields_ = [('biSize', wt.DWORD), ('biWidth', ctypes.c_long), ('biHeight', ctypes.c_long),
                        ('biPlanes', wt.WORD), ('biBitCount', wt.WORD), ('biCompression', wt.DWORD),
                        ('biSizeImage', wt.DWORD), ('a', ctypes.c_long), ('b', ctypes.c_long),
                        ('c', wt.DWORD), ('d', wt.DWORD)]
        bih = BIH(); bih.biSize = ctypes.sizeof(BIH); bih.biWidth = w; bih.biHeight = -hgt
        bih.biPlanes = 1; bih.biBitCount = 32
        buf = ctypes.create_string_buffer(w * hgt * 4)
        gdi.GetDIBits(mdc, bmp, 0, hgt, buf, ctypes.byref(bih), 0)
        gdi.DeleteObject(bmp); gdi.DeleteDC(mdc); u32.ReleaseDC(h, hdc)
        step = max(1, -(-w // max_w))
        raw = buf.raw; ow = len(range(0, w, step)); rows = []
        for y in range(0, hgt, step):
            line = bytearray(b'\0')
            base = y * w * 4
            px = raw[base: base + w * 4]
            for x in range(0, w, step):
                b, g, rr = px[x * 4], px[x * 4 + 1], px[x * 4 + 2]
                line += bytes((rr, g, b))
            rows.append(bytes(line))
        oh = len(rows)
        def chunk(t, d):
            return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
        png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, oh, 8, 2, 0, 0, 0))
               + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))
        open(path, 'wb').write(png)
        return w, hgt


# ---- keyboard (scan codes, so DirectInput sees them)
class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wt.WORD), ('wScan', wt.WORD), ('dwFlags', wt.DWORD), ('time', wt.DWORD),
                ('dwExtraInfo', ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('ki', KEYBDINPUT), ('pad', ctypes.c_byte * 32)]
    _anonymous_ = ('u',)
    _fields_ = [('type', wt.DWORD), ('u', _U)]


SC = dict(f2=0x3C, enter=0x1C, esc=0x01, up=(0x48, True), down=(0x50, True), left=(0x4B, True), right=(0x4D, True),
          space=0x39, q=0x10, e=0x12)


def key(name, hold=0.08):
    sc = SC[name]; ext = False
    if isinstance(sc, tuple):
        sc, ext = sc
    flags = 0x0008 | (0x0001 if ext else 0)  # KEYEVENTF_SCANCODE (+EXTENDED)
    for up in (0, 0x0002):
        i = INPUT(type=1); i.ki = KEYBDINPUT(0, sc, flags | up, 0, 0)
        u32.SendInput(1, ctypes.byref(i), ctypes.sizeof(INPUT))
        time.sleep(hold)
