import sys, struct, bisect
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
import subprocess
class Bin:
    def __init__(self, path):
        self.f = open(path,'rb'); self.data = self.f.read(); self.elf = ELFFile(open(path,'rb'))
        self.segs = [(s['p_vaddr'], s['p_filesz'], s['p_offset']) for s in self.elf.iter_segments() if s['p_type']=='PT_LOAD']
        self.syms = {}; self.byname = {}
        for secname in ('.dynsym','.symtab'):
            sec = self.elf.get_section_by_name(secname)
            if not sec: continue
            for s in sec.iter_symbols():
                v = s['st_value']
                if v and s.name:
                    self.syms.setdefault(v & ~1, s.name); self.byname[s.name] = (v, s['st_size'])
        self.addrs = sorted(self.syms)
        # PLT: map plt stubs via relocations
        self.plt = {}
        rel = self.elf.get_section_by_name('.rel.plt'); plt = self.elf.get_section_by_name('.plt')
        if rel and plt:
            dsym = self.elf.get_section_by_name('.dynsym')
            got2name = {r['r_offset']: dsym.get_symbol(r['r_info_sym']).name for r in rel.iter_relocations()}
            base = plt['sh_addr']; size = plt['sh_size']
            a = base + 20
            while a < base + size:
                ins = struct.unpack('<3I', self.read(a, 12))
                # add ip, pc, #x ; add ip, ip, #y ; ldr pc, [ip, #z]!
                try:
                    def imm(w):
                        rot = ((w >> 8) & 0xf) * 2; v = w & 0xff
                        return ((v >> rot) | (v << (32 - rot))) & 0xffffffff
                    got = a + 8 + imm(ins[0]) + imm(ins[1]) + (ins[2] & 0xfff)
                    if got in got2name: self.plt[a] = got2name[got] + '@plt'
                except Exception: pass
                a += 12
    def off(self, va):
        for v, sz, o in self.segs:
            if v <= va < v + sz: return o + va - v
        raise KeyError(hex(va))
    def read(self, va, n): o = self.off(va); return self.data[o:o+n]
    def u32(self, va): return struct.unpack('<I', self.read(va, 4))[0]
    def name(self, va):
        if va in self.plt: return self.plt[va]
        i = bisect.bisect_right(self.addrs, va) - 1
        if i >= 0:
            b = self.addrs[i]; n = self.syms[b]
            return n + ('' if b == va else f'+{va-b:#x}')
        return hex(va)
def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
    return out
def disasm(b, start, size, show=True):
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM); md.detail = False
    code = b.read(start, size); lines = []
    for i in md.disasm(code, start):
        s = f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}'
        if i.mnemonic in ('bl','b','blx') and i.op_str.startswith('#'):
            t = int(i.op_str[1:], 16); s += '    ; ' + demangle([b.name(t)])[0]
        if 'pc' in i.op_str and i.mnemonic.startswith('ldr') and '[pc' in i.op_str:
            try:
                o = int(i.op_str.split('#')[-1].rstrip(']'), 16) if '#' in i.op_str else 0
                lit = b.u32(i.address + 8 + o); s += f'    ; ={lit:#x} {demangle([b.name(lit)])[0] if lit in b.syms else ""}'
            except Exception: pass
        lines.append(s)
    return lines
def func(b, name_sub, which=0):
    c = [(n, v) for n, v in b.byname.items() if name_sub in n]
    return c
