#!/bin/sh
# Adjust PipeWire stereo speaker volume with physical Volume Up/Down keys
export XDG_RUNTIME_DIR="/run/user/$(id -u)"
case "$1" in
    up)   wpctl set-volume -l 1.2 @DEFAULT_AUDIO_SINK@ 5%+ ;;
    down) wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%- ;;
    mute) wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle ;;
esac
VOL=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null | awk '{printf "%d%%", $2*100}')
MUTE=$(wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null | grep -q MUTED && echo " (静音)" || echo "")
notify-send -t 1000 -h string:x-canonical-private-synchronous:volume "音量: ${VOL}${MUTE}" 2>/dev/null || true
