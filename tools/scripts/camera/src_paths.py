# Lista caminhos de fonte embutidos no exe (prefixo "Main.Lava") que casam com regex.
import sys, re
d = open(sys.argv[1], 'rb').read()
pat = re.compile(sys.argv[2].encode(), re.I)
seen = set()
for m in re.finditer(rb'Main[.]Lava[\x20-\x7e]{4,200}', d):
    s = m.group()
    if pat.search(s) and s not in seen:
        seen.add(s); print(hex(m.start()), s.decode())
