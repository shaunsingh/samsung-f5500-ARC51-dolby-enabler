import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 if 'ColorSpaceCustom' in n and ('ScreenObject' in n or 'ScreenItem' in n or 'Store' in n):
  print(hex(v),sz,n,'=>',demangle([n])[0])
print('=== Set TPTRGB');print('\n'.join(disasm(b,0x814b2c,116)))
