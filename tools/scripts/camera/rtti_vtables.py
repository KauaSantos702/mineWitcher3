# Resolve vtables via MSVC RTTI x64: TypeDescriptor -> CompleteObjectLocator -> vtable (RVA).
import sys, re, struct, pefile
path = sys.argv[1]; names = sys.argv[2].split(',')
pe = pefile.PE(path, fast_load=True); img = pe.get_memory_mapped_image(); base = pe.OPTIONAL_HEADER.ImageBase
rdata = [s for s in pe.sections if s.Name.rstrip(b'\0') in (b'.rdata',)]
for n in names:
    mangled = f'.?AV{n}@@'.encode()
    i = img.find(mangled + b'\0')
    if i < 0: print(n, 'TD not found'); continue
    td = i - 0x10
    print(f'{n}: TypeDescriptor RVA 0x{td:x}')
    # COL: sig(4)=1, offset(4), cdOffset(4), pTypeDescriptor(4 RVA), pClassHierarchy(4), pSelf(4)
    for s in rdata:
        start, end = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
        blob = img[start:end]
        for m in re.finditer(re.escape(struct.pack('<I', td)), blob):
            col = start + m.start() - 12
            sig, off, cdo, ptd, pch, pself = struct.unpack_from('<IIIIII', img, col)
            if sig != 1 or pself != col: continue
            colva = struct.pack('<Q', base + col)
            for m2 in re.finditer(re.escape(colva), blob):
                vt = start + m2.start() + 8
                f0 = struct.unpack_from('<Q', img, vt)[0] - base
                print(f'   COL 0x{col:x} offset={off} -> vtable RVA 0x{vt:x} (vf0=0x{f0:x})')
