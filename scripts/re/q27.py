import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV')
terms=('ExternalOut','SPDIF','Spdif','AudioMode','AudioFormat','HDMIAudio','AC3','Dolby','Surround')
for n,(v,sz) in sorted(b.byname.items(),key=lambda x:x[1][0]):
 d=demangle([n])[0]
 if any(t in d for t in terms) and ('TCSpeaker' in d or 'ExternalOutControl' in d or 'VenderAudio' in d or 'Speaker' in d and 'Ss' in d):
  print(hex(v),sz,d)
