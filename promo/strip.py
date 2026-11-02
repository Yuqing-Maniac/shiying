import subprocess, sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
F='/home/claude/shiying/vendor/ffmpeg-x86_64'
src, crop, fps, cols, t0, t1 = sys.argv[1], sys.argv[2], float(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5]), float(sys.argv[6])
cw,ch,cx,cy = map(int, crop.split(':')); tw=320; th=int(ch*tw/cw)
raw=subprocess.run([F,'-v','error','-ss',str(t0),'-to',str(t1),'-i',src,'-vf',f'fps={fps},crop={crop},scale={tw}:{th}','-f','rawvideo','-pix_fmt','rgb24','-'],capture_output=True).stdout
fr=np.frombuffer(raw,np.uint8).reshape(-1,th,tw,3); n=len(fr); rows=(n+cols-1)//cols
out=Image.new('RGB',(cols*tw,rows*th)); d=ImageDraw.Draw(out); f=ImageFont.truetype('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',18)
for i,a in enumerate(fr):
    x,y=(i%cols)*tw,(i//cols)*th; out.paste(Image.fromarray(a),(x,y)); d.rectangle([x,y,x+70,y+22],fill='black'); d.text((x+3,y+1),f'{t0+i/fps:.1f}',font=f,fill='yellow')
out.save(sys.argv[7])
