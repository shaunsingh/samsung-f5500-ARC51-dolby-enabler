import numpy as np, colour
colors=['R','G','B','Y','C','M']
xyY={'R':(.6481,.3402,31.3747),'G':(.2839,.6194,93.7781),'B':(.1453,.0635,9.0305)}
M=np.stack([np.array([x/y*Y,Y,(1-x-y)/y*Y]) for x,y,Y in xyY.values()],axis=1)
W=np.array([117.3570,122.1888,131.2933]); wxy=W[:2]/W.sum()
gamma_measured=2.2; gamma_current=2.4
P=M.copy(); P[:,1]=(M[:,1]-0.05**gamma_measured*M[:,2])/1.2**gamma_measured
prs=[(.64,.33,.2126),(.30,.60,.7152),(.15,.06,.0722)]
Tprim=[]
for x,y,yr in prs:
 Y=yr*W[1]; Tprim.append(np.array([x/y*Y,Y,(1-x-y)/y*Y]))
Tprim=np.stack(Tprim,axis=1)
T=np.concatenate([Tprim,np.stack([Tprim[:,0]+Tprim[:,1],Tprim[:,1]+Tprim[:,2],Tprim[:,0]+Tprim[:,2]],axis=1)],axis=1)
L=np.linalg.solve(P,T)

def on_axis(i,j):
 return (i<3 and i==j) or (i==3 and j<2) or (i==4 and j>0) or (i==5 and j!=1)
def initial(i,j):
 v=L[j,i]
 return (50 if on_axis(i,j) else 100)*(np.sign(v)*abs(v)**(1/gamma_current))
def output(s,i):
 w=np.array([s[j]/(50 if on_axis(i,j) else 100) for j in range(3)])
 return (w**gamma_current)@P.T

def de(xyz,i):
 return float(colour.delta_E(colour.XYZ_to_Lab(xyz/100,wxy),colour.XYZ_to_Lab(T[:,i]/100,wxy),method='CIE 2000'))
def desc(xyz):
 return 'xy=(%.4f,%.4f) Y=%.2f dE00=%.3f'%(xyz[0]/xyz.sum(),xyz[1]/xyz.sum(),xyz[1],de(xyz,current_i))

results=[]
for current_i,c in enumerate(colors):
 s=np.array([np.clip(initial(current_i,j),0,100) for j in range(3)],float)
 direct=np.rint(s)
 for step in (10,5,2,1,.5,.2,.1,.05,.02,.01):
  improved=True
  while improved:
   improved=False; base=de(output(s,current_i),current_i)
   best=s.copy(); bestde=base
   for j in range(3):
    for d in (-step,step):
     q=s.copy();q[j]=np.clip(q[j]+d,0,100)
     e=de(output(q,current_i),current_i)
     if e<bestde-1e-9:bestde=e;best=q
   if bestde<base-1e-9:s=best;improved=True
 # integer search +/-3 around continuous optimum
 center=np.rint(s).astype(int); besti=center.copy(); bestde=de(output(center,current_i),current_i)
 for a in range(max(0,center[0]-3),min(100,center[0]+3)+1):
  for b in range(max(0,center[1]-3),min(100,center[1]+3)+1):
   for cc in range(max(0,center[2]-3),min(100,center[2]+3)+1):
    q=np.array([a,b,cc]);e=de(output(q,current_i),current_i)
    if e<bestde:bestde=e;besti=q.copy()
 results.append(besti)
 print('\n',c,'target',desc(T[:,current_i]))
 print(' direct ',direct.astype(int).tolist(),desc(output(direct,current_i)))
 print(' continuous',np.round(s,3).tolist(),desc(output(s,current_i)))
 print(' INTEGER ',besti.tolist(),desc(output(besti,current_i)))
print('\nFINAL')
for c,s in zip(colors,results):print(c,*s)
print('\nNative measured dE vs target')
for i,c in enumerate(colors[:3]):print(c,desc(M[:,i]))
