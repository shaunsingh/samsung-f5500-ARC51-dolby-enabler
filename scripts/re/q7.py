import sys; sys.path.insert(0,'/tmp/re')
from redis import *
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
b=Bin('/tmp/re/libUTOPIA.so')
def dis(a,n):
    md=Cs(CS_ARCH_ARM, CS_MODE_ARM); md.skipdata=True
    for i in md.disasm(b.read(a,n),a):
        s=f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}'
        if i.mnemonic in ('bl','b') and i.op_str.startswith('#'):
            s+='    ; '+demangle([b.name(int(i.op_str[1:],16))])[0]
        if '[pc' in i.op_str and i.mnemonic.startswith('ldr') and '#' in i.op_str:
            try:
                o=int(i.op_str.split('#')[-1].rstrip(']'),16); lit=b.u32(i.address+8+o)
                st=''
                try:
                    raw=b.read(lit,60); 
                    if raw.find(b'\0')>3 and all(32<=c<127 for c in raw[:raw.find(b'\0')]): st=raw[:raw.find(b'\0')].decode()
                except Exception: pass
                s+=f' ; ={lit:#x} {demangle([b.name(lit)])[0]} {st}'
            except Exception: pass
        print(s)
for n,(v,sz) in b.byname.items():
    if 'Get_Pixel_RGB' in n: print('SYM',hex(v),sz,n)
dis(0x42ba1268,1112)
