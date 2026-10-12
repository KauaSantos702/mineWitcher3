"""Find RIP-relative LEA references to a string (by file offset) in a PE, then
disassemble around the reference. Read-only.
Usage: python -I find_str_xref.py <pe> <hex_file_offset_of_string> [before] [after]
"""
import re
import sys

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

pe = pefile.PE(sys.argv[1], fast_load=True)
str_off = int(sys.argv[2], 16)
before = int(sys.argv[3]) if len(sys.argv) > 3 else 0x60
after = int(sys.argv[4]) if len(sys.argv) > 4 else 0x40
data = pe.__data__
base = pe.OPTIONAL_HEADER.ImageBase
str_rva = pe.get_rva_from_offset(str_off)
md = Cs(CS_ARCH_X86, CS_MODE_64)
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000):
        continue
    s_off, s_size, s_rva = sec.PointerToRawData, sec.SizeOfRawData, sec.VirtualAddress
    blob = data[s_off:s_off + s_size]
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", blob):
        i = m.start()
        disp = int.from_bytes(blob[i + 3:i + 7], "little", signed=True)
        if s_rva + i + 7 + disp == str_rva:
            ref_rva = s_rva + i
            print("xref at VA %#x" % (base + ref_rva))
            start = max(0, i - before)
            for ins in md.disasm(blob[start:i + after], base + s_rva + start):
                mark = "  <==" if ins.address == base + ref_rva else ""
                print("   %#x  %-8s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))
