import sys;sys.path.insert(0,'/tmp/re')
from redis import *
for lib in ('/tmp/re/libUTOPIA.so','/tmp/re/libSDAL.so','/tmp/re/exeTV'):
 b=Bin(lib); print('===',lib)
 for n,(v,sz) in b.byname.items():
  ln=n.lower()
  if ('lut' in ln and ('3d' in ln or 'color' in ln or 'xv' in ln)) or '3dlut' in ln or 'colorcorrection' in ln or 'colormapping' in ln:
   print(hex(v),sz,demangle([n])[0])
