# Busca strings ASCII/UTF-16 no exe por regex, com RVA e secao.
import sys, re, pefile
pe = pefile.PE(sys.argv[1], fast_load=True); d = open(sys.argv[1],'rb').read()
pat = re.compile(sys.argv[2], re.I)
def rva(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData+s.SizeOfRawData:
            return s.VirtualAddress+off-s.PointerToRawData, s.Name.rstrip(b'\0').decode()
    return 0,'?'
seen=set()
for m in re.finditer(rb'[\x20-\x7e]{4,160}', d):
    s=m.group().decode()
    if pat.search(s) and s not in seen:
        seen.add(s); r,n=rva(m.start()); print(f'0x{r:x} {n} {s}')
for m in re.finditer(rb'(?:[\x20-\x7e]\x00){4,160}', d):
    s=m.group().decode('utf-16le')
    if pat.search(s) and s not in seen:
        seen.add(s); r,n=rva(m.start()); print(f'0x{r:x} {n} [u16] {s}')
