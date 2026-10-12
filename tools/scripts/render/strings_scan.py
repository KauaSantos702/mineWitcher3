"""Scan PE for ASCII/UTF-16LE strings matching regexes; prints RVA + section. Read-only.
usage: python -I strings_scan.py <pe> <regex> [minlen]
"""
import sys, re, pefile

path, pat = sys.argv[1], sys.argv[2]
minlen = int(sys.argv[3]) if len(sys.argv) > 3 else 4
rx = re.compile(pat, re.I)
pe = pefile.PE(path, fast_load=True)
data = open(path, 'rb').read()

def off2rva(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + off - s.PointerToRawData, s.Name.rstrip(b'\0').decode()
    return None, '?'

seen = 0
for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
    s = m.group().decode()
    if rx.search(s):
        rva, sec = off2rva(m.start())
        print(f"A {rva:#010x} {sec:7s} {s[:200]}" if rva is not None else f"A off={m.start():#x} {s[:200]}")
        seen += 1
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen, data):
    s = m.group().decode('utf-16le')
    if rx.search(s):
        rva, sec = off2rva(m.start())
        print(f"W {rva:#010x} {sec:7s} {s[:200]}" if rva is not None else f"W off={m.start():#x} {s[:200]}")
        seen += 1
print(f"# {seen} matches", file=sys.stderr)
