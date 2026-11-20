import sys; sys.path.insert(0,'/tmp/re')
exec(open('/tmp/re/q7.py').read().split("for n,(v,sz)")[0])
for n,(v,sz) in b.byname.items():
    if n in ('MApi_XC_Get_Pixel_RGB','MApi_XC_ReportPixelInfo') or 'Get_Pixel_RGB' in n or 'ReportPixelInfo' in n:
        print('=====',n,hex(v),sz); dis(v&~1, sz)
