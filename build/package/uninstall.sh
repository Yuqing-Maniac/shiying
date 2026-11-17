#!/bin/sh
# 映刻录屏：移除指向本便携目录的菜单和桌面入口。
D=$(cd "$(dirname "$0")" && pwd -P) || exit 1
system=false
all=false
for arg do
  case "$arg" in
    --system) system=true ;;
    --all) all=true ;;
    *) echo "用法：./卸载.sh [--system] [--all]" >&2; exit 1 ;;
  esac
done

exec_dir=$(printf '%s' "$D" | sed 's/\\/\\\\\\\\/g; s/["`$]/\\\\&/g; s/%/%%/g')
remove_entry() {
  [ -f "$1" ] && [ ! -L "$1" ] && grep -qx 'StartupWMClass=shiying' "$1" || return 0
  for NAME in 映刻录屏 寸光录屏 拾影录屏; do
    if grep -Fqx "Exec=\"$exec_dir/$NAME\"" "$1" || grep -Fqx "Exec=\"$D/$NAME\"" "$1"; then
      rm -f "$1"
      return
    fi
  done
}

if [ "$system" = true ]; then
  remove_entry /usr/share/applications/shiying.desktop
else
  remove_entry "$HOME/.local/share/applications/shiying.desktop"
  DESKTOP=$(xdg-user-dir DESKTOP 2>/dev/null)
  case "$DESKTOP" in ''|"$HOME") DESKTOP="$HOME/桌面" ;; esac
  for DIR in "$DESKTOP" "$HOME/桌面" "$HOME/Desktop"; do
    remove_entry "$DIR/映刻录屏.desktop"
    remove_entry "$DIR/寸光录屏.desktop"
    remove_entry "$DIR/拾影录屏.desktop"
    remove_entry "$DIR/shiying.desktop"
  done
fi
if [ "$all" = true ]; then
  rm -rf "$HOME/.config/shiying" "$HOME/.cache/shiying"
  echo "已移除指向本程序目录的快捷方式和配置。"
else
  echo "已移除指向本程序目录的快捷方式，配置保留在 ~/.config/shiying（加 --all 一并删除）。"
fi
echo "程序文件夹可直接删除，录好的视频不受影响。"
