"""Map RVAs to containing function using .pdata (RUNTIME_FUNCTION), following chained unwind info to the primary start.
Usage: python -I pdata_func.py <exe> rva1 rva2 ...
"""
import sys, struct, bisect, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); data = pe.__data__
s = [x for x in pe.sections if x.Name.startswith(b'.pdata')][0]
b = data[s.PointerToRawData:s.PointerToRawData + s.Misc_VirtualSize]
ents = [struct.unpack_from('<III', b, i) for i in range(0, len(b) - 11, 12)]
ents = [e for e in ents if e[0]]
ents.sort(); starts = [e[0] for e in ents]
def primary(e):
    # follow UNW_FLAG_CHAININFO
    for _ in range(16):
        ui = e[2] & ~1
        hdr = pe.get_data(ui, 4)
        flags = hdr[0] >> 3; cnt = hdr[2]
        if not (flags & 4): return e
        off = ui + 4 + ((cnt + 1) & ~1) * 2
        e = struct.unpack('<III', pe.get_data(off, 12))
    return e
for a in sys.argv[2:]:
    r = int(a, 16); i = bisect.bisect_right(starts, r) - 1
    e = ents[i]
    if not (e[0] <= r < e[1]): print(f'0x{r:x}: no pdata'); continue
    p = primary(e)
    print(f'0x{r:x}: chunk 0x{e[0]:x}-0x{e[1]:x} primary func 0x{p[0]:x}-0x{p[1]:x}')
