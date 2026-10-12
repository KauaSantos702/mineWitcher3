"""Disassemble a window of witcher3.exe at an RVA. Annotates RIP-relative targets with ascii strings when present.
Usage: python -I disasm.py <exe> <rva_hex> [before_bytes] [after_bytes]
"""
import sys, pefile, capstone, re
exe = sys.argv[1]; rva = int(sys.argv[2], 16)
before = int(sys.argv[3],0) if len(sys.argv) > 3 else 0x40
after = int(sys.argv[4],0) if len(sys.argv) > 4 else 0x80
pe = pefile.PE(exe, fast_load=True)
data = pe.__data__
def rd(r, n):
    return pe.get_data(r, n)
def s_at(r):
    try:
        b = rd(r, 80)
    except Exception:
        return None
    m = re.match(rb'[\x20-\x7e]{3,}\0', b)
    if m: return m.group()[:-1].decode()
    m = re.match(rb'((?:[\x20-\x7e]\0){3,})\0\0', b)
    if m: return 'L"' + m.group(1).decode('utf-16le') + '"'
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
start = rva - before
code = rd(start, before + after)
for ins in md.disasm(code, start):
    note = ''
    if 'rip' in ins.op_str:
        m = re.search(r'rip ([+-]) (0x[0-9a-f]+)', ins.op_str)
        if m:
            d = int(m.group(2), 16) * (1 if m.group(1) == '+' else -1)
            t = ins.address + ins.size + d
            s = s_at(t)
            note = f'  ; -> 0x{t:x}' + (f' "{s}"' if s else '')
    elif ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
        note = ''
    mark = '>>' if ins.address == rva else '  '
    print(f'{mark}0x{ins.address:x}: {ins.mnemonic} {ins.op_str}{note}')
