#!/bin/sh
for g in /sys/devices/system/cpu/cpufreq/policy*/schedutil; do
    [ -f "$g/rate_limit_us" ] && echo 500 > "$g/rate_limit_us" 2>/dev/null || true
    [ -f "$g/up_rate_limit_us" ] && echo 500 > "$g/up_rate_limit_us" 2>/dev/null || true
    [ -f "$g/down_rate_limit_us" ] && echo 40000 > "$g/down_rate_limit_us" 2>/dev/null || true
done
for gpu in /sys/class/devfreq/*gpu*; do
    [ -f "$gpu/min_freq" ] && echo 364000000 > "$gpu/min_freq" 2>/dev/null || true
done
sysctl -q -w vm.swappiness=10 vm.vfs_cache_pressure=50 2>/dev/null || true
modprobe st-ftm5 2>/dev/null || true
iw dev wlan0 set power_save off 2>/dev/null || true
