import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/libUTOPIA.so')
for a,n in [(0x42c1176c,0x40),(0x42c4ebac,0x100),(0x42c4eb24,0x90),(0x42c4ebd0,0x80)]:print('\n===',hex(a));print('\n'.join(disasm(b,a,n)))
