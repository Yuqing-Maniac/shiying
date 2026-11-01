#!/bin/bash
# 构建界面程序：bash build/build.sh [x86_64|aarch64|all]
# 用 zig cc 交叉编译，目标 glibc 2.17，兼容银河麒麟 V10 全系。
# 链接期使用 libX11 桩库，运行期加载系统 libX11；其余库全部运行时 dlopen。
# 产物：$SY_WORK/build/<arch>/shiying（SY_WORK 默认 <仓库>/_work）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
B="$WORK/build"
cd "$ROOT"
ARCHS=${1:-all}; [ "$ARCHS" = all ] && ARCHS="x86_64 aarch64"
ZIG="python3 -m ziglang"
$ZIG version >/dev/null 2>&1 || { echo "缺少 zig：pip install --break-system-packages ziglang==0.17.0" >&2; exit 1; }
mkdir -p "$B/include"
if [ ! -d "$B/include/X11" ]; then
  for d in /usr/include/X11 /usr/include/freetype2 /usr/include/fontconfig /usr/include/pulse; do
    [ -d "$d" ] || { echo "缺少头文件 $d，见 CLAUDE.md 第 5 节的 apt 依赖" >&2; exit 1; }
  done
  cp -r /usr/include/X11 "$B/include/"
  cp -r /usr/include/freetype2/* "$B/include/"
  cp -r /usr/include/fontconfig "$B/include/"
  cp -r /usr/include/pulse "$B/include/"
fi
SRC="src/main.c src/draw.c src/util.c src/audio.c src/rec.c src/session.c src/cfg.c src/path.c"
CFLAGS="-O2 -std=gnu11 -Wall -Wno-unused-result -Wno-format-truncation -D_FILE_OFFSET_BITS=64 -I$B/include -fno-strict-aliasing"
# 需要的 libX11 符号：先在本机正常编译一遍取未定义符号
X11SO=$(ls /usr/lib/x86_64-linux-gnu/libX11.so.6 /usr/lib/*/libX11.so.6 2>/dev/null | head -1)
gcc $CFLAGS $SRC -o "$B/probe" -lX11 -lpthread -ldl -lm
nm -D --defined-only "$X11SO" | awk '{print $3}' | sort -u > "$B/x11.syms"
nm -D --undefined-only "$B/probe" | awk '{print $2}' | sed 's/@.*//' | sort -u > "$B/need.syms"
comm -12 "$B/x11.syms" "$B/need.syms" > "$B/use.syms"
echo "libX11 符号 $(wc -l < "$B/use.syms") 个"
{ echo "/* libX11 链接桩，仅用于链接期 */"; while read s; do echo "void $s(void){}"; done < "$B/use.syms"; } > "$B/x11stub.c"
for A in $ARCHS; do
  T="$A-linux-gnu.2.17"
  mkdir -p "$B/$A/stub"
  $ZIG cc -target $T -shared -fPIC -o "$B/$A/stub/libX11.so.6" -Wl,-soname,libX11.so.6 "$B/x11stub.c"
  ln -sf libX11.so.6 "$B/$A/stub/libX11.so"
  $ZIG cc -target $T $CFLAGS $SRC -o "$B/$A/shiying" -L"$B/$A/stub" -lX11 -lpthread -ldl -lm -s
  echo "== $A: $(stat -c %s "$B/$A/shiying") 字节"
done
