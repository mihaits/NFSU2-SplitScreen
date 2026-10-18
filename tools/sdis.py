"""Disassemble speed2.exe from the file (no running game needed). python sdis.py addr [count]"""
import os, struct, sys, capstone
EXE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'speed2.exe')
d = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3C)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]; opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 52)[0]
secs = [struct.unpack_from('<IIII', d, pe + 24 + opt + i * 40 + 8) for i in range(nsec)]   # vsize, va, rawsize, raw

def read(va, n):
    for vs, sva, rs, raw in secs:
        if base + sva <= va < base + sva + max(vs, rs):
            o = raw + va - base - sva
            return d[o:o + n]
    raise ValueError(hex(va))

if __name__ == '__main__':
    a = int(sys.argv[1], 16); n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i, ins in enumerate(md.disasm(read(a, n * 8), a)):
        if i >= n: break
        print(f'{ins.address:08x}  {ins.mnemonic} {ins.op_str}')
