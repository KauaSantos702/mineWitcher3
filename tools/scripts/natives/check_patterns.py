import sys,re,pefile
pe=pefile.PE(sys.argv[1],fast_load=True);d=pe.__data__
t=[s for s in pe.sections if s.Name.startswith(b'.text')][0];tb=d[t.PointerToRawData:t.PointerToRawData+t.SizeOfRawData]
p=bytes.fromhex('BAF8000000488D4DF041B810000000E8')
print('WNB alloc pattern prefix matches:',len(re.findall(re.escape(p),tb)))
print('RegisterGlobal prologue @0x26ce2d0:',pe.get_data(0x26ce2d0,15).hex(' '))
print('InitializeRTTI prologue @0x2288f80:',pe.get_data(0x2288f80,17).hex(' '))
print('CFunction global ctor @0x26fdda0:',pe.get_data(0x26fdda0,16).hex(' '))
