#!/bin/bash
export DISPLAY=:99 HOME=/tmp/t/home XDG_RUNTIME_DIR=/tmp/t/run
pkill -x shiying; sleep 0.8; sed -i "s/^format=.*/format=0/; s/^audio=.*/audio=3/" /tmp/t/home/.config/shiying/config.ini
pactl unload-module module-sine 2>/dev/null; xsetroot -solid black
(/tmp/t/app/shiying > /tmp/t/app.out 2>&1 &); sleep 1.5
xdotool mousemove 960 748 click 1; sleep 2; xdotool mousemove 10 10
PEAK=0
for i in $(seq 1 40); do
  sleep 1
  if [ $i = 3 ]; then xsetroot -solid white; pactl load-module module-sine sink=speaker frequency=440 >/dev/null; fi
  if [ $i = 5 ]; then xsetroot -solid black; pactl unload-module module-sine; fi
  if [ $i = 36 ]; then xsetroot -solid white; pactl load-module module-sine sink=speaker frequency=440 >/dev/null; fi
  R=$(ps -o rss= -C ffmpeg | sort -n | tail -1); [ -n "$R" ] && [ $R -gt $PEAK ] && PEAK=$R
done
G=$(ps -o rss= -C shiying)
xdotool key ctrl+alt+r; sleep 4
pactl unload-module module-sine; xsetroot -solid black
echo "peak ffmpeg RSS ${PEAK}KB, GUI RSS ${G}KB"
tail -2 /tmp/t/home/.cache/shiying/shiying.log
python3 /tmp/t/sync.py "/tmp/t/home/视频/录屏/$(ls -t /tmp/t/home/视频/录屏/ | head -1)" 2>&1 | tail -3
