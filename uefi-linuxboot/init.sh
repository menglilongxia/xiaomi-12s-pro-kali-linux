#!/bin/busybox sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /sys/kernel/config /mnt/kali /etc /var/lib/misc
mount -t devpts devpts /dev/pts 2>/dev/null || true
mount -t configfs none /sys/kernel/config 2>/dev/null || true

[ -c /dev/kmsg ] || mknod -m 666 /dev/kmsg c 1 11 2>/dev/null || true
[ -c /dev/console ] || mknod -m 600 /dev/console c 5 1 2>/dev/null || true
[ -c /dev/null ] || mknod -m 666 /dev/null c 1 3 2>/dev/null || true

log() {
    echo "$*" > /dev/kmsg 2>/dev/null || true
}

log "======================================================"
log "  MAINLINE LINUX 7.2.10 INITRAMFS ON XIAOMI 12S PRO"
log "======================================================"
log "uname: $(uname -a)"
log "mem: $(free -m | tr '\n' ' ')"

# Wait up to 8s for UDC and UFS /dev/sda36 async probes
i=0
while [ "$i" -lt 8 ]; do
    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
    [ -n "$UDC" ] && [ -b /dev/sda36 ] && break
    sleep 1
    i=$((i + 1))
done

log "UDC=$(ls /sys/class/udc 2>/dev/null | tr '\n' ' ')"
log "BLOCKS=$(ls /dev/sda* 2>/dev/null | head -n 15 | tr '\n' ' ')"
log "FB=$(ls /dev/fb* /dev/dri/* 2>/dev/null | tr '\n' ' ')"

setup_usb_gadget() {
    G=/sys/kernel/config/usb_gadget/g1
    UDC=$(ls /sys/class/udc 2>/dev/null | head -n 1)
    if [ -z "$UDC" ]; then
        log "[usb] NO UDC found in /sys/class/udc!"
        return 1
    fi
    log "[usb] Configuring NCM+ACM (1d6b:0104) on UDC=$UDC ..."
    mkdir -p "$G"
    echo 0x1d6b > "$G/idVendor"
    echo 0x0104 > "$G/idProduct"
    echo 0x0200 > "$G/bcdUSB"
    echo 0xEF > "$G/bDeviceClass"
    echo 0x02 > "$G/bDeviceSubClass"
    echo 0x01 > "$G/bDeviceProtocol"

    mkdir -p "$G/strings/0x409"
    echo "unicorn-kali-7210" > "$G/strings/0x409/serialnumber"
    echo "Xiaomi" > "$G/strings/0x409/manufacturer"
    echo "Xiaomi 12S Pro Kali Linux" > "$G/strings/0x409/product"

    mkdir -p "$G/configs/c.1/strings/0x409"
    echo "NCM+ACM" > "$G/configs/c.1/strings/0x409/configuration"
    echo 500 > "$G/configs/c.1/MaxPower"

    mkdir -p "$G/functions/ncm.usb0"
    echo "02:12:34:56:78:9a" > "$G/functions/ncm.usb0/host_addr"
    echo "02:12:34:56:78:9b" > "$G/functions/ncm.usb0/dev_addr"

    mkdir -p "$G/functions/acm.GS0"

    ln -sf "$G/functions/ncm.usb0" "$G/configs/c.1/"
    ln -sf "$G/functions/acm.GS0" "$G/configs/c.1/"

    echo "$UDC" > "$G/UDC"
    sleep 1
    IFACE=$(cat "$G/functions/ncm.usb0/ifname" 2>/dev/null || echo usb0)
    ip link set "$IFACE" up 2>/dev/null || ifconfig "$IFACE" up 2>/dev/null || true
    ip addr add 172.16.42.1/24 dev "$IFACE" 2>/dev/null || ifconfig "$IFACE" 172.16.42.1 netmask 255.255.255.0 up 2>/dev/null || true
    log "[usb] NCM UP on $IFACE 172.16.42.1/24 + ACM (/dev/ttyGS0)"
}

start_rescue_shell() {
    IFACE=$(cat /sys/kernel/config/usb_gadget/g1/functions/ncm.usb0/ifname 2>/dev/null || echo usb0)
    cat > /etc/udhcpd.conf <<EOF
start 172.16.42.2
end 172.16.42.10
interface $IFACE
opt subnet 255.255.255.0
lease_file /var/lib/misc/udhcpd.leases
EOF
    touch /var/lib/misc/udhcpd.leases
    udhcpd /etc/udhcpd.conf 2>/dev/null &
    telnetd -l /bin/sh -p 23 -b 0.0.0.0 2>/dev/null &
    if [ -c /dev/ttyGS0 ]; then
        ( while true; do /bin/sh </dev/ttyGS0 >/dev/ttyGS0 2>&1; sleep 1; done ) &
    fi
    log "=== RESCUE SHELL READY (telnet 172.16.42.1:23 / COM ttyGS0) ==="
    while true; do sleep 60; done
}

setup_usb_gadget

if [ -b /dev/sda36 ]; then
    if mount -t ext4 -o rw /dev/sda36 /mnt/kali 2>/dev/null; then
        MARKER=$(cat /mnt/kali/etc/unicorn-kali-rootfs-release 2>/dev/null || true)
        log "[ufs] Mounted /dev/sda36 rw! marker=$MARKER"
        if [ -x /mnt/kali/sbin/init ] && [ -n "$MARKER" ]; then
            log "[boot] Switching root to Kali Linux (/sbin/init)..."
            umount /dev/pts 2>/dev/null || true
            umount /sys/kernel/config 2>/dev/null || true
            mount --move /dev /mnt/kali/dev 2>/dev/null || true
            mount --move /proc /mnt/kali/proc 2>/dev/null || true
            mount --move /sys /mnt/kali/sys 2>/dev/null || true
            exec switch_root /mnt/kali /sbin/init
            log "[boot] ERROR: switch_root failed!"
        else
            log "[ufs] Missing /sbin/init or marker on /dev/sda36; dropping to rescue shell"
        fi
    else
        log "[ufs] Mount /dev/sda36 failed rc=$?; dropping to rescue shell"
    fi
else
    log "[ufs] /dev/sda36 not present; dropping to rescue shell"
fi

start_rescue_shell
