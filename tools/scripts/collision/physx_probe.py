"""Read-only probe: physics engine evidence in witcher3.exe and neighbor DLLs.
Usage: python -I physx_probe.py <bin_dir>
Prints imports (DLL -> functions) for physics-related DLLs, exports of PhysX DLLs,
and string hits (ASCII + UTF-16LE) for physics keywords in the exe.
"""
import os
import re
import sys

import pefile

BIN = sys.argv[1]
EXE = os.path.join(BIN, "witcher3.exe")
KEYS = [rb"PhysX", rb"PxCreate", rb"PxScene", rb"PxRigid", rb"PxActor", rb"PxShape",
        rb"PxCooking", rb"PxHeightField", rb"PxTriangleMesh", rb"PxFoundation",
        rb"APEX", rb"Apex", rb"Havok", rb"hkp", rb"Bullet", rb"NxApex", rb"PxVehicle",
        rb"PxCharacter", rb"PxController", rb"SceneQuery", rb"CPhysicsWorld",
        rb"CollisionGroup", rb"collision_groups", rb"PhysicalCollision",
        rb"PHYSX_", rb"PX_PHYSICS_VERSION", rb"PxGetFoundation", rb"PxPvd", rb"PVD",
        rb"physx", rb"GRB", rb"PxBatchQuery", rb"PxSceneQuery"]


def dump_pe(path, show_exports=True):
    pe = pefile.PE(path, fast_load=True)
    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
    print("==", os.path.basename(path))
    for imp in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
        dll = imp.dll.decode(errors="replace")
        names = [(i.name or b"#%d" % i.ordinal).decode(errors="replace") for i in imp.imports]
        print("  import", dll, len(names))
        if re.search(r"physx|apex|grb|cuda|havok", dll, re.I):
            for n in names:
                print("     ", n)
    if show_exports and hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        ex = pe.DIRECTORY_ENTRY_EXPORT.symbols
        print("  exports", len(ex))
        for s in ex[:200]:
            print("     ", (s.name or b"").decode(errors="replace"))
    return pe


def strings(data, minlen=5):
    for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, data):
        yield m.start(), m.group()
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen, data):
        yield m.start(), m.group().decode("utf-16le").encode()


def main():
    dump_pe(EXE, show_exports=False)
    for f in os.listdir(BIN):
        if re.search(r"physx|grb", f, re.I):
            dump_pe(os.path.join(BIN, f))
    data = open(EXE, "rb").read()
    hits = {}
    for off, s in strings(data):
        for k in KEYS:
            if k in s:
                hits.setdefault(s, off)
                break
    print("== string hits in exe:", len(hits))
    for s, off in sorted(hits.items(), key=lambda x: x[1]):
        print("  %08x %s" % (off, s[:200].decode(errors="replace")))


if __name__ == "__main__":
    main()
