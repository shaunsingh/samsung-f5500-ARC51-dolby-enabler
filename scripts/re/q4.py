import sys; sys.path.insert(0,'/tmp/re')
from redis import *
b=Bin('/tmp/re/libUTOPIA.so')
def dis(a,n):
    print('\n'.join(disasm(b,a,n)))
dis(0x42bab07c,36); print('--'); dis(0x42bab0c4,40)
