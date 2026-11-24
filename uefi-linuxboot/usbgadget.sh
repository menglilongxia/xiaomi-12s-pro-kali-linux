#!/bin/sh
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

G=/sys/kernel/config/usb_gadget/g1
mkdir -p /sys/kernel/config
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config 2>/dev/null || true

UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
if [ -n "$UDC" ]; then
    CUR=$(cat "$G/UDC" 2>/dev/null || true)
    if [ -z "$CUR" ]; then
        mkdir -p "$G"
        echo 0x1d6b > "$G/idVendor"
        echo 0x0104 > "$G/idProduct"
        echo 0x0200 > "$G/bcdUSB"
        echo 0xEF > "$G/bDeviceClass"
        echo 0x02 > "$G/bDeviceSubClass"
        echo 0x01 > "$G/bDeviceProtocol"
        mkdir -p "$G/strings/0x409"
        echo unicorn-kali-7210 > "$G/strings/0x409/serialnumber"
        echo Xiaomi > "$G/strings/0x409/manufacturer"
        echo "Xiaomi 12S Pro Kali Linux" > "$G/strings/0x409/product"
        mkdir -p "$G/configs/c.1/strings/0x409"
        echo NCM+ACM > "$G/configs/c.1/strings/0x409/configuration"
        echo 500 > "$G/configs/c.1/MaxPower"
        mkdir -p "$G/functions/ncm.usb0"
        echo 02:12:34:56:78:9a > "$G/functions/ncm.usb0/host_addr"
        echo 02:12:34:56:78:9b > "$G/functions/ncm.usb0/dev_addr"
        mkdir -p "$G/functions/acm.GS0"
        ln -sf "$G/functions/ncm.usb0" "$G/configs/c.1/"
        ln -sf "$G/functions/acm.GS0" "$G/configs/c.1/"
        echo "$UDC" > "$G/UDC"
        sleep 1
    fi
fi

IFACE=$(cat "$G/functions/ncm.usb0/ifname" 2>/dev/null || echo usb0)
ip link set "$IFACE" up 2>/dev/null || true
ip addr show dev "$IFACE" 2>/dev/null | grep -q '172.16.42.1/' || ip addr add 172.16.42.1/24 dev "$IFACE" 2>/dev/null || true

mkdir -p /run/usb0
cat > /run/usb0/udhcpd.conf <<EOF
start 172.16.42.2
end 172.16.42.10
interface $IFACE
opt subnet 255.255.255.0
lease_file /run/usb0/udhcpd.leases
EOF
touch /run/usb0/udhcpd.leases
pkill -f '/run/usb0/udhcpd.conf' 2>/dev/null || true
/bin/busybox udhcpd /run/usb0/udhcpd.conf &
exit 0
