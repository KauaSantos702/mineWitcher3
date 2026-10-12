"""Grep ascii and utf-16le strings in witcher3.exe by regex (case-insensitive). Prints RVA, encoding, string.
Usage: python -I grep_strings.py <exe> <regex> [max]
"""
import sys, re, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); data = pe.__data__
rx = re.compile(sys.argv[2], re.I); mx = int(sys.argv[3]) if len(sys.argv) > 3 else 200
def off2rva(o):
    for s in pe.sections:
        if s.PointerToRawData <= o < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + o - s.PointerToRawData
n = 0
for m in re.finditer(rb'[\x20-\x7e]{5,}', data):
    s = m.group().decode()
    if rx.search(s):
        print(f'0x{off2rva(m.start()):x} A {s[:160]}'); n += 1
        if n >= mx: break
n = 0
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){5,}', data):
    s = m.group().decode('utf-16le')
    if rx.search(s):
        print(f'0x{off2rva(m.start()):x} W {s[:160]}'); n += 1
        if n >= mx: break
