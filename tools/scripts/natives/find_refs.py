"""Find references (E8/E9 calls, RIP-relative LEA/MOV, absolute 64-bit pointers in data) to given RVAs.
Usage: python -I find_refs.py <exe> rva1 rva2 ...
"""
import sys, re, struct, pefile
exe = sys.argv[1]; targets = [int(a, 16) for a in sys.argv[2:]]
pe = pefile.PE(exe, fast_load=True); data = pe.__data__; base = pe.OPTIONAL_HEADER.ImageBase
secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
t = secs['.text']; tva = t.VirtualAddress; tb = data[t.PointerToRawData:t.PointerToRawData + t.SizeOfRawData]
ts = set(targets)
for m in re.finditer(rb'[\xE8\xE9]', tb):
    i = m.start()
    if i + 5 > len(tb): break
    tgt = tva + i + 5 + struct.unpack_from('<i', tb, i + 1)[0]
    if tgt in ts: print(f'0x{tgt:x} <- {"call" if tb[i]==0xE8 else "jmp"} at 0x{tva+i:x}')
for m in re.finditer(rb'[\x48\x4C]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]', tb):
    i = m.start(); tgt = tva + i + 7 + struct.unpack_from('<i', tb, i + 3)[0]
    if tgt in ts: print(f'0x{tgt:x} <- lea at 0x{tva+i:x}')
for name, s in secs.items():
    if name == '.text': continue
    b = data[s.PointerToRawData:s.PointerToRawData + s.SizeOfRawData]
    for tg in targets:
        pat = struct.pack('<Q', base + tg)
        for m in re.finditer(re.escape(pat), b):
            print(f'0x{tg:x} <- ptr in {name} at 0x{s.VirtualAddress + m.start():x}')
        pat4 = struct.pack('<I', tg)
        if name == '.pdata':
            for m in re.finditer(re.escape(pat4), b):
                if m.start() % 12 == 0:
                    end = struct.unpack_from('<I', b, m.start() + 4)[0]
                    print(f'0x{tg:x} is .pdata function start (end 0x{end:x})')
