import sys,struct
sys.path.insert(0,'/tmp/re')
from redis import Bin
from unicorn import *
from unicorn.arm_const import *
b=Bin('/tmp/re/exeTV');mu=Uc(UC_ARCH_ARM,UC_MODE_ARM)
for s in b.elf.iter_segments():
 if s['p_type']!='PT_LOAD':continue
 va=s['p_vaddr'];lo=va&~0xfff;hi=(va+s['p_memsz']+0xfff)&~0xfff
 mu.mem_map(lo,hi-lo);mu.mem_write(va,b.data[s['p_offset']:s['p_offset']+s['p_filesz']])
STACK=0x70000000;mu.mem_map(STACK-0x100000,0x100000)
OBJ=0x60000000;mu.mem_map(OBJ,0x10000);SET=OBJ+0x1000
mu.reg_write(UC_ARM_REG_C1_C0_2,mu.reg_read(UC_ARM_REG_C1_C0_2)|(0xf<<20));mu.reg_write(UC_ARM_REG_FPEXC,0x40000000)
RET=0x60008000;writes={};regs_in=[512]*0x52
def hook(mu,addr,size,ud):
 lr=mu.reg_read(UC_ARM_REG_LR)
 if addr==0x6306e4:mu.reg_write(UC_ARM_REG_R0,regs_in[mu.reg_read(UC_ARM_REG_R1)])
 elif addr==0x62de40:writes[mu.reg_read(UC_ARM_REG_R1)]=mu.reg_read(UC_ARM_REG_R2)
 elif addr in (0x6401bc,0x64022c):mu.reg_write(UC_ARM_REG_R0,0)
 else:return
 mu.reg_write(UC_ARM_REG_PC,lr)
for a in (0x6306e4,0x62de40,0x6401bc,0x64022c):mu.hook_add(UC_HOOK_CODE,hook,begin=a,end=a)
def run(cid,fields,plat=2):
 writes.clear();mu.mem_write(OBJ,b'\0'*0x100);mu.mem_write(SET,struct.pack('<54I',*fields));mu.mem_write(0x1eb40a0,struct.pack('<I',plat))
 mu.reg_write(UC_ARM_REG_R0,OBJ);mu.reg_write(UC_ARM_REG_R1,cid);mu.reg_write(UC_ARM_REG_R2,SET);mu.reg_write(UC_ARM_REG_SP,STACK-0x1000);mu.reg_write(UC_ARM_REG_LR,RET)
 mu.emu_start(0x702428,RET,count=200000)
 for k,v in writes.items():regs_in[k]=v
 return regs_in[1:0x52]
def table_run(vals,ids,offset):
 for k in range(0x52):regs_in[k]=512
 f=[0]*54;f[offset:offset+18]=vals
 out=[512]*81
 for cid in ids:out=run(cid,f)
 return out
native=[50,0,0, 0,60,5, 0,0,50, 52,48,0, 0,48,52, 50,0,50]
custom=[46,0,17, 34,57,16, 20,0,49, 48,56,21, 37,56,51, 46,0,50]
for name,vals,ids,off in [('native',native,[0x2c2,0x2c5,0x2c8,0x2cb,0x2ce,0x2d1],18),('custom',custom,[0x2b0,0x2b3,0x2b6,0x2b9,0x2bc,0x2bf],0)]:
 out=table_run(vals,ids,off)
 print(name)
 for base in range(0,81,27):print('RGB'[base//27],out[base:base+27])
 print('nonidentity',[(i+1,v) for i,v in enumerate(out) if v!=512])
