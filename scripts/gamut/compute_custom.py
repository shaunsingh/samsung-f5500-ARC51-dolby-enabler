import numpy as np
colors=['R','G','B','Y','C','M']
xyY={'R':(.6481,.3402,31.3747),'G':(.2839,.6194,93.7781),'B':(.1453,.0635,9.0305)}
M=np.stack([np.array([x/y*Y,Y,(1-x-y)/y*Y]) for x,y,Y in xyY.values()],axis=1)
Wxyz=np.array([117.3570,122.1888,131.2933])
# Rec.709 primaries, with reference color Y from measured 100% white.
prs=[(.64,.33,.2126),(.30,.60,.7152),(.15,.06,.0722)]
Tprim=[]
for x,y,yr in prs:
 Y=yr*Wxyz[1];Tprim.append(np.array([x/y*Y,Y,(1-x-y)/y*Y]))
Tprim=np.stack(Tprim,axis=1)
T=np.concatenate([Tprim,np.stack([Tprim[:,0]+Tprim[:,1],Tprim[:,1]+Tprim[:,2],Tprim[:,0]+Tprim[:,2]],axis=1)],axis=1)
for gamma in (1.0,2.0,2.2,2.4):
 P=M.copy()
 P[:,1]=(M[:,1]-0.05**gamma*M[:,2])/1.2**gamma
 L=np.linalg.solve(P,T)
 print('\n=== gamma',gamma,'===')
 print('physical G xyY',P[0,1]/P[:,1].sum(),P[1,1]/P[:,1].sum(),P[1,1])
 print('linear coefficients rows\n',np.round(L.T,5))
 print('custom sliders rows R,G,B,Y,C M')
 S=np.zeros((6,3))
 for i in range(6):
  for j in range(3):
   on=(i<3 and i==j) or (i==3 and j<2) or (i==4 and j>0) or (i==5 and j!=1)
   Lij=L[j,i]
   w=np.sign(Lij)*abs(Lij)**(1/gamma)
   S[i,j]=(50 if on else 100)*w
 print(np.round(S,2))
 print('rounded\n',np.rint(S).astype(int))
