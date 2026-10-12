# Para cada nome de funcao script-import, acha o registro (lea rax,<fn>; ... lea rdx,<nome>) e desmonta o native.
import sys, re, struct, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
path = sys.argv[1]; names = sys.argv[2].split(','); n_ins = int(sys.argv[3]) if len(sys.argv)>3 else 30
pe = pefile.PE(path, fast_load=True); img = pe.get_memory_mapped_image()
text = next(s for s in pe.sections if s.Name.startswith(b'.text')); t0=text.VirtualAddress; code=img[t0:t0+text.Misc_VirtualSize]
md = Cs(CS_ARCH_X86, CS_MODE_64)
for nm in names:
    tgt = nm.encode()+b'\0'
    locs = [m.start() for m in re.finditer(re.escape(tgt), img) if img[m.start()-1]==0]
    for loc in locs:
        for m in re.finditer(rb'\x48\x8d\x15', code):
            o=m.start(); d=struct.unpack_from('<i',code,o+3)[0]
            if t0+o+7+d != loc: continue
            # procura lea rax,[rip+x] (48 8d 05) nos 48 bytes anteriores
            fn=None
            for k in range(o-7, max(o-64,0), -1):
                if code[k:k+3]==b'\x48\x8d\x05':
                    fn=t0+k+7+struct.unpack_from('<i',code,k+3)[0]; break
            print(f'== {nm}: reg @0x{t0+o:x} native=0x{fn:x}' if fn else f'== {nm}: reg @0x{t0+o:x} native=?')
            if fn:
                for j,i in enumerate(md.disasm(img[fn:fn+400],fn)):
                    print(f'    0x{i.address:x}: {i.mnemonic} {i.op_str}')
                    if i.mnemonic=='ret' or j>=n_ins: break
