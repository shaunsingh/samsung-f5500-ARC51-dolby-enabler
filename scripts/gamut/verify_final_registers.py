import re,ast
log=open('/tmp/re/cmfactory_final.log').read().splitlines()
rows=[list(map(int,m.group(1).split())) for line in log if (m:=re.match(r'BANK14 \S+ ((?:\d+ ?)+)$',line))]
vals=sum(rows[-6:],[])[:81]
text=open('/tmp/re/preset_registers.txt').read().split('custom\n',1)[1]
expected=[]
for ch in 'RGB':expected += ast.literal_eval(re.search(rf'^{ch} (\[.*\])$',text,re.M).group(1))
print('post-reboot live registers',len(vals),'expected',len(expected),'exact match',vals==expected)
print('mismatches',[(i,a,b) for i,(a,b) in enumerate(zip(vals,expected)) if a!=b])
