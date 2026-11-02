from act import *
move(1500, 800, 0.1)
cap_start('s1_main')
sh('(/tmp/v/app/shiying >/dev/null 2>&1 &)'); time.sleep(1.6)
click(*W(402, 251), dur=0.6); time.sleep(0.5)       # 声音：两者
click(*W(422, 291), dur=0.5); time.sleep(0.5)       # 画质：高清
click(*W(174, 291), dur=0.4); time.sleep(0.4)       # 帧率 24
sh('pactl load-module module-sine sink=speaker frequency=523 >/dev/null')
click(*W(240, 504), dur=0.6); time.sleep(0.2)       # 开始录制
move(1450, 700, 0.8)
time.sleep(6.5)                                       # 倒计时 + 录制
click(1032, 35, dur=0.7); time.sleep(2.8)           # 停止 → 保存
cap_stop()
sh('pactl unload-module module-sine')
