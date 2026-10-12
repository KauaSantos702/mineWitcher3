"""Print VS_VERSIONINFO string tables of given PE files (read-only)."""
import sys, pefile
for p in sys.argv[1:]:
    pe = pefile.PE(p, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"]])
    print("==", p)
    for fi in getattr(pe, "FileInfo", []) or []:
        for e in fi:
            for st in getattr(e, "StringTable", []):
                for k, v in st.entries.items():
                    print("  ", k.decode(), "=", v.decode())
