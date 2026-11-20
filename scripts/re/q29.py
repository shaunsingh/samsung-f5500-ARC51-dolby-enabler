import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for a,n in [(0xa0207c,228),(0x8c11e4,220),(0x8c0e98,220)]:print('===',hex(a));print('\n'.join(disasm(b,a,n)))
