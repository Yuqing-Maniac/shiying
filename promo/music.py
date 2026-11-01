# 原创配乐：120 BPM 电子节拍，34 秒；0–2 秒滤波引子，2 秒落拍，30 秒收尾重拍
import numpy as np
from scipy.signal import butter, lfilter
import wave

SR = 48000
BPM = 120
B = 60 / BPM                     # 每拍 0.5 秒
DUR = 34.0
N = int(SR * DUR)
L = np.zeros(N); R = np.zeros(N)
rng = np.random.default_rng(7)

def add(sig, t0, gain=1.0, pan=0.0):
    i = int(t0 * SR)
    if i >= N: return
    s = sig[: N - i] * gain
    L[i:i + len(s)] += s * (1 - max(pan, 0))
    R[i:i + len(s)] += s * (1 + min(pan, 0))

def env(n, a=0.002, d=0.2):
    t = np.arange(n) / SR
    return np.minimum(t / a, 1) * np.exp(-t / d)

def bp(x, lo, hi):
    b, a = butter(2, [lo / (SR / 2), hi / (SR / 2)], 'band'); return lfilter(b, a, x)
def hp(x, f):
    b, a = butter(2, f / (SR / 2), 'high'); return lfilter(b, a, x)
def lp(x, f):
    b, a = butter(2, min(f, SR / 2 - 100) / (SR / 2), 'low'); return lfilter(b, a, x)

def kick():
    n = int(0.42 * SR); t = np.arange(n) / SR
    f = 46 + 120 * np.exp(-t * 28)
    ph = 2 * np.pi * np.cumsum(f) / SR
    s = np.sin(ph) * np.exp(-t * 7.5)
    s[:200] += rng.normal(0, 0.5, 200) * np.linspace(1, 0, 200)
    return np.tanh(s * 1.6)

def snare():
    n = int(0.3 * SR); t = np.arange(n) / SR
    nz = bp(rng.normal(0, 1, n), 900, 7000) * np.exp(-t * 16)
    tone = np.sin(2 * np.pi * 190 * t) * np.exp(-t * 25) * 0.6
    return (nz + tone) * 0.9

def hat(open_=False):
    n = int((0.22 if open_ else 0.05) * SR); t = np.arange(n) / SR
    return hp(rng.normal(0, 1, n), 7000) * np.exp(-t * (12 if open_ else 70)) * 0.35

def saw(f, n):
    t = np.arange(n) / SR
    return 2 * ((t * f) % 1) - 1

def note(m): return 440 * 2 ** ((m - 69) / 12)

def stab(chord, dur, cutoff=3500):
    n = int(dur * SR)
    s = sum(saw(note(m) * (1 + d), n) for m in chord for d in (-0.004, 0.004))
    s = lp(s / (len(chord) * 2), cutoff) * env(n, 0.003, dur * 0.45)
    return s

def bass(m, dur):
    n = int(dur * SR); t = np.arange(n) / SR
    s = np.sign(np.sin(2 * np.pi * note(m) * t)) * 0.6 + np.sin(2 * np.pi * note(m) * t)
    return lp(s, 900) * env(n, 0.004, dur * 0.6) * 0.55

def riser(dur):
    n = int(dur * SR); t = np.arange(n) / SR
    nz = rng.normal(0, 1, n)
    out = np.zeros(n); seg = 2400
    for i in range(0, n, seg):
        fc = 400 + 9000 * (i / n) ** 2
        out[i:i + seg] = bp(nz[i:i + seg], fc * 0.6, min(fc * 1.6, 20000))
    return out * (t / dur) ** 2 * 0.5

def impact():
    n = int(1.6 * SR); t = np.arange(n) / SR
    sub = np.sin(2 * np.pi * (38 + 50 * np.exp(-t * 6)) * t) * np.exp(-t * 2.2)
    nz = lp(rng.normal(0, 1, n), 2500) * np.exp(-t * 5) * 0.5
    return np.tanh((sub + nz) * 1.3)

# 和弦进行：Am – F – C – G（每小节 4 拍）
prog = [(57, [57, 60, 64, 69]), (53, [53, 57, 60, 65]), (48, [55, 60, 64, 67]), (55, [55, 59, 62, 67])]
beats = int(DUR / B)
side = np.ones(N)                 # 侧链压缩包络

for k in range(beats):
    t = k * B
    bar = k // 4; pos = k % 4
    root, ch = prog[bar % 4]
    if t < 2.0:                                   # 引子：滤波重音
        add(kick(), t, 0.8)
        add(stab(ch, 0.45, 900 + 700 * k), t, 0.5)
        continue
    if t >= 30.0: continue
    add(kick(), t, 1.0)
    i = int(t * SR); m = min(int(0.28 * SR), N - i)
    side[i:i + m] = np.minimum(side[i:i + m], 0.35 + 0.65 * np.linspace(0, 1, m) ** 0.6)
    if pos in (1, 3): add(snare(), t, 0.55)
    for h in range(4):
        add(hat(open_=(h == 2)), t + h * B / 4, 0.55 if h % 2 else 0.3, pan=0.25 if h % 2 else -0.25)
    add(bass(root - 12, B / 2 * 0.9), t + B / 2, 1.0)
    add(bass(root - 12, B / 4 * 0.9), t + B * 0.75, 0.7)
    if pos == 0: add(stab(ch, 1.9, 3000), t, 0.38, pan=-0.1)
    if pos == 2: add(stab([c + 12 for c in ch[1:]], 0.25, 5000), t + B / 2, 0.18, pan=0.3)

# 段落连接
add(riser(1.5), 0.5, 0.7)
add(impact(), 2.0, 0.9)
for tt in (12.0, 21.0, 25.5):
    add(riser(0.5), tt - 0.5, 0.35)
add(riser(1.5), 28.5, 0.75)
add(impact(), 30.0, 1.0)
add(stab([45, 57, 60, 64, 69, 72], 3.8, 2200), 30.0, 0.55)

L *= side; R *= side
# 尾部淡出
fade = np.ones(N); fs = int(31.8 * SR); fade[fs:] = np.linspace(1, 0, N - fs) ** 1.5
L *= fade; R *= fade
mx = max(np.abs(L).max(), np.abs(R).max())
L = np.tanh(L / mx * 1.25) * 0.89; R = np.tanh(R / mx * 1.25) * 0.89
pcm = (np.stack([L, R], 1) * 32767).astype(np.int16)
with wave.open('/tmp/v/music.wav', 'wb') as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes(pcm.tobytes())
print('music ok', DUR)
