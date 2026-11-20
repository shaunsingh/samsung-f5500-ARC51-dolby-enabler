import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for a,n in [(0x9100fc,736),(0x9106c0,232)]:
 print('===',demangle([b.name(a)])[0]);print('\n'.join(disasm(b,a,n)))
