"""Minimal CR2W (W3) reader: dumps exports and their top-level/nested properties for
simple types (Float, Bool, Int, Uint, CName, enums printed raw, nested structs like Box/Vector).
Read-only. Input is a raw (uncompressed) file extracted from a bundle by offset/size.
Usage: python -I cr2w_props.py <bundle> <offset> <size>
Also parses nested CR2W blobs (entity templates' flatCompiledData).
"""
import re
import struct
import sys

b, off, size = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(b, "rb") as f:
    f.seek(off)
    whole = f.read(size)
assert whole[:4] == b"CR2W", whole[:4]


def parse(d):
    # header: magic, version, flags, timestamp(u64), buildVersion, fileSize, bufferSize, crc32, numChunks
    tables = [struct.unpack_from("<III", d, 0x28 + i * 12) for i in range(10)]
    s_off, s_cnt, _ = tables[0]
    strings_blob = d[s_off:s_off + s_cnt]


    def cstr(o):
        e = strings_blob.index(b"\0", o)
        return strings_blob[o:e].decode("latin-1")


    n_off, n_cnt, _ = tables[1]
    names = [cstr(struct.unpack_from("<I", d, n_off + i * 8)[0]) for i in range(n_cnt)]
    e_off, e_cnt, _ = tables[4]
    exports = []
    for i in range(e_cnt):
        cls, flags, parent, dsize, doff, tmpl, crc = struct.unpack_from("<HHIIIII", d, e_off + i * 24)
        exports.append((names[cls], parent, dsize, doff))

    SIMPLE = {"Float": "<f", "Bool": "<B", "Int32": "<i", "Uint32": "<I", "Int16": "<h",
              "Uint16": "<H", "Uint8": "<B", "Int8": "<b"}
    STRUCTS = {"Box", "Vector", "EulerAngles", "EngineTransform"}


    def walk(p, end, depth):
        if p < end and d[p] == 0:
            p += 1
        while p + 8 <= end:
            ni, ti, sz = struct.unpack_from("<HHI", d, p)
            if ni == 0:
                return p + 2
            nm, ty = names[ni], names[ti]
            vstart, vend = p + 8, p + 4 + sz
            pad = "  " * depth
            if ty in SIMPLE:
                print(pad, nm, ty, struct.unpack_from(SIMPLE[ty], d, vstart)[0])
            elif ty == "CName":
                print(pad, nm, ty, names[struct.unpack_from("<H", d, vstart)[0]])
            elif ty == "EngineTransform":
                fl = d[vstart]
                q = vstart + 1
                out = {}
                for bit, key in ((1, "pos"), (2, "rot"), (4, "scale")):
                    if fl & bit:
                        out[key] = struct.unpack_from("<3f", d, q)
                        q += 12
                print(pad, nm, ty, out)
            elif ty == "EDrawableFlags":
                # bitfield enum: u8 count? then u16 name indices (observed: '17000000' = idx 0x17, then 0)
                idxs = []
                q = vstart
                while q + 2 <= vend:
                    ix = struct.unpack_from("<H", d, q)[0]
                    if ix == 0:
                        break
                    idxs.append(names[ix] if ix < len(names) else ix)
                    q += 2
                print(pad, nm, ty, idxs)
            elif ty == "handle:CMesh":
                print(pad, nm, ty, "import#", struct.unpack_from("<i", d, vstart)[0])
            elif ty == "CPhysicalCollision":
                print(pad, nm, ty, d[vstart:vend].hex())
            elif ty.startswith("E") and sz in (6, 8):
                print(pad, nm, ty, "nameIdx", names[struct.unpack_from("<H", d, vstart)[0]] if sz == 6 else d[vstart:vend].hex())
            elif ty in ("Box", "Vector", "EulerAngles"):
                print(pad, nm, ty)
                walk(vstart, vend, depth + 1)
            else:
                print(pad, nm, ty, "(%d bytes)" % (vend - vstart))
            p = vend
        return p


    for i, (cls, parent, dsize, doff) in enumerate(exports):
        print("export", i, cls, "parent", parent, "size", dsize)
        walk(doff, doff + dsize, 1)


# also parse CR2W files embedded (e.g. CEntityTemplate.flatCompiledData)
starts = [m.start() for m in re.finditer(b"CR2W", whole)]
for k, st in enumerate(starts):
    print("######## CR2W at +%#x" % st)
    try:
        parse(whole[st:])
    except Exception as ex:
        print("   parse error", ex)
