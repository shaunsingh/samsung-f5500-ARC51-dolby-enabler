import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/libUTOPIA.so')
for a,n in [(0x42badd24,0x80),(0x42bade34,0x80),(0x42c3d720,0x140),(0x42bf1838,0x100),(0x42bab360,0x100),(0x42bab350,0x100),(0x42badfb0,0x80),(0x42badfa0,0x80)]:
 print('\n===',hex(a),demangle([b.name(a)])[0]);print('\n'.join(disasm(b,a,n)))
