import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if 'GetUniqueItemData' in n or 'GetCustomColorDefaultData' in n:
  print(hex(v),sz,n,'=>',demangle([n])[0])
