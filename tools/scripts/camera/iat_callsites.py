# Encontra call/jmp [rip+X] para entradas IAT de interesse e desmonta o contexto anterior.
import sys, re, pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
path = sys.argv[1]; want = sys.argv[2].split(',') ; ctx = int(sys.argv[3]) if len(sys.argv)>3 else 0
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
iat = {}
for e in pe.DIRECTORY_ENTRY_IMPORT:
    for i in e.imports:
        if i.name and i.name.decode() in want: iat[i.address-base] = i.name.decode()
text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
code = text.get_data(); tva = text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64)
hits = []
for m in re.finditer(rb'(?:\xff\x15|\xff\x25|\x48\xff\x15|\x48\x8b\x05|\x48\x8b\x1d|\x48\x8b\x35|\x48\x8b\x3d|\x4c\x8b\x05|\x4c\x8b\x0d|\x4c\x8b\x15|\x4c\x8b\x1d|\x4c\x8b\x25|\x4c\x8b\x2d|\x4c\x8b\x35|\x4c\x8b\x3d|\x48\x8b\x0d|\x48\x8b\x15|\x48\x8b\x2d)', code):
    off = m.end(); 
    if off+4 > len(code): continue
    disp = struct.unpack_from('<i', code, off)[0]
    tgt = tva + off + 4 + disp
    if tgt in iat: hits.append((tva+m.start(), iat[tgt], code[m.start():m.end()].hex()))
for rva, name, op in hits:
    print(f'== {name} ref at RVA 0x{rva:x} (op {op})')
    if ctx:
        start = max(0, rva - tva - ctx)
        ins = list(md.disasm(code[start: rva - tva + 8], tva + start))
        # re-sync: keep only tail that ends at rva
        tail = [i for i in ins if i.address <= rva][-25:]
        for i in tail: print(f'   0x{i.address:x}: {i.mnemonic} {i.op_str}')
