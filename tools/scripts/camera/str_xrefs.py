# Acha referencias RIP-relativas (lea/mov) a uma string ASCII exata e desmonta janela ao redor.
import sys, re, struct, pefile, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
path = sys.argv[1]; target = sys.argv[2].encode() + b'\0'; win = int(sys.argv[3]) if len(sys.argv)>3 else 40
pe = pefile.PE(path, fast_load=True); img = pe.get_memory_mapped_image()
locs = [m.start() for m in re.finditer(re.escape(target), img) if img[m.start()-1:m.start()] == b'\0']
text = next(s for s in pe.sections if s.Name.startswith(b'.text')); t0 = text.VirtualAddress; t1 = t0 + text.Misc_VirtualSize
code = img[t0:t1]; md = Cs(CS_ARCH_X86, CS_MODE_64)
for loc in locs:
    print(f'## string @0x{loc:x}')
    for m in re.finditer(rb'[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]', code):
        o = m.start(); disp = struct.unpack_from('<i', code, o+3)[0]
        if t0 + o + 7 + disp == loc:
            a = t0 + o
            print(f'  xref lea @0x{a:x}')
            for ins in md.disasm(code[o-win: o+win+40], a-win):
                if ins.address >= a - win + 0:
                    print(f'     0x{ins.address:x}: {ins.mnemonic} {ins.op_str}')
