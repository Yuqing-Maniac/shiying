#!/bin/bash
# 输出：$SY_WORK/dist/
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
cd "$ROOT"
V=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' src/path.h)
[ -n "$V" ] || { echo "读不到版本号（src/path.h APP_VERSION）" >&2; exit 1; }
OUT="$WORK/dist"; STAGE="$WORK/stage"
rm -rf "$OUT" "$STAGE"; mkdir -p "$OUT" "$STAGE"
for A in aarch64 x86_64; do
  [ -f "$WORK/build/$A/shiying" ] || { echo "缺少 $WORK/build/$A/shiying，先运行 build/build.sh" >&2; exit 1; }
  [ -f "$WORK/vendor/ffmpeg-$A" ] || { echo "缺少 $WORK/vendor/ffmpeg-$A，先运行 build/fetch_deps.sh" >&2; exit 1; }
  DA=$([ $A = aarch64 ] && echo arm64 || echo amd64)
  AN=$([ $A = aarch64 ] && echo 飞腾arm64 || echo x86_64)
  S="$STAGE/$A/映刻录屏"; mkdir -p "$S/录制引擎"
  cp "$WORK/build/$A/shiying" "$S/映刻录屏"; cp "$WORK/vendor/ffmpeg-$A" "$S/录制引擎/ffmpeg"
  # 兼容同目录升级时旧快捷方式保存的启动文件名。
  cat > "$S/拾影录屏" <<'LAUNCH'
#!/bin/sh
D=$(cd "$(dirname "$0")" && pwd -P) || exit 1
exec "$D/映刻录屏" "$@"
LAUNCH
  cp "$S/拾影录屏" "$S/寸光录屏"
  chmod 755 "$S/拾影录屏" "$S/寸光录屏"
  cp assets/shiying.png "$S/映刻录屏.png"
  cp build/package/README.txt "$S/使用说明.txt"; cp build/package/LICENSE-ffmpeg.txt "$S/许可说明.txt"
  cp build/package/install.sh "$S/安装到菜单.sh"; cp build/package/uninstall.sh "$S/卸载.sh"
  chmod 755 "$S/映刻录屏" "$S/录制引擎/ffmpeg" "$S/安装到菜单.sh" "$S/卸载.sh"; chmod 644 "$S"/*.txt "$S/映刻录屏.png"
  (cd "$STAGE/$A" && tar --owner=0 --group=0 -czf "$OUT/映刻录屏_${V}_${AN}_免安装.tar.gz" 映刻录屏)
  python3 - "$STAGE/$A" "$OUT/映刻录屏_${V}_${AN}_免安装.zip" <<'PY'
import zipfile, os, sys
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, _, fs in os.walk(root):
        for f in sorted(fs):
            p = os.path.join(d, f); arc = os.path.relpath(p, root)
            zi = zipfile.ZipInfo.from_file(p, arc)
            zi.create_system = 3
            zi.external_attr = (os.stat(p).st_mode & 0xFFFF) << 16
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.flag_bits |= 0x800                      # 文件名按 UTF-8 标记，避免乱码
            with open(p, "rb") as fh: z.writestr(zi, fh.read())
PY
  # 安装版 deb
  R="$STAGE/deb-$A"; O="$R/opt/映刻录屏"; mkdir -p "$R/DEBIAN" "$O/录制引擎" "$R/usr/bin" "$R/usr/share/applications"
  cp "$S/映刻录屏" "$S/映刻录屏.png" "$S/使用说明.txt" "$S/许可说明.txt" "$O/"; cp "$S/录制引擎/ffmpeg" "$O/录制引擎/"
  for sz in 16 24 32 48 64 128 256; do mkdir -p "$R/usr/share/icons/hicolor/${sz}x${sz}/apps"; cp assets/shiying-$sz.png "$R/usr/share/icons/hicolor/${sz}x${sz}/apps/shiying.png"; done
  printf '#!/bin/sh\nexec "/opt/映刻录屏/映刻录屏" "$@"\n' > "$R/usr/bin/映刻录屏"; cp "$R/usr/bin/映刻录屏" "$R/usr/bin/shiying"
  # 旧安装版的自定义按键或固定项可能保存了这些绝对路径。
  chmod 755 "$R/usr/bin/映刻录屏" "$R/usr/bin/shiying"
  for OLD in 拾影录屏 寸光录屏; do
    mkdir -p "$R/opt/$OLD"
    cp "$R/usr/bin/映刻录屏" "$R/usr/bin/$OLD"
    cp "$R/usr/bin/映刻录屏" "$R/opt/$OLD/$OLD"
    chmod 755 "$R/usr/bin/$OLD" "$R/opt/$OLD/$OLD"
  done
  cat > "$R"/usr/share/applications/shiying.desktop <<DESK
[Desktop Entry]
Type=Application
Name=映刻录屏
Name[zh_CN]=映刻录屏
GenericName=截图与录屏工具
Comment=全屏、窗口和区域截图与录屏
Exec=/usr/bin/shiying
Icon=/opt/映刻录屏/映刻录屏.png
Path=/opt/映刻录屏
Terminal=false
Categories=AudioVideo;Video;Recorder;Utility;
Keywords=截图;录屏;屏幕录制;录像;screen;recorder;
StartupWMClass=shiying
StartupNotify=false
DESK
  SIZE=$(du -sk "$R/opt" "$R/usr" | awk '{s+=$1} END {print s}')
  cat > "$R/DEBIAN/control" <<CTL
Package: shiying
Version: $V
Section: video
Priority: optional
Architecture: $DA
Installed-Size: $SIZE
Recommends: libpulse0
Maintainer: Shiying Maintainers <noreply@localhost>
Description: 映刻录屏 - 银河麒麟截图与录屏工具
 轻量截图与录屏工具，支持全屏、窗口和区域采集，
 系统声音与麦克风混录，暂停继续，异常中断自动恢复。
CTL
  cat > "$R/DEBIAN/postinst" <<'PI'
#!/bin/sh
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database /usr/share/applications >/dev/null 2>&1
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1
exit 0
PI
  chmod 755 "$R/DEBIAN/postinst"
  find "$R" -type d -exec chmod 755 {} +
  dpkg-deb --root-owner-group -Zxz --build "$R" "$OUT/映刻录屏_${V}_${AN}_安装包.deb" >/dev/null
done
WINEXE="$WORK/build/x86_64-windows/映刻录屏.exe"
WINFF="$WORK/vendor/ffmpeg-win-x86_64.exe"
[ -f "$WINEXE" ] || { echo "缺少 $WINEXE，先运行 build/build_win.sh" >&2; exit 1; }
[ -f "$WINFF" ] || { echo "缺少 $WINFF，先运行 build/fetch_deps.sh" >&2; exit 1; }
WS="$STAGE/windows"
mkdir -p "$WS/录制引擎"
cp "$WINEXE" "$WS/映刻录屏.exe"
# 同目录升级时，旧任务栏固定项仍可通过此前的文件名启动新版本。
cp "$WINEXE" "$WS/拾影录屏.exe"
cp "$WINEXE" "$WS/寸光录屏.exe"
cp "$WINFF" "$WS/录制引擎/ffmpeg.exe"
cp assets/shiying.png "$WS/映刻录屏.png"
cp build/package/README.txt "$WS/使用说明.txt"
cp build/package/启动说明.txt "$WS/启动说明.txt"
cp build/package/LICENSE-ffmpeg.txt "$WS/许可说明.txt"
chmod 755 "$WS/映刻录屏.exe" "$WS/拾影录屏.exe" "$WS/寸光录屏.exe" "$WS/录制引擎/ffmpeg.exe"
chmod 644 "$WS"/*.txt "$WS/映刻录屏.png"
python3 - "$WS" "$OUT/映刻录屏_${V}_Windows_免安装.zip" <<'PY'
import codecs, os, sys, zipfile
class GBKZipInfo(zipfile.ZipInfo):
    def _encodeFilenameFlags(self):
        return self.filename.encode("gbk"), self.flag_bits & ~0x800

root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for d, folders, files in os.walk(root):
        folders.sort()
        for name in sorted(files):
            path = os.path.join(d, name)
            if os.path.islink(path):
                raise SystemExit("发布目录含符号链接")
            archive_name = os.path.relpath(path, root).replace(os.sep, "/")
            # 中文 Windows 解压器用代码页 936 读取未设置 UTF-8 标志的条目。
            entry = GBKZipInfo.from_file(path, archive_name)
            with open(path, "rb") as file:
                data = file.read()
            if name.endswith(".txt") and not data.startswith(codecs.BOM_UTF8):
                data = codecs.BOM_UTF8 + data
            z.writestr(entry, data, compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
with zipfile.ZipFile(out, metadata_encoding="gbk") as archive:
    if archive.testzip() or "映刻录屏.exe" not in archive.namelist():
        raise SystemExit("Windows 压缩包校验失败")
PY
# 免安装包内不得有符号链接（U 盘 FAT/exFAT/NTFS 上解压会失败）
if find "$STAGE"/aarch64 "$STAGE"/x86_64 "$STAGE"/windows -type l | grep -q .; then echo "发布目录含符号链接" >&2; exit 1; fi
(cd "$OUT" && sha256sum 映刻录屏_*.tar.gz 映刻录屏_*.zip 映刻录屏_*.deb > 校验值.txt)
ls -la "$OUT"
