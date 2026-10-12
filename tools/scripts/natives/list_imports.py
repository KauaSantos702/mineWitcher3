import sys,pefile
pe=pefile.PE(sys.argv[1])
print(sorted(set(d.dll.decode().lower() for d in pe.DIRECTORY_ENTRY_IMPORT)))
