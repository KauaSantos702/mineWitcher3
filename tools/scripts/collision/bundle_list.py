"""List file paths in W3 .bundle files (POTATO70 header). Read-only.
Usage: python -I bundle_list.py <out.tsv> <bundle_or_dir> [...]
Header: 'POTATO70' u32 bundleSize, u32 dummy, u32 tableSize at 0x10; entries start at 0x20,
each 0x130 bytes (layout as observed in next-gen v5.0 bundles):
  +0x000 path[0x100]  +0x100 md5[16]  +0x110 u32 dataOffset  +0x114 u32 ?
  +0x118 u32 size     +0x11c u32 zsize  +0x120 u32 ? (crc/ts)  +0x12c u32 compression?
Writes: bundle \t path \t size \t zsize \t dataOffset \t comp
"""
import os
import struct
import sys

out = open(sys.argv[1], "w", encoding="utf-8")
files = []
for a in sys.argv[2:]:
    if os.path.isdir(a):
        for root, _, fs in os.walk(a):
            files += [os.path.join(root, f) for f in fs if f.endswith(".bundle")]
    elif os.path.exists(a):
        files.append(a)
total = 0
for f in files:
    with open(f, "rb") as fh:
        hdr = fh.read(0x20)
        if hdr[:8] != b"POTATO70":
            continue
        table_size = struct.unpack_from("<I", hdr, 0x10)[0]
        table = fh.read(table_size)
    n = 0
    for off in range(0, len(table) - 0x130 + 1, 0x130):
        e = table[off:off + 0x130]
        path = e[:0x100].split(b"\0", 1)[0].decode("latin-1")
        if not path:
            continue
        doff = struct.unpack_from("<I", e, 0x110)[0]
        size, zsize = struct.unpack_from("<II", e, 0x118)
        comp = struct.unpack_from("<I", e, 0x12c)[0]
        out.write("%s\t%s\t%d\t%d\t%d\t%d\n" % (os.path.relpath(f), path, size, zsize, doff, comp))
        n += 1
    total += n
    print(os.path.basename(f), n)
print("total", total)
