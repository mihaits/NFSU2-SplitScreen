"""Instructions in .text whose operand is an address in [lo, hi) (disp32/imm32), decoded properly.
python refs.py lo hi"""
import sys, struct, capstone, sdis
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
d = sdis.d
for vs, sva, rs, raw in sdis.secs:
    if sva != 0x1000: continue
    for off in range(raw, raw + rs - 4):
        v = struct.unpack_from('<I', d, off)[0]
        if not lo <= v < hi: continue
        va = sdis.base + sva + off - raw
        for back in range(1, 8):
            ins = next(md.disasm(d[off - back:off - back + 16], va - back), None)
            if ins and ins.address + ins.size >= va + 4 and hex(v) in ins.op_str:
                print(f'{ins.address:08x} {ins.mnemonic} {ins.op_str}'); break
