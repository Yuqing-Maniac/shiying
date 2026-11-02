from act import *
import subprocess
def bar_geom():
    out = subprocess.run("for w in $(xdotool search --class shiying); do n=$(xdotool getwindowname $w); case \"$n\" in *录制中*) xdotool getwindowgeometry --shell $w;; esac; done", shell=True, capture_output=True, text=True).stdout
    g = dict(l.split('=') for l in out.split() if '=' in l); return int(g['X']), int(g['Y'])
move(1500, 850, 0.1)
cap_start('s2_select')
click(*W(344, 127), dur=0.6); time.sleep(0.9)          # 自选区域/窗口 → 框选浮层
move(400, 500, 0.6); time.sleep(0.7)                    # 悬停终端窗口
move(1400, 520, 0.6); time.sleep(0.7)                   # 悬停架构图窗口
move(1100, 245, 0.5); time.sleep(0.2)
down(); move(1790, 700, 1.0); time.sleep(0.3); up()     # 拖动框选
time.sleep(1.4)
cap_stop()
cap_start('s3_region')
click(*W(104, 371), dur=0.5); time.sleep(0.3)          # 倒计时：关闭
click(*W(240, 504), dur=0.6); time.sleep(1.2)          # 开始录制
bx, by = bar_geom()
move(1500, 900, 0.6); time.sleep(1.6)
click(bx + 170, by + 23, dur=0.6); time.sleep(1.4)     # 暂停
click(bx + 170, by + 23, dur=0.2); time.sleep(1.8)     # 继续
click(bx + 206, by + 23, dur=0.4); time.sleep(3.0)     # 停止
move(*W(240, 600), 0.6); time.sleep(1.2)
cap_stop()
