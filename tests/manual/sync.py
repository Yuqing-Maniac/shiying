import numpy as np, subprocess, sys
F='/tmp/t/app/bin/ffmpeg'; f=sys.argv[1]
r=subprocess.run([F,'-v','error','-i',f,'-map','0:a','-af','aresample=async=1:first_pts=0','-f','s16le','-ac','1','-ar','48000','-'],capture_output=True).stdout
a=np.frombuffer(r,dtype=np.int16).astype(float)
win=480; env=np.array([np.sqrt((a[i:i+win]**2).mean()) for i in range(0,len(a)-win,win)])
on=env>3000; ta=[i*win/48000 for i in range(1,len(on)) if on[i]!=on[i-1]]
# video: mean luma of a 64x36 thumb per frame with timestamps
p=subprocess.run([F,'-v','error','-i',f,'-map','0:v','-fps_mode','passthrough','-vf','scale=64:36,format=gray','-f','rawvideo','-'],capture_output=True).stdout
fr=np.frombuffer(p,dtype=np.uint8).reshape(-1,36*64)
pts=subprocess.run([F,'-v','info','-i',f,'-map','0:v','-fps_mode','passthrough','-vf','showinfo','-f','null','-'],capture_output=True,text=True).stderr
import re; ts=[float(x) for x in re.findall(r'pts_time:([0-9.]+)',pts)]
lum=fr[:, :].mean(1)  # 
# use bottom-left region away from window
lum=fr.reshape(-1,36,64)[:,30:36,0:10].mean((1,2))
w=lum>128; tv=[ts[i] for i in range(1,len(w)) if w[i]!=w[i-1]]
print('dur audio',len(a)/48000,'video frames',len(ts),'last pts',ts[-1] if ts else None)
print('audio edges',[round(x,3) for x in ta])
print('video edges',[round(x,3) for x in tv])
