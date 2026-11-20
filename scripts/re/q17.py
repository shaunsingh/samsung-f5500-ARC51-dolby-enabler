import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV');vt=0x1a1ef38
for off in range(0,0x60,4):
 a=b.u32(vt+off)
 print(hex(off),hex(a),demangle([b.name(a&~1)])[0])
