"""PE overview: sections+entropy, imports, delay imports, exports, TLS, entry point. Read-only.
usage: python -I pe_overview.py <pe> [--imports-filter substr,substr]
"""
import sys, math, pefile, collections

def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())

path = sys.argv[1]
flt = None
if len(sys.argv) > 3 and sys.argv[2] == '--imports-filter':
    flt = [s.lower() for s in sys.argv[3].split(',')]
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT'],
    pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_TLS'],
])
oh = pe.OPTIONAL_HEADER
print(f"ImageBase={oh.ImageBase:#x} EntryRVA={oh.AddressOfEntryPoint:#x} SizeOfImage={oh.SizeOfImage:#x} TimeDateStamp={pe.FILE_HEADER.TimeDateStamp:#x}")
print(f"DllCharacteristics={oh.DllCharacteristics:#x} Subsystem={oh.Subsystem}")
for s in pe.sections:
    name = s.Name.rstrip(b'\0').decode(errors='replace')
    data = s.get_data()
    ep = ' <EP' if s.contains_rva(oh.AddressOfEntryPoint) else ''
    print(f"  {name:10s} VA={s.VirtualAddress:#010x} VS={s.Misc_VirtualSize:#010x} RS={s.SizeOfRawData:#010x} ch={s.Characteristics:#010x} H={entropy(data):.3f}{ep}")
if hasattr(pe, 'DIRECTORY_ENTRY_TLS'):
    t = pe.DIRECTORY_ENTRY_TLS.struct
    print(f"TLS callbacks array VA={t.AddressOfCallBacks:#x}")
print("== IMPORTS ==")
for e in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
    dll = e.dll.decode()
    names = [(i.name.decode() if i.name else f"#{i.ordinal}", i.address) for i in e.imports]
    if flt and not any(f in dll.lower() for f in flt):
        print(f"  {dll} ({len(names)})")
        continue
    print(f"  {dll} ({len(names)}):")
    for n, a in names:
        print(f"      {n}  IAT={a - oh.ImageBase:#x}")
print("== DELAY IMPORTS ==")
for e in getattr(pe, 'DIRECTORY_ENTRY_DELAY_IMPORT', []):
    dll = e.dll.decode()
    print(f"  {dll}: " + ", ".join((i.name.decode() if i.name else f"#{i.ordinal}") for i in e.imports))
if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT'):
    print("== EXPORTS ==")
    for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        print(f"  {s.ordinal} {s.name.decode() if s.name else ''} RVA={s.address:#x}" + (f" -> {s.forwarder.decode()}" if s.forwarder else ''))
