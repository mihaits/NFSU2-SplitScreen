"""List call/jmp rel32 sites to a function with a little context. python callers.py addr"""
import sys, re, struct, capstone, sdis
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
tgt = int(sys.argv[1], 16); d = sdis.d
for vs, sva, rs, raw in sdis.secs:
    seg = d[raw:raw + rs]; va0 = sdis.base + sva
    for m in re.finditer(b'[\xe8\xe9]', seg):
        o = m.start()
        if o + 5 > len(seg): continue
        if va0 + o + 5 + struct.unpack_from('<i', seg, o + 1)[0] == tgt:
            a = va0 + o
            ins = list(md.disasm(sdis.read(a - 30, 46), a - 30))
            print(hex(a), ' | '.join(f'{i.mnemonic} {i.op_str}' for i in ins if a - 20 <= i.address <= a))
