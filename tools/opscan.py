"""List .text instructions whose operand text contains a substring. python opscan.py substr [lo hi]"""
import sys, capstone, sdis
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.skipdata = True
sub = sys.argv[1].lower()
lo = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x401000
hi = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x75C000
for ins in md.disasm(sdis.read(lo, hi - lo), lo):
    if sub in ins.op_str.lower(): print(f'{ins.address:08x} {ins.mnemonic} {ins.op_str}')
