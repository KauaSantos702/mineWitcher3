"""Disassemble RVA range. usage: dis.py <pe> <rva_start> <rva_end>"""
import sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = pefile.PE(sys.argv[1], fast_load=True)
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
md = Cs(CS_ARCH_X86, CS_MODE_64)
for i in md.disasm(pe.get_data(a, b - a), a):
    print(f"{i.address:#x}: {i.mnemonic} {i.op_str}")
