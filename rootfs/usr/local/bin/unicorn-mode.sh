#!/usr/bin/env bash
set -euo pipefail

CMD="${1:-status}"
BL_DIR="/sys/class/backlight/ae94000.dsi.0"
[ ! -d "$BL_DIR" ] && BL_DIR=$(ls -d /sys/class/backlight/* 2>/dev/null | head -n 1 || true)
BAT=/sys/class/power_supply/qcom-battmgr-bat
USB=/sys/class/power_supply/qcom-battmgr-usb
CONF=/etc/unicorn-batt-guard.conf

case "$CMD" in
    server|headless)
        echo "Switching Xiaomi 12S Pro to 7x24 Headless Server Mode..."
        systemctl stop lightdm 2>/dev/null || true
        if [ -n "$BL_DIR" ] && [ -w "$BL_DIR/brightness" ]; then
            echo 0 > "$BL_DIR/brightness"
            [ -w "$BL_DIR/bl_power" ] && echo 4 > "$BL_DIR/bl_power" || true
        fi
        echo "Headless Server Mode ACTIVE (OLED off, LightDM stopped, SSH/Wi-Fi/5G/Watchdog/BattGuard online)."
        ;;
    desktop|gui)
        echo "Switching Xiaomi 12S Pro to 2K 120Hz Desktop Mode..."
        if [ -n "$BL_DIR" ] && [ -w "$BL_DIR/brightness" ]; then
            [ -w "$BL_DIR/bl_power" ] && echo 0 > "$BL_DIR/bl_power" || true
            echo 900 > "$BL_DIR/brightness"
        fi
        systemctl start lightdm
        echo "Desktop Mode ACTIVE."
        ;;
    charge-limit)
        ARG="${2:-status}"
        if [ "$ARG" = "off" ] || [ "$ARG" = "100" ]; then
            sed -i 's|^ENABLED=.*|ENABLED=no|' "$CONF"
            echo 2000000 > "$BAT/constant_charge_current" 2>/dev/null || true
            echo "Charge limit DISABLED -> full 100% fast charging enabled."
        elif [[ "$ARG" =~ ^[0-9]+$ ]] && [ "$ARG" -ge 55 ] && [ "$ARG" -le 95 ]; then
            START=$((ARG - 15))
            [ "$START" -lt 45 ] && START=45
            sed -i "s|^ENABLED=.*|ENABLED=yes|; s|^STOP_SOC=.*|STOP_SOC=${ARG}|; s|^START_SOC=.*|START_SOC=${START}|" "$CONF"
            systemctl restart unicorn-batt-guard
            echo "Charge limit ENABLED: hardware USB bypass at ${ARG}%, resume at ${START}%."
        else
            cat "$CONF"
            echo "Live FCC=$(cat $BAT/constant_charge_current 2>/dev/null || echo n/a) uA"
        fi
        ;;
    status)
        echo "=== Xiaomi 12S Pro Mode & 7x24 Health ==="
        GUI=$(systemctl is-active lightdm 2>/dev/null || true)
        BL=$(cat "${BL_DIR}/brightness" 2>/dev/null || echo "n/a")
        echo "Mode       : $([ "$GUI" = "active" ] && echo "DESKTOP (lightdm active, backlight=$BL)" || echo "HEADLESS SERVER (lightdm $GUI, backlight=$BL)")"
        echo "Time       : $(date) (chrony: $(systemctl is-active chrony 2>/dev/null))"
        echo "Uptime     : $(uptime -p) | Load: $(awk '{print $1,$2,$3}' /proc/loadavg)"
        echo "Memory     : $(free -h | awk '/^Mem:/ {print $3"/"$2" (avail "$7")"}') | Swap: $(free -h | awk '/^Swap:/ {print $3"/"$2}')"
        echo "Disk (/)   : $(df -h / | awk 'NR==2 {print $3"/"$2" ("$5" used, "$4" free)"}')"
        if [ -d "$BAT" ]; then
            CAP=$(cat "$BAT/capacity" 2>/dev/null || echo "?")
            ST=$(cat "$BAT/status" 2>/dev/null || echo "?")
            BV=$(awk '{printf "%.2fV", $1/1000000}' "$BAT/voltage_now" 2>/dev/null || echo "?")
            BI=$(awk '{printf "%.0fmA", $1/1000}' "$BAT/current_now" 2>/dev/null || echo "?")
            FCC=$(awk '{printf "%.0fmA", $1/1000}' "$BAT/constant_charge_current" 2>/dev/null || echo "?")
            BT=$(awk '{printf "%.1fC", $1/10}' "$BAT/temp" 2>/dev/null || echo "?")
            UV=$(awk '{printf "%.2fV", $1/1000000}' "$USB/voltage_now" 2>/dev/null || echo "?")
            UI=$(awk '{printf "%.0fmA", $1/1000}' "$USB/current_now" 2>/dev/null || echo "?")
            PD=$(cat /sys/class/typec/port0/power_operation_mode 2>/dev/null || echo "n/a")
            echo "Battery    : ${CAP}% (${ST}), ${BV}, net ${BI}, FCC=${FCC}, temp ${BT}"
            echo "USB Input  : ${UV} @ ${UI} (PD mode: ${PD})"
        fi
        MAXT=$(for z in /sys/class/thermal/thermal_zone*; do cat "$z/temp" 2>/dev/null; done | sort -rn | head -n 1 | awk '{printf "%.1f C", $1/1000}')
        echo "Peak Temp  : $MAXT | Watchdog: $(ls /dev/watchdog0 2>/dev/null || echo none)"
        echo "Guards     : wifi-wd=$(systemctl is-active unicorn-wifi-watchdog 2>/dev/null) | batt-guard=$(systemctl is-active unicorn-batt-guard 2>/dev/null) | zram=$(swapon --show --noheadings 2>/dev/null | awk '{print $1" "$3}' | tr '\n' ' ')"
        ;;
    *)
        echo "Usage: $0 {status|server|desktop|charge-limit [55-95|off|status]}"
        exit 1
        ;;
esac
