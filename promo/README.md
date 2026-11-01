# 介绍片工程（2026-10-21，34 秒，1080P 30 帧）

成片 `拾影录屏_宣传片.mp4` 与中间产物（录屏 mkv、配乐 wav、字体转换文件）不入库，可按下述步骤重建。**脚本保留了原工作区的绝对路径**，迁移后先替换：

| 原路径 | 对应 |
|---|---|
| `/tmp/v` | `promo/`（工作目录） |
| `/tmp/v/app/shiying`、`/tmp/v/app/bin/ffmpeg` | `_work/build/x86_64/shiying`、`_work/vendor/ffmpeg-x86_64` |
| `/home/claude/shiying/vendor/ffmpeg-x86_64` | `_work/vendor/ffmpeg-x86_64` |
| `/home/claude/shiying/assets/shiying-256.png` | `assets/shiying-256.png` |
| `/tmp/v/dyh_cmap.json`、`/tmp/v/dyhttf/` | Smiley Sans 字体分片（见第 1 步） |

## 流程
1. **字体**：`npm pack @chinese-fonts/dyh@3.0.0`，解压后用 fontTools 把 `dist/SmileySans-Oblique/*.woff2` 转成 TTF，并生成“码位→分片文件”映射 `dyh_cmap.json`（转换代码当时直接在终端执行，未存成脚本：遍历 woff2，`TTFont(f).flavor=None` 另存 ttf，合并各分片 `getBestCmap()`）。
2. **桌面布置**：Xvfb :99（1920×1080）＋openbox＋PulseAudio（`pa.pa`）；壁纸与“架构图”窗口由当时终端里的一段 PIL 代码生成（未存成脚本），终端窗口为 `xterm -e top`。
3. **录屏**：`act.py` 提供平滑鼠标、点击、按键、x11grab 录制函数；`s1.py`（主界面→开录→停止）、`s23.py`（框选、区域录制、暂停继续）、`s45.py`（全局快捷键、强杀后恢复）依次运行，输出 `rec/*.mkv`。主窗口坐标 `W()` 依赖窗口实际位置，需先用 `xwininfo` 核对。
4. **配乐**：`python3 music.py` → `music.wav`（120 BPM 原创电子乐，34 秒，0–2 秒引子、2 秒落拍、30 秒收尾重拍）。
5. **合成**：`python3 compose.py [t1,t2,…]` 带参数只出预览帧，不带参数逐帧合成并用 ffmpeg 编码；之后用 `-af loudnorm=I=-14:TP=-1.0:LRA=9 -ar 48000` 重新封装音频（compose.py 里直接 loudnorm 会输出 96 kHz）。`strip.py` 生成带时间戳的抽帧总览，用来对齐剪辑点。

成片无旁白：配音用本机 TTS，音色与环境不入库。
