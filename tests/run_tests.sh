#!/bin/bash
# 拾影录屏 回归测试（无头：Xvfb＋PulseAudio 空设备，x86_64 宿主）
# 写于 2026-10-22，把开发期间手工执行过的检查固化成脚本；原始的手工测试脚本保留在 tests/manual/。
# 用法：bash tests/run_tests.sh          需先 bash build/build_all.sh（或至少 fetch_deps.sh＋build.sh x86_64）
# 覆盖：自检、单实例命令转发、录制／暂停／继续／停止、音画同步、两路混音、异常中断恢复、引擎禁止执行时自动复制、
#       发布包结构（无符号链接、文件名、deb 字段）、arm64 产物依赖与 glibc 版本。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
T="$WORK/test"; rm -rf "$T"; mkdir -p "$T"
APP="$T/app"; mkdir -p "$APP/录制引擎"
cp "$WORK/build/x86_64/shiying" "$APP/拾影录屏" && cp "$WORK/vendor/ffmpeg-x86_64" "$APP/录制引擎/ffmpeg" || { echo "缺少构建产物，先运行 build/build_all.sh"; exit 2; }
FF="$APP/录制引擎/ffmpeg"
PASS=0; FAIL=0
ok()  { echo "  [通过] $*"; PASS=$((PASS+1)); }
bad() { echo "  [失败] $*"; FAIL=$((FAIL+1)); }
chk() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

export HOME="$T/home" XDG_RUNTIME_DIR="$T/run" XDG_CONFIG_HOME= XDG_CACHE_HOME= LANG=C.UTF-8
mkdir -p "$HOME/视频" "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
DNUM=:$((90 + RANDOM % 9)); export DISPLAY=$DNUM
Xvfb $DNUM -screen 0 1920x1080x24 +extension RANDR >/dev/null 2>&1 & XPID=$!
cat > "$T/pa.pa" <<EOF
load-module module-native-protocol-unix
load-module module-null-sink sink_name=speaker
load-module module-pipe-source source_name=mic file=$T/mic.fifo format=s16le rate=48000 channels=2
set-default-sink speaker
set-default-source mic
EOF
pulseaudio -n -F "$T/pa.pa" --exit-idle-time=-1 --daemonize=yes 2>/dev/null
sleep 1.5
# 麦克风：按墙钟送 880 Hz 正弦
python3 - "$T/mic.fifo" > /dev/null 2>&1 <<'PY' & MICPID=$!
import numpy as np, time, os, sys
fd = os.open(sys.argv[1], os.O_WRONLY); t0 = time.time(); n = 0
while True:
    due = int((time.time() - t0) * 48000) - n
    if due > 0:
        s = (np.sin(2 * np.pi * 880 * np.arange(n, n + due) / 48000) * 6000).astype(np.int16)
        os.write(fd, np.repeat(s, 2).tobytes()); n += due
    time.sleep(0.01)
PY
FLICKERPID=""
REDPID=""
cleanup() { pkill -f "$APP/拾影录屏" 2>/dev/null; [ -n "$FLICKERPID" ] && kill "$FLICKERPID" 2>/dev/null; [ -n "$REDPID" ] && kill "$REDPID" 2>/dev/null; kill $MICPID $XPID 2>/dev/null; pulseaudio --kill 2>/dev/null; }
trap cleanup EXIT
CONF="$HOME/.config/shiying"; mkdir -p "$CONF"
setcfg() { printf 'area=0\nmonitor=-1\naudio=%s\nfps=24\nquality=1\nscale=0\nformat=%s\ncountdown=0\nmouse=1\noutdir=%s\n' "$1" "$2" "$HOME/视频/录屏" > "$CONF/config.ini"; }
writecfg() { mkdir -p "$1"; printf 'area=0\nmonitor=-1\naudio=%s\nfps=24\nquality=%s\nscale=0\nformat=%s\ncountdown=0\ncursor=%s\ncodec=%s\nsegment_n=%s\nsegment_unit=%s\noutdir=%s\n' "$2" "$8" "$3" "$4" "$5" "$6" "$7" "$1" > "$CONF/config.ini"; }
vcodec() { "$FF" -hide_banner -i "$1" 2>&1 | sed -n 's/.*Video: \([A-Za-z0-9]*\).*/\1/p' | head -1; }
LOG="$HOME/.cache/shiying/shiying.log"
OUTD="$HOME/视频/录屏"
latest() { ls -t "$OUTD"/*.m* 2>/dev/null | head -1; }
dur() { "$FF" -hide_banner -i "$1" 2>&1 | sed -n 's/.*Duration: \([0-9:.]*\).*/\1/p' | awk -F: '{print $1*3600+$2*60+$3}'; }
streams() { "$FF" -hide_banner -i "$1" 2>&1 | grep -c "Stream #"; }
start_app() { ("$APP/拾影录屏" >/dev/null 2>&1 &); sleep 1.5; }
stop_app() { "$APP/拾影录屏" --quit; sleep 1; pkill -f "$APP/拾影录屏" 2>/dev/null; sleep 0.3; }

echo "== 1. 自检"
setcfg 3 0
"$APP/拾影录屏" --check > "$T/check.txt" 2>&1
chk "--check 全部通过" "grep -q '自检通过' '$T/check.txt'"
chk "从中文路径定位录制引擎" "grep -q '录制引擎/ffmpeg' '$T/check.txt'"

echo "== 2. 录制、暂停、继续、停止（两路声音、MP4）"
start_app
chk "单实例：第二次启动不再新开进程" "\"$APP/拾影录屏\" --show; sleep 0.5; [ \$(pgrep -fc \"$APP/拾影录屏\") -eq 1 ]"
pactl load-module module-sine sink=speaker frequency=440 >/dev/null
"$APP/拾影录屏" --toggle; sleep 3
"$APP/拾影录屏" --pause; sleep 1.5
"$APP/拾影录屏" --pause; sleep 3
"$APP/拾影录屏" --toggle; sleep 4
pactl unload-module module-sine 2>/dev/null
F=$(latest)
chk "生成 mp4 文件" "[ -n '$F' ] && [[ '$F' == *.mp4 ]]"
chk "两段合并（日志含 segment 1）" "grep -q 'segment 1 pid' '$LOG'"
D=$(dur "$F"); chk "时长约 6 秒（暂停段不计入，实测 $D）" "python3 -c 'import sys; sys.exit(0 if 5.0 < $D < 7.5 else 1)'"
chk "含音视频两路流" "[ \$(streams '$F') -eq 2 ]"
python3 - "$FF" "$F" > "$T/mix.txt" <<'PY'
import numpy as np, subprocess, sys
r = subprocess.run([sys.argv[1], '-v', 'error', '-i', sys.argv[2], '-map', '0:a', '-af', 'aresample=async=1:first_pts=0',
                    '-f', 's16le', '-ac', '1', '-ar', '48000', '-'], capture_output=True).stdout
a = np.frombuffer(r, np.int16).astype(float)
seg = a[48000:96000]; sp = np.abs(np.fft.rfft(seg))
print(int(sp[438:443].max() > 1e6), int(sp[878:883].max() > 1e6))
PY
chk "系统声音（440 Hz）与麦克风（880 Hz）均录入" "[ \"\$(cat '$T/mix.txt')\" = '1 1' ]"
stop_app

echo "== 3. 音画同步（屏幕变白与声音同时出现）"
setcfg 1 1
start_app
which xsetroot >/dev/null && xsetroot -solid black
"$APP/拾影录屏" --toggle; sleep 3
xsetroot -solid white; pactl load-module module-sine sink=speaker frequency=440 >/dev/null; sleep 3
"$APP/拾影录屏" --toggle; sleep 4
pactl unload-module module-sine 2>/dev/null; xsetroot -solid black
F=$(latest)
python3 - "$FF" "$F" > "$T/sync.txt" <<'PY'
import numpy as np, subprocess, sys, re
FF, f = sys.argv[1], sys.argv[2]
r = subprocess.run([FF, '-v', 'error', '-i', f, '-map', '0:a', '-af', 'aresample=async=1:first_pts=0', '-f', 's16le', '-ac', '1', '-ar', '48000', '-'], capture_output=True).stdout
a = np.frombuffer(r, np.int16).astype(float); w = 480
env = np.array([np.sqrt((a[i:i + w] ** 2).mean()) for i in range(0, len(a) - w, w)])
ta = next(i * w / 48000 for i in range(1, len(env)) if env[i] > 3000 and env[i - 1] <= 3000)
p = subprocess.run([FF, '-v', 'error', '-i', f, '-map', '0:v', '-fps_mode', 'passthrough', '-vf', 'scale=64:36,format=gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
fr = np.frombuffer(p, np.uint8).reshape(-1, 36, 64)[:, 30:36, 0:10].mean((1, 2))
info = subprocess.run([FF, '-v', 'info', '-i', f, '-map', '0:v', '-fps_mode', 'passthrough', '-vf', 'showinfo', '-f', 'null', '-'], capture_output=True, text=True).stderr
ts = [float(x) for x in re.findall(r'pts_time:([0-9.]+)', info)]
tv = next(ts[i] for i in range(1, min(len(fr), len(ts))) if fr[i] > 128 >= fr[i - 1])
print(f'{abs(ta - tv):.3f}')
PY
OFF=$(cat "$T/sync.txt" 2>/dev/null); chk "MKV 输出且音画偏差 < 0.15 秒（实测 ${OFF:-无} 秒）" "[[ '$F' == *.mkv ]] && python3 -c 'import sys; sys.exit(0 if float(\"${OFF:-9}\") < 0.15 else 1)'"
stop_app

echo "== 4. 异常中断后自动恢复"
setcfg 0 0
start_app
"$APP/拾影录屏" --toggle; sleep 6
pkill -9 -f "$APP/拾影录屏"; sleep 1
chk "中断后留下临时分段目录" "ls -d '$OUTD'/.shiying-* >/dev/null 2>&1"
start_app; sleep 3
F=$(latest)
chk "重开后生成 …_恢复.mp4" "[[ '$F' == *_恢复.mp4 ]]"
D=$(dur "$F"); chk "恢复的录像有内容（时长 $D 秒）" "python3 -c 'import sys; sys.exit(0 if float(\"${D:-0}\") > 2 else 1)'"
chk "临时目录已清理" "! ls -d '$OUTD'/.shiying-* >/dev/null 2>&1"
stop_app

echo "== 5. 引擎无执行权限时自动复制到缓存目录"
chmod -x "$FF"
"$APP/拾影录屏" --check > "$T/check2.txt" 2>&1
chk "改用 ~/.cache/shiying/ffmpeg" "grep -q '.cache/shiying/ffmpeg' '$T/check2.txt'"
chmod +x "$FF"

echo "== 6. 5 MB 封存两段，强杀后较早的文件仍可播放"
OUT5="$T/seg5"
stop_app
writecfg "$OUT5" 1 0 1 0 5 0 0
gcc -O2 -o "$T/flicker" -x c - -lX11 <<'EOF'
#include <X11/Xlib.h>
#include <stdlib.h>
#include <unistd.h>
int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    int s = DefaultScreen(d);
    int w = DisplayWidth(d, s), h = DisplayHeight(d, s);
    char *data = malloc((size_t)w * (size_t)h * 4);
    if (!data) return 1;
    XImage *im = XCreateImage(d, DefaultVisual(d, s), (unsigned)DefaultDepth(d, s), ZPixmap, 0, data, (unsigned)w, (unsigned)h, 32, 0);
    if (!im) return 1;
    unsigned seed = 1;
    for (;;) {
        for (int i = 0; i < w * h; i++) {
            seed = seed * 1664525u + 1013904223u;
            ((unsigned *)data)[i] = seed;
        }
        XPutImage(d, RootWindow(d, s), DefaultGC(d, s), im, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
        XFlush(d);
        usleep(80000);
    }
}
EOF
"$T/flicker" >/dev/null 2>&1 & FLICKERPID=$!
sleep 0.3
stop_app
start_app
MARK=$(wc -l < "$LOG" 2>/dev/null || echo 0)
"$APP/拾影录屏" --toggle
GOT=0
for _i in $(seq 1 90); do
  N=$(awk -v s="${MARK:-0}" 'NR>s && /sealed / {c++} END{print c+0}' "$LOG" 2>/dev/null || echo 0)
  if [ "${N:-0}" -ge 2 ]; then GOT=1; break; fi
  sleep 1
done
mapfile -t SEALS < <(awk -v s="${MARK:-0}" 'NR>s && /sealed / { sub(/^.*sealed /,""); print }' "$LOG" | head -2)
F1=${SEALS[0]:-}
F2=${SEALS[1]:-}
D1=0; D2=0
if [ -n "$F1" ] && [ -f "$F1" ]; then D1=$(dur "$F1"); fi
if [ -n "$F2" ] && [ -f "$F2" ]; then D2=$(dur "$F2"); fi
pkill -9 -f "$APP/拾影录屏" || true
sleep 1
chk "封存出两个文件" "[ '$GOT' = 1 ] && [ -n '$F1' ] && [ -n '$F2' ] && [ -f '$F1' ] && [ -f '$F2' ] && [ '$F1' != '$F2' ]"
chk "两段都有时长（$D1，$D2）" "python3 -c 'import sys; sys.exit(0 if float(\"${D1:-0}\") > 0.05 and float(\"${D2:-0}\") > 0.05 else 1)'"
D1b=0
if [ -n "$F1" ] && [ -f "$F1" ]; then D1b=$(dur "$F1"); fi
chk "kill -9 之后较早文件时长不变（$D1 -> $D1b）" "python3 -c 'import sys; sys.exit(0 if abs(float(\"${D1:-0}\") - float(\"${D1b:-0}\")) < 0.05 and float(\"${D1b:-0}\") > 0.05 else 1)'"
writecfg "$OUT5" 1 0 1 0 5 0 0
start_app
sleep 4
stop_app
chk "未封存的尾巴已从临时目录收走" "! ls -d '$OUT5'/.shiying-* >/dev/null 2>&1"
if [ -n "$FLICKERPID" ]; then kill "$FLICKERPID" 2>/dev/null || true; FLICKERPID=""; fi
xsetroot -solid black >/dev/null 2>&1 || true

echo "== 7. 短录制：无声、指针、H.264、H.265"
short_take() {
  local name="$1" out="$2" audio="$3" cursor="$4" codec="$5" settle="$6"
  stop_app
  writecfg "$out" "$audio" 0 "$cursor" "$codec" 0 0 0
  start_app
  "$APP/拾影录屏" --toggle
  sleep 2
  "$APP/拾影录屏" --toggle
  sleep "$settle"
  local f d v
  f=$(ls -t "$out"/*.m* 2>/dev/null | head -1 || true)
  chk "$name 生成文件" "[ -n '${f:-}' ] && [ -f '${f:-/none}' ]"
  if [ -z "${f:-}" ] || [ ! -f "$f" ]; then return; fi
  d=$(dur "$f"); v=$(vcodec "$f")
  chk "$name 有视频且时长 $d 秒（$v）" "python3 -c 'import sys; sys.exit(0 if float(\"${d:-0}\") > 0.3 else 1)' && \"$FF\" -hide_banner -i '$f' 2>&1 | grep -q 'Video:'"
  case "$(basename "$f")" in
    *_00[0-9].*) bad "$name 短录制不应切段" ;;
    *) ok "$name 文件名未加序号" ;;
  esac
  if [ "$audio" = 0 ]; then chk "$name 没有音频流" "[ \"\$(streams '$f')\" -eq 1 ]"
  else chk "$name 保留音频流" "[ \"\$(streams '$f')\" -eq 2 ]"; fi
  if [ "$codec" = 1 ]; then chk "$name 视频是 hevc" "[ '$v' = hevc ]"; fi
  if [ "$codec" = 0 ]; then chk "$name 视频是 h264" "[ '$v' = h264 ]"; fi
}
short_take 无声 "$T/take-silence" 0 1 0 4
short_take 指针关 "$T/take-coff" 1 0 0 4
short_take 指针原大 "$T/take-corig" 1 1 0 4
short_take 指针放大 "$T/take-cmag" 1 2 0 4
short_take H.264 "$T/take-h264" 1 1 0 4
short_take H.265 "$T/take-h265" 1 1 1 8
stop_app

echo "== 7b. 放大光标是光标图像，不是旁边的屏幕"
cat > "$T/redcur.c" <<'EOF'
#include <X11/Xlib.h>
#include <unistd.h>
int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) return 1;
    Window root = DefaultRootWindow(d);
    int W = 32, H = 32;
    Pixmap src = XCreatePixmap(d, root, W, H, 1);
    Pixmap msk = XCreatePixmap(d, root, W, H, 1);
    GC gc = XCreateGC(d, src, 0, NULL);
    XSetForeground(d, gc, 0);
    XFillRectangle(d, src, gc, 0, 0, W, H);
    XFillRectangle(d, msk, gc, 0, 0, W, H);
    XSetForeground(d, gc, 1);
    XFillRectangle(d, src, gc, 0, 0, W, H);
    XFillRectangle(d, msk, gc, 0, 0, W, H);
    XColor fg, bg;
    fg.red = 65535; fg.green = 0; fg.blue = 0; fg.flags = DoRed | DoGreen | DoBlue;
    bg.red = 0; bg.green = 0; bg.blue = 0; bg.flags = DoRed | DoGreen | DoBlue;
    Cursor c = XCreatePixmapCursor(d, src, msk, &fg, &bg, 0, 0);
    XDefineCursor(d, root, c);
    XWarpPointer(d, None, root, 0, 0, 0, 0, 160, 160);
    XFlush(d);
    for (;;) pause();
}
EOF
if gcc -O2 -o "$T/redcur" "$T/redcur.c" -lX11; then ok "红色光标辅助程序已编译"; else bad "红色光标辅助程序编译失败"; fi
xsetroot -solid black >/dev/null 2>&1 || true
"$T/redcur" >/dev/null 2>&1 & REDPID=$!
sleep 0.4
redbox() {
  "$FF" -v error -ss 0.6 -i "$1" -vf crop=12:12:206:206 -frames:v 1 -f rawvideo -pix_fmt rgb24 - 2>/dev/null | python3 -c '
import sys
b = sys.stdin.buffer.read()
n = len(b) // 3
red = 0
for i in range(n):
    r, g, bl = b[i*3], b[i*3+1], b[i*3+2]
    if r > 150 and g < 90 and bl < 90: red += 1
print(red, n)
'
}
pix_take() {
  local name="$1" out="$2" cursor="$3"
  stop_app
  writecfg "$out" 0 0 "$cursor" 0 0 0 2
  start_app
  "$APP/拾影录屏" --toggle
  sleep 2
  "$APP/拾影录屏" --toggle
  sleep 4
  local f counts red n
  f=$(ls -t "$out"/*.m* 2>/dev/null | head -1 || true)
  chk "$name 生成文件" "[ -n '${f:-}' ] && [ -f '${f:-/none}' ]"
  if [ -z "${f:-}" ] || [ ! -f "$f" ]; then printf '0\n' > "$out/red.count"; printf '0\n' > "$out/red.total"; return; fi
  counts=$(redbox "$f")
  red=${counts%% *}
  n=${counts##* }
  printf '%s\n' "$red" > "$out/red.count"
  printf '%s\n' "$n" > "$out/red.total"
}
pix_take 指针关像素 "$T/pix-off" 0
OFFR=$(cat "$T/pix-off/red.count" 2>/dev/null || echo 0)
OFFN=$(cat "$T/pix-off/red.total" 2>/dev/null || echo 0)
pix_take 指针原大像素 "$T/pix-orig" 1
ORIGR=$(cat "$T/pix-orig/red.count" 2>/dev/null || echo 0)
pix_take 指针放大像素 "$T/pix-mag" 2
MAGR=$(cat "$T/pix-mag/red.count" 2>/dev/null || echo 0)
chk "原大光标不盖住外侧采样区（红点 $ORIGR / $OFFN，关为 $OFFR）" "python3 -c 'import sys; sys.exit(0 if int(\"${ORIGR:-0}\") <= 8 and int(\"${OFFR:-0}\") <= 8 else 1)'"
chk "放大光标盖住热点外侧（红点 $MAGR / $OFFN）" "python3 -c 'import sys; sys.exit(0 if int(\"${MAGR:-0}\") >= 80 else 1)'"
kill "$REDPID" 2>/dev/null || true
stop_app

echo "== 8. 发布包"
DIST="$WORK/dist"
if ls "$DIST"/*.deb >/dev/null 2>&1; then
  for A in 飞腾arm64 x86_64; do
    TG=$(ls "$DIST"/拾影录屏_*_${A}_免安装.tar.gz)
    chk "$A 免安装包无符号链接" "! tar -tvzf '$TG' | grep -q '^l'"
    chk "$A 免安装包文件齐全" "[ \$(tar -tzf '$TG' | grep -cE '拾影录屏/(拾影录屏|录制引擎/ffmpeg|使用说明.txt|安装到菜单.sh|卸载.sh)$') -eq 5 ]"
  done
  chk "arm64 deb 架构字段" "dpkg-deb -f \"\$(ls '$DIST'/*飞腾arm64_安装包.deb)\" Architecture | grep -qx arm64"
  chk "校验值文件与包一致" "(cd '$DIST' && sha256sum -c --quiet 校验值.txt)"
else
  echo "  （未找到 $DIST，跳过；先运行 build/release.sh）"
fi
B64="$WORK/build/aarch64/shiying"
if [ -f "$B64" ]; then
  chk "arm64 程序为 aarch64 ELF" "file '$B64' | grep -q 'ARM aarch64'"
  chk "arm64 程序只依赖 libX11／glibc" "[ \"\$(readelf -d '$B64' | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | LC_ALL=C sort | tr '\n' ' ')\" = 'libX11.so.6 libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 ' ]"
  chk "arm64 程序 glibc 需求 ≤ 2.17" "[ \"\$(readelf -V '$B64' | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)\" = GLIBC_2.17 ]"
fi

echo
echo "通过 $PASS 项，失败 $FAIL 项"
[ $FAIL -eq 0 ]
