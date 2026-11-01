#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
DL="$WORK/dl"; V="$WORK/vendor"
mkdir -p "$DL" "$V"
fetch() {   # 架构 平台标签 轮子sha256 二进制名 二进制sha256
  local A=$1 PLAT=$2 WSHA=$3 BIN=$4 BSHA=$5
  if [ -f "$V/ffmpeg-$A" ] && echo "$BSHA  $V/ffmpeg-$A" | sha256sum -c --quiet 2>/dev/null; then echo "== ffmpeg-$A 已存在且校验通过"; return; fi
  local W="$DL/imageio_ffmpeg-0.6.0-py3-none-$PLAT.whl"
  [ -f "$W" ] || pip download --no-deps --only-binary=:all: --platform "$PLAT" --python-version 3.10 imageio-ffmpeg==0.6.0 -d "$DL" >/dev/null
  echo "$WSHA  $W" | sha256sum -c --quiet
  python3 - "$W" "imageio_ffmpeg/binaries/$BIN" "$V/ffmpeg-$A" <<'PY'
import zipfile, sys, os
with zipfile.ZipFile(sys.argv[1]) as z, open(sys.argv[3], 'wb') as f:
    f.write(z.read(sys.argv[2]))
os.chmod(sys.argv[3], 0o755)
PY
  echo "$BSHA  $V/ffmpeg-$A" | sha256sum -c --quiet
  echo "== ffmpeg-$A 就绪"
}
fetch x86_64  manylinux2014_x86_64  c7e46fcec401dd990405049d2e2f475e2b397779df2519b544b8aab515195282 ffmpeg-linux-x86_64-v7.0.2 e7e7fb30477f717e6f55f9180a70386c62677ef8a4d4d1a5d948f4098aa3eb99
fetch aarch64 manylinux2014_aarch64 1d47bebd83d2c5fc770720d211855f208af8a596c82d17730aa51e815cdee6dc ffmpeg-linux-aarch64-v7.0.2 6bb182d0d75d23028db82e9e4f723ca69b853d055698486e6984ddb2c06fb8ce

WSHA=02fa47c83703c37df6bfe4896aab339013f62bf02c5ebf2dce6da56af04ffc0a
BSHA=2ce797a0f88d7f067180338fb227f7b1928ea727bd9a4d7a1d022f7c52af71a3
BSIZE=87638016
WOUT="$V/ffmpeg-win-x86_64.exe"
WBIN="imageio_ffmpeg/binaries/ffmpeg-win-x86_64-v7.1.exe"
if [ -f "$WOUT" ] && [ "$(stat -c %s "$WOUT")" = "$BSIZE" ] && echo "$BSHA  $WOUT" | sha256sum -c --quiet 2>/dev/null; then
  echo "== ffmpeg-win-x86_64.exe 已存在且校验通过"
else
  WW="$DL/imageio_ffmpeg-0.6.0-py3-none-win_amd64.whl"
  [ -f "$WW" ] || pip download --no-deps --only-binary=:all: --platform win_amd64 --python-version 3.10 imageio-ffmpeg==0.6.0 -d "$DL" >/dev/null
  echo "$WSHA  $WW" | sha256sum -c --quiet
  python3 - "$WW" "$WBIN" "$WOUT" <<'PY'
import zipfile, sys
with zipfile.ZipFile(sys.argv[1]) as z, open(sys.argv[3], "wb") as f:
    f.write(z.read(sys.argv[2]))
PY
  [ "$(stat -c %s "$WOUT")" = "$BSIZE" ] || { echo "ffmpeg-win-x86_64.exe 大小不符" >&2; exit 1; }
  echo "$BSHA  $WOUT" | sha256sum -c --quiet
  echo "== ffmpeg-win-x86_64.exe 就绪"
fi
