import sys;sys.path.insert(0,'/tmp/re')
from redis import *
from capstone import Cs,CS_ARCH_ARM,CS_MODE_ARM
b=Bin('/tmp/re/exeTV');md=Cs(CS_ARCH_ARM,CS_MODE_ARM);md.skipdata=True
for a,n in [(0x910a40,148),(0x910960,224),(0x910ad4,1280)]:
 print('===',demangle([b.name(a)])[0])
 for i in md.disasm(b.read(a,n),a):
  s=f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}'
  if i.mnemonic in ('bl','b','blx') and i.op_str.startswith('#'):s+=' ; '+demangle([b.name(int(i.op_str[1:],16))])[0]
  if '[pc' in i.op_str and i.mnemonic.startswith('ldr') and '#' in i.op_str:
   try:
    o=int(i.op_str.split('#')[-1].rstrip(']'),16);lit=b.u32(i.address+8+o);s+=f' ; ={lit:#x} '+demangle([b.name(lit)])[0]
   except Exception:pass
  print(s)
