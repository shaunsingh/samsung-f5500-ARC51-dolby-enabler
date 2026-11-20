import sys;sys.path.insert(0,'/tmp/re')
from redis import Bin
b=Bin('/tmp/re/exeTV')
for a in [0xa0207c,0x8c11e4,0x8c0e98,0x8c175c,0x8c0be4]:print(hex(a),b.name(a))
