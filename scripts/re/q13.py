import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if ('SetColorSpace' in n or 'GetColorSpace' in n) and ('TCScreen' in n or 'TPCScreen' in n or 'SsScreen' in n):
  print(hex(v),sz,n,'=>',demangle([n])[0])
