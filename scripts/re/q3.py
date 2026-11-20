import sys; sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/libSDAL.so')
for n,v in b.byname.items():
    if 'VenderVideo_Write2BytesMsk' in n or 'VenderVideo_Read2BytesMsk' in n:
        print(hex(v[0]),v[1],n)
        print('\n'.join(disasm(b,v[0]&~1,min(v[1],400))))
