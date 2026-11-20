import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if 'TDsPrimePanel' in n and ('ColorSpace' in n or 'ColorMapping' in n):print(hex(v),sz,demangle([n])[0])
