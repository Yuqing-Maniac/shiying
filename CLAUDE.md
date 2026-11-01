# CLAUDE.md：拾影录屏 Shiying 维护指南（给接手的 Claude）

## 1. 一句话
银河麒麟 V10 轻量录屏工具，飞腾 arm64 为主、兼出 x86_64。C 语言写的 Xlib 自绘界面（约 0.12 MB）负责交互和调度，录制交给同目录的静态 ffmpeg 子进程（x11grab＋libx264＋AAC），声音在界面进程里用 libpulse-simple 采集混音后经管道送给 ffmpeg。

本目录位于 `Yuqing-Maniac/toolbox` 仓库的 `shiying/` 下，先读仓库根目录的 `CLAUDE.md`（总则、分支与发版、红线）。下文所说的“仓库”“<仓库>”都指本工具目录 `shiying/`。

## 2. 目录
```
src/          common.h（Linux 界面公共声明）、path.h（版本号 APP_VERSION）
              main.c   界面与主循环：主窗口、框选浮层、录制控制条、倒计时、目录选择、全局快捷键、单实例、--check 自检
              draw.c   软件绘制：抗锯齿圆角／圆／多边形、双线性贴图、FreeType 文字（运行时 dlopen）
              rec.c    录制引擎：分段 ffmpeg、暂停／继续、合并、崩溃恢复、引擎定位
              audio.c  声音：libpulse-simple 采集、全局时间轴混音、按段供声
              util.c   配置读写、XDG 视频目录、XRandR 显示器枚举
assets/       图标 PNG（16–256）与 icon_data.h（_NET_WM_ICON 与界面内绘制用），由 build/mkicon.py 生成
build/        build.sh（麒麟编译）、build_win.sh（Windows 交叉编译）、fetch_deps.sh（取录制引擎）、release.sh（出包）、build_all.sh（一键）、mkicon.py
build/package/ 进发布包的文件：README.txt→使用说明.txt、install.sh→安装到菜单.sh、uninstall.sh→卸载.sh、LICENSE-ffmpeg.txt→许可说明.txt
tests/        run_tests.sh（自动回归，2026-10-22 写）；manual/（开发期手工测试脚本原件，路径未改，见其 README）
docs/         HANDOFF_2026-10-21.md（开发当天的交接说明原件）、TEST_REPORT.md（开发期实测数据）
promo/        34 秒介绍片的录屏驱动、配乐合成、逐帧合成脚本（原件，路径未改，见 promo/README.md）
_work/        （gitignore）构建、依赖、发布包、测试临时文件；可用环境变量 SY_WORK 改位置
```

## 3. 交付约定（用户的长期要求，必须遵守）
- **离线、免安装、无需管理员权限**；目标机常为不联网的银河麒麟桌面版 V10（飞腾等 arm64 机型，内核 5.4，UKUI，kysec 可能拦截未签名程序）。
- **免安装版与安装版（deb）分开打包**；任何发布包都**不内嵌教程／宣传视频**。
- **本产品的发布包与包内文件名一律中文**（用户 2026-10-21 明确要求，覆盖了其他工具“包内 ASCII 文件名”的通用做法）：tar.gz 原生保留 UTF-8 文件名；zip 设置 UTF-8 标志位（flag bit 11）。若用户机器上 zip 出现乱码，让其改用 tar.gz，或回退为包内 ASCII、首次运行改名的方案。**仓库内的文件名保持 ASCII。**
- 包内**不得有符号链接**（U 盘 FAT／exFAT／NTFS 解压失败），`release.sh` 会检查。
- 交付附 `校验值.txt`（sha256）；中文说明用全角标点。
- 用户偏好增量升级包、软件不依赖本机系统时间（部分目标机时间不准且无权修改）：本程序只用单调时钟计时，文件名中的日期取系统时间，这一点可接受。
- 回复用中文，简洁，适合手机阅读；端到端自主执行，少打断。

## 4. 关键技术决定与踩过的坑
**依赖与兼容**
- 界面程序只链接 libX11 与 glibc（目标 glibc 2.17），FreeType、fontconfig、XRandR、libpulse-simple 全部 `dlopen`，缺失时降级（无中文字体则拒绝启动并提示，无 Pulse 则只能无声）。链接期用自动生成的 libX11 **桩库**，所以交叉编译不需要目标架构的 X11 库。
- 交叉编译用 `zig cc`（PyPI 包 `ziglang==0.17.0`），`-target <arch>-linux-gnu.2.17`。沙箱里 ports.ubuntu.com、GitHub、ffmpeg.org 都不可达，所以既没有 arm64 系统库也没法自己编 ffmpeg，这是“桩库＋dlopen＋现成静态 ffmpeg”方案的由来。
- 录制引擎取自 PyPI `imageio-ffmpeg==0.6.0` 的 manylinux2014 轮子（FFmpeg 7.0.2 全静态，含 x11grab／XCB、libx264、aac）。npm `@ffmpeg-installer/linux-arm64`（4.1.4）也能用但版本旧，未采用。
- 中文字体：fontconfig 按 `sans-serif:lang=zh-cn` 匹配并校验含 CJK 字形，失败再按常见路径兜底（Noto CJK、文泉驿、Droid）。粗体缺失时用 FreeType 合成粗体。

**ffmpeg 7.0.2 的三个坑**
1. **x11grab 不响应 SIGINT／SIGTERM**（进程挂在 futex 上不退出）。改为：标准输入作控制管道，写 `q` 正常收尾（0.1–0.5 秒退出）；声音因此改走 fd 3（`-i /proc/self/fd/3`），因为用 `pipe:0` 会关掉键盘交互。
2. **输入探测会吞掉开头约 2.4 秒**：两路输入都要 `-probesize 32 -analyzeduration 0`。
3. **原始帧队列会占爆内存**：视频 `-thread_queue_size 16`；x264 用 `sliced-threads=1:rc-lookahead=0:sync-lookahead=0`、`-threads 4`。实测 1080P 标准档 RSS 由约 200 MB 降到约 150 MB，流畅档约 75 MB。

**音画同步**
- x11grab 的时间戳是**系统实时时钟**。每段开录时同时取实时与单调两个时钟：ffmpeg 用 `-copyts`，声音输入 `-itsoffset <实时秒>`，输出 `-output_ts_offset -<实时秒>`；混音线程以单调时钟驱动，全局样本 n 对应 `g_origin + n/48000`，每段从自己起点对应的样本开始写。
- 暂停时旧段的声音管道转为“收尾端”继续供声，直到该段 ffmpeg 退出，保证每段音画同时结束；新段立即开始，不等旧段退出。
- 管道写端非阻塞、容量 1 页；混音历史 1 秒；每路声源抖动缓冲 40 ms 起步，欠载时自适应加大到最多 300 ms，积压超过 200 ms 丢旧数据。
- 实测 40 秒连续录制无漂移，音画偏差 ≤ 0.06 秒（见 docs/TEST_REPORT.md）。

**可靠性**
- 每段写 mkv：`-cluster_time_limit 2000 -flush_packets 1`，异常中断最多损失约 2 秒。子进程设 `PR_SET_PDEATHSIG=SIGKILL`，界面被杀时 ffmpeg 不会一直录下去。
- 临时目录 `<保存目录>/.shiying-<时间>/`，内含 `target.txt`（目标文件名）与 `seg_NNN.mkv`；正常结束后用 concat 无损合并并删除。启动时发现残留目录即后台合并为 `…_恢复.<ext>`；若无有效分段则删除。
- 收尾超过 12 秒强制 SIGKILL。合并失败时保留分段并在界面提示路径。
- 引擎所在分区 `noexec`（U 盘等）或无执行位时，自动复制到 `~/.cache/shiying/ffmpeg` 再用。

**桌面集成**（吸取千寻的教训）
- 窗口 WM_CLASS、`.desktop` 的 `StartupWMClass`、deb 包名统一为 ASCII 的 `shiying`；`.desktop` 文件名也用 ASCII（`shiying.desktop`，桌面上的副本叫 `拾影录屏.desktop`）；Icon 用 PNG 绝对路径。
- 单实例：选择区 `_SHIYING_INSTANCE`，第二个进程把命令（显示／切换／暂停／停止／退出）用 ClientMessage 转给第一个后退出。
- 全局快捷键用 XGrabKey，同时抓 CapsLock、NumLock 组合；被占用时界面提示改用系统自定义快捷键绑定 `--toggle`。
- 控制条在有合成器时用 32 位 ARGB 视觉做圆角半透明，没有时退化为不透明矩形。
- 界面缩放读 `Xft.dpi`（可用 `SHIYING_SCALE` 覆盖）。

**调试开关**：`SHIYING_KEEP_SEGMENTS=1` 保存后不删分段（排查同步问题用）。

## 5. 构建与测试

**依赖（Ubuntu 24.04 x86_64；需能访问 archive.ubuntu.com 与 pypi.org）**
```
apt-get install -y gcc binutils dpkg-dev python3-pip python3-numpy python3-pil \
                   libx11-dev libxrandr-dev libfreetype-dev libfontconfig-dev libpulse-dev
pip install --break-system-packages ziglang==0.17.0
# 测试另需：
apt-get install -y xvfb pulseaudio pulseaudio-utils x11-xserver-utils fonts-noto-cjk file
```
头文件只取自宿主（X11、freetype2、fontconfig、pulse），由 build.sh 复制到 `_work/build/include`；arm64 不需要任何目标库。

**不入库、需重建的依赖**

| 依赖 | 来源 | 重建方式 | 校验 |
|---|---|---|---|
| 录制引擎 `ffmpeg-x86_64` | PyPI `imageio-ffmpeg==0.6.0`，`manylinux2014_x86_64` 轮子内 `ffmpeg-linux-x86_64-v7.0.2` | `build/fetch_deps.sh` | 轮子 `c7e46fce…5282`，二进制 `e7e7fb30…eb99` |
| 录制引擎 `ffmpeg-aarch64` | 同上，`manylinux2014_aarch64` 轮子内 `ffmpeg-linux-aarch64-v7.0.2` | `build/fetch_deps.sh` | 轮子 `1d47bebd…6dc`，二进制 `6bb182d0…b8ce` |
| 录制引擎 `ffmpeg-win-x86_64.exe` | 同上，`win_amd64` 轮子内 `ffmpeg-win-x86_64-v7.1.exe` | `build/fetch_deps.sh` | 轮子 `02fa47c8…c0a`，二进制 `2ce797a0…1a3`，87638016 字节 |
| zig 交叉编译器 | PyPI `ziglang==0.17.0` | pip | — |
| 发布包、编译产物 | — | `build/build_all.sh` | `_work/dist/校验值.txt` |
| 宣传片字体 Smiley Sans | npm `@chinese-fonts/dyh@3.0.0`（woff2 分片，转 TTF） | 见 promo/README.md | — |

完整 sha256 写在 `build/fetch_deps.sh` 里，下载后自动校验。

**构建**
```
bash build/fetch_deps.sh        # 录制引擎 → _work/vendor/ffmpeg-{x86_64,aarch64,win-x86_64.exe}
bash build/build.sh all         # 麒麟界面 → _work/build/{x86_64,aarch64}/shiying
bash build/build_win.sh         # Windows 界面 → _work/build/x86_64-windows/拾影录屏.exe
bash build/release.sh           # 发布包 → _work/dist/（麒麟包在前，Windows zip 在后，另有校验值.txt）
bash build/build_all.sh         # 以上三步
python3 build/mkicon.py assets  # 改图标时重新生成 assets/（含 icon_data.h）
```

**测试**
```
bash tests/run_tests.sh         # 无头回归：自检、单实例、录制／暂停／继续／停止、混音、音画同步、崩溃恢复、
                                #          noexec 回退、发布包结构、arm64 依赖与 glibc 版本
```
arm64 界面程序无法在 x86 沙箱里启动（没有 arm64 运行库），只能核对依赖与符号；arm64 引擎可以用 `qemu-aarch64-static` 包装成 `录制引擎/ffmpeg` 跑通全流程（开发时做过，见 docs/TEST_REPORT.md）。

**CI（仓库根目录 `.github/workflows/`）**
- `shiying-ci.yml`：PR 与合并到 main 时，装依赖 → `fetch_deps.sh` → `build.sh all` → `build_win.sh` → `release.sh` → `tests/run_tests.sh`。Windows 免安装包再在 `windows-latest` 上只跑 `拾影录屏.exe --check`，不录屏。
- `shiying-release.yml`：在 Releases 页面发布标签 `shiying-v<版本>` 后，先校验标签版本等于 `APP_VERSION`（不等即失败），再 `build_all.sh` 从零构建，由 `build/release_assets.sh` 把 `_work/dist/` 下的包复制成 ASCII 资产名（如 `shiying_<版本>_kylin_arm64_portable.tar.gz`），以中文名作显示标签挂到该版本，并附对应 ASCII 名的 `SHA256SUMS.txt`（显示名“校验值.txt”）。发版前先通过 PR 更新 `src/path.h` 的 `APP_VERSION` 与 `CHANGELOG.md`；release.sh 从 `APP_VERSION` 取版本号。

## 6. 现状与待办
- **未在真机验证**：飞腾麒麟上的界面、kwin 合成下控制条圆角、kysec 拦截、arm64 机型上高清档能否跑满帧率；中文名 zip 在用户机器上的解压表现。
- **可选功能**：摄像头画中画、标注画笔、GIF 导出、录制前选择具体音频设备、增量升级包。
- 改录制参数后必须跑 `tests/run_tests.sh` 的同步与混音两项；改了打包要核对包内文件名与无符号链接。

## 7. 评审清单（提 PR 与评审时逐项核对）
- 改录制参数（ffmpeg 参数、混音、暂停／继续逻辑）：必须跑 `tests/run_tests.sh` 的音画同步与混音两项，并在 PR 里贴结果。
- 改打包（release.sh、release_assets.sh、build/package/）：核对包内中文文件名正确、包内无符号链接、deb 字段无单位或个人信息。
- 改版本：同步 `src/path.h` 的 `APP_VERSION` 与 `CHANGELOG.md`；发版标签 `shiying-v<版本>` 必须与之一致。
- 不引入联网依赖：程序运行期不联网；构建期依赖固定版本并校验 sha256。
- arm64 产物只核对依赖与 glibc 符号版本（不超过 2.17），不在沙箱里运行。
- 仓库内不得出现单位名称（含拼音）、真实人名、部署环境的密级或体系属性、设备型号。
