"""Locate MSVC x64 RTTI vtables for given mangled type names (e.g. .?AVNpScene@physx@@).
Read-only. Usage: python -I rtti_vtable.py <pe> <typename> [...]
Prints TypeDescriptor VA, each CompleteObjectLocator (offset) and vtable VA + first slots.
"""
import struct
import sys

import pefile

pe = pefile.PE(sys.argv[1], fast_load=True)
data = pe.__data__
base = pe.OPTIONAL_HEADER.ImageBase


def off2rva(o):
    return pe.get_rva_from_offset(o)


def rva2off(r):
    return pe.get_offset_from_rva(r)


for tname in sys.argv[2:]:
    key = tname.encode() + b"\0"
    o = data.find(key)
    if o < 0:
        print(tname, "not found")
        continue
    td_rva = off2rva(o) - 0x10  # TypeDescriptor: pVFTable(8), spare(8), name
    print("==", tname, "TypeDescriptor VA %#x" % (base + td_rva))
    needle = struct.pack("<iI", 1, 0)  # signature=1, offset (we filter later)
    td = struct.pack("<I", td_rva)
    pos = 0
    while True:
        pos = data.find(td, pos + 1)
        if pos < 0:
            break
        col = pos - 12
        sig, offs, cdoff, tdr, chd, selfr = struct.unpack_from("<IIIIII", data, col)
        if sig != 1 or tdr != td_rva:
            continue
        col_rva = off2rva(col)
        if selfr != col_rva:
            continue
        ptr = struct.pack("<Q", base + col_rva)
        p2 = data.find(ptr)
        while p2 >= 0:
            vt_rva = off2rva(p2) + 8
            slots = struct.unpack_from("<8Q", data, rva2off(vt_rva))
            print("   COL %#x offset %#x vtable VA %#x slots:" % (base + col_rva, offs, base + vt_rva),
                  " ".join("%#x" % s for s in slots[:6]))
            p2 = data.find(ptr, p2 + 1)
