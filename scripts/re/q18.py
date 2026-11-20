import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if ('ColorSpace' in n and ('Screen' in n or 'Object' in n or 'Item' in n)) or 'TPTRGB' in n:
  print(hex(v),sz,demangle([n])[0])
