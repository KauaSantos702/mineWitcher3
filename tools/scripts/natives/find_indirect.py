import sys,re,struct,pefile
pe=pefile.PE(sys.argv[1],fast_load=True);d=pe.__data__
t=[s for s in pe.sections if s.Name.startswith(b'.text')][0];tva=t.VirtualAddress;tb=d[t.PointerToRawData:t.PointerToRawData+t.SizeOfRawData]
T=set(int(a,16) for a in sys.argv[2:])
for m in re.finditer(rb'\xFF[\x15\x25]',tb):
    i=m.start();tg=tva+i+6+struct.unpack_from('<i',tb,i+2)[0]
    if tg in T:print(f'0x{tg:x} <- call/jmp [rip] at 0x{tva+i:x}')
for m in re.finditer(rb'[\x48\x4C]\x8B[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]',tb):
    i=m.start();tg=tva+i+7+struct.unpack_from('<i',tb,i+3)[0]
    if tg in T:print(f'0x{tg:x} <- mov reg,[rip] at 0x{tva+i:x}')
