"""Find direct call/jmp rel32 (E8/E9) to target RVAs; print containing func and N preceding instrs.
usage: callers.py <pe> <rva>[,<rva>] [N]"""
import sys, struct, bisect, pefile
import numpy as np
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True)
targets = [int(x, 16) for x in sys.argv[2].split(',')]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 0
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
tva = text.VirtualAddress; code = text.get_data()[:text.Misc_VirtualSize]
pdata = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
pd = pdata.get_data()[:pdata.Misc_VirtualSize]
funcs = sorted((b, e) for b, e, u in struct.iter_unpack('<III', pd[:len(pd)//12*12]) if b)
fst = [f[0] for f in funcs]
def cont(r):
    i = bisect.bisect_right(fst, r) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= r < funcs[i][1] else None
a = np.frombuffer(code, dtype=np.uint8); n = len(a)
d = (a[1:n-3].astype(np.int64) | (a[2:n-2].astype(np.int64) << 8) | (a[3:n-1].astype(np.int64) << 16) | (a[4:n].astype(np.int64) << 24))
d = np.where(d >= 2**31, d - 2**32, d)
p = np.arange(n - 4, dtype=np.int64)
tgt = tva + p + 5 + d
op = a[:n-4]
m = np.isin(tgt, np.array(targets)) & ((op == 0xE8) | (op == 0xE9))
md = Cs(CS_ARCH_X86, CS_MODE_64)
for q in np.nonzero(m)[0]:
    q = int(q); r = tva + q; f = cont(r)
    print(f"{'call' if code[q]==0xE8 else 'jmp '} -> {int(tgt[q]):#x} from {r:#x} func={f[0]:#x}" if f else f"from {r:#x} func=?")
    if N and f:
        ins = [i for i in md.disasm(code[f[0]-tva:q+5], f[0])]
        for i in ins[-N:]:
            print(f"     {i.address:#x}: {i.mnemonic} {i.op_str}")
