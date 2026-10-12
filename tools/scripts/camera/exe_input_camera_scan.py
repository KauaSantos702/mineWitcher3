# Varredura read-only do witcher3.exe: imports de input/janela e RTTI/strings de camera.
import sys, re, pefile
path = sys.argv[1]
pe = pefile.PE(path, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_IMPORT'],
                                       pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT']])
base = pe.OPTIONAL_HEADER.ImageBase
INTEREST = re.compile(r'(RawInput|GetAsyncKeyState|GetKeyState|GetKeyboardState|SetWindowLong|CallWindowProc|DefWindowProc|RegisterClass|CreateWindow|PeekMessage|GetMessage|DispatchMessage|TranslateMessage|SetCursor|ClipCursor|ShowCursor|GetCursorPos|SetCapture|ReleaseCapture|SetWindowsHookEx|XInput|DirectInput|GameInput|MapVirtualKey|ToUnicode|ToAscii|SetFocus|GetForegroundWindow|GetActiveWindow)', re.I)
print('== IMPORTS (input/window) ==')
for attr in ('DIRECTORY_ENTRY_IMPORT','DIRECTORY_ENTRY_DELAY_IMPORT'):
    for e in getattr(pe, attr, []):
        dll = e.dll.decode(errors='replace')
        names = [(i.name.decode() if i.name else f'ord{i.ordinal}', i.address) for i in e.imports]
        hits = [(n,a) for n,a in names if INTEREST.search(n)]
        if re.search(r'xinput|dinput|gameinput|hid', dll, re.I) or hits:
            print(f'[{attr[16:]}] {dll}')
            for n,a in (hits or names):
                print(f'    {n:40s} IAT_RVA=0x{(a-base) if a else 0:x}')
print('== ALL DLLs ==')
for attr in ('DIRECTORY_ENTRY_IMPORT','DIRECTORY_ENTRY_DELAY_IMPORT'):
    print(attr, sorted(e.dll.decode() for e in getattr(pe, attr, [])))
data = open(path,'rb').read()
def rva_of(off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + off - s.PointerToRawData, s.Name.rstrip(b'\0').decode()
    return None, '?'
pats = [rb'\.\?AV[A-Za-z0-9_]*Camera[A-Za-z0-9_]*@@', rb'\.\?AV[A-Za-z0-9_]*(InputManager|RawInput|InputDevice|Keyboard|Mouse|Viewport)[A-Za-z0-9_]*@@']
print('== RTTI ==')
for p in pats:
    seen=set()
    for m in re.finditer(p, data):
        s=m.group().decode()
        if s in seen: continue
        seen.add(s); r,sec=rva_of(m.start()); print(f'  {s:70s} rva=0x{r:x} {sec}')
print('== STRINGS ==')
strpats = [rb'(?:Render|Game|Free|Custom|Static|Photomode)?Camera(?:Director|Component|Matrix|Position|Rotation|Fov|Tick|Controller)?[A-Za-z_]*',
           rb'(?i)(worldToView|viewToWorld|viewProj|ViewProjection|CameraToWorld|WorldToCamera|nearPlane|farPlane)',
           rb'(?i)(rawinput|WM_INPUT|hidusage|GameInput|XInput[0-9_]*\.dll|dinput8\.dll)']
for p in strpats:
    cnt={}
    for m in re.finditer(rb'[\x20-\x7e]{4,120}', data):
        s=m.group()
        if re.search(p, s) and s not in cnt:
            cnt[s]=m.start()
    for s,o in list(cnt.items())[:int(sys.argv[2]) if len(sys.argv)>2 else 80]:
        r,sec=rva_of(o); print(f'  {s.decode()[:100]:100s} rva=0x{(r or 0):x} {sec}')
    # UTF-16 too
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){5,80}', data):
        s=m.group().decode('utf-16le')
        if re.search(p.decode(), s):
            r,sec=rva_of(m.start()); print(f'  [u16] {s[:100]:100s} rva=0x{(r or 0):x} {sec}')
