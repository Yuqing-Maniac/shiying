from act import *
move(1500, 900, 0.1)
cap_start('s4_hotkey')
time.sleep(0.4); key('Control_L', 'Alt_L', 'r'); time.sleep(2.2)
key('Control_L', 'Alt_L', 'h'); time.sleep(1.4)
key('Control_L', 'Alt_L', 'h'); time.sleep(1.4)
key('Control_L', 'Alt_L', 'r'); time.sleep(2.8)
cap_stop()
key('Control_L', 'Alt_L', 'r'); time.sleep(5)           # 开始录制（未录屏）
sh('pkill -9 -x shiying'); time.sleep(1)
cap_start('s5_recover')
sh('(/tmp/v/app/shiying >/dev/null 2>&1 &)'); time.sleep(4.5)
cap_stop()
