#!/usr/bin/env bash
echo "=== Xiaomi 12S Pro (unicorn) Mainline Linux Hardware Status ==="
echo "Kernel: $(uname -r)"
echo "DSPs:"
for r in /sys/class/remoteproc/remoteproc*; do
    echo "  $(basename "$r"): $(cat "$r/name") = $(cat "$r/state")"
done
echo "Cameras (/dev/media0):"
media-ctl -p -d /dev/media0 2>/dev/null | grep -E "^- entity.*(imx707|s5kjn1)" || echo "  (none)"
echo "5G Modem:"
qmicli -d qrtr://0 --dms-get-operating-mode 2>/dev/null | grep -E "Mode:|HW restricted:" || true
mmcli -m any 2>/dev/null | grep -E "own:|state:|signal quality:|operator name:|operator id:" || true
echo "Video Decoder: $(v4l2-ctl --list-devices 2>/dev/null | grep -m1 -A1 Iris | tr '\n' ' ')"
echo "Sensors (iio-sensor-proxy): $(systemctl is-active iio-sensor-proxy 2>/dev/null) / hexagonrpcd: $(systemctl is-active unicorn-sensors 2>/dev/null)"
echo "NFC: $(ls /sys/class/nfc 2>/dev/null | tr '\n' ' ') | IR: $(ls /dev/lirc* 2>/dev/null) | Flash: $(ls /sys/class/leds | grep flash | tr '\n' ' ')"
