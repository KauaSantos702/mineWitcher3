"""Find 8-byte absolute pointers (ImageBase+rva) to targets in non-code sections. usage: dataref.py <pe> <rva>[,..]"""
import sys, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); base = pe.OPTIONAL_HEADER.ImageBase
for t in [int(x, 16) for x in sys.argv[2].split(',')]:
    pat = (base + t).to_bytes(8, 'little')
    for s in pe.sections:
        if s.Name.startswith(b'.text'): continue
        d = s.get_data(); i = d.find(pat)
        while i != -1:
            print(f"ptr to {t:#x} at {s.VirtualAddress + i:#x} ({s.Name.rstrip(b'\0').decode()})")
            i = d.find(pat, i + 1)
