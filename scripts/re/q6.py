import sys; sys.path.insert(0,'/tmp/re')
from redis import *
for lib in ('/tmp/re/libUTOPIA.so','/tmp/re/libSDAL.so','/tmp/re/exeTV'):
    b=Bin(lib)
    for n,(v,sz) in b.byname.items():
        if 'PixelData' in n or 'ReadPixel' in n or 'PixelRGB' in n or 'GetPixel' in n:
            print(lib, hex(v), sz, demangle([n])[0])
