#!/bin/bash
# 发版资产映射：把 release.sh 产出的中文名发布包复制成 ASCII 资产名（GitHub 会改写非 ASCII 资产名），
# 中文名作为 Releases 页面的显示标签；并生成对应 ASCII 名的 SHA256SUMS.txt。
# 用法：bash build/release_assets.sh [dist目录] [输出目录]    默认 $SY_WORK/dist 与 $SY_WORK/assets
# 输出：<输出目录>/ 下的 ASCII 名包、SHA256SUMS.txt，以及 upload.list（每行“文件路径#显示名”，供 gh release upload 使用）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${SY_WORK:-$ROOT/_work}"
DIST="${1:-$WORK/dist}"; OUT="${2:-$WORK/assets}"
[ -d "$DIST" ] || { echo "缺少发布目录 $DIST，先运行 build/release.sh" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"; : > "$OUT/upload.list"
N=0
for f in "$DIST"/拾影录屏_*; do
  [ -f "$f" ] || continue
  zh=$(basename "$f")
  rest=${zh#拾影录屏_}; ver=${rest%%_*}; rest=${rest#*_}
  case "$rest" in
    飞腾arm64_*) a=kylin_arm64; rest=${rest#飞腾arm64_} ;;
    x86_64_*) a=linux_x86_64; rest=${rest#x86_64_} ;;
    Windows_*) a=windows_x86_64; rest=${rest#Windows_} ;;
    *) echo "未识别的架构：$zh" >&2; exit 1 ;;
  esac
  kind=${rest%%.*}; ext=${rest#*.}
  case "$kind" in 免安装) k=portable ;; 安装包) k=installer ;; *) echo "未识别的包类型：$zh" >&2; exit 1 ;; esac
  en="shiying_${ver}_${a}_${k}.${ext}"
  cp "$f" "$OUT/$en"
  echo "$OUT/$en#$zh" >> "$OUT/upload.list"; N=$((N+1))
done
[ "$N" -gt 0 ] || { echo "$DIST 下没有发布包" >&2; exit 1; }
(cd "$OUT" && sha256sum shiying_* > SHA256SUMS.txt)
# 中文校验值.txt 里的文件名与 ASCII 资产名对不上，不上传；ASCII 校验文件以中文显示名“校验值.txt”挂出
echo "$OUT/SHA256SUMS.txt#校验值.txt" >> "$OUT/upload.list"
cat "$OUT/upload.list"
