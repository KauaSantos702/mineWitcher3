"""Resolve pdata chained entries to primary function start. usage: primary.py <pe> <rva>[,..]"""
import sys, struct, bisect, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
pdata = next(s for s in pe.sections if s.Name.startswith(b'.pdata'))
pd = pdata.get_data()[:pdata.Misc_VirtualSize]
ents = sorted(e for e in struct.iter_unpack('<III', pd[:len(pd)//12*12]) if e[0])
st = [e[0] for e in ents]
def primary(rva):
    i = bisect.bisect_right(st, rva) - 1
    b, e, u = ents[i]
    while True:
        hdr = pe.get_data(u, 4); flags = hdr[0] >> 3; cnt = hdr[2]
        if not flags & 4: return b
        off = u + 4 + ((cnt + 1) & ~1) * 2
        b, e, u = struct.unpack('<III', pe.get_data(off, 12))
for x in sys.argv[2].split(','):
    print(x, hex(primary(int(x, 16))))
