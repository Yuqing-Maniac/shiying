#!/bin/bash
export DISPLAY=:99 HOME=/tmp/t/home XDG_RUNTIME_DIR=/tmp/t/run
pkill -x shiying; sleep 0.8; [ -n "$1" ] && sed -i "s/^format=.*/format=$1/" /tmp/t/home/.config/shiying/config.ini
cp /home/claude/shiying/build/x86_64/shiying /tmp/t/app/
pactl unload-module module-sine 2>/dev/null; xsetroot -solid black
(SHIYING_KEEP_SEGMENTS=1 /tmp/t/app/shiying > /tmp/t/app.out 2>&1 &); sleep 1.5
xdotool mousemove 960 748 click 1; sleep 3; xdotool mousemove 10 10; sleep 0.5
xsetroot -solid white; pactl load-module module-sine sink=speaker frequency=440 >/dev/null; sleep 2.5
xdotool key ctrl+alt+p; sleep 1.5; xdotool key ctrl+alt+p; sleep 0.8
xsetroot -solid black; pactl unload-module module-sine; sleep 1.2
xsetroot -solid white; pactl load-module module-sine sink=speaker frequency=440 >/dev/null; sleep 1.5
xdotool key ctrl+alt+r; sleep 3
pactl unload-module module-sine; xsetroot -solid black
tail -3 /tmp/t/home/.cache/shiying/shiying.log
python3 /tmp/t/sync.py "/tmp/t/home/视频/录屏/$(ls -t /tmp/t/home/视频/录屏/ | head -1)" 2>&1 | tail -3
