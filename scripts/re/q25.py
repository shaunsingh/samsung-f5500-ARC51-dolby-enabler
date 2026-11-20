import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
print('\n'.join(disasm(b,0x7043f8,0x340)))
