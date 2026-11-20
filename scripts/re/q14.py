import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for a,n in [(0xaf3b58,44),(0xaf5f54,164),(0xaf3b84,44),(0xaf5e98,188)]:
 print('===',hex(a));print('\n'.join(disasm(b,a,n)))
