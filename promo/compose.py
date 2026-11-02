# 拾影录屏 快节奏宣传片合成：PIL 逐帧绘制 → ffmpeg 编码
import json, math, subprocess, sys, numpy as np
from functools import lru_cache
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FF = '/home/claude/shiying/vendor/ffmpeg-x86_64'
W, H, FPS, T = 1920, 1080, 30, 34.0
B = 0.5                                     # 一拍
ONLY = None if len(sys.argv) < 2 else [float(x) for x in sys.argv[1].split(',')]   # 抽帧预览

ACC = (47, 125, 246); RED = (238, 63, 53); GREEN = (25, 191, 105); ORANGE = (245, 160, 40); WHITE = (255, 255, 255)

# ---------------- 字体 ----------------
DYH = {int(k): v for k, v in json.load(open('/tmp/v/dyh_cmap.json')).items()}
NOTO_R = '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc'
NOTO_B = '/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc'
NOTO_K = '/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc'

@lru_cache(None)
def font(path, size, index=2):
    return ImageFont.truetype(path, size, index=index if path.endswith('.ttc') else 0)

@lru_cache(None)
def text(s, size, kind='dyh', color=WHITE, spacing=0):
    """返回 RGBA 文字图。kind: dyh 得意黑（逐字选子集字体）/ r / b / k 思源黑体简体"""
    if kind == 'dyh':
        fallback = font(NOTO_K, size)
        glyphs = []
        for ch in s:
            f = font(DYH[ord(ch)], size) if ord(ch) in DYH else fallback
            glyphs.append((ch, f, f.getlength(ch)))
        w = int(sum(g[2] for g in glyphs) + spacing * max(0, len(s) - 1) + size * 0.3)
        im = Image.new('RGBA', (w, int(size * 1.35)), (0, 0, 0, 0))
        d = ImageDraw.Draw(im); x = size * 0.1
        for ch, f, adv in glyphs:
            d.text((x, size * 0.12), ch, font=f, fill=color); x += adv + spacing
        return im.crop(im.getbbox() or (0, 0, 1, 1))
    f = font({'r': NOTO_R, 'b': NOTO_B, 'k': NOTO_K}[kind], size)
    bb = f.getbbox(s)
    im = Image.new('RGBA', (bb[2] - bb[0] + 4, bb[3] - bb[1] + 4), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((2 - bb[0], 2 - bb[1]), s, font=f, fill=color)
    return im

# ---------------- 缓动 ----------------
def clamp(x, a=0.0, b=1.0): return max(a, min(b, x))
def ease_out(x): x = clamp(x); return 1 - (1 - x) ** 3
def ease_back(x, s=1.9):
    x = clamp(x); x -= 1; return x * x * ((s + 1) * x + s) + 1
def ease_in(x): x = clamp(x); return x ** 3
def prog(t, t0, d): return clamp((t - t0) / d)

def paste(dst, im, cx, cy, scale=1.0, alpha=1.0, anchor='c', rot=0):
    if alpha <= 0.01 or scale <= 0.01: return
    if scale != 1.0:
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.BICUBIC)
    if rot: im = im.rotate(rot, Image.BICUBIC, expand=True)
    if alpha < 1.0:
        a = im.getchannel('A').point(lambda v: int(v * alpha)); im = im.copy(); im.putalpha(a)
    if anchor == 'c': x, y = cx - im.width // 2, cy - im.height // 2
    elif anchor == 'l': x, y = cx, cy - im.height // 2
    elif anchor == 'tl': x, y = cx, cy
    else: x, y = cx - im.width, cy - im.height // 2
    dst.alpha_composite(im, (int(x), int(y)))

# ---------------- 背景 ----------------
def make_bg():
    y = np.linspace(0, 1, H)[:, None]; x = np.linspace(0, 1, W)[None, :]
    r = 9 + 10 * x + 6 * y; g = 16 + 18 * x + 14 * y; b = 32 + 34 * x + 30 * y
    img = np.stack([r + 0 * y, g + 0 * y, b + 0 * x], 2)
    for cx, cy, rad, col in [(0.78, 0.25, 0.45, (40, 90, 200)), (0.15, 0.85, 0.4, (20, 60, 140))]:
        d = np.sqrt((x - cx) ** 2 * (W / H) ** 2 + (y - cy) ** 2)
        k = np.exp(-(d / rad) ** 2)[..., None] * 0.55
        img = img * (1 - k) + np.array(col) * k
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).convert('RGBA')
BG = make_bg()
GRID = Image.new('RGBA', (W + 80, H + 80), (0, 0, 0, 0))
gd = ImageDraw.Draw(GRID)
for gx in range(0, W + 80, 40):
    for gy in range(0, H + 80, 40):
        gd.ellipse([gx - 1.2, gy - 1.2, gx + 1.2, gy + 1.2], fill=(255, 255, 255, 22))

def background(t):
    im = BG.copy()
    off = int((t * 20) % 40)
    im.alpha_composite(GRID.crop((off, off, off + W, off + H)))
    return im

# ---------------- 素材 ----------------
ICON = Image.open('/home/claude/shiying/assets/shiying-256.png').convert('RGBA')

clip_cache = {}
def clip(name, t0, t1, speed, crop, outw):
    """读取录屏片段（裁切、变速），返回帧列表"""
    key = (name, t0, t1, speed, crop, outw)
    if key in clip_cache: return clip_cache[key]
    clip_cache.clear()
    cw, ch, cx, cy = crop
    oh = int(ch * outw / cw) // 2 * 2
    raw = subprocess.run([FF, '-v', 'error', '-ss', str(t0), '-to', str(t1), '-i', f'/tmp/v/rec/{name}.mkv',
                          '-vf', f'setpts=PTS/{speed},fps={FPS},crop={cw}:{ch}:{cx}:{cy},scale={outw}:{oh}:flags=lanczos',
                          '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
    fr = np.frombuffer(raw, np.uint8).reshape(-1, oh, outw, 3)
    clip_cache[key] = fr
    return fr

@lru_cache(None)
def card_mask(w, h, r):
    m = Image.new('L', (w, h), 0); ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], r, fill=255); return m

@lru_cache(None)
def card_shadow(w, h, r):
    pad = 60
    s = Image.new('RGBA', (w + pad * 2, h + pad * 2), (0, 0, 0, 0))
    ImageDraw.Draw(s).rounded_rectangle([pad, pad + 18, pad + w, pad + h + 18], r, fill=(0, 0, 0, 150))
    return s.filter(ImageFilter.GaussianBlur(26))

def card(dst, frame, cx, cy, scale=1.0, alpha=1.0, r=18):
    im = Image.fromarray(frame).convert('RGBA')
    if scale != 1.0:
        im = im.resize((int(im.width * scale), int(im.height * scale)), Image.BICUBIC)
    w, h = im.size
    im.putalpha(card_mask(w, h, r))
    sh = card_shadow(w, h, r)
    if alpha < 1:
        sh = sh.copy(); sh.putalpha(sh.getchannel('A').point(lambda v: int(v * alpha)))
    dst.alpha_composite(sh, (int(cx - w / 2 - 60), int(cy - h / 2 - 60)))
    paste(dst, im, cx, cy, 1.0, alpha)
    # 细描边
    ov = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle([0, 0, w - 1, h - 1], r, outline=(255, 255, 255, int(60 * alpha)), width=2)
    dst.alpha_composite(ov, (int(cx - w / 2), int(cy - h / 2)))

def clip_frame(fr, t, t0):
    i = int((t - t0) * FPS)
    return fr[min(max(i, 0), len(fr) - 1)]

def pill(dst, x, y, s, size=34, bg=(255, 255, 255, 30), fg=WHITE, kind='b', anchor='l'):
    tx = text(s, size, kind, fg)
    w, h = tx.width + size * 1.1, tx.height + size * 0.7
    if anchor == 'c': x -= w / 2
    ov = Image.new('RGBA', (int(w), int(h)), (0, 0, 0, 0))
    ImageDraw.Draw(ov).rounded_rectangle([0, 0, w - 1, h - 1], h / 2, fill=bg)
    dst.alpha_composite(ov, (int(x), int(y - h / 2)))
    paste(dst, tx, x + w / 2, y, anchor='c')
    return w

def caption(dst, t, t0, title, sub=None, x=110, y=470, color=WHITE, sub_col=(190, 205, 230)):
    """左侧大字标题：得意黑斜体，逐拍弹入"""
    p = ease_back(prog(t, t0 + 0.05, 0.28))
    bar = Image.new('RGBA', (12, 120), ACC + (255,))
    paste(dst, bar, x - 30, y, 1.0, clamp(p * 2), anchor='c')
    paste(dst, text(title, 112, 'dyh', color), x + (1 - p) * -80, y, 1.0, clamp(p * 1.5), anchor='l')
    if sub:
        q = ease_out(prog(t, t0 + 0.22, 0.3))
        paste(dst, text(sub, 38, 'r', sub_col), x + (1 - q) * -40, y + 105, 1.0, q, anchor='l')

def flash(dst, t, t0, d=0.18, a=0.85):
    k = 1 - prog(t, t0, d)
    if 0 < k < 1 or (t >= t0 and k > 0):
        ov = Image.new('RGBA', (W, H), (255, 255, 255, int(255 * a * k))); dst.alpha_composite(ov)

def enter(t, t0, d=0.22):
    """场景入场：返回 (位移, 缩放, 透明度)"""
    p = ease_out(prog(t, t0, d)); return (1 - p) * 140, 1.0 + (1 - p) * 0.06, p

# ---------------- 场景 ----------------
def sc_hook(im, t):
    words = ['麒麟上录个屏', '要装一堆依赖？', '要管理员权限？', '还卡、还占内存？']
    k = min(int(t / B), 3); t0 = k * B
    p = prog(t, t0, 0.14)
    sc = 1.5 - 0.5 * ease_out(p)
    shake = (1 - prog(t, t0, 0.25)) * 10
    col = WHITE if k == 0 else (255, 214, 102) if k < 3 else (255, 120, 110)
    paste(im, text(words[k], 150, 'dyh', col), W / 2 + math.sin(t * 90) * shake, H / 2, sc, clamp(p * 3))
    # 节拍条
    for i in range(4):
        c = (255, 255, 255, 230) if i <= k else (255, 255, 255, 50)
        ov = Image.new('RGBA', (60, 8), c); paste(im, ov, W / 2 - 105 + i * 70, H / 2 + 140)
    flash(im, t, t0, 0.12, 0.25)

def sc_logo(im, t):
    t0 = 2.0
    p = ease_back(prog(t, t0, 0.35), 2.4)
    glow = Image.new('RGBA', (520, 520), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([60, 60, 460, 460], fill=ACC + (90,))
    paste(im, glow.filter(ImageFilter.GaussianBlur(60)), W / 2 - 420, H / 2, 0.6 + 0.4 * p, clamp(p))
    paste(im, ICON, W / 2 - 420, H / 2, 0.2 + 0.8 * p, clamp(p * 2), rot=(1 - clamp(p)) * -25)
    q = ease_out(prog(t, t0 + 0.15, 0.3))
    paste(im, text('拾影录屏', 190, 'dyh'), W / 2 - 240 + (1 - q) * 120, H / 2 - 50, 1.0, q, anchor='l')
    r = ease_out(prog(t, t0 + 0.5, 0.3))
    paste(im, text('银河麒麟 V10 · 免安装轻量录屏', 46, 'b', (200, 215, 240)), W / 2 - 232 + (1 - r) * 60, H / 2 + 95, 1.0, r, anchor='l')
    s = ease_out(prog(t, t0 + 1.0, 0.25))
    if s > 0:
        pill(im, W / 2 - 230, H / 2 + 185, '一个都不用', 40, ACC + (int(255 * s),), WHITE, 'k')
    flash(im, t, t0, 0.22, 0.9)

def sc_stats(im, t):
    t0 = 4.0
    stats = [('0.1', 'MB', '程序体积'), ('20', 'MB', '界面内存'), ('0', '个', '需另装的依赖')]
    for i, (num, unit, lab) in enumerate(stats):
        ti = t0 + i * B
        p = ease_back(prog(t, ti, 0.3))
        if p <= 0: continue
        cx = W / 2 + (i - 1) * 560
        box = Image.new('RGBA', (480, 420), (0, 0, 0, 0))
        ImageDraw.Draw(box).rounded_rectangle([0, 0, 479, 419], 36, fill=(255, 255, 255, 22), outline=(255, 255, 255, 60), width=2)
        paste(im, box, cx, H / 2 + (1 - p) * 120, 1.0, clamp(p))
        v = float(num) * ease_out(prog(t, ti, 0.4))
        sv = (f'{v:.1f}' if '.' in num else f'{int(round(v))}')
        n_im = text(sv, 190, 'dyh', [ACC, GREEN, ORANGE][i]); u_im = text(unit, 64, 'dyh', WHITE)
        wtot = n_im.width + 16 + u_im.width
        paste(im, n_im, cx - wtot / 2, H / 2 - 40 + (1 - p) * 120, 1.0, clamp(p), anchor='l')
        paste(im, u_im, cx - wtot / 2 + n_im.width + 16, H / 2 - 10 + (1 - p) * 120, 1.0, clamp(p), anchor='l')
        paste(im, text(lab, 44, 'b', (205, 215, 235)), cx, H / 2 + 110 + (1 - p) * 120, 1.0, clamp(p))
    k = ease_out(prog(t, t0 + 1.5, 0.3))
    paste(im, text('免安装 · 不要管理员 · 离线可用', 48, 'b', (255, 214, 102)), W / 2, H / 2 + 300, 1.0, k)

def sc_main(im, t):
    t0 = 6.0
    fr = clip('s1_main', 2.2, 6.6, 1.1, (545, 700, 690, 205), 600)
    dx, sc, a = enter(t, t0)
    z = 1.0 + 0.05 * prog(t, t0, 4.0)
    card(im, clip_frame(fr, t, t0), 1360 + dx, 560, sc * z * 1.1, a)
    caption(im, t, t0, '一键开录', '全屏 · 多屏 · 区域 · 窗口 随你选')
    for i, s in enumerate(['系统声音 + 麦克风', '10–30 帧', '流畅 / 标准 / 高清']):
        q = ease_out(prog(t, t0 + 0.9 + i * B, 0.25))
        if q > 0: pill(im, 110 + (1 - q) * -60, 700 + i * 78, s, 32, (255, 255, 255, int(34 * q)), (230, 238, 250))

def sc_count(im, t):
    t0 = 10.0
    dx, sc, a = enter(t, t0)
    if t < 11.0:
        fr = clip('s1_main', 6.9, 9.9, 3.0, (1920, 1080, 0, 0), 1500); f = clip_frame(fr, t, t0)
    else:
        fr = clip('s1_main', 10.2, 11.4, 1.2, (960, 540, 480, 0), 1500); f = clip_frame(fr, t, 11.0)
    card(im, f, W / 2 + dx, 600, sc, a)
    paste(im, text('3 · 2 · 1 开录' if t < 11.0 else '控制条：计时 · 暂停 · 停止', 74, 'dyh'), W / 2, 92,
          1.0, ease_out(prog(t, t0 if t < 11 else 11.0, 0.2)))
    flash(im, t, 11.0, 0.1, 0.35)

def sc_select(im, t):
    t0 = 12.0
    fr = clip('s2_select', 1.9, 7.6, 1.425, (1920, 1080, 0, 0), 1520)
    dx, sc, a = enter(t, t0)
    z = 1.0 + 0.06 * ease_out(prog(t, t0 + 1.5, 2.5))
    card(im, clip_frame(fr, t, t0), W / 2 + dx + (z - 1) * -700, 610, sc * z, a)
    paste(im, text('拖动框选 · 单击选窗口', 80, 'dyh'), W / 2, 92, 1.0, ease_out(prog(t, t0, 0.25)))

def sc_region(im, t):
    t0 = 16.0
    fr = clip('s3_region', 2.5, 9.0, 2.1667, (980, 700, 940, 120), 1000)
    dx, sc, a = enter(t, t0)
    card(im, clip_frame(fr, t, t0), 1300 + dx, 560, sc, a)
    caption(im, t, t0, '暂停 · 继续', '红框标出范围，控制条不进画面')
    q = ease_out(prog(t, t0 + 1.2, 0.25))
    if q > 0: pill(im, 110, 720, '分段写盘，最后无损合并', 34, (238, 63, 53, int(200 * q)), WHITE)

def wave_img(phase, freqs, col, w=760, h=150, amp=1.0):
    ov = Image.new('RGBA', (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
    pts = []
    for x in range(0, w, 4):
        u = x / w
        v = sum(math.sin(2 * math.pi * (f * u * 3 + phase * (1 + i * 0.3))) / (i + 1) for i, f in enumerate(freqs))
        env = math.sin(math.pi * u) ** 0.7
        pts.append((x, h / 2 + v * h * 0.28 * env * amp))
    d.line(pts, fill=col + (255,), width=6, joint='curve')
    return ov

def sc_audio(im, t):
    t0 = 19.0
    a = ease_out(prog(t, t0, 0.25))
    paste(im, text('系统声音 ＋ 麦克风 混录', 92, 'dyh'), W / 2, 150, 1.0, a)
    ph = t * 1.6
    p1 = ease_out(prog(t, t0 + 0.1, 0.3)); p2 = ease_out(prog(t, t0 + 0.35, 0.3)); p3 = ease_back(prog(t, t0 + 0.8, 0.35))
    paste(im, text('系统声音', 40, 'b', (170, 200, 255)), 240, 380, 1.0, p1, anchor='l')
    paste(im, wave_img(ph, [1.0, 2.3], ACC), 450, 380, 1.0, p1, anchor='l')
    paste(im, text('麦克风', 40, 'b', (160, 235, 190)), 240, 560, 1.0, p2, anchor='l')
    paste(im, wave_img(ph * 1.3 + 1, [1.7, 3.1], GREEN), 450, 560, 1.0, p2, anchor='l')
    # 汇合箭头
    ov = Image.new('RGBA', (220, 260), (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
    d.line([(0, 40), (120, 130), (0, 220)], fill=(255, 255, 255, 160), width=6); d.polygon([(120, 108), (200, 130), (120, 152)], fill=(255, 255, 255, 200))
    paste(im, ov, 1330, 470, 1.0, p3)
    paste(im, wave_img(ph * 1.1, [1.0, 1.7, 2.3, 3.1], ORANGE, 300, 200, 1.2), 1560, 470, 1.0, p3, anchor='l')
    paste(im, text('混录', 52, 'dyh', (255, 214, 140)), 1710, 610, 1.0, p3)
    q = ease_out(prog(t, t0 + 1.1, 0.3))
    if q > 0: pill(im, W / 2, 800, '实测 40 秒无漂移，音画误差 ≤ 0.06 秒', 40, (255, 255, 255, int(36 * q)), (255, 230, 170), 'b', 'c')

def keycap(lbl, pressed, w=None):
    f = text(lbl, 72, 'k', (30, 34, 40)); w = w or max(170, f.width + 70); h = 150
    ov = Image.new('RGBA', (w, h + 14), (0, 0, 0, 0)); d = ImageDraw.Draw(ov)
    dy = 10 if pressed else 0
    d.rounded_rectangle([0, 14, w - 1, h + 13], 26, fill=(150, 160, 175, 255))
    d.rounded_rectangle([0, dy, w - 1, h - 1 + dy], 26, fill=(255, 214, 102) if pressed else (240, 243, 247))
    paste(ov, f, w / 2, h / 2 + dy - 4)
    return ov

def sc_keys(im, t):
    t0 = 21.0
    fr = clip('s4_hotkey', 1.0, 5.5, 2.25, (1920, 1080, 0, 0), 1600)
    f = Image.fromarray(clip_frame(fr, t, t0)).convert('RGBA')
    f = f.filter(ImageFilter.GaussianBlur(3)); f.putalpha(110)
    paste(im, f, W / 2, H / 2)
    keys = [('R', '开始 / 停止'), ('P', '暂停 / 继续'), ('H', '隐藏控制条')]
    k = min(int((t - t0) / (2.0 / 3)), 2); tk = t0 + k * 2.0 / 3
    pressed = (t - tk) < 0.22
    a = ease_out(prog(t, t0, 0.2))
    paste(im, text('全局快捷键', 80, 'dyh'), W / 2, 170, 1.0, a)
    xs = [W / 2 - 430, W / 2 - 130, W / 2 + 250]
    paste(im, keycap('Ctrl', pressed, 260), xs[0], 520, 1.0, a)
    paste(im, text('+', 80, 'k'), (xs[0] + xs[1]) / 2 + 55, 510, 1.0, a)
    paste(im, keycap('Alt', pressed, 220), xs[1] + 40, 520, 1.0, a)
    paste(im, text('+', 80, 'k'), xs[1] + 205, 510, 1.0, a)
    pk = ease_back(prog(t, tk, 0.2), 2.5)
    paste(im, keycap(keys[k][0], pressed, 180), xs[2] + 60, 520, 0.6 + 0.4 * pk, 1.0)
    paste(im, text(keys[k][1], 64, 'dyh', (255, 214, 102)), W / 2, 760, 1.0, clamp(pk * 1.5))

def sc_recover(im, t):
    t0 = 23.0
    a = ease_out(prog(t, t0, 0.2))
    if t < 23.7:
        shake = (1 - prog(t, t0, 0.5)) * 14
        paste(im, text('程序被强制结束？', 120, 'dyh', (255, 120, 110)), W / 2 + math.sin(t * 80) * shake, H / 2, 1.0, a)
        return
    fr = clip('s5_recover', 1.6, 3.4, 1.0, (500, 260, 711, 690), 1000)
    dx, sc, b = enter(t, 23.7)
    card(im, clip_frame(fr, t, 23.7), 1310 + dx, 560, sc * 1.05, b)
    caption(im, t, 23.7, '断电不丢', '每 2 秒写盘，重开自动恢复')

def sc_check(im, t):
    t0 = 25.5
    dx, sc, a = enter(t, t0)
    w, h = 1300, 640
    box = Image.new('RGBA', (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(box)
    d.rounded_rectangle([0, 0, w - 1, h - 1], 22, fill=(18, 20, 24, 245), outline=(255, 255, 255, 50), width=2)
    for i, c in enumerate([(255, 95, 86), (255, 189, 46), (39, 201, 63)]): d.ellipse([28 + i * 34, 24, 48 + i * 34, 44], fill=c)
    paste(box, text('$ ./shiying --check', 40, 'b', (120, 230, 140)), 40, 100, anchor='l')
    lines = ['录制引擎（x11grab）', '图形界面连接', '中文字体', '显示器 1 块 · 1920 × 1080', '声音服务库 libpulse-simple', '系统声音与麦克风']
    for i, s in enumerate(lines):
        if t < t0 + 0.3 + i * 0.16: break
        paste(box, text('通过', 34, 'b', (39, 201, 63)), 40, 175 + i * 66, anchor='l')
        paste(box, text(s, 36, 'r', (225, 230, 236)), 150, 175 + i * 66, anchor='l')
    if t > t0 + 1.35: paste(box, text('自检通过。', 38, 'b', (255, 214, 102)), 40, 585, anchor='l')
    paste(im, box, W / 2 + 230 + dx, 560, sc, a)
    paste(im, text('一条命令', 96, 'dyh'), 120, 420, 1.0, a, anchor='l')
    paste(im, text('自检排障', 96, 'dyh', (255, 214, 102)), 120, 540, 1.0, ease_out(prog(t, t0 + 0.15, 0.25)), anchor='l')

def sc_platform(im, t):
    t0 = 27.5
    a = ease_out(prog(t, t0, 0.2))
    paste(im, text('飞腾 arm64 · x86_64', 110, 'dyh'), W / 2, 330, 1.0 + (1 - a) * 0.2, a)
    chips = ['绿色版 免安装', 'deb 安装包', '离线可用', '无需管理员']
    for i, s in enumerate(chips):
        p = ease_back(prog(t, t0 + 0.4 + i * B / 2, 0.25))
        if p <= 0: continue
        x = W / 2 + (i - 1.5) * 400
        pill(im, x, 560 + (1 - p) * 60, s, 44, (255, 255, 255, int(40 * clamp(p))), WHITE, 'b', 'c')
    q = ease_out(prog(t, t0 + 1.4, 0.3))
    paste(im, text('银河麒麟 V10 全系可用', 56, 'b', (190, 205, 230)), W / 2, 720, 1.0, q)

def sc_outro(im, t):
    t0 = 30.0
    p = ease_back(prog(t, t0, 0.4), 2.2)
    paste(im, ICON, W / 2, 380, 0.3 + 0.7 * p, clamp(p * 2))
    q = ease_out(prog(t, t0 + 0.15, 0.35))
    paste(im, text('拾影录屏', 200, 'dyh'), W / 2, 640 + (1 - q) * 50, 1.0, q)
    r = ease_out(prog(t, t0 + 0.55, 0.35))
    paste(im, text('轻 · 快 · 稳', 64, 'dyh', (255, 214, 102)), W / 2, 790, 1.0, r)
    s = ease_out(prog(t, t0 + 0.9, 0.4))
    paste(im, text('银河麒麟 V10 轻量录屏工具 · v1.0.0', 36, 'r', (170, 185, 210)), W / 2, 880, 1.0, s)
    flash(im, t, t0, 0.25, 0.9)
    fo = prog(t, 33.2, 0.8)
    if fo > 0: im.alpha_composite(Image.new('RGBA', (W, H), (0, 0, 0, int(255 * fo))))

SCENES = [(0, 2, sc_hook), (2, 4, sc_logo), (4, 6, sc_stats), (6, 10, sc_main), (10, 12, sc_count), (12, 16, sc_select),
          (16, 19, sc_region), (19, 21, sc_audio), (21, 23, sc_keys), (23, 25.5, sc_recover), (25.5, 27.5, sc_check),
          (27.5, 30, sc_platform), (30, 34.01, sc_outro)]

def frame(t):
    im = background(t)
    for a, b, fn in SCENES:
        if a <= t < b: fn(im, t); break
    return im.convert('RGB')

if ONLY:
    for t in ONLY: frame(t).save(f'/tmp/v/prev_{t:05.2f}.png')
    sys.exit()

enc = subprocess.Popen([FF, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-r', str(FPS), '-i', '-',
                        '-i', '/tmp/v/music.wav', '-map', '0:v', '-map', '1:a', '-c:v', 'libx264', '-preset', 'medium', '-crf', '18',
                        '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '192k', '-af', 'loudnorm=I=-14:TP=-1.0:LRA=9',
                        '-movflags', '+faststart', '-shortest', '/tmp/v/拾影录屏_宣传片.mp4'], stdin=subprocess.PIPE)
n = int(T * FPS)
for i in range(n):
    enc.stdin.write(frame(i / FPS).tobytes())
    if i % 60 == 0: print(f'{i}/{n}', flush=True)
enc.stdin.close(); enc.wait()
print('done')
