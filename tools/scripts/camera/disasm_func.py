# Desmonta a funcao (via .pdata) que contem cada RVA dado; opcional janela [rva-before, rva+after].
import sys, pefile, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
path = sys.argv[1]; rvas = [int(x,16) for x in sys.argv[2].split(',')]
before = int(sys.argv[3]) if len(sys.argv)>3 else 10**9; after = int(sys.argv[4]) if len(sys.argv)>4 else 10**9
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted((e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
starts = [f[0] for f in funcs]
md = Cs(CS_ARCH_X86, CS_MODE_64)
img = pe.get_memory_mapped_image()
for r in rvas:
    i = bisect.bisect_right(starts, r) - 1
    b, e = funcs[i]
    print(f'##### RVA 0x{r:x} in func 0x{b:x}-0x{e:x} (size {e-b})')
    for ins in md.disasm(img[b:e], b):
        if r - before <= ins.address <= r + after:
            mark = '>>' if ins.address == r else '  '
            print(f'{mark}0x{ins.address:x}: {ins.mnemonic} {ins.op_str}')
