import numpy as np, subprocess, sys
F='/tmp/t/app/bin/ffmpeg'; f=sys.argv[1]
r=subprocess.run([F,'-v','error','-i',f,'-map','0:a','-af','aresample=async=1:first_pts=0','-f','s16le','-ac','1','-ar','48000','-'],capture_output=True).stdout
a=np.frombuffer(r,dtype=np.int16).astype(float)
for t in range(1,int(len(a)/48000)-1,4):
    seg=a[t*48000:(t+1)*48000]; sp=np.abs(np.fft.rfft(seg))
    p440=sp[438:443].max(); p880=sp[878:883].max()
    env=[np.sqrt((seg[i:i+480]**2).mean()) for i in range(0,48000-480,480)]
    print(f"t={t:2d}s 440Hz={p440/1e6:6.2f} 880Hz={p880/1e6:6.2f} min10msRMS={min(env):7.0f}")
