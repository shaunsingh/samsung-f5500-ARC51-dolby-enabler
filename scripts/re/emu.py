import sys, struct
sys.path.insert(0,'/tmp/re')
from redis import Bin
from unicorn import *
from unicorn.arm_const import *
b = Bin('/tmp/re/exeTV')
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
for s in b.elf.iter_segments():
    if s['p_type']!='PT_LOAD': continue
    va=s['p_vaddr']; lo=va&~0xfff; hi=(va+s['p_memsz']+0xfff)&~0xfff
    mu.mem_map(lo, hi-lo)
    mu.mem_write(va, b.data[s['p_offset']:s['p_offset']+s['p_filesz']])
STACK=0x70000000; mu.mem_map(STACK-0x100000, 0x100000)
OBJ=0x60000000; mu.mem_map(OBJ, 0x10000)
SET=OBJ+0x1000
# enable VFP
mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xf<<20))
mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
RET=0x60008000
writes={}
regs_in=[512]*0x52
def hook(mu, addr, size, ud):
    lr=mu.reg_read(UC_ARM_REG_LR)
    if addr==0x6306e4:
        mu.reg_write(UC_ARM_REG_R0, regs_in[mu.reg_read(UC_ARM_REG_R1)])
    elif addr==0x62de40:
        writes[mu.reg_read(UC_ARM_REG_R1)]=mu.reg_read(UC_ARM_REG_R2)
    elif addr in (0x6401bc,0x64022c):
        mu.reg_write(UC_ARM_REG_R0,0)
    else: return
    mu.reg_write(UC_ARM_REG_PC, lr)
for a in (0x6306e4,0x62de40,0x6401bc,0x64022c):
    mu.hook_add(UC_HOOK_CODE, hook, begin=a, end=a)
def run(cid, fields, plat):
    writes.clear()
    mu.mem_write(OBJ, b'\0'*0x100)
    mu.mem_write(SET, struct.pack('<54I', *fields))
    mu.mem_write(0x1eb40a0, struct.pack('<I', plat))
    mu.reg_write(UC_ARM_REG_R0, OBJ); mu.reg_write(UC_ARM_REG_R1, cid); mu.reg_write(UC_ARM_REG_R2, SET)
    mu.reg_write(UC_ARM_REG_SP, STACK-0x1000); mu.reg_write(UC_ARM_REG_LR, RET)
    mu.emu_start(0x702428, RET, count=200000)
    return [writes.get(k) for k in range(1,0x52)]
ident=[50,0,0, 0,50,0, 0,0,50, 50,50,0, 0,50,50, 50,0,50]
names=[c+'_'+ch for c in ('R','G','B','Y','C','M') for ch in 'RGB']
plat=int(sys.argv[1],0)
for tbl,base in ((0,0x2b0),):
    f=ident*3
    out0=run(base, f, plat)
    print('plat',plat,'ident out (reg-512):', [None if v is None else v-512 for v in out0])
    for cid in range(base, base+18):
        o0=run(cid,f,plat)
        for i in range(18):
            g=list(f); g[tbl*18+i]+=10
            o=run(cid,g,plat)
            d={k+1:(o[k]-o0[k]) for k in range(81) if o[k]!=o0[k]}
            if d: print(hex(cid), names[i], d)
