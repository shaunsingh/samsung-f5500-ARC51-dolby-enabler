import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if n.startswith('_ZN10ColorSpace') or 'ColorSpace::' in n and 't_' in n:
  print(hex(v),sz,demangle([n])[0])
