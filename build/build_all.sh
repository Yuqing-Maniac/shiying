#!/bin/bash
# 一键：取依赖 → 编译两种架构 → 出全部发布包（$SY_WORK/dist，默认 <仓库>/_work/dist）
# 写于 2026-10-22。
set -e
D="$(cd "$(dirname "$0")" && pwd)"
bash "$D/fetch_deps.sh"
bash "$D/build.sh" all
bash "$D/build_win.sh"
bash "$D/release.sh"
