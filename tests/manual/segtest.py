import os, subprocess, time, threading, sys
F='app/bin/ffmpeg'
preset, q, dur, size = sys.argv[1], sys.argv[2], float(sys.argv[3]), sys.argv[4]
cr, cw = os.pipe(); ar, aw = os.pipe()
args=[F,'-hide_banner','-loglevel','info','-thread_queue_size','4096','-probesize','32','-analyzeduration','0','-f','s16le','-ar','48000','-ac','2','-i','/proc/self/fd/3',
 '-thread_queue_size',q,'-probesize','32','-analyzeduration','0','-f','x11grab','-framerate','24','-draw_mouse','1','-video_size',size,'-i',':99+0,0',
 '-map','1:v','-map','0:a','-c:v','libx264','-preset',preset,'-crf','24','-pix_fmt','yuv420p','-g','96','-threads','4',
 '-x264-params','rc-lookahead=10:sync-lookahead=0','-c:a','aac','-b:a','128k','-f','matroska','-y','/tmp/t/seg.mkv']
def pre(): os.dup2(cr,0); os.dup2(ar,3)
t0=time.time()
p=subprocess.Popen(args, preexec_fn=pre, pass_fds=(3,), stderr=open('/tmp/t/seg.log','w'))
os.close(cr); os.close(ar)
stop=False
def w():
    t0=time.time(); n=0
    while not stop:
        due=int((time.time()-t0)*48000)-n
        if due>0:
            try: os.write(aw, b'\x00\x10'*2*due)
            except OSError: break
            n+=due
        time.sleep(0.01)
threading.Thread(target=w,daemon=True).start()
peak=0
while time.time()-t0<dur:
    time.sleep(0.2)
    try: peak=max(peak,int(open(f'/proc/{p.pid}/status').read().split('VmRSS:')[1].split()[0]))
    except: pass
os.write(cw,b'q'); t=time.time(); p.wait(); stop=True
r=subprocess.run([F,'-hide_banner','-i','/tmp/t/seg.mkv'],capture_output=True,text=True).stderr
d=[l for l in r.splitlines() if 'Duration' in l][0].strip()
print(preset,q,size,'wall',dur,'|',d[:40],'| peakRSS MB',peak//1024,'| exit',round(time.time()-t,2))
