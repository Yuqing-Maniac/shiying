#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
cd "$ROOT"
ZIG="python3 -m ziglang"
$ZIG version >/dev/null 2>&1 || { echo "缺少 zig：pip install --break-system-packages ziglang==0.17.0" >&2; exit 1; }
command -v x86_64-w64-mingw32-windres >/dev/null || { echo "缺少 x86_64-w64-mingw32-windres（apt install binutils-mingw-w64-x86-64）" >&2; exit 1; }
OUT="$WORK/build/x86_64-windows"
mkdir -p "$OUT"
(cd build/win && x86_64-w64-mingw32-windres --preprocessor cpp -O coff -F pe-x86-64 -i app.rc -o "$OUT/app.o")
$ZIG cc -target x86_64-windows-gnu -O2 -std=gnu11 -Wall -Wno-unused-result \
  -D_FILE_OFFSET_BITS=64 -DSHIYING_WIN -Wl,--subsystem,windows \
  src/winmain.c src/session.c src/cfg.c src/path.c "$OUT/app.o" \
  -o "$OUT/拾影录屏.exe" \
  -lgdi32 -luser32 -lole32 -lshell32 -luuid -loleaut32 -lcomctl32
echo "== windows: $(stat -c %s "$OUT/拾影录屏.exe") 字节"
x86_64-w64-mingw32-objdump -x "$OUT/拾影录屏.exe" | grep -q "Entry: ID: 0x00000e" || { echo "exe 缺少 RT_GROUP_ICON" >&2; exit 1; }
x86_64-w64-mingw32-objdump -x "$OUT/拾影录屏.exe" | grep -q "Entry: ID: 0x000003" || { echo "exe 缺少 RT_ICON" >&2; exit 1; }
