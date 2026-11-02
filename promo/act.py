# 场景驱动：平滑鼠标、点击、按键、录屏
import os, sys, time, subprocess, math, json
from Xlib import X, display, XK
from Xlib.ext import xtest
d = display.Display(':99')
FF = '/home/claude/shiying/vendor/ffmpeg-x86_64'
cap = None
def pos():
    p = d.screen().root.query_pointer(); return p.root_x, p.root_y
def move(x, y, dur=0.5):
    x0, y0 = pos(); n = max(2, int(dur * 90))
    for i in range(1, n + 1):
        t = i / n; e = t * t * (3 - 2 * t)
        xtest.fake_input(d, X.MotionNotify, x=int(x0 + (x - x0) * e), y=int(y0 + (y - y0) * e)); d.sync()
        time.sleep(dur / n)
def click(x=None, y=None, dur=0.45, hold=0.08):
    if x is not None: move(x, y, dur)
    time.sleep(0.06)
    xtest.fake_input(d, X.ButtonPress, 1); d.sync(); time.sleep(hold)
    xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
def down(): xtest.fake_input(d, X.ButtonPress, 1); d.sync()
def up(): xtest.fake_input(d, X.ButtonRelease, 1); d.sync()
def key(*names):
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in names]
    for c in codes: xtest.fake_input(d, X.KeyPress, c); d.sync(); time.sleep(0.03)
    for c in reversed(codes): xtest.fake_input(d, X.KeyRelease, c); d.sync()
def cap_start(name):
    global cap
    cap = subprocess.Popen([FF, '-hide_banner', '-loglevel', 'error', '-f', 'x11grab', '-framerate', '30', '-draw_mouse', '1',
        '-video_size', '1920x1080', '-i', ':99+0,0', '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '12', '-pix_fmt', 'yuv420p',
        '-y', f'/tmp/v/rec/{name}.mkv'], stdin=subprocess.PIPE)
    time.sleep(1.0)
def cap_stop():
    global cap
    time.sleep(0.6); cap.stdin.write(b'q'); cap.stdin.flush(); cap.wait(); cap = None
def sh(c): subprocess.run(c, shell=True)
def W(x, y): return 721 + x, 244 + y     # 主窗口客户区坐标 → 屏幕坐标
