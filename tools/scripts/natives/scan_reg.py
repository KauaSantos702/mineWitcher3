"""Scan witcher3.exe .text for E8 calls to given targets; report counts, enclosing function starts (int3/pad heuristic),
and for global-function registrations, recover (name, native fn) pairs.
Also dumps MSVC RTTI type-descriptor names matching a filter.
Usage: python -I scan_reg.py <exe>
"""
import sys, re, struct, pefile, collections
exe = sys.argv[1]
pe = pefile.PE(exe, fast_load=True)
data = pe.__data__
secs = {s.Name.rstrip(b'\0').decode(): s for s in pe.sections}
t = secs['.text']; tva = t.VirtualAddress; tb = data[t.PointerToRawData:t.PointerToRawData + t.SizeOfRawData]

CF_GLOBAL_CTOR = 0x26fdda0   # CFunction::CFunction(CName&, nativeFn) (global)
CF_METHOD_CTOR = 0x26fda50   # CFunction::CFunction(CClass*, CName&, nativeThunk) (method)
REG_GLOBAL = 0x26ce2d0       # CRTTISystem::RegisterGlobalFunction(func)
ADD_METHOD = 0x26c2fd0       # CClass::AddNativeFunction(func)
CNAME_CTOR = 0x26b90a0       # CNamesPool::AddDynamicPoolName / CName(const char*)

calls = collections.defaultdict(list)
for m in re.finditer(rb'\xE8', tb):
    i = m.start()
    if i + 5 > len(tb): break
    tgt = tva + i + 5 + struct.unpack_from('<i', tb, i + 1)[0]
    if tgt in (CF_GLOBAL_CTOR, CF_METHOD_CTOR, REG_GLOBAL, ADD_METHOD, CNAME_CTOR, 0x31e800):
        calls[tgt].append(tva + i)
for k, v in calls.items():
    print(f'call target 0x{k:x}: {len(v)} call sites')

def func_start(rva):
    i = rva - tva
    while i > 0:
        if tb[i - 1] == 0xCC and tb[i - 2] == 0xCC:
            return tva + i
        i -= 1

def cstr(rva):
    try:
        b = pe.get_data(rva, 96)
    except Exception:
        return None
    m = re.match(rb'[\x20-\x7e]+\0', b)
    return m.group()[:-1].decode() if m else None

# For each global ctor call, look backward ~0x40 bytes for "lea rdx,[rip+X]" (string) and "lea r8,[rip+Y]" (native fn)
pairs = []
funcs = collections.Counter()
for site in calls[CF_GLOBAL_CTOR]:
    win = tb[site - tva - 0x40: site - tva]
    name = fn = None
    for m in re.finditer(rb'(?=([\x48\x4C])\x8D([\x05\x0D\x15\x1D\x25\x2D\x35\x3D]))', win):
        j = m.start(); i = site - 0x40 + j
        disp = struct.unpack_from('<i', win, j + 3)[0] if j + 7 <= len(win) else None
        if disp is None: continue
        tgt = i + 7 + disp
        reg = ((win[j + 2] >> 3) & 7) + (8 if win[j] == 0x4C else 0)
        if reg == 2: name = cstr(tgt) or name
        if reg == 8: fn = tgt
    pairs.append((site, name, fn))
    funcs[func_start(site)] += 1
print(f'\nGlobal native registrations: {len(pairs)}; enclosing functions:')
for f, c in funcs.most_common(40):
    print(f'  func 0x{f:x}: {c} registrations')
with open(sys.argv[2] if len(sys.argv) > 2 else 'global_natives.txt', 'w') as fo:
    for site, name, fn in pairs:
        fo.write(f'0x{site:x}\t{name}\t{hex(fn) if fn else None}\n')
print('sample:', [(n, hex(f) if f else None) for _, n, f in pairs[:15]])

# MSVC RTTI type descriptors
rt = []
for m in re.finditer(rb'\.\?A[VU]([A-Za-z0-9_@]+)@@\0', data):
    rt.append(m.group(1).decode())
print(f'\nRTTI type descriptor strings (.?AV/.?AU): {len(rt)}')
want = ['CScriptStackFrame', 'CRTTISystem', 'CFunction', 'CClass', 'CR4Game', 'CR4Player', 'CScriptThread', 'IScriptable',
        'CScriptingSystem', 'CScriptCompiler', 'CNamesPool', 'CProperty', 'CObject', 'CGame', 'CPlayer', 'CEntity', 'CActor', 'CNewNPC']
for w in want:
    hits = [r for r in rt if r == w or r.startswith(w + '@')]
    print(f'  {w}: {hits[:3]}')
print('  sample containing Script:', [r for r in rt if 'Script' in r][:25])
