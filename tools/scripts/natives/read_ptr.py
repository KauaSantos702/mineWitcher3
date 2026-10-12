"""Read qwords at RVAs from witcher3.exe file image, show section and pointed-to RVA (or import name if IAT slot).
Usage: python -I read_ptr.py <exe> rva1 ...
"""
import sys, struct, pefile
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
iat = {}
for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    for imp in d.imports:
        iat[imp.address - base] = f'{d.dll.decode()}!{imp.name.decode() if imp.name else imp.ordinal}'
for a in sys.argv[2:]:
    r = int(a, 16)
    sec = [s.Name.rstrip(b'\0').decode() for s in pe.sections if s.VirtualAddress <= r < s.VirtualAddress + s.Misc_VirtualSize]
    try:
        q = struct.unpack('<Q', pe.get_data(r, 8))[0]
    except Exception:
        q = None
    v = f'0x{q:x}' + (f' (rva 0x{q-base:x})' if q and q > base else '') if q is not None else 'bss/uninit'
    print(f'0x{r:x} sec={sec} value={v} iat={iat.get(r)}')
