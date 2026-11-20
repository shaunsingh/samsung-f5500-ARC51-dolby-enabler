import sys; sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
c=[(n,v) for n,v in b.byname.items() if 'ColorSpace' in n and 'PrimePicture' in n]
for n,v in c: print(hex(v[0]),v[1],demangle([n])[0])
