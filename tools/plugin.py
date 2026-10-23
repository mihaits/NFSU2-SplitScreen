"""The loaded plugin's globals for the live tools: its module base, map symbols and the local player array."""
import ctypes, ctypes.wintypes as W, os, re
import paths

k32 = ctypes.WinDLL('kernel32')
class ME(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('id', W.DWORD), ('pid', W.DWORD), ('g', W.DWORD), ('p', W.DWORD),
                ('base', ctypes.c_void_p), ('size', W.DWORD), ('h', W.HMODULE), ('name', ctypes.c_char * 256),
                ('path', ctypes.c_char * 260)]

def module_base(pid, name=b'nfsu2splitscreen.asi'):
    snap = k32.CreateToolhelp32Snapshot(0x18, pid); e = ME(); e.dwSize = ctypes.sizeof(e)
    ok = k32.Module32First(snap, ctypes.byref(e))
    try:
        while ok:
            if e.name.lower() == name: return e.base
            ok = k32.Module32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)

def symbol(name):
    """VA of a plugin global from the linker map (preferred base 0x10000000)."""
    for line in open(os.path.join(paths.HERE, '..', 'build', 'NFSU2SplitScreen.map')):
        if name in line: return int(line.split()[2], 16)
    raise KeyError(name)

def max_players():
    """MAX_LOCAL_PLAYERS of the checkout that built the map (4 on main, 6 on 6p-support): the plugin's array size."""
    src = open(os.path.join(paths.HERE, '..', 'src', 'parts', 'core.cpp')).read()
    return int(re.search(r'#define MAX_LOCAL_PLAYERS (\d+)', src).group(1))

class LocalPlayers:
    """g_localPlayers {byIndex[MAX], inOrder[MAX], count} in the running game."""
    def __init__(self, g):
        self.g, self.max = g, max_players()
        self.addr = symbol('?g_localPlayers@@') - 0x10000000 + module_base(g.pid)

    def __call__(self):
        """The player objects by index."""
        n = min(self.g.u32(self.addr + 8 * self.max), self.max)
        return [self.g.u32(self.addr + 4 * k) for k in range(n)]
