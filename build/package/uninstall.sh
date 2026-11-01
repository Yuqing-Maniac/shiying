#!/bin/sh
# 拾影录屏：移除开始菜单和桌面图标（不删除录好的视频）
rm -f "$HOME/.local/share/applications/shiying.desktop"
DESKTOP=$(xdg-user-dir DESKTOP 2>/dev/null)
[ -n "$DESKTOP" ] && rm -f "$DESKTOP/拾影录屏.desktop" "$DESKTOP/shiying.desktop"
rm -f "$HOME/桌面/拾影录屏.desktop" "$HOME/Desktop/拾影录屏.desktop"
[ -w /usr/share/applications/shiying.desktop ] && rm -f /usr/share/applications/shiying.desktop
if [ "$1" = "--all" ]; then
  rm -rf "$HOME/.config/shiying" "$HOME/.cache/shiying"
  echo "已移除菜单、桌面图标和配置。"
else
  echo "已移除菜单和桌面图标。配置保留在 ~/.config/shiying（加 --all 一并删除）。"
fi
echo "程序文件夹可直接删除；录好的视频不受影响。"
