import numpy as np
colors=['R','G','B','Y','C','M']
xyY={'R':(.6481,.3402,31.37),'G':(.2839,.6194,93.78),'B':(.1453,.0635,9.03)}
XYZ={c:np.array([x/y*Y,Y,(1-x-y)/y*Y]) for c,(x,y,Y) in xyY.items()}
native=np.array([[50,0,0],[0,60,5],[0,0,50],[52,48,0],[0,48,52],[50,0,50]],float)
auto=np.array([[59,29,30],[86,54,28],[13,29,53],[50,53,39],[85,48,52],[44,39,52]],float)
Bp=XYZ['B']; Rp=XYZ['R']; Gp=(XYZ['G']-.05*Bp)/1.2
P=np.stack([Rp,Gp,Bp],axis=1)
print('physical primaries XYZ columns\n',P)
for i,c in enumerate('RGB'):
 v=P[:,i]; s=v.sum(); print('physical',c,'xy',v[:2]/s,'Y',v[1])
prim709=np.array([[.64,.30,.15],[.33,.60,.07]])
Y709=np.array([.2126,.7152,.0722])*122.189
T=[]
for i in range(3):
 x,y=prim709[:,i]; Y=Y709[i]; T.append([x/y*Y,Y,(1-x-y)/y*Y])
T=np.stack(T,axis=1)
print('target matrix\n',T)
print('linear physical->709\n',np.linalg.solve(P,T))
def coeff_for(tab):
 coeff=np.zeros((6,3))
 for i in range(6):
  for j in range(3):
   on = (i<3 and i==j) or (i==3 and j<2) or (i==4 and j>0) or (i==5 and j!=1)
   coeff[i,j]=tab[i,j]/(50 if on else 100)
 return coeff
for name,tab in [('native',native),('auto',auto)]:
 coeff=coeff_for(tab)
 print('\n',name,'coeff\n',coeff)
 for gamma in (1.0,2.2,2.4):
  print('domain gamma',gamma)
  for i,c in enumerate(colors):
   w=coeff[i]
   out=(np.maximum(w,0)**gamma)@P.T
   xy=out[:2]/out.sum()
   print(c,'xy',np.round(xy,4),'Y',round(out[1],2))
