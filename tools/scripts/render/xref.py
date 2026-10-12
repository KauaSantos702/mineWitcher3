"""Find RIP-relative references (lea/mov/call/jmp [rip+disp32]) to target RVAs in .text,
map each to its containing function via .pdata, optional disasm around it. Read-only.
usage: xref.py <pe> <rva_hex>[,<rva_hex>...] [--dis N] [--func]
"""
import sys, struct, bisect, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
targets = [int(x, 16) for x in sys.argv[2].split(',')]
dis_n = int(sys.argv[sys.argv.index('--dis') + 1]) if '--dis' in sys.argv else 0
show_func = '--func' in sys.argv
pe = pefile.PE(path, fast_load=True)
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
tva = text.VirtualAddress
code = text.get_data()[:text.Misc_VirtualSize]
base = pe.OPTIONAL_HEADER.ImageBase

# .pdata
pdata = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
pd = pdata.get_data()[:pdata.Misc_VirtualSize]
funcs = []
for i in range(0, len(pd) - 11, 12):
    b, e, u = struct.unpack_from('<III', pd, i)
    if b:
        funcs.append((b, e))
funcs.sort()
fstarts = [f[0] for f in funcs]

def containing(rva):
    i = bisect.bisect_right(fstarts, rva) - 1
    if i >= 0 and funcs[i][0] <= rva < funcs[i][1]:
        return funcs[i]
    return None

import numpy as np
arr = np.frombuffer(code, dtype=np.uint8)
md = Cs(CS_ARCH_X86, CS_MODE_64)
tset = set(targets)
# candidate disp32 positions: any offset p where rva_next = tva+p+4+k ; we brute force: for each p, disp=int32 at p
n = len(code)
disp = np.frombuffer(code[: n - (n % 1)], dtype=np.uint8)
# build int32 view at every offset via stride tricks
from numpy.lib.stride_tricks import as_strided
d32 = as_strided(np.frombuffer(code, dtype=np.uint8), shape=(n - 4, 4), strides=(1, 1))
vals = (d32[:, 0].astype(np.int64) | (d32[:, 1].astype(np.int64) << 8) | (d32[:, 2].astype(np.int64) << 16) | (d32[:, 3].astype(np.int64) << 24))
vals = np.where(vals >= 2**31, vals - 2**32, vals)
pos = np.arange(n - 4, dtype=np.int64)
hits = []
for extra in (0, 1, 4):  # instruction ends at p+4+extra (imm8/imm32 after disp)
    tgt = tva + pos + 4 + extra + vals
    m = np.isin(tgt, np.array(targets, dtype=np.int64))
    for p in np.nonzero(m)[0]:
        hits.append((int(p), int(tgt[p]), extra))
seen = set()
for p, t, extra in sorted(hits):
    # try decoding instruction starting a few bytes back
    for back in range(2, 9):
        st = p - back
        if st < 0:
            continue
        ins = next(md.disasm(code[st:st + 16], tva + st), None)
        if ins and ins.size == back + 4 + extra and ('rip' in ins.op_str):
            key = st
            if key in seen:
                break
            seen.add(key)
            f = containing(tva + st)
            fs = f"func={f[0]:#x}-{f[1]:#x}" if f else 'func=?'
            print(f"ref {t:#x} @ {tva + st:#x} ({fs}): {ins.mnemonic} {ins.op_str}")
            if dis_n:
                s0 = max(0, st - dis_n * 4)
                # disassemble from function start if close, else from st
                start = f[0] - tva if f and st - (f[0] - tva) < 0x400 else st
                for k, i2 in enumerate(md.disasm(code[start:st + dis_n * 6], tva + start)):
                    if i2.address >= tva + st - dis_n * 5:
                        print(f"    {i2.address:#x}: {i2.mnemonic} {i2.op_str}")
            if show_func and f:
                for i2 in md.disasm(code[f[0] - tva:f[1] - tva], f[0]):
                    print(f"    {i2.address:#x}: {i2.mnemonic} {i2.op_str}")
            break
