#!/bin/sh
# 映刻录屏：为免安装版添加开始菜单和桌面入口。
# 用法：./安装到菜单.sh            为当前用户添加
#       sudo ./安装到菜单.sh --system   为本机所有用户添加
D=$(cd "$(dirname "$0")" && pwd -P) || exit 1
EXE="$D/映刻录屏"
ICON="$D/映刻录屏.png"
[ -f "$EXE" ] && [ -f "$ICON" ] || { echo "程序或图标缺失，请完整解压后重试。" >&2; exit 1; }
chmod +x "$EXE" "$D/录制引擎/ffmpeg" || exit 1
if [ "${1-}" = "--system" ]; then
  APPS=/usr/share/applications
else
  APPS="$HOME/.local/share/applications"
fi

is_product_entry() {
  [ -f "$1" ] && [ ! -L "$1" ] &&
    grep -qx 'Type=Application' "$1" &&
    grep -qx 'StartupWMClass=shiying' "$1" &&
    grep -Eq '^Exec=.*/(拾影录屏|寸光录屏|映刻录屏)"?$' "$1" &&
    grep -Eq '^Icon=.*/(拾影录屏|寸光录屏|映刻录屏)\.png$' "$1"
}

mkdir -p "$APPS" || exit 1
F="$APPS/shiying.desktop"
if { [ -e "$F" ] || [ -L "$F" ]; } && ! is_product_entry "$F"; then
  echo "已保留同名的其他菜单入口：$F" >&2
  exit 1
fi

# Exec 有字符串转义和引号转义两层，Icon 和 Path 只需字符串转义。
exec_path=$(printf '%s' "$EXE" | sed 's/\\/\\\\\\\\/g; s/["`$]/\\\\&/g; s/%/%%/g')
icon_path=$(printf '%s' "$ICON" | sed 's/\\/\\\\/g')
work_dir=$(printf '%s' "$D" | sed 's/\\/\\\\/g')
TMP=$(mktemp "$APPS/.shiying.desktop.XXXXXX") || exit 1
trap 'rm -f "$TMP"' EXIT
cat > "$TMP" <<DESK
[Desktop Entry]
Type=Application
Name=映刻录屏
Name[zh_CN]=映刻录屏
GenericName=截图与录屏工具
Comment=全屏、窗口和区域截图与录屏
Exec="$exec_path"
Icon=$icon_path
Path=$work_dir
Terminal=false
Categories=AudioVideo;Video;Recorder;Utility;
Keywords=截图;录屏;屏幕录制;录像;screen;recorder;
StartupWMClass=shiying
StartupNotify=false
DESK
chmod 644 "$TMP" && mv -f "$TMP" "$F" || exit 1
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APPS" >/dev/null 2>&1 || :
fi

added_desktop=
if [ "${1-}" != "--system" ]; then
  DESKTOP=$(xdg-user-dir DESKTOP 2>/dev/null)
  case "$DESKTOP" in ''|"$HOME") DESKTOP="$HOME/桌面" ;; esac
  [ -d "$DESKTOP" ] || DESKTOP="$HOME/Desktop"
  if [ -d "$DESKTOP" ]; then
    TARGET="$DESKTOP/映刻录屏.desktop"
    copy_entry() {
      if cp "$F" "$1" && chmod 755 "$1"; then
        added_desktop=和桌面
        if command -v gio >/dev/null 2>&1; then
          gio set "$1" metadata::trusted true >/dev/null 2>&1 || :
        fi
      fi
    }
    found=
    for ENTRY in "$TARGET" "$DESKTOP/寸光录屏.desktop" "$DESKTOP/拾影录屏.desktop" "$DESKTOP/shiying.desktop"; do
      if is_product_entry "$ENTRY"; then
        # 复用旧入口路径，保留引用该路径的固定项。
        found=1
        copy_entry "$ENTRY"
      fi
    done
    if [ -z "$found" ]; then
      if [ -e "$TARGET" ] || [ -L "$TARGET" ]; then
        echo "已保留同名的其他桌面入口：$TARGET" >&2
      else
        copy_entry "$TARGET"
      fi
    fi
  fi
fi
echo "已添加到开始菜单$added_desktop。程序位置：$D"
echo "如需检查运行环境，可执行：\"$EXE\" --check"
