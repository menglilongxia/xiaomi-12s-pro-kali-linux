#!/bin/sh
if xrandr 2>/dev/null | grep -q '^DSI-1 connected'; then
    xrandr --output DSI-1 --mode 1440x3200 --rate 120 --rotate right 2>/dev/null || \
    xrandr --output DSI-1 --rotate right 2>/dev/null || true
else
    xrandr --output None-1 --rotate right 2>/dev/null || true
fi
