import sys; sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
L=disasm(b,0x70437c,2404)
open('/tmp/re/setcs.txt','w').write('\n'.join(L))
L2=disasm(b,0x704120,604)
open('/tmp/re/setcsc.txt','w').write('\n'.join(L2))
