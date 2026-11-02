#!/bin/sh
# 拾影录屏：添加到开始菜单和桌面（免安装版用，不需要管理员权限）
# 用法：./安装到菜单.sh            为当前用户添加
#       sudo ./安装到菜单.sh --system   为本机所有用户添加
D=$(cd "$(dirname "$0")" && pwd)
chmod +x "$D/拾影录屏" "$D/录制引擎/ffmpeg" 2>/dev/null
if [ "$1" = "--system" ]; then
  APPS=/usr/share/applications
else
  APPS="$HOME/.local/share/applications"
fi
mkdir -p "$APPS"
F="$APPS/shiying.desktop"
cat > "$F" <<DESK
[Desktop Entry]
Type=Application
Name=拾影录屏
Name[zh_CN]=拾影录屏
GenericName=录屏工具
Comment=轻量录屏：全屏、区域、窗口录制，支持系统声音和麦克风
Exec="$D/拾影录屏"
Icon=$D/拾影录屏.png
Terminal=false
Categories=AudioVideo;Video;Recorder;Utility;
Keywords=录屏;屏幕录制;录像;screen;recorder;
StartupWMClass=shiying
StartupNotify=false
DESK
chmod 644 "$F"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" >/dev/null 2>&1
if [ "$1" != "--system" ]; then
  DESKTOP=$(xdg-user-dir DESKTOP 2>/dev/null)
  [ -z "$DESKTOP" ] || [ "$DESKTOP" = "$HOME" ] && DESKTOP="$HOME/桌面"
  [ -d "$DESKTOP" ] || DESKTOP="$HOME/Desktop"
  if [ -d "$DESKTOP" ]; then
    cp "$F" "$DESKTOP/拾影录屏.desktop" && chmod 755 "$DESKTOP/拾影录屏.desktop"
    command -v gio >/dev/null 2>&1 && gio set "$DESKTOP/拾影录屏.desktop" metadata::trusted true >/dev/null 2>&1
  fi
fi
echo "已添加到开始菜单${DESKTOP:+和桌面}。程序位置：$D"
echo "如需检查运行环境，可执行：\"$D/拾影录屏\" --check"
