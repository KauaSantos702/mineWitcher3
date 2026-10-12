"""Find string RVAs of known script natives and RIP-relative LEA/MOV xrefs to them in .text.
Read-only static analysis of witcher3.exe.
Usage: python -I find_native_xrefs.py <exe> name1 name2 ...
"""
import sys, re, struct, pefile

exe = sys.argv[1]
names = sys.argv[2:]
pe = pefile.PE(exe, fast_load=True)
data = open(exe, 'rb').read()
base = pe.OPTIONAL_HEADER.ImageBase

secs = [(s.Name.rstrip(b'\0').decode(), s.VirtualAddress, s.Misc_VirtualSize, s.PointerToRawData, s.SizeOfRawData) for s in pe.sections]
def off2rva(o):
    for n, va, vs, pr, ps in secs:
        if pr <= o < pr + ps:
            return va + (o - pr)
def rva2off(r):
    for n, va, vs, pr, ps in secs:
        if va <= r < va + ps:
            return pr + (r - va)

text = [s for s in secs if s[0] == '.text'][0]
_, tva, tvs, tpr, tps = text
tbytes = data[tpr:tpr + tps]

# Build LEA index once: REX(48/4C) 8D modrm(mod=00,rm=101) disp32
import collections
lea_targets = collections.defaultdict(list)
for m in re.finditer(rb'[\x48\x4C]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]', tbytes):
    i = m.start()
    disp = struct.unpack_from('<i', tbytes, i + 3)[0]
    insn_rva = tva + i
    tgt = insn_rva + 7 + disp
    reg = ((tbytes[i + 2] >> 3) & 7) + (8 if tbytes[i] == 0x4C else 0)
    lea_targets[tgt].append((insn_rva, reg))

REG = ['rax','rcx','rdx','rbx','rsp','rbp','rsi','rdi','r8','r9','r10','r11','r12','r13','r14','r15']
for name in names:
    for enc, label in ((name.encode() + b'\0', 'ascii'), (name.encode('utf-16le') + b'\0\0', 'utf16')):
        for m in re.finditer(re.escape(b'\0' + enc), data):
            o = m.start() + 1
            r = off2rva(o)
            sec = [s[0] for s in secs if s[1] <= r < s[1] + s[4]]
            xr = lea_targets.get(r, [])
            print(f"{name} [{label}] rva=0x{r:x} sec={sec} xrefs={len(xr)} " +
                  ' '.join(f"0x{a:x}({REG[g]})" for a, g in xr[:8]))
