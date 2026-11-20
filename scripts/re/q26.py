import sys;sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/exeTV');a=b.byname['_ZN17PrimePictureShare26m_PrimePicture_SetColorSpaceEP10Settings_t'][0] if '_ZN17PrimePictureShare26m_PrimePicture_SetColorSpaceEP10Settings_t' in b.byname else 0x70437c
print(hex(a),demangle([b.name(a)])[0]);print('\n'.join(disasm(b,a,0x90)))
