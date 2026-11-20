import sys;sys.path.insert(0,'/tmp/re')
from redis import *
jobs=[('/tmp/re/libSDAL.so',[
'SdSound_InstallCallbackPCMCapture','SdSound_GetCallbackPCMCapture','AU_PCM_Capture_CB','AU_PcmCapture_Init','AU_PCM_Capture_Dump','SdSound_GetPcmBufferLevel','MApi_AUDIO_UNI_Get_OutPCMLevel']),('/tmp/re/libUTOPIA.so',['MApi_AUDIO_UNI_Get_OutPCMLevel','MApi_AUDIO_GetPCMBufferSize','MApi_AUDIO_GetPCMBufferSize2'])]
for path,names in jobs:
 b=Bin(path);print('FILE',path)
 for n,(a,sz) in b.byname.items():
  d=demangle([n])[0]
  if any(d.startswith(x+'(') for x in names):
   print('\n===',hex(a),sz,d);print('\n'.join(disasm(b,a,sz)))
