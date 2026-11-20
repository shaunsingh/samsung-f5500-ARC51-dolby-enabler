import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for a,n in [(0x9108f0,112),(0x812d58,112),(0x812cd8,128),(0x910dc0,0x140)]:
 print('===',hex(a),demangle([b.name(a)])[0]);print('\n'.join(disasm(b,a,n)))
