"""Dump all ASCII strings (minlen 3) in an RVA range. usage: strings_range.py <pe> <rva_start> <rva_end>"""
import sys, re, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
a, b = int(sys.argv[2], 16), int(sys.argv[3], 16)
blob = pe.get_data(a, b - a)
for m in re.finditer(rb'[\x20-\x7e]{3,}', blob):
    print(f"{a + m.start():#010x} {m.group().decode()}")
