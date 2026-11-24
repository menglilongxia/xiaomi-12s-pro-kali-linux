#!/usr/bin/env bash
# Install Xiaomi 12S Pro (unicorn) Kali/Debian rootfs configurations, services, and helper scripts.
# Run as root on the target device (or pass ROOTFS=/path/to/mounted/rootfs).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
TARGET="${ROOTFS:-/}"

echo "=== 1. Deploying rootfs overlay to $TARGET ==="
cp -a "$REPO_ROOT/rootfs/." "$TARGET/"
chmod 0755 "$TARGET/usr/local/bin"/unicorn-*.sh "$TARGET/usr/local/libexec"/unicorn-* 2>/dev/null || true

if [ "$TARGET" = "/" ]; then
    echo "=== 2. Installing recommended desktop, camera, modem, and 7x24 server packages ==="
    if command -v apt-get >/dev/null 2>&1; then
        DEBIAN_FRONTEND=noninteractive apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y \
            libcamera-tools libcamera-ipa libspa-0.2-libcamera gstreamer1.0-libcamera \
            gstreamer1.0-pipewire libcamera-v4l2 gnome-snapshot \
            xdg-desktop-portal xdg-desktop-portal-gtk \
            modemmanager libqmi-utils rmtfs tqftpserv \
            iio-sensor-proxy chrony v4l-utils
    fi

    echo "=== 3. Reloading systemd, udev, sysctl, and enabling Unicorn services ==="
    sysctl --system >/dev/null 2>&1 || true
    udevadm control --reload-rules 2>/dev/null || true
    systemctl daemon-reload
    systemctl enable \
        chrony \
        rmtfs \
        tqftpserv \
        ModemManager \
        unicorn-perf.service \
        unicorn-sensors.service \
        unicorn-modem-setup.service \
        unicorn-cam-ready.service \
        unicorn-zram.service \
        unicorn-batt-guard.service \
        unicorn-wifi-watchdog.service 2>/dev/null || true
fi

echo "Done! Remember to extract device firmware with scripts/extract-firmware-from-device.sh before rebooting."
