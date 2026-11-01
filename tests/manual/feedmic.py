import numpy as np, time, os
fd=os.open('/tmp/t/mic.fifo', os.O_WRONLY)
t0=time.time(); n=0
while True:
    due=int((time.time()-t0)*48000)-n
    if due>0:
        t=(np.arange(n,n+due))/48000.0
        s=(np.sin(2*np.pi*880*t)*6000).astype(np.int16)
        os.write(fd, np.repeat(s,2).tobytes()); n+=due
    time.sleep(0.01)
