# 手工测试脚本（开发期原件，未改动）

开发时在 `/tmp/t` 下临时编写、逐个手动运行，**路径是当时的绝对路径**，直接运行前需替换。自动化回归请用 `tests/run_tests.sh`，它覆盖了这些脚本的检查项。

| 文件 | 用途 | 原路径假设 |
|---|---|---|
| `env.sh` | 设置 DISPLAY=:99、HOME=/tmp/t/home 等 | /tmp/t |
| `pa.pa` | PulseAudio 测试配置（null sink 作扬声器） | — |
| `feedmic.py` | 向 `module-pipe-source` 的 FIFO 按墙钟送 880 Hz，当作麦克风 | /tmp/t/mic.fifo |
| `segtest.py`／`segtest2.py` | 单段 ffmpeg 参数试验：探测参数、队列长度、x264 参数对时长与内存的影响 | /tmp/t/app/bin/ffmpeg |
| `synctest.sh` | 端到端：录制→屏幕变白＋出声→暂停→继续→停止，再调 `sync.py` 比较起点 | /tmp/t/app、/home/claude/shiying/build |
| `sync.py` | 分析成片：声音包络与画面亮度跳变的时刻 | /tmp/t/app/bin/ffmpeg |
| `drift.sh` | 40 秒长录，测漂移与 ffmpeg 峰值内存 | 同上 |
| `mixcheck.py` | 检查成片中 440 Hz 与 880 Hz 两路是否全程存在 | 同上 |

**没有存成脚本的手工检查**（当时直接在终端执行；现已并入 `run_tests.sh` 或记录在 docs/TEST_REPORT.md）：崩溃恢复（kill -9 后重启）、引擎无执行权限回退、单实例转发、`--check` 自检、qemu 跑 arm64 引擎、deb 安装与 `安装到菜单.sh`、界面截图检查（xdotool 点击各控件、框选浮层、目录选择窗）。其中**界面点击与框选浮层**、**qemu 跑 arm64 引擎**两项目前没有自动化脚本。
