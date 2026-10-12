"""Le a memoria de pilha capturada da thread da excecao e conta enderecos de retorno por modulo."""
import sys, struct, collections
from minidump.minidumpfile import MinidumpFile
md = MinidumpFile.parse(sys.argv[1]); mods = md.modules.modules
def mod_of(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return f"{m.name.split(chr(92))[-1]}+0x{a - m.baseaddress:X}"
exc = md.exception.exception_records[0]; tid = exc.ThreadId
th = [t for t in md.threads.threads if t.ThreadId == tid][0]
st = th.Stack; base = st.StartOfMemoryRange; size = st.MemoryLocation.DataSize
print("pilha capturada", hex(base), "tamanho", hex(size))
r = md.get_reader(); data = r.read(base, size)
c = collections.Counter(); first = []
for i in range(0, len(data) - 8, 8):
    m = mod_of(struct.unpack_from("<Q", data, i)[0])
    if m:
        c[m] += 1
        if len(first) < 30: first.append(m)
print("primeiros (topo):"); [print("  ", x) for x in first]
print("mais repetidos:"); [print(f"  {n:6d}  {m}") for m, n in c.most_common(15)]
