"""Resumo de um minidump: excecao, modulo do endereco e pilha bruta (enderecos que caem em modulos)."""
import sys, struct
from minidump.minidumpfile import MinidumpFile

def mod_of(mods, a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return f"{m.name.split(chr(92))[-1]}+0x{a - m.baseaddress:X}"
    return None

md = MinidumpFile.parse(sys.argv[1])
mods = md.modules.modules
exc = md.exception.exception_records[0]
er = exc.ExceptionRecord
print("excecao", str(er.ExceptionCode), "em", mod_of(mods, er.ExceptionAddress) or hex(er.ExceptionAddress),
      "params", [hex(p) for p in er.ExceptionInformation[:er.NumberParameters]])
tid = exc.ThreadId
reader = md.get_reader()
th = [t for t in md.threads.threads if t.ThreadId == tid][0]
ctx = th.ContextObject
rip, rsp = ctx.Rip, ctx.Rsp
print("thread", tid, "rip", mod_of(mods, rip), "rsp", hex(rsp))
data = reader.read(rsp, 0x4000)
seen = 0
for i in range(0, len(data) - 8, 8):
    v = struct.unpack_from("<Q", data, i)[0]
    m = mod_of(mods, v)
    if m:
        print(f"  [rsp+{i:#06x}] {m}")
        seen += 1
        if seen > 40: break
