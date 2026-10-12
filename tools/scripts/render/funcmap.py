"""Summarize a function (all chained pdata fragments): calls + string refs. usage: funcmap.py <pe> <primary_rva> [--all]"""
import sys, struct, re, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True); P = int(sys.argv[2], 16); ALL = '--all' in sys.argv
pdata = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
pd = pdata.get_data()[:pdata.Misc_VirtualSize]
ents = sorted(e for e in struct.iter_unpack('<III', pd[:len(pd)//12*12]) if e[0])
cache = {}
def primary(b, u):
    while True:
        hdr = pe.get_data(u, 4)
        if not (hdr[0] >> 3) & 4: return b
        off = u + 4 + ((hdr[2] + 1) & ~1) * 2
        b, e, u = struct.unpack('<III', pe.get_data(off, 12))
frags = [(b, e) for b, e, u in ents if (b == P or (b > P and b < P + 0x200000 and primary(b, u) == P))]
rd = next(s for s in pe.sections if s.Name.startswith(b'.rdata'))
def sref(t):
    if rd.VirtualAddress <= t < rd.VirtualAddress + rd.Misc_VirtualSize:
        d = pe.get_data(t, 120)
        m = re.match(rb'[\x20-\x7e]{4,}', d)
        if m: return 'A"' + m.group().decode() + '"'
        m = re.match(rb'(?:[\x20-\x7e]\x00){4,}', d)
        if m: return 'W"' + m.group().decode('utf-16le') + '"'
    return None
md = Cs(CS_ARCH_X86, CS_MODE_64)
for b, e in frags:
    print(f"-- fragment {b:#x}-{e:#x}")
    for i in md.disasm(pe.get_data(b, e - b), b):
        txt = f"{i.address:#x}: {i.mnemonic} {i.op_str}"
        note = ''
        if 'rip +' in i.op_str or 'rip -' in i.op_str:
            m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', i.op_str)
            t = i.address + i.size + (int(m.group(2), 16) * (1 if m.group(1) == '+' else -1))
            s = sref(t)
            note = f"  ; ->{t:#x} {s or ''}"
            if s: print(txt + note); continue
        if ALL or i.mnemonic in ('call', 'jmp') and not i.op_str.startswith('0x1') is False or i.mnemonic == 'call':
            print(txt + note)
