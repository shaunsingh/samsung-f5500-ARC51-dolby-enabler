import struct
src='/tmp/re/pic_pre709.bin';dst='/tmp/re/PicSettings_HDMI3_2_BT709.bin'
b=bytearray(open(src,'rb').read())
vals=[46,0,17, 34,57,16, 20,0,49, 48,56,21, 37,56,51, 46,0,50]
b[0x28:0x2c]=struct.pack('<i',4)
for i in range(6):b[0x2c+4*i:0x2c+4*i+3]=bytes(vals[3*i:3*i+3])
open(dst,'wb').write(b)
a=open(src,'rb').read()
diffs=[(i,a[i],b[i]) for i in range(len(a)) if a[i]!=b[i]]
print('length',len(b),'changed bytes',len(diffs))
print('diffs',diffs)
assert len(a)==len(b)==312
assert all((0x28<=i<0x2c) or any(0x2c+4*j<=i<0x2c+4*j+3 for j in range(6)) for i,_,_ in diffs)
print('color space',struct.unpack('<i',b[0x28:0x2c])[0])
for i,c in enumerate(('R','G','B','Y','C','M')):print(c,list(b[0x2c+4*i:0x2c+4*i+3]))
