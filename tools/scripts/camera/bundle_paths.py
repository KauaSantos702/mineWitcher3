# Lista caminhos de recursos nos TOCs (nao comprimidos) dos .bundle que casam com um regex.
import sys, re, os, glob
root, pat = sys.argv[1], re.compile(sys.argv[2].encode(), re.I)
seen=set()
for b in glob.glob(os.path.join(root,'**','*.bundle'), recursive=True):
    with open(b,'rb') as f: data=f.read(32*1024*1024)
    for m in re.finditer(rb'[\x20-\x7e]{6,255}', data):
        s=m.group()
        if pat.search(s) and s not in seen:
            seen.add(s); print(os.path.basename(b), s.decode())
