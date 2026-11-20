import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for a,n in [(0x814a44,116),(0x814ab8,116),(0x8149d0,116),(0x113b678,68),(0x113b6bc,60)]:
 print('===',hex(a),demangle([b.name(a)])[0]);print('\n'.join(disasm(b,a,n)))
