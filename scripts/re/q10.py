import sys; sys.path.insert(0,'/tmp/re')
from redis import *
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
b=Bin('/tmp/re/exeTV')
for name,addr,size in [('GetFactoryCM',0x701754,0x668),('GetFactoryCM3D',0x701dbc,0x668)]:
 print('====',name)
 md=Cs(CS_ARCH_ARM,CS_MODE_ARM); md.skipdata=True
 for i in md.disasm(b.read(addr,size),addr):
  s=f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}'
  if i.mnemonic in ('bl','b','blx') and i.op_str.startswith('#'):
   s+=' ; '+demangle([b.name(int(i.op_str[1:],16))])[0]
  if '[pc' in i.op_str and i.mnemonic.startswith('ldr') and '#' in i.op_str:
   try:
    o=int(i.op_str.split('#')[-1].rstrip(']'),16); lit=b.u32(i.address+8+o); s+=f' ; ={lit:#x} '+demangle([b.name(lit)])[0]
   except Exception: pass
  print(s)
