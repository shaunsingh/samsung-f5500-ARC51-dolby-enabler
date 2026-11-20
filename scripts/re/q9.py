import sys; sys.path.insert(0,'/tmp/re')
exec(open('/tmp/re/q7.py').read().split("for n,(v,sz)")[0])
for n,(v,sz) in b.byname.items():
    if 'Hal_SC_get_pixel_rgb' in n:
        print('=====',n,hex(v),sz); dis(v&~1, sz)
