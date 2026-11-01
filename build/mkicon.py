# 生成程序图标：PNG（桌面文件用）＋ C 头文件（_NET_WM_ICON 与界面内绘制用）
from PIL import Image, ImageDraw, ImageFilter
import sys, os
out = sys.argv[1]
def make(S):
    k = 4; N = S*k
    im = Image.new("RGBA", (N, N), (0,0,0,0))
    d = ImageDraw.Draw(im)
    # 渐变底：深蓝→亮蓝
    grad = Image.new("RGBA", (N, N))
    gd = ImageDraw.Draw(grad)
    for y in range(N):
        t = y/(N-1)
        c = (int(30+(55-30)*t), int(96+(144-96)*t), int(210+(250-210)*t), 255)
        gd.line([(0,y),(N,y)], fill=c)
    mask = Image.new("L", (N,N), 0)
    m = ImageDraw.Draw(mask)
    pad = int(N*0.06); r = int(N*0.22)
    m.rounded_rectangle([pad,pad,N-pad,N-pad], radius=r, fill=255)
    im.paste(grad, (0,0), mask)
    # 屏幕框
    sx0, sy0, sx1, sy1 = int(N*0.20), int(N*0.25), int(N*0.80), int(N*0.68)
    d.rounded_rectangle([sx0,sy0,sx1,sy1], radius=int(N*0.06), outline=(255,255,255,255), width=int(N*0.055))
    # 底座
    d.rounded_rectangle([int(N*0.38), int(N*0.74), int(N*0.62), int(N*0.79)], radius=int(N*0.025), fill=(255,255,255,235))
    # 录制红点（白圈）
    cx, cy, rr = int(N*0.50), int(N*0.465), int(N*0.115)
    d.ellipse([cx-rr-int(N*0.03), cy-rr-int(N*0.03), cx+rr+int(N*0.03), cy+rr+int(N*0.03)], fill=(255,255,255,255))
    d.ellipse([cx-rr, cy-rr, cx+rr, cy+rr], fill=(240,65,52,255))
    return im.resize((S,S), Image.LANCZOS)
os.makedirs(out, exist_ok=True)
for S in (16,24,32,48,64,128,256):
    make(S).save(f"{out}/shiying-{S}.png")
make(256).save(f"{out}/shiying.png")
with open(f"{out}/icon_data.h","w") as f:
    f.write("/* 自动生成：tools/mkicon.py */\n")
    for S in (32,48,64):
        im = make(S); px = im.load()
        vals = []
        for y in range(S):
            for x in range(S):
                r,g,b,a = px[x,y]
                # 预乘 alpha，便于界面直接合成
                vals.append((a<<24)|((r*a//255)<<16)|((g*a//255)<<8)|(b*a//255))
        f.write(f"static const unsigned int ICON{S}[{S*S}] = {{\n")
        for i in range(0,len(vals),8):
            f.write("  "+",".join(f"0x{v:08x}u" for v in vals[i:i+8])+",\n")
        f.write("};\n")
print("ok")
