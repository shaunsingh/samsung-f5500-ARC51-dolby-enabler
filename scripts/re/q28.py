import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
for n,(v,sz) in b.byname.items():
 d=demangle([n])[0]
 if ('TCAPI' in d and ('Speaker' in d or 'External' in d)) or 'TPCExternalOut::Instance' in d or 'TPCSpeaker::Instance' in d or 'ExternalOutControl::Instance' in d:
  print(hex(v),sz,d)
